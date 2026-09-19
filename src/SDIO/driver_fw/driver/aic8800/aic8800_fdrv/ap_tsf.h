/* SPDX-License-Identifier: GPL-2.0 */
/* Experimental, opt-in TSF export for AP / AIC8800D80 on Linux 6.18. */
#include <linux/rtnetlink.h>
#include <linux/sysfs.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/workqueue.h>

static bool ap_tsf;
module_param(ap_tsf, bool, 0444);
MODULE_PARM_DESC(ap_tsf, "Enable experimental D80 binary TSF export (AP only)");

#define AP_TSF_LO 0x403280a4
#define AP_TSF_HI 0x403280a8

static int ap_tsf_read_word(struct rwnx_hw *hw, u32 address, u32 *value)
{
	struct dbg_mem_read_cfm reply = { 0 };
	int ret = rwnx_send_dbg_mem_read_req(hw, address, &reply);

	if (ret)
		return ret < 0 ? ret : -EIO;
	if (reply.memaddr != address)
		return -EIO;
	*value = reply.memdata;
	return 0;
}

/* One coherent sample: high, low, high again, retried while the high word
 * moves. This is the expensive part - three SDIO register round trips. */
static int ap_tsf_sample(struct rwnx_hw *hw, u64 *tsf)
{
	u32 high, low, high_after;
	int attempt, ret;

	for (attempt = 0; attempt < 3; attempt++) {
		ret = ap_tsf_read_word(hw, AP_TSF_HI, &high);
		if (ret)
			return ret;
		ret = ap_tsf_read_word(hw, AP_TSF_LO, &low);
		if (ret)
			return ret;
		ret = ap_tsf_read_word(hw, AP_TSF_HI, &high_after);
		if (ret)
			return ret;
		if (high == high_after) {
			*tsf = ((u64)high << 32) | low;
			return 0;
		}
	}
	return -EAGAIN;
}

/* ---------------------------------------------------------------------------
 * Cached sampling.
 *
 * Three SDIO round trips per read is hundreds of microseconds, and a media
 * pacer reads the TSF once per video frame, so the cost lands in its critical
 * path. The counter is a free-running microsecond clock, so a sample plus
 * monotonic time since the sample is exact to within the SDIO latency of the
 * sample itself: keep a worker refreshing every AP_TSF_REFRESH_MS while
 * something is reading the file, serve reads from cache, and let the worker
 * go idle when the file has not been read for AP_TSF_STALE_MS.
 *
 * The state is module-wide because the export is opt-in, D80-only and for a
 * single AP interface; reads fall back to a direct sample whenever the cache
 * is stale or the device is not reachable.
 * --------------------------------------------------------------------------- */
#define AP_TSF_REFRESH_MS 100
#define AP_TSF_STALE_MS   1000

static struct ap_tsf_cache {
	spinlock_t lock;
	u64 tsf;
	ktime_t sampled_at;
	bool valid;
	struct rwnx_hw *hw;
	unsigned long last_read;
	struct delayed_work work;
} ap_tsf_cache = {
	.lock = __SPIN_LOCK_UNLOCKED(ap_tsf_cache.lock),
	.last_read = INITIAL_JIFFIES,
};

static DEFINE_MUTEX(ap_tsf_cache_mutex);

static void ap_tsf_refresh(struct work_struct *work)
{
	struct ap_tsf_cache *cache =
		container_of(work, struct ap_tsf_cache, work.work);
	struct rwnx_hw *hw = cache->hw;
	u64 tsf;

	if (!ap_tsf || !hw)
		return;
	if (ap_tsf_sample(hw, &tsf) == 0) {
		spin_lock_bh(&cache->lock);
		cache->tsf = tsf;
		cache->sampled_at = ktime_get();
		cache->valid = true;
		spin_unlock_bh(&cache->lock);
	}

	if (time_before(jiffies, cache->last_read +
			msecs_to_jiffies(AP_TSF_STALE_MS)))
		schedule_delayed_work(&cache->work,
				      msecs_to_jiffies(AP_TSF_REFRESH_MS));
}

