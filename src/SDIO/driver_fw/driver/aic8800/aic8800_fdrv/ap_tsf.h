/* SPDX-License-Identifier: GPL-2.0 */
/* Experimental, opt-in TSF export for AP / AIC8800D80 on Linux 6.18. */
#include <linux/rtnetlink.h>
#include <linux/sysfs.h>

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

static ssize_t ap_tsf_read(struct file *file, struct kobject *kobj,
			     const struct bin_attribute *attr, char *buf,
			     loff_t offset, size_t count)
{
	struct net_device *ndev = to_net_dev(kobj_to_dev(kobj));
	struct rwnx_vif *vif = netdev_priv(ndev);
	u32 high, low, high_after;
	u64 tsf;
	int attempt, ret;

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

	for (attempt = 0; attempt < 3; attempt++) {
		ret = ap_tsf_read_word(vif->rwnx_hw, AP_TSF_HI, &high);
		if (ret)
			goto out;
		ret = ap_tsf_read_word(vif->rwnx_hw, AP_TSF_LO, &low);
		if (ret)
			goto out;
		ret = ap_tsf_read_word(vif->rwnx_hw, AP_TSF_HI, &high_after);
		if (ret)
			goto out;
		if (high == high_after) {
			tsf = ((u64)high << 32) | low;
			memcpy(buf, &tsf, sizeof(tsf));
			ret = sizeof(tsf);
			goto out;
		}
	}
	ret = -EAGAIN;
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
