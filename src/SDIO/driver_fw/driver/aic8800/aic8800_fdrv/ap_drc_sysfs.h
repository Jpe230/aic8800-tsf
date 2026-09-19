/* SPDX-License-Identifier: GPL-2.0 */
/*
 * One driver-owned sysfs group for the optional AP diagnostics.
 *
 * struct net_device gives a driver exactly one usable sysfs_groups slot.
 * netdev_register_kobject() (net/core/net-sysfs.c) skips slot 0 when the
 * driver filled it, then unconditionally writes the netdev statistics group
 * and, on a wireless netdev, the wireless group into the next two slots.
 * In effect it does:
 *
 *     groups = ndev->sysfs_groups;      - the driver's slot 0
 *     if (*groups)
 *             groups++;
 *     *groups++ = &netstat_group;       - slot 1
 *     if (wireless_group_needed(ndev))
 *             *groups++ = &wireless_group;  - slot 2
 *
 * Anything a driver registers at index 1 or 2 is therefore overwritten before
 * the device is added, so only slot 0 survives. That is why the rate/retry
 * counters never appeared while the TSF export did: both groups were
 * registered, but the counter group at index 1 was replaced by the netdev
 * statistics group on the way in.
 *
 * Merge whichever attributes are enabled into a single group at slot 0.
 * Include this after ap_tsf.h and ap_rate_counters.h.
 */
#ifndef _AP_DRC_SYSFS_H_
#define _AP_DRC_SYSFS_H_

static const struct bin_attribute *const ap_tsf_only_attrs[] = {
	&ap_tsf_attr,
	NULL,
};

static const struct bin_attribute *const ap_rate_counters_only_attrs[] = {
	&ap_rate_counters_attr,
	NULL,
};

static const struct bin_attribute *const ap_drc_attrs[] = {
	&ap_tsf_attr,
	&ap_rate_counters_attr,
	NULL,
};

static const struct attribute_group ap_tsf_only_group = {
	.bin_attrs = ap_tsf_only_attrs,
};

static const struct attribute_group ap_rate_counters_only_group = {
	.bin_attrs = ap_rate_counters_only_attrs,
};

static const struct attribute_group ap_drc_group = {
	.bin_attrs = ap_drc_attrs,
};

#endif /* _AP_DRC_SYSFS_H_ */
