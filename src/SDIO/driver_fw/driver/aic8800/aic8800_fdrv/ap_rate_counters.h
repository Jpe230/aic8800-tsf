/* SPDX-License-Identifier: GPL-3.0 */
/*
 * Opt-in per-station rate/retry counters for AP-style AP operation.
 *
 * The vendor build ships CONFIG_DEBUG_FS=n, so rwnx_debugfs.o is not compiled
 * and the firmware's rate-control statistics are unreachable from userspace.
 * A host that streams video and audio needs them: the retry rate per rate is how
 * you tell a link that is carrying the stream from one that is quietly losing
 * frames, and this driver exposes no retry counter anywhere else.
 *
 * Enable with ap_rate_counters=1; a read-only "rc_counters" file then appears
 * next to the TSF export on the AP interface. Output is one line per station
 * followed by one line per used rate:
 *
 *   sta <mac> idx=<n> band=<n> ampdu_len=<n> ampdu_pkts=<n> avg_ampdu=<n> samples=<n>
 *   rate <mac> idx=<n> format=<n> mcs=<n> bw=<n> gi=<n> attempts=<n> success=<n> retries=<n> tp=<n>
 *
 * attempts/success come from the firmware's per-sample rate statistics and are
 * per sampling interval, so read the file repeatedly (or sum) for totals.
 */
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/sysfs.h>

static bool ap_rate_counters;
module_param(ap_rate_counters, bool, 0444);
MODULE_PARM_DESC(ap_rate_counters, "Export per-station rate/retry counters on the AP interface");

/* Snapshot of station indexes taken under the driver lock, then queried
 * outside it: rwnx_send_me_rc_stats() sleeps and must not hold cb_lock. */
#define AP_RATE_COUNTERS_MAX_STA (NX_REMOTE_STA_MAX + NX_VIRT_DEV_MAX)

static ssize_t ap_rate_counters_read(struct file *file, struct kobject *kobj,
				   const struct bin_attribute *attr, char *buf,
				   loff_t offset, size_t count)
{
	struct net_device *ndev = to_net_dev(kobj_to_dev(kobj));
	struct rwnx_vif *vif = netdev_priv(ndev);
	struct rwnx_hw *hw;
	struct rwnx_sta *sta;
	u8 sta_idx[AP_RATE_COUNTERS_MAX_STA];
	u8 macs[AP_RATE_COUNTERS_MAX_STA][ETH_ALEN];
	int found = 0, i, len = 0;

	if (offset)
		return 0;
	if (count < 256)
		return -EINVAL;
	if (!rtnl_trylock())
		return -EAGAIN;

	if (ndev->reg_state != NETREG_REGISTERED || !netif_device_present(ndev) ||
	    RWNX_VIF_TYPE(vif) != NL80211_IFTYPE_AP) {
		rtnl_unlock();
		return -EOPNOTSUPP;
	}
	hw = vif->rwnx_hw;

	spin_lock_bh(&hw->cb_lock);
	list_for_each_entry(sta, &vif->ap.sta_list, list) {
		if (!sta->valid || found >= AP_RATE_COUNTERS_MAX_STA)
			continue;
		sta_idx[found] = sta->sta_idx;
		memcpy(macs[found], sta->mac_addr, ETH_ALEN);
		found++;
	}
	spin_unlock_bh(&hw->cb_lock);

	for (i = 0; i < found; i++) {
		struct me_rc_stats_cfm cfm;
		int r;

		memset(&cfm, 0, sizeof(cfm));
		if (rwnx_send_me_rc_stats(hw, sta_idx[i], &cfm))
			continue;

		len += scnprintf(buf + len, count - len,
				 "sta %pM idx=%u ampdu_len=%u ampdu_pkts=%u avg_ampdu=%u samples=%u\n",
				 macs[i], sta_idx[i], cfm.ampdu_len,
				 cfm.ampdu_packets, cfm.avg_ampdu_len,
				 cfm.no_samples);

		for (r = 0; r < RC_MAX_N_SAMPLE + 1; r++) {
			const struct rc_rate_stats *rs = &cfm.rate_stats[r];
			unsigned int retries;

			if (!rs->attempts)
				continue;
			retries = rs->attempts > rs->success ?
				rs->attempts - rs->success : 0;
			/* rate_config is a packed rwnx_rate_ctrl_info. */
			len += scnprintf(buf + len, count - len,
					 "rate %pM idx=%d format=%u mcs=%u bw=%u gi=%u attempts=%u success=%u retries=%u tp=%u\n",
					 macs[i], r,
					 (rs->rate_config >> 11) & 0x7,
					 rs->rate_config & 0x7f,
					 (rs->rate_config >> 7) & 0x3,
					 (rs->rate_config >> 9) & 0x3,
					 rs->attempts, rs->success, retries,
					 cfm.tp[r]);
			if (len > (int)count - 256)
				break;
		}
		if (len > (int)count - 256)
			break;
	}

	rtnl_unlock();
	return len;
}

static const struct bin_attribute ap_rate_counters_attr = {
	.attr = { .name = "rc_counters", .mode = 0444 },
	.size = 4096,
	.read = ap_rate_counters_read,
};

static const struct bin_attribute *const ap_rate_counters_attrs[] = {
	&ap_rate_counters_attr,
	NULL,
};

static const struct attribute_group ap_rate_counters_group = {
	.bin_attrs = ap_rate_counters_attrs,
};