/* Called from the read path: arm (or re-arm) the refresher. */
static void ap_tsf_keep_warm(struct rwnx_hw *hw)
{
	mutex_lock(&ap_tsf_cache_mutex);
	if (!ap_tsf_cache.hw) {
		ap_tsf_cache.hw = hw;
		INIT_DELAYED_WORK(&ap_tsf_cache.work, ap_tsf_refresh);
	}
	ap_tsf_cache.last_read = jiffies;
	if (!work_pending(&ap_tsf_cache.work.work))
		schedule_delayed_work(&ap_tsf_cache.work,
				      msecs_to_jiffies(AP_TSF_REFRESH_MS));
	mutex_unlock(&ap_tsf_cache_mutex);
}

/* Must run before the SDIO device goes away. */
static void ap_tsf_stop(void)
{
	mutex_lock(&ap_tsf_cache_mutex);
	if (ap_tsf_cache.hw) {
		cancel_delayed_work_sync(&ap_tsf_cache.work);
		ap_tsf_cache.hw = NULL;
	}
	mutex_unlock(&ap_tsf_cache_mutex);

	spin_lock_bh(&ap_tsf_cache.lock);
	ap_tsf_cache.valid = false;
	spin_unlock_bh(&ap_tsf_cache.lock);
}

static ssize_t ap_tsf_read(struct file *file, struct kobject *kobj,
			     const struct bin_attribute *attr, char *buf,
			     loff_t offset, size_t count)
{
	struct net_device *ndev = to_net_dev(kobj_to_dev(kobj));
	struct rwnx_vif *vif = netdev_priv(ndev);
	u64 tsf = 0;
	bool have_tsf = false;
	int ret;

	/* Each read must return one coherent native-endian u64 in one call. */
	if (offset == sizeof(tsf))
		return 0;
	if (offset || count != sizeof(tsf))
		return -EINVAL;

	/* Never block on RTNL under sysfs active protection: unregister holds RTNL. */
	if (!rtnl_trylock())
		return -EAGAIN;
	ret = -ENODEV;
	if (ndev->reg_state != NETREG_REGISTERED || !netif_device_present(ndev))
		goto out;
	ret = -ENETDOWN;
	if (!netif_running(ndev) || !vif->up)
		goto out;
	ret = -EOPNOTSUPP;
	if (RWNX_VIF_TYPE(vif) != NL80211_IFTYPE_AP || !vif->ap.bcn.head)
		goto out;
	if (!vif->rwnx_hw->sdiodev ||
	    vif->rwnx_hw->sdiodev->chipid != PRODUCT_ID_AIC8800D80)
		goto out;

	ap_tsf_keep_warm(vif->rwnx_hw);

	spin_lock_bh(&ap_tsf_cache.lock);
	if (ap_tsf_cache.valid &&
	    ktime_to_ms(ktime_sub(ktime_get(), ap_tsf_cache.sampled_at)) <
		    AP_TSF_STALE_MS) {
		const s64 elapsed_us = ktime_to_us(
			ktime_sub(ktime_get(), ap_tsf_cache.sampled_at));
		tsf = ap_tsf_cache.tsf + (u64)elapsed_us;
		have_tsf = true;
	}
	spin_unlock_bh(&ap_tsf_cache.lock);

	/* Nothing sampled recently (first read, or the worker has not caught up
	 * yet): pay for a direct sample so the counter is never stale. */
	if (!have_tsf && ap_tsf_sample(vif->rwnx_hw, &tsf) != 0) {
		ret = -EAGAIN;
		goto out;
	}
	memcpy(buf, &tsf, sizeof(tsf));
	ret = sizeof(tsf);
out:
	rtnl_unlock();
	return ret;
}

static const struct bin_attribute ap_tsf_attr = {
	.attr = { .name = "tsf", .mode = 0444 },
	.size = sizeof(u64),
	.read = ap_tsf_read,
};

static const struct bin_attribute *const ap_tsf_attrs[] = {
	&ap_tsf_attr,
	NULL,
};

static const struct attribute_group ap_tsf_group = {
	.bin_attrs = ap_tsf_attrs,
};
