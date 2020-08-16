/* linux/drivers/devfreq/exynos/exynos7870_bus_mif.c
 *
 * Copyright (c) 2015 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Samsung EXYNOS7870 SoC MIF devfreq driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation, either version 2 of the License,
 * or (at your option) any later version.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/list.h>
#include <linux/clk.h>
#include <linux/workqueue.h>

#include <soc/samsung/exynos-devfreq.h>
#include <soc/samsung/bts.h>
#include <linux/apm-exynos.h>
#include <soc/samsung/asv-exynos.h>
#include <linux/mcu_ipc.h>

#include "../../../drivers/soc/samsung/pwrcal/pwrcal.h"
#include "../../../drivers/soc/samsung/pwrcal/S5E7870/S5E7870-vclk.h"
#include "../governor.h"

#define DEVFREQ_MIF_REBOOT_FREQ	(900000)
#define DEVFREQ_MIF_SWITCH_FREQ	(667000)

u32 sw_volt_table;

int is_dll_on(void)
{
	return cal_dfs_ext_ctrl(dvfs_mif, cal_dfs_mif_is_dll_on, 0);
}
EXPORT_SYMBOL_GPL(is_dll_on);

static struct exynos_devfreq_data *mif_data;

static int exynos7870_devfreq_mif_cmu_dump(struct device *dev,
					struct exynos_devfreq_data *data)
{
	mutex_lock(&data->devfreq->lock);
	cal_vclk_dbg_info(dvfs_mif);
	mutex_unlock(&data->devfreq->lock);

	return 0;
}

static int exynos7870_devfreq_mif_reboot(struct device *dev,
					struct exynos_devfreq_data *data)
{
	u32 freq = DEVFREQ_MIF_REBOOT_FREQ;

	data->max_freq = freq;
	data->devfreq->max_freq = data->max_freq;

	mutex_lock(&data->devfreq->lock);
	update_devfreq(data->devfreq);
	mutex_unlock(&data->devfreq->lock);

	return 0;
}

static int exynos7870_devfreq_cl_dvfs_start(struct exynos_devfreq_data *data)
{
	int ret = 0;

#ifdef CONFIG_EXYNOS_CL_DVFS_MIF
	ret = exynos_cl_dvfs_start(ID_MIF);
#endif

	return ret;
}

static int exynos7870_devfreq_cl_dvfs_stop(u32 target_idx,
					struct exynos_devfreq_data *data)
{
	int ret = 0;

#ifdef CONFIG_EXYNOS_CL_DVFS_MIF
	ret = exynos_cl_dvfs_stop(ID_MIF, target_idx);
#endif

	return ret;
}

static int exynos7870_devfreq_mif_get_switch_freq(u32 cur_freq, u32 new_freq,
						u32 *switch_freq)
{
	*switch_freq = DEVFREQ_MIF_SWITCH_FREQ;

	return 0;
}

static int exynos7870_devfreq_mif_get_switch_voltage(u32 cur_freq, u32 new_freq,
						struct exynos_devfreq_data *data)
{
	if (DEVFREQ_MIF_SWITCH_FREQ >= cur_freq)
		if (new_freq >= DEVFREQ_MIF_SWITCH_FREQ)
			data->switch_volt = data->new_volt;
		else
			data->switch_volt = sw_volt_table;
	else
		if (cur_freq >= new_freq)
			data->switch_volt = data->old_volt;
		else
			data->switch_volt = data->new_volt;

	//pr_info("Selected switching voltage: %uuV\n", data->switch_volt);
	return 0;
}

static int exynos7870_devfreq_mif_get_freq(struct device *dev, u32 *cur_freq,
					struct exynos_devfreq_data *data)
{
	*cur_freq = (u32)clk_get_rate(data->clk);
	if (*cur_freq == 0) {
		dev_err(dev, "failed get frequency from CAL\n");
		return -EINVAL;
	}

	return 0;
}

static int exynos7870_devfreq_mif_change_to_switch_freq(struct device *dev,
					struct exynos_devfreq_data *data)
{
	if (clk_set_rate(data->sw_clk, data->switch_freq)) {
		dev_err(dev, "failed to set switching frequency by CAL (%uKhz for %uKhz)\n",
				data->switch_freq, data->new_freq);
		return -EINVAL;
	}

	return 0;
}

static int exynos7870_devfreq_mif_restore_from_switch_freq(struct device *dev,
					struct exynos_devfreq_data *data)
{
	if (clk_set_rate(data->clk, data->new_freq)) {
		dev_err(dev, "failed to set frequency by CAL (%uKhz)\n",
				data->new_freq);
		return -EINVAL;
	}

	return 0;
}

static int exynos8890_devfreq_mif_set_freq_post(struct device *dev,
					struct exynos_devfreq_data *data)
{
	/* Send information about MIF frequency to mailbox */
	mbox_set_value(MCU_CP, MCU_IPC_INT13, data->new_freq);

	return 0;
}

