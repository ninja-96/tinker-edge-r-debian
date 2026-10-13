/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Glue between the Rockchip BSP mpp_service sources and mainline Linux.
 *
 * The BSP kernel provides a few vendor interfaces (IOMMU helpers, PMU power
 * domain control, OPP/system-monitor, SiP SMC). Mainline lacks them. The IOMMU
 * helpers are provided by patches/linux/0001-iommu-rockchip-*.patch; the rest
 * is stubbed here (no DVFS, no PMU idle handling: power domains are driven by
 * runtime PM).
 */
#ifndef __MPP_COMPAT_H
#define __MPP_COMPAT_H

#include <linux/arm-smccc.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/of.h>
#include <linux/types.h>

/* --- IOMMU (implemented in drivers/iommu/rockchip-iommu.c by our patch) --- */
int rockchip_iommu_enable(struct device *dev);
int rockchip_iommu_disable(struct device *dev);
bool rockchip_iommu_is_enabled(struct device *dev);
int rockchip_iommu_force_reset(struct device *dev);
void rockchip_iommu_mask_irq(struct device *dev);
void rockchip_iommu_unmask_irq(struct device *dev);

/* --- PMU power domain: left to runtime PM / genpd --- */
static inline int rockchip_pmu_pd_on(struct device *dev) { return 0; }
static inline int rockchip_pmu_pd_off(struct device *dev) { return 0; }
static inline bool rockchip_pmu_pd_is_on(struct device *dev) { return true; }
static inline int rockchip_pmu_idle_request(struct device *dev, bool idle)
{
	return 0;
}
static inline int rockchip_pmu_block(void) { return 0; }
static inline void rockchip_pmu_unblock(void) { }

/* --- OPP / DVFS: not supported in this port --- */
struct rockchip_opp_info {
	int unused;
};

static inline int rockchip_init_opp_table(struct device *dev,
					  struct rockchip_opp_info *info,
					  const char *lkg_name,
					  const char *reg_name)
{
	return 0;
}

static inline void rockchip_uninit_opp_table(struct device *dev,
					     struct rockchip_opp_info *info)
{
}

static inline void rockchip_opp_dvfs_lock(struct rockchip_opp_info *info) { }
static inline void rockchip_opp_dvfs_unlock(struct rockchip_opp_info *info) { }

/* --- system monitor / devfreq coupling: not supported --- */
struct monitor_dev_info;
struct monitor_dev_profile;

static inline struct monitor_dev_info *
rockchip_system_monitor_register(struct device *dev,
				 struct monitor_dev_profile *devp)
{
	return ERR_PTR(-ENODEV);
}

static inline void
rockchip_system_monitor_unregister(struct monitor_dev_info *info)
{
}

static inline int rockchip_monitor_suspend_low_temp_adjust(int id)
{
	return 0;
}

static inline int rockchip_monitor_resume_low_temp_adjust(int id)
{
	return 0;
}

/* --- DMC (DDR frequency) locks --- */
static inline int rockchip_dmcfreq_lock(void) { return 0; }
static inline int rockchip_dmcfreq_unlock(void) { return 0; }

/* --- SiP SMC VPU reset (BL31 call); a plain reset line is used instead --- */
static inline struct arm_smccc_res sip_smc_vpu_reset(u32 a0, u32 a1, u32 a2)
{
	struct arm_smccc_res res = { 0 };

	return res;
}

#endif /* __MPP_COMPAT_H */