static int exynos7870_devfreq_mif_init_freq_table(struct device *dev,
						struct exynos_devfreq_data *data)
{
	u32 max_freq, min_freq, cur_freq;
	unsigned long tmp_max, tmp_min;
	struct dev_pm_opp *target_opp;
	u32 flags = 0;
	int i, ret;

	ret = cal_clk_enable(dvfs_mif);
	if (ret) {
		dev_err(dev, "failed to enable MIF\n");
		return -EINVAL;
	}

	max_freq = (u32)cal_dfs_get_max_freq(dvfs_mif);
	if (!max_freq) {
		dev_err(dev, "failed get max frequency\n");
		return -EINVAL;
	}

	dev_info(dev, "max_freq: %uKhz, get_max_freq: %uKhz\n",
			data->max_freq, max_freq);

	if (max_freq < data->max_freq) {
		rcu_read_lock();
		flags |= DEVFREQ_FLAG_LEAST_UPPER_BOUND;
		tmp_max = (unsigned long)max_freq;
		target_opp = devfreq_recommended_opp(dev, &tmp_max, flags);
		if (IS_ERR(target_opp)) {
			rcu_read_unlock();
			dev_err(dev, "not found valid OPP for max_freq\n");
			return PTR_ERR(target_opp);
		}

		data->max_freq = dev_pm_opp_get_freq(target_opp);
		rcu_read_unlock();
	}

	min_freq = (u32)cal_dfs_get_min_freq(dvfs_mif);
	if (!min_freq) {
		dev_err(dev, "failed get min frequency\n");
		return -EINVAL;
	}

	dev_info(dev, "min_freq: %uKhz, get_min_freq: %uKhz\n",
			data->min_freq, min_freq);

	if (min_freq > data->min_freq) {
		rcu_read_lock();
		flags &= ~DEVFREQ_FLAG_LEAST_UPPER_BOUND;
		tmp_min = (unsigned long)min_freq;
		target_opp = devfreq_recommended_opp(dev, &tmp_min, flags);
		if (IS_ERR(target_opp)) {
			rcu_read_unlock();
			dev_err(dev, "not found valid OPP for min_freq\n");
			return PTR_ERR(target_opp);
		}

		data->min_freq = dev_pm_opp_get_freq(target_opp);
		rcu_read_unlock();
	}

	dev_info(dev, "min_freq: %uKhz, max_freq: %uKhz\n",
			data->min_freq, data->max_freq);

	cur_freq = clk_get_rate(data->clk);
	dev_info(dev, "current frequency: %uKhz\n", cur_freq);

	for (i = 0; i < data->max_state; i++) {
		if (data->opp_list[i].freq > data->max_freq ||
			data->opp_list[i].freq < data->min_freq)
			dev_pm_opp_disable(dev, (unsigned long)data->opp_list[i].freq);
	}

	return 0;
}

static int exynos7870_devfreq_mif_get_volt_table(struct device *dev, u32 *volt_table,
						struct exynos_devfreq_data *data)
{
	struct dvfs_rate_volt mif_rate_volt[data->max_state];
	int table_size;
	int i;

	table_size = cal_dfs_get_rate_asv_table(dvfs_mif, mif_rate_volt);
	if (!table_size) {
		dev_err(dev, "failed get ASV table\n");
		return -ENODEV;
	}

	if (table_size != data->max_state) {
		dev_err(dev, "ASV table size is not matched\n");
		return -ENODEV;
	}

	for (i = 0; i < data->max_state; i++) {
		if (data->opp_list[i].freq != (u32)(mif_rate_volt[i].rate)) {
			dev_err(dev, "Freq table is not matched(%u:%u)\n",
				data->opp_list[i].freq, (u32)mif_rate_volt[i].rate);
			return -EINVAL;
		}
		volt_table[i] = (u32)mif_rate_volt[i].volt;

		/* Fill switch voltage table */
		if (!sw_volt_table &&
			data->opp_list[i].freq < DEVFREQ_MIF_SWITCH_FREQ)
			sw_volt_table = (u32)mif_rate_volt[i-1].volt;
	}

	dev_info(dev, "SW_volt %uuV in freq %uKhz\n",
			sw_volt_table, DEVFREQ_MIF_SWITCH_FREQ);

	return 0;
}

static int exynos7870_mif_ppmu_register(struct device *dev,
					struct exynos_devfreq_data *data)
{
#if 0
	int ret;
	struct devfreq_exynos *ppmu_data = (struct devfreq_exynos *)&data->ppmu_data;

	ret = exynos7870_devfreq_register(ppmu_data);
	if (ret) {
		dev_err(dev, "failed ppmu register\n");
		return ret;
	}

	ret = exynos7870_ppmu_register_notifier(MIF, &data->ppmu_nb->nb);
	if (ret) {
		dev_err(dev, "failed ppmu notifier register\n");
		return ret;
	}
#endif

	return 0;
}

static int exynos7870_mif_ppmu_unregister(struct device *dev,
					struct exynos_devfreq_data *data)
{
#if 0
	exynos7870_ppmu_unregister_notifier(MIF, &data->ppmu_nb->nb);
#endif

	return 0;
}

static int exynos7870_devfreq_mif_init(struct device *dev,
					struct exynos_devfreq_data *data)
{
	data->clk = clk_get(dev, "dvfs_mif");
	if (IS_ERR_OR_NULL(data->clk)) {
		dev_err(dev, "failed get dvfs vclk\n");
		return -ENODEV;
	}

	data->sw_clk = clk_get(dev, "dvfs_mif_sw");
	if (IS_ERR_OR_NULL(data->sw_clk)) {
		dev_err(dev, "failed get dvfs sw vclk\n");
		clk_put(data->clk);
		return -ENODEV;
	}

	return 0;
}

static int exynos7870_devfreq_mif_exit(struct device *dev,
					struct exynos_devfreq_data *data)
{
	clk_put(data->sw_clk);
	clk_put(data->clk);

	return 0;
}

static int __init exynos7870_devfreq_mif_init_prepare(struct exynos_devfreq_data *data)
{
	data->ops.init = exynos7870_devfreq_mif_init;
	data->ops.exit = exynos7870_devfreq_mif_exit;
	data->ops.get_volt_table = exynos7870_devfreq_mif_get_volt_table;
	data->ops.ppmu_register = exynos7870_mif_ppmu_register;
	data->ops.ppmu_unregister = exynos7870_mif_ppmu_unregister;
	data->ops.get_switch_freq = exynos7870_devfreq_mif_get_switch_freq;
	data->ops.get_switch_voltage = exynos7870_devfreq_mif_get_switch_voltage;
	data->ops.get_freq = exynos7870_devfreq_mif_get_freq;
	data->ops.change_to_switch_freq = exynos7870_devfreq_mif_change_to_switch_freq;
	data->ops.restore_from_switch_freq = exynos7870_devfreq_mif_restore_from_switch_freq;
	data->ops.set_freq_post = exynos8890_devfreq_mif_set_freq_post;
	data->ops.init_freq_table = exynos7870_devfreq_mif_init_freq_table;
	data->ops.cl_dvfs_start = exynos7870_devfreq_cl_dvfs_start;
	data->ops.cl_dvfs_stop = exynos7870_devfreq_cl_dvfs_stop;
	data->ops.reboot = exynos7870_devfreq_mif_reboot;
	data->ops.cmu_dump = exynos7870_devfreq_mif_cmu_dump;

	mif_data = data;

	return 0;
}

static int __init exynos7870_devfreq_mif_initcall(void)
{
	if (register_exynos_devfreq_init_prepare(DEVFREQ_MIF,
				exynos7870_devfreq_mif_init_prepare))
		return -EINVAL;

	return 0;
}
fs_initcall(exynos7870_devfreq_mif_initcall);
