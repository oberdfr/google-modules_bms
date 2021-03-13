/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2020 Google, LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#ifdef CONFIG_PM_SLEEP
#define SUPPORT_PM_SLEEP 1
#endif

#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/pm_runtime.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>
#include <linux/slab.h>
#include <misc/gvotable.h>
#include "gbms_power_supply.h"
#include "google_bms.h"
#include "google_dc_pps.h"
#include "google_psy.h"
#include <misc/logbuffer.h>

#include <linux/debugfs.h>

#define get_boot_sec() div_u64(ktime_to_ns(ktime_get_boottime()), NSEC_PER_SEC)


/* Non DC Charger is the default */
#define GCPM_DEFAULT_CHARGER	0
/* Will need to handle capabilities based on index number */
#define GCPM_INDEX_DC_ENABLE	1
#define GCPM_MAX_CHARGERS	4

/* tier based, disabled now */
#define GCPM_DEFAULT_DC_LIMIT_DEMAND	0

/* voltage based */
#define GCPM_DEFAULT_DC_LIMIT_VBATT_MIN		3600000
#define GCPM_DEFAULT_DC_LIMIT_DELTA_LOW		200000

#define GCPM_DEFAULT_DC_LIMIT_VBATT_MAX		4400000
#define GCPM_DEFAULT_DC_LIMIT_DELTA_HIGH	50000

/* TODO: move to configuration */
#define DC_TA_VMAX_MV		9800000
/* TODO: move to configuration */
#define DC_TA_VMIN_MV		8000000
/* TODO: move to configuration */
#define DC_VBATT_HEADROOM_MV	500000

enum gcpm_dc_state_t {
	DC_DISABLED = -1,
	DC_IDLE = 0,
	DC_ENABLE,
	DC_RUNNING,
	DC_ENABLE_PASSTHROUGH,
	DC_PASSTHROUGH,
};

struct gcpm_drv  {
	struct device *device;
	struct power_supply *psy;
	struct delayed_work init_work;

	/* combine PPS, route to the active PPS source */
	struct power_supply *pps_psy;

	int chg_psy_retries;
	struct power_supply *chg_psy_avail[GCPM_MAX_CHARGERS];
	const char *chg_psy_names[GCPM_MAX_CHARGERS];
	struct mutex chg_psy_lock;
	int chg_psy_active;
	int chg_psy_count;

	/* force a charger, this might have side effects */
	int force_active;

	/* TCPM state for wired PPS charging */
	const char *tcpm_psy_name;
	struct power_supply *tcpm_psy;
	struct pd_pps_data tcpm_pps_data;
	int log_psy_ratelimit;
	u32 tcpm_phandle;

	/* TCPM state for wireless PPS charging */
	const char *wlc_dc_name;
	struct power_supply *wlc_dc_psy;
	struct pd_pps_data wlc_pps_data;
	u32 wlc_phandle;

	struct delayed_work select_work;

	/* set to force PPS negoatiation */
	bool force_pps;
	/* pps state and detect */
	struct delayed_work pps_work;
	/* request of output ua, */
	int out_ua;
	int out_uv;

	int dcen_gpio;
	u32 dcen_gpio_default;

	/* >0 when enabled, pps charger to use */
	int pps_index;
	/* >0 when enabled, dc_charger */
	int dc_index;
	/* dc_charging state */
	int dc_state;

	ktime_t dc_start_time;

	/* force check of the DC limit again (debug) */
	bool new_dc_limit;
	/* force disable */
	bool taper_control;

	/* policy: power demand limit for DC charging */
	u32 dc_limit_vbatt_low;		/* DC will not stop until low */
	u32 dc_limit_vbatt_min;		/* DC will start at min */
	u32 dc_limit_vbatt_high;	/* DC will not start over high */
	u32 dc_limit_vbatt_max;		/* DC stop at max */
	u32 dc_limit_demand;

	/* cc_max and fv_uv are demand from google_charger */
	int cc_max;
	int fv_uv;

	bool dc_init_complete;
	bool init_complete;
	bool resume_complete;
	struct notifier_block chg_nb;

	/* tie up to charger mode */
	struct gvotable_election *gbms_mode;

	/* debug fs */
	struct dentry *debug_entry;
};

static struct power_supply *gcpm_chg_get_default(const struct gcpm_drv *gcpm)
{
	return gcpm->chg_psy_avail[GCPM_DEFAULT_CHARGER];
}

/* TODO: place a lock around the operation? */
static struct power_supply *gcpm_chg_get_active(const struct gcpm_drv *gcpm)
{
	if (gcpm->chg_psy_active == -1)
		return NULL;

	return gcpm->chg_psy_avail[gcpm->chg_psy_active];
}

static int gcpm_chg_ping(struct gcpm_drv *gcpm, int index, bool online)
{
	struct power_supply *chg_psy;
	int ret;

	chg_psy = gcpm->chg_psy_avail[index];
	if (!chg_psy)
		return 0;

	ret = GPSY_SET_PROP(chg_psy, POWER_SUPPLY_PROP_ONLINE, 0);
	if (ret < 0)
		pr_debug("adapter %d cannot ping (%d)", index, ret);

	return 0;
}

/*
 * Switch between chargers using ONLINE.
 * NOTE: online doesn't enable charging.
 * NOTE: call holding a lock on charger
 */
static int gcpm_chg_offline(struct gcpm_drv *gcpm)
{
	struct power_supply *chg_psy;
	int ret;

	chg_psy = gcpm_chg_get_active(gcpm);
	if (!chg_psy)
		return 0;

	/* OFFLINE should stop charging, this make sure that it does */
	ret = GPSY_SET_PROP(chg_psy, GBMS_PROP_CHARGING_ENABLED, 0);
	if (ret == 0)
		ret = GPSY_SET_PROP(chg_psy, POWER_SUPPLY_PROP_ONLINE, 0);
	if (ret == 0)
		gcpm->chg_psy_active = -1;

	pr_debug("%s: active=%d offline_ok=%d\n", __func__,
		 gcpm->chg_psy_active, ret == 0);
	return ret;
}

/* turn current offline (if a current exists), switch to new */
static int gcpm_chg_set_online(struct gcpm_drv *gcpm, int index)
{
	const int index_old = gcpm->chg_psy_active;
	struct power_supply *active;
	int ret;

	if (index < 0 || index >= gcpm->chg_psy_count)
		return -ERANGE;
	if (index == index_old)
		return 0;

	active = gcpm->chg_psy_avail[index];
	if (!active) {
		pr_err("invalid index %d\n", index);
		return -EINVAL;
	}

	/* offline the current one */
	ret = gcpm_chg_offline(gcpm);
	if (ret < 0) {
		pr_err("cannot turn %d offline\n", index_old);
		return -EIO;
	}

	ret = GPSY_SET_PROP(active, POWER_SUPPLY_PROP_ONLINE, 1);
	if (ret < 0) {
		/* TODO: re-enable the old one if this fail???  */
		goto error_exit;
	}

	gcpm->chg_psy_active = index;

error_exit:
	pr_info("%s: active charger %d->%d (%d)\n",
		__func__, index_old, index, ret);
	return ret;
}

/* use the charger one when avaiable or fallback to the generated one */
static uint64_t gcpm_get_charger_state(const struct gcpm_drv *gcpm,
				       struct power_supply *chg_psy)
{
	union gbms_charger_state chg_state;
	int rc;

	rc = gbms_read_charger_state(&chg_state, chg_psy);
	if (rc < 0)
		return 0;

	return chg_state.v;
}

/* Enable DirectCharge mode, PPS and DC charger must be already initialized */
static int gcpm_dc_enable(struct gcpm_drv *gcpm, bool enabled)
{
	if (!gcpm->gbms_mode) {
		struct gvotable_election *v;

		v = gvotable_election_get_handle(GBMS_MODE_VOTABLE);
		if (IS_ERR_OR_NULL(v))
			return -ENODEV;
		gcpm->gbms_mode = v;
	}

	return gvotable_cast_vote(gcpm->gbms_mode, "GCPM",
				  (void*)GBMS_CHGR_MODE_CHGR_DC,
				  enabled);
}

/*
 * disable DC and switch back to the default charger. Final DC statate is
 * DC_IDLE (i.e. this can be used to reset dc_state from DC_DISABLED).
 * NOTE: call with a lock around gcpm->chg_psy_lock
 */
static int gcpm_dc_stop(struct gcpm_drv *gcpm, int final_state)
{
	int ret;

	/* enabled in dc_ready after programming the charger  */
	if (gcpm->dcen_gpio >= 0 && !gcpm->dcen_gpio_default)
		gpio_set_value(gcpm->dcen_gpio, 0);

	switch (gcpm->dc_state) {
	case DC_RUNNING:
	case DC_PASSTHROUGH:
		ret = gcpm_dc_enable(gcpm, false);
		if (ret < 0) {
			pr_err("DC_PPS: Cannot disable DC (%d)", ret);
			break;
		}
		gcpm->dc_state = DC_ENABLE;
		/* Fall Through */
	case DC_ENABLE:
	case DC_ENABLE_PASSTHROUGH:
		ret = gcpm_chg_set_online(gcpm, 0);
		if (ret < 0) {
			pr_err("DC_PPS: Cannot enable default charger (%d)",
			       ret);
			break;
		}
		/* Fall Through */
	default:
		gcpm->dc_state = final_state;
		ret = 0;
		break;
	}

	return ret;
}

/* NOTE: call with a lock around gcpm->chg_psy_lock */
static int gcpm_dc_start(struct gcpm_drv *gcpm, int index)
{
	struct power_supply *dc_psy;
	int ret;

	ret = gcpm_chg_set_online(gcpm, index);
	if (ret < 0) {
		pr_err("PPS_DC: cannot online index=%d (%d)\n", index, ret);
		return ret;
	}

	dc_psy = gcpm_chg_get_active(gcpm);
	if (!dc_psy) {
		pr_err("PPS_DC: gcpm->dc_state == DC_READY, no adapter\n");
		return -ENODEV;
	}

	/* VFLOAT = vbat */
	ret = GPSY_SET_PROP(dc_psy,
			    POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX,
			    gcpm->fv_uv);
	if (ret < 0) {
		pr_err("PPS_DC: no fv_uv (%d)\n", ret);
		return ret;
	}

	/* ICHG_CHG = cc_max */
	ret = GPSY_SET_PROP(dc_psy,
			    POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
			    gcpm->cc_max);
	if (ret < 0) {
		pr_err("PPS_DC: no cc_max (%d)\n", ret);
		return ret;
	}

	/* set IIN_CFG, */
	ret = GPSY_SET_PROP(dc_psy, POWER_SUPPLY_PROP_CURRENT_MAX,
			    gcpm->out_ua);
	if (ret < 0) {
		pr_err("PPS_DC: no IIN (%d)\n", ret);
		return ret;
	}

	/* enabled in dc_ready after programming the charger  */
	if (gcpm->dcen_gpio >= 0 && !gcpm->dcen_gpio_default)
		gpio_set_value(gcpm->dcen_gpio, 1);

	/* vote on MODE */
	ret = gcpm_dc_enable(gcpm, true);
	if (ret < 0) {
		pr_err("PPS_DC: dc_ready failed=%d\n", ret);
		return ret;
	}

	pr_debug("PPS_DC: dc_ready ok state=%d fv_uv=%d cc_max=%d, out_ua=%d\n",
		gcpm->dc_state, gcpm->fv_uv, gcpm->cc_max, gcpm->out_ua);

	return 0;
}

/*
 * Select the DC charger using the thermal policy.
 * NOTE: program target before enabling chaging.
 */
static int gcpm_chg_select(const struct gcpm_drv *gcpm)
{
	struct power_supply *chg_psy;
	const int vbatt_min = gcpm->dc_limit_vbatt_min;
	const int vbatt_max = gcpm->dc_limit_vbatt_max;
	int batt_demand, index = GCPM_DEFAULT_CHARGER;

	if (gcpm->force_active >= 0)
		return gcpm->force_active;

	/* keep on default */
	if (gcpm->cc_max <= 0 || gcpm->fv_uv <= 0)
		return GCPM_DEFAULT_CHARGER;

	/* battery demand comes from charging tier */
	batt_demand = (gcpm->cc_max / 1000) * (gcpm->fv_uv / 1000);
	if (batt_demand > gcpm->dc_limit_demand)
		index = GCPM_INDEX_DC_ENABLE;

	/* TODO: add debounce on demand */

	pr_debug("%s: index=%d count=%d demand=%d dc_limit=%d\n",
		 __func__, index, gcpm->chg_psy_count, batt_demand,
		 gcpm->dc_limit_demand);

	/* could select different modes here depending on capabilities */
	chg_psy = gcpm_chg_get_default(gcpm);
	if (chg_psy && (vbatt_max || vbatt_min)) {
		const int vbatt_high = gcpm->dc_limit_vbatt_high;
		const int vbatt_low = gcpm->dc_limit_vbatt_low;
		int vbatt;

		/* NOTE: check the current charger, should check battery? */
		vbatt = GPSY_GET_PROP(chg_psy, POWER_SUPPLY_PROP_VOLTAGE_NOW);
		if (vbatt < 0) {
			pr_err("CHG_CHK cannot read vbatt %d\n", vbatt);
			return gcpm->dc_index;
		}

		if (vbatt_low && vbatt < vbatt_low)
			return -EAGAIN;

		/* Hard limits */
		if (vbatt_min && vbatt < vbatt_min)
			index = gcpm->dc_index == GCPM_DEFAULT_CHARGER ?
				-EAGAIN : gcpm->dc_index; /* debounce? */
		else if (vbatt_high && vbatt > vbatt_high)
			index = gcpm->dc_index; /* debounce */
		else if (vbatt_max && vbatt > vbatt_max)
			index = GCPM_DEFAULT_CHARGER; /* disable */
		else if (vbatt_min && vbatt > vbatt_min)
			index = GCPM_INDEX_DC_ENABLE; /* enable */

		pr_debug("%s: index=%d vbatt=%d: low=%d min=%d high=%d max=%d\n",
			 __func__, index, vbatt, vbatt_low, vbatt_min,
			 vbatt_high, vbatt_max);
	}

	if (index >= gcpm->chg_psy_count) {
		pr_err("CHG_CHK index=%d out of bounds %d\n", index,
		       gcpm->chg_psy_count);
		return GCPM_DEFAULT_CHARGER;
	}

	/* TODO: more qualifiers here */

	return index;
}

static bool gcpm_chg_dc_check_source(const struct gcpm_drv *gcpm, int index)
{
	if (gcpm->taper_control)
		return false;

	/* Will run detection only the first time */
	if (gcpm->tcpm_pps_data.stage == PPS_NOTSUPP &&
	    gcpm->wlc_pps_data.stage == PPS_NOTSUPP )
		return false;

	return index == GCPM_INDEX_DC_ENABLE;
}

/* */
static void gcpm_pps_online(struct gcpm_drv *gcpm)
{
	/* reset setpoint */
	gcpm->out_ua = -1;
	gcpm->out_uv = -1;

	/* reset detection */
	if (gcpm->tcpm_pps_data.pps_psy)
		pps_init_state(&gcpm->tcpm_pps_data);
	if (gcpm->wlc_pps_data.pps_psy)
		pps_init_state(&gcpm->wlc_pps_data);
	gcpm->pps_index = 0;
}

/* DC_ERROR_RETRY_MS <= DC_RUN_DELAY_MS */
#define DC_ENABLE_DELAY_MS	5000
#define DC_RUN_DELAY_MS		9000
#define DC_ERROR_RETRY_MS	PPS_ERROR_RETRY_MS

#define PPS_PROG_TIMEOUT_S	10
#define PPS_PROG_RETRY_MS	5000
#define PPS_ACTIVE_RETRY_MS	1500
#define PPS_ACTIVE_TIMEOUT_S	25

#define PPS_ERROR_RETRY_MS	1000

enum {
	PPS_INDEX_NOT_SUPP = -1,
	PPS_INDEX_TCPM = 1,
	PPS_INDEX_WLC = 2,
	PPS_INDEX_MAX = 2,
};

static struct pd_pps_data *gcpm_pps_data(struct gcpm_drv *gcpm)
{
	struct pd_pps_data *pps_data = NULL;

	if (gcpm->pps_index == PPS_INDEX_TCPM)
		pps_data = &gcpm->tcpm_pps_data;
	else if (gcpm->pps_index == PPS_INDEX_WLC)
		pps_data = &gcpm->wlc_pps_data;

	return pps_data;
}

/*
 * Pick the first PPS source that transition to PPS_ACTIVE:
 *
 * ->stage ==
 *	DISABLED => NONE -> AVAILABLE -> ACTIVE -> DISABLED
 *		 -> DISABLED
 *		 -> NOTSUPP
 *
 * return 0 if needs to continue polling, -ENODEV if none of the sources
 * support pps.
 */
static int gcpm_pps_work(struct gcpm_drv *gcpm)
{
	int ret = 0, pps_index = 0;
	int not_supported = 0;

	if (gcpm->tcpm_pps_data.stage != PPS_NOTSUPP) {
		struct pd_pps_data *pps_data = &gcpm->tcpm_pps_data;
		int pps_ui;

		pps_ui = pps_work(pps_data, pps_data->pps_psy);
		if (pps_ui >= 0 && pps_data->stage == PPS_ACTIVE)
			pps_index = PPS_INDEX_TCPM;

		if (pps_data->pd_online < PPS_PSY_PROG_ONLINE)
			pr_debug("PPS_Work: TCPM Wait pps_ui=%d online=%d, stage=%d\n",
				pps_ui, pps_data->pd_online, pps_data->stage);
	} else {
		not_supported += 1;
	}

	if (gcpm->wlc_pps_data.stage != PPS_NOTSUPP) {
		struct pd_pps_data *pps_data = &gcpm->wlc_pps_data;
		int pps_ui;

		pps_ui = pps_work(pps_data, pps_data->pps_psy);
		if (pps_ui >= 0 && pps_data->stage == PPS_ACTIVE)
			pps_index = PPS_INDEX_WLC;

		if (pps_data->pd_online < PPS_PSY_PROG_ONLINE)
			pr_debug("PPS_Work: WLC Wait pps_ui=%d online=%d, stage=%d\n",
				pps_ui, pps_data->pd_online, pps_data->stage);
	} else {
		not_supported += 1;
	}

	pr_debug("PPS_Work: tcpm[online=%d, stage=%d] wlc[online=%d, stage=%d] ns=%d pps_index=%d\n",
		 gcpm->tcpm_pps_data.pd_online, gcpm->tcpm_pps_data.stage,
		 gcpm->wlc_pps_data.pd_online, gcpm->wlc_pps_data.stage,
		 not_supported, pps_index);

	/* 2 sources */
	if (not_supported == PPS_INDEX_MAX)
		return -ENODEV;

	/* index==0 meand detecting */
	if (gcpm->pps_index != pps_index)
		pr_debug("PPS_Work: pps_index %d->%d\n",
			gcpm->pps_index, pps_index);
	/* went away! */
	if (gcpm->pps_index && !pps_index)
		ret = -ENODEV;

	gcpm->pps_index = pps_index;
	return ret;
}

static int gcpm_pps_offline(struct gcpm_drv *gcpm)
{
	int ret;

	if (gcpm->tcpm_pps_data.pps_psy) {
		ret = pps_prog_offline(&gcpm->tcpm_pps_data,
				       gcpm->tcpm_pps_data.pps_psy);
		if (ret < 0)
			pr_err("PPS_DC: fail tcpm offline (%d)\n", ret);
	}

	if (gcpm->wlc_pps_data.pps_psy) {
		ret = pps_prog_offline(&gcpm->wlc_pps_data,
				       gcpm->wlc_pps_data.pps_psy);
		if (ret < 0)
			pr_err("PPS_DC: fail wlc offline (%d)\n", ret);
	}

	gcpm->pps_index = 0;
	return 0;
}

/* triggered on every FV_UV, keep polling if in -EAGAIN */
static void gcpm_chg_select_work(struct work_struct *work)
{
	struct gcpm_drv *gcpm =
		container_of(work, struct gcpm_drv, select_work.work);
	int index, schedule_pps_interval = -1;
	bool dc_done = false, dc_ena;

	mutex_lock(&gcpm->chg_psy_lock);

	index = gcpm_chg_select(gcpm);

	if (gcpm->taper_control) {


		/* TODO: smooth from dc_index to the default charger */
		index = GCPM_DEFAULT_CHARGER;
		dc_done = true;
	} else if (index < 0) {
		const int interval = 5; /* 5 seconds */

		pr_debug("CHG_CHK: reschedule in %d seconds\n", interval);
		schedule_delayed_work(&gcpm->select_work,
				      msecs_to_jiffies(interval * 1000));
		mutex_unlock(&gcpm->chg_psy_lock);
		return;
	}

	/*
	 * NOTE: disabling DC might need to transition to charger mode 0
	 *       same might apply when switching between WLC-DC and PPS-DC.
	 *       Figure out a way to do this if needed.
	 */
	dc_ena = gcpm_chg_dc_check_source(gcpm, index);
	pr_debug("CHG_CHK: DC dc_ena=%d dc_state=%d dc_index=%d->%d\n",
		 dc_ena, gcpm->dc_state, gcpm->dc_index, index);
	if (!dc_ena) {

		if (gcpm->dc_state > DC_IDLE && gcpm->dc_index > 0) {
			pr_info("CHG_CHK: stop PPS_Work for dc_index=%d\n",
				gcpm->dc_index);

			gcpm->dc_index =  dc_done ? -1 : GCPM_DEFAULT_CHARGER;
			schedule_pps_interval = 0;
		}
	} else if (gcpm->dc_state == DC_DISABLED) {
		pr_debug("CHG_CHK: PPS_Work disabled for the session\n");
	} else if (gcpm->dc_state == DC_IDLE) {
		pr_info("CHG_CHK: start PPS_Work for dc_index=%d\n", index);

		/* reset pps state to re-enable detection */
		gcpm_pps_online(gcpm);

		/* TODO: DC_ENABLE or DC_PASSTHROUGH depending on index */
		gcpm->dc_state = DC_ENABLE_PASSTHROUGH;
		gcpm->dc_index = index;

		/* grace period of 5000ms, PPS Work not called during grace */
		gcpm->dc_start_time = get_boot_sec();
		schedule_pps_interval = 5000;
	}

	if (schedule_pps_interval >= 0)
		mod_delayed_work(system_wq, &gcpm->pps_work,
				 msecs_to_jiffies(schedule_pps_interval));

	mutex_unlock(&gcpm->chg_psy_lock);
}

/*
 * pps_data->stage:
 *  PPS_NONE -> PPS_AVAILABLE -> PPS_ACTIVE
 * 	     -> PPS_DISABLED  -> PPS_DISABLED
 */
static void gcpm_pps_wlc_dc_work(struct work_struct *work)
{
	struct gcpm_drv *gcpm =
		container_of(work, struct gcpm_drv, pps_work.work);
	const ktime_t elap = get_boot_sec() - gcpm->dc_start_time;
	struct pd_pps_data *pps_data;
	int ret, pps_ui = -ENODEV;

	/* spurious during init */
	mutex_lock(&gcpm->chg_psy_lock);
	if (!gcpm->resume_complete || !gcpm->init_complete) {
		mutex_unlock(&gcpm->chg_psy_lock);
		return;
	}

	/* disconnect, gcpm_chg_check() and most errors reset ->dc_index */
	if (gcpm->dc_index <= 0) {
		const int dc_state = gcpm->dc_state; /* will change */

		if (dc_state <= DC_IDLE) {
			pr_warn("PPS_Work: spurious, elap=%lld dc_index=%d dc_state=%d\n",
				elap, gcpm->dc_index, dc_state);
			goto pps_dc_done;
		}

		/* First disable DC */
		ret = gcpm_dc_stop(gcpm, DC_DISABLED);
		if (ret < 0) {
			pr_err("PPS_Work: retry disable, elap=%lld dc_state=%d->%d (%d)\n",
			       elap, dc_state, gcpm->dc_state, ret);
			pps_ui = DC_ERROR_RETRY_MS;
			goto pps_dc_reschedule;
		}

		/* and then disable PPS */
		ret = gcpm_pps_offline(gcpm);
		if (ret < 0) {
			pr_err("PPS_Work: fail pps offline, elap=%lld dc_state=%d (%d)\n",
				elap, gcpm->dc_state, ret);
			pps_ui = PPS_ERROR_RETRY_MS;
			goto pps_dc_reschedule;
		}

		/* and then re-enable if switching to the default */
		if (gcpm->dc_index == GCPM_DEFAULT_CHARGER)
			gcpm->dc_state = DC_IDLE;

		pr_info("PPS_Work: done elap=%lld dc_state=%d\n",
			elap, gcpm->dc_state);

		goto pps_dc_done;
	}

	/* PPS was handed over to the DC driver, just monitor it... */
	if (gcpm->dc_state == DC_PASSTHROUGH) {
		struct power_supply *dc_psy;
		bool prog_online = false;
		int index;

		/* the dc driver needs to keep the source online */
		pps_data = gcpm_pps_data(gcpm);
		if (pps_data)
			prog_online = pps_check_online(pps_data);
		if (!prog_online) {
			pr_err("PPS_Work: PPS offline, elap=%lld dc_index:%d->0\n",
			       elap, gcpm->dc_index);

			pps_ui = DC_ERROR_RETRY_MS;
			gcpm->dc_index = 0;
			goto pps_dc_reschedule;
		}

		/* likely changed from debug, bail */
		dc_psy = gcpm_chg_get_active(gcpm);
		if (!dc_psy) {
			pr_err("PPS_Work: No adapter, elap=%lld in PASSTHROUGH\n",
			       elap);

			pps_ui = DC_ERROR_RETRY_MS;
			goto pps_dc_reschedule;
		}

		/* check crossing demand or hard limits */
		index = gcpm_chg_select(gcpm);
		if (index != gcpm->dc_index)
			mod_delayed_work(system_wq, &gcpm->select_work, 0);

		/* ->pps_index valid: set/ping source to DC, ping watchdog */
		ret = GPSY_SET_PROP(dc_psy, GBMS_PROP_CHARGING_ENABLED,
				    gcpm->pps_index);
		if (ret == 0) {
			ret = gcpm_chg_ping(gcpm, GCPM_DEFAULT_CHARGER, 0);
			if (ret < 0)
				pr_err("PPS_Work: ping failed, elap=%lld with %d\n",
				       elap, ret);

			/* keep running to ping the adapters */
			pps_ui = DC_RUN_DELAY_MS;
		} else if (ret == -EBUSY || ret == -EAGAIN) {
			pps_ui = DC_ERROR_RETRY_MS;
		} else {
			pr_err("PPS_Work: ping DC, elap=%lld (%d)\n", elap, ret);

			ret = gcpm_chg_set_online(gcpm, GCPM_DEFAULT_CHARGER);
			if (ret < 0) {
				pr_err("PPS_Work: cannot online default %d\n", ret);
				pps_ui = DC_ERROR_RETRY_MS;
			 } else {
				pr_err("PPS_Work: dc offline\n");
				pps_ui = 0;
			}
		}

		goto pps_dc_reschedule;
	}

	/*
	 * Wait until one of the sources come online, <0 when PPS is not
	 * supported from ANY source. Deadline at PPS_PROG_TIMEOUT_S.
	 */
	ret = gcpm_pps_work(gcpm);
	if (ret < 0) {
		if (elap < PPS_PROG_TIMEOUT_S) {
			/* retry for the session  */
			pps_ui = PPS_PROG_RETRY_MS;
			gcpm_pps_online(gcpm);
		} else {
			/* TODO: abort for the session  */
			pr_err("PPS_Work: PROG timeout, elap=%lld dc_state=%d (%d)\n",
			       elap, gcpm->dc_state, ret);
			pps_ui = PPS_ERROR_RETRY_MS;
			gcpm->dc_index = 0;
		}

		goto pps_dc_reschedule;
	}

	/*
	 * DC runs only when PPS is active: abort for the session if a source
	 * went PROG_ONLINE but !active within PPS_ACTIVE_TIMEOUT_S.
	 */
	pps_data = gcpm_pps_data(gcpm);
	if (!pps_data) {
		if (elap < PPS_ACTIVE_TIMEOUT_S) {
			/* give more time to turn online  */
			pps_ui = PPS_ACTIVE_RETRY_MS;
		} else {
			pr_err("PPS_Work: ACTIVE timeout, elap=%lld dc_state=%d (%d)\n",
			       elap, gcpm->dc_state, ret);
			/* TODO: abort for the session  */
			pps_ui = PPS_ERROR_RETRY_MS;
			gcpm->dc_index = 0;
		}

		goto pps_dc_reschedule;
	}

	if (gcpm->dc_state == DC_ENABLE_PASSTHROUGH) {
		struct power_supply *pps_psy = pps_data->pps_psy;

		/* steady on PPS, DC is about to be enabled */
		pps_ui = pps_update_adapter(pps_data, -1, -1, pps_psy);
		if (pps_ui < 0) {
			pr_err("PPS_Work: pps update, elap=%lld dc_state=%d (%d)\n",
			       elap, gcpm->dc_state, pps_ui);
			pps_ui = PPS_ERROR_RETRY_MS;
		}

		/*
		 * offine current adapter and start new. Charging is enabled
		 * in DC_PASSTHROUGH setting GBMS_PROP_CHARGING_ENABLED to
		 * the PPS source.
		 * NOTE: There are a bunch of interesting recovery scenarios.
		 */
		ret = gcpm_chg_offline(gcpm);
		if (ret == 0)
			ret = gcpm_dc_start(gcpm, gcpm->dc_index);
		if (ret == 0) {
			gcpm->dc_state = DC_PASSTHROUGH;
			pps_ui = DC_ENABLE_DELAY_MS;
		} else if (pps_ui > DC_ERROR_RETRY_MS) {
			pps_ui = DC_ERROR_RETRY_MS;
		}
	} else {
		struct power_supply *pps_psy = pps_data->pps_psy;

		/* steady on PPS, if DC state is DC_ENABLE or DC_RUNNING */
		pps_ui = pps_update_adapter(pps_data, -1, -1, pps_psy);

		pr_info("PPS_Work: STEADY pd_online=%d pps_ui=%d dc_ena=%d dc_state=%d\n",
			pps_data->pd_online, pps_ui, gcpm->dc_index,
			gcpm->dc_state);
		if (pps_ui < 0)
			pps_ui = PPS_ERROR_RETRY_MS;
	}

pps_dc_reschedule:
	if (pps_ui <= 0) {
		pr_debug("PPS_Work: pps_ui=%d dc_state=%d",
			 pps_ui, gcpm->dc_state);
	} else {
		pr_debug("PPS_Work: reschedule in %d dc_state=%d (%d:%d)",
			 pps_ui, gcpm->dc_state, gcpm->out_uv, gcpm->out_ua);

		schedule_delayed_work(&gcpm->pps_work,
				      msecs_to_jiffies(pps_ui));
	}

pps_dc_done:
	mutex_unlock(&gcpm->chg_psy_lock);
}

static int gcpm_psy_set_property(struct power_supply *psy,
				 enum power_supply_property psp,
				 const union power_supply_propval *pval)
{
	struct gcpm_drv *gcpm = power_supply_get_drvdata(psy);
	bool taper_control, ta_check = false;
	struct power_supply *chg_psy = NULL;
	bool route = true;
	int ret = 0;

	pm_runtime_get_sync(gcpm->device);
	if (!gcpm->init_complete || !gcpm->resume_complete) {
		pm_runtime_put_sync(gcpm->device);
		return -EAGAIN;
	}
	pm_runtime_put_sync(gcpm->device);

	mutex_lock(&gcpm->chg_psy_lock);
	switch (psp) {
	/* do not route to the active charger */
	case GBMS_PROP_TAPER_CONTROL:
		taper_control = pval->intval != GBMS_TAPER_CONTROL_OFF;
		ta_check = taper_control != gcpm->taper_control;
		gcpm->taper_control = taper_control;
		route = false;
		break;

	/* also route to the active charger */
	case GBMS_PROP_CHARGE_DISABLE:
		/*
		 * google_charger send this on disconnect.
		 * TODO: reset DC state and PPS detection, disable dc
		 */
		pr_info("%s: ChargeDisable value=%d\n", __func__, pval->intval);
		ta_check = true;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		ta_check = true;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		psp = POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX;
		/* compat, fall through */
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
		ta_check = gcpm->fv_uv != pval->intval;
		gcpm->fv_uv = pval->intval;
		break;

	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		ta_check = gcpm->cc_max != pval->intval;
		gcpm->cc_max = pval->intval;
		break;

	/* just route to the active charger */
	default:
		break;
	}

	/* used only for debug */
	if (gcpm->new_dc_limit) {
		gcpm->new_dc_limit = false;
		ta_check = true;
	}

	/* logic that select the active charging */
	if (gcpm->dc_init_complete && ta_check)
		mod_delayed_work(system_wq, &gcpm->select_work, 0);

	/*  route to active charger when needed */
	if (!route)
		goto done;

	chg_psy = gcpm_chg_get_active(gcpm);
	if (chg_psy) {
		ret = power_supply_set_property(chg_psy, psp, pval);
		if (ret < 0) {
			const char *name= (chg_psy->desc && chg_psy->desc->name) ?
				chg_psy->desc->name : "???";

			pr_err("cannot route prop=%d to %d:%s (%d)\n",
				psp, gcpm->chg_psy_active, name, ret);
		}
	} else {
		pr_err("invalid active charger = %d for prop=%d\n",
			gcpm->chg_psy_active, psp);
	}

done:
	/* the charger should not call into gcpm: this can change though */
	mutex_unlock(&gcpm->chg_psy_lock);
	return ret;
}

static int gcpm_psy_get_property(struct power_supply *psy,
				 enum power_supply_property psp,
				 union power_supply_propval *pval)
{
	struct gcpm_drv *gcpm = power_supply_get_drvdata(psy);
	union gbms_charger_state chg_state;
	struct power_supply *chg_psy;
	bool route = false;
	int ret = 0;

	pm_runtime_get_sync(gcpm->device);
	if (!gcpm->init_complete || !gcpm->resume_complete) {
		pm_runtime_put_sync(gcpm->device);
		return -EAGAIN;
	}
	pm_runtime_put_sync(gcpm->device);

	mutex_lock(&gcpm->chg_psy_lock);
	chg_psy = gcpm_chg_get_active(gcpm);
	if (!chg_psy) {
		mutex_unlock(&gcpm->chg_psy_lock);
		return -ENODEV;
	}

	switch (psp) {
	/* handle locally for now */
	case GBMS_PROP_CHARGE_CHARGER_STATE:
		chg_state.v = gcpm_get_charger_state(gcpm, chg_psy);
		gbms_propval_int64val(pval) = chg_state.v;
		break;

	/* route to the active charger */
	default:
		route = true;
		break;
	}

	if (route)
		ret = power_supply_get_property(chg_psy, psp, pval);

	mutex_unlock(&gcpm->chg_psy_lock);
	return ret;
}

static int gcpm_psy_is_writeable(struct power_supply *psy,
				 enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
	case POWER_SUPPLY_PROP_CURRENT_MAX:
	case GBMS_PROP_CHARGE_DISABLE:
	case GBMS_PROP_TAPER_CONTROL:
		return 1;
	default:
		break;
	}

	return 0;
}

/*
 * TODO: POWER_SUPPLY_PROP_RERUN_AICL, POWER_SUPPLY_PROP_TEMP
 */
static enum power_supply_property gcpm_psy_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	/* pixel battery management subsystem */
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,	/* cc_max */
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX,	/* fv_uv */
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_CURRENT_MAX,	/* input current limit */
	POWER_SUPPLY_PROP_VOLTAGE_MAX,	/* set float voltage, compat */
	POWER_SUPPLY_PROP_STATUS,
};

static struct power_supply_desc gcpm_psy_desc = {
	.name = "gcpm",
	.type = POWER_SUPPLY_TYPE_UNKNOWN,
	.get_property = gcpm_psy_get_property,
	.set_property = gcpm_psy_set_property,
	.property_is_writeable = gcpm_psy_is_writeable,
	.properties = gcpm_psy_properties,
	.num_properties = ARRAY_SIZE(gcpm_psy_properties),
};

#define gcpm_psy_changed_tickle_pps(gcpm) \
	((gcpm)->dc_state == DC_PASSTHROUGH || (gcpm)->dc_state == DC_RUNNING)

static int gcpm_psy_changed(struct notifier_block *nb, unsigned long action,
			    void *data)
{
	struct gcpm_drv *gcpm = container_of(nb, struct gcpm_drv, chg_nb);
	const int index = gcpm->chg_psy_active;
	struct power_supply *psy = data;
	bool tickle_pps_work = false;

	if (index == -1)
		return NOTIFY_OK;

	if ((action != PSY_EVENT_PROP_CHANGED) ||
	    (psy == NULL) || (psy->desc == NULL) || (psy->desc->name == NULL))
		return NOTIFY_OK;

	if (strcmp(psy->desc->name, gcpm->chg_psy_names[index]) == 0) {
		/* route upstream when the charger active and found */
		if (gcpm->chg_psy_avail[index])
			power_supply_changed(gcpm->psy);

		tickle_pps_work = gcpm_psy_changed_tickle_pps(gcpm);
	} else if (strcmp(psy->desc->name, gcpm->chg_psy_names[0]) == 0) {
		/* possibly JEITA or other violation, check PPS */
		tickle_pps_work = gcpm_psy_changed_tickle_pps(gcpm);
	} else if (gcpm->tcpm_psy_name &&
		   !strcmp(psy->desc->name, gcpm->tcpm_psy_name)) {

		/* from tcpm source (even if not selected) */
		tickle_pps_work = gcpm_psy_changed_tickle_pps(gcpm);
	} else if (gcpm->wlc_dc_name &&
	      !strcmp(psy->desc->name, gcpm->wlc_dc_name)) {

		/* from wc source (even if not selected) */
		tickle_pps_work = gcpm_psy_changed_tickle_pps(gcpm);
	}

	/* should tickle the PPS loop only when is running */
	if (tickle_pps_work)
		mod_delayed_work(system_wq, &gcpm->pps_work, 0);

	return NOTIFY_OK;
}

#define INIT_DELAY_MS 100
#define INIT_RETRY_DELAY_MS 1000

#define GCPM_TCPM_PSY_MAX 2

/*thiscan run */
static void gcpm_init_work(struct work_struct *work)
{
	struct gcpm_drv *gcpm = container_of(work, struct gcpm_drv,
					     init_work.work);
	int i, found = 0, ret = 0;
	bool dc_not_done;

	/* might run along set_property() */
	mutex_lock(&gcpm->chg_psy_lock);

	/*
	 * could call pps_init() in probe() and use lazy init for ->tcpm_psy
	 * when the device an APDO in the sink capabilities.
	 */
	if (gcpm->tcpm_phandle && !gcpm->tcpm_psy) {
		struct power_supply *tcpm_psy;

		tcpm_psy = pps_get_tcpm_psy(gcpm->device->of_node,
					    GCPM_TCPM_PSY_MAX);
		if (!IS_ERR_OR_NULL(tcpm_psy)) {
			const char *name = tcpm_psy->desc->name;

			gcpm->tcpm_psy_name = name;
			gcpm->tcpm_psy = tcpm_psy;

			/* PPS charging: needs an APDO */
			ret = pps_init(&gcpm->tcpm_pps_data, gcpm->device,
				       gcpm->tcpm_psy);
			if (ret == 0 && gcpm->debug_entry)
				pps_init_fs(&gcpm->tcpm_pps_data, gcpm->debug_entry);
			if (ret < 0) {
				pr_err("PPS init failure for %s (%d)\n",
				       name, ret);
			} else {
				gcpm->tcpm_pps_data.port_data =
					power_supply_get_drvdata(tcpm_psy);

				pps_init_state(&gcpm->tcpm_pps_data);
				pr_info("PPS available for %s\n",
					gcpm->tcpm_psy_name);
			}

		} else if (!tcpm_psy || !gcpm->log_psy_ratelimit) {
			/* abort on an error */
			pr_warn("PPS not available for tcpm\n");
			gcpm->tcpm_phandle = 0;
		} else {
			pr_warn("tcpm power supply not found, retrying... ret:%d\n",
				ret);
			gcpm->log_psy_ratelimit--;
		}

	}

	/* TODO: lookup by phandle as the dude above */
	if (gcpm->wlc_dc_name && !gcpm->wlc_dc_psy) {
		struct power_supply *wlc_dc_psy;

		wlc_dc_psy = power_supply_get_by_name(gcpm->wlc_dc_name);
		if (wlc_dc_psy) {
			const char *name = gcpm->wlc_dc_name;

			gcpm->wlc_dc_psy = wlc_dc_psy;

			/* PPS charging: needs an APDO */
			ret = pps_init(&gcpm->wlc_pps_data, gcpm->device,
					gcpm->wlc_dc_psy);
			if (ret == 0 && gcpm->debug_entry)
				pps_init_fs(&gcpm->wlc_pps_data, gcpm->debug_entry);
			if (ret < 0) {
				pr_err("PPS init failure for %s (%d)\n",
				       name, ret);
			} else {
				/* TODO: TBD */
				gcpm->wlc_pps_data.port_data = NULL;
				pps_init_state(&gcpm->wlc_pps_data);
				pr_info("PPS available for %s\n", name);
			}

		} else if (!gcpm->log_psy_ratelimit) {
			/* give up if wlc_dc_psy return an error */
			pr_warn("PPS not available for %s\n", gcpm->wlc_dc_name);
			gcpm->wlc_dc_name = NULL;
		} else {
			pr_warn("%s power supply not found, retrying... ret:%d\n",
				gcpm->wlc_dc_name, ret);
			gcpm->log_psy_ratelimit--;
		}
	}

	/* default is index 0 */
	for (i = 0; i < gcpm->chg_psy_count; i++) {
		if (!gcpm->chg_psy_avail[i]) {
			const char *name = gcpm->chg_psy_names[i];

			gcpm->chg_psy_avail[i] = power_supply_get_by_name(name);
			if (gcpm->chg_psy_avail[i])
				pr_info("init_work found %d:%s\n", i, name);
		}

		found += !!gcpm->chg_psy_avail[i];
	}

	/* we done when we have (at least) the primary */
	if (gcpm->chg_psy_avail[0]) {

		/* register the notifier only when have one (the default) */
		if (!gcpm->init_complete) {
			gcpm->chg_nb.notifier_call = gcpm_psy_changed;
			ret = power_supply_reg_notifier(&gcpm->chg_nb);
			if (ret < 0)
				pr_err("cannot register power supply notifer, ret=%d\n",
				ret);
		}

		/* this is the reason why we need a lock here */
		gcpm->resume_complete = true;
		gcpm->init_complete = true;
	}

	/* keep looking for late arrivals, TCPM and WLC if set */
	if (found == gcpm->chg_psy_count)
		gcpm->chg_psy_retries = 0;
	else if (gcpm->chg_psy_retries)
		gcpm->chg_psy_retries--;

	dc_not_done = (gcpm->tcpm_phandle && !gcpm->tcpm_psy) ||
		      (gcpm->wlc_dc_name && !gcpm->wlc_dc_psy);

	pr_warn("%s:%d %s retries=%d dc_not_done=%d tcpm_ok=%d wlc_ok=%d\n",
		__FILE__, __LINE__, __func__,
		gcpm->chg_psy_retries,
		dc_not_done,
		(!gcpm->tcpm_phandle || gcpm->tcpm_psy),
		(!gcpm->wlc_dc_name || gcpm->wlc_dc_psy));

	if (gcpm->chg_psy_retries || dc_not_done) {
		const unsigned long jif = msecs_to_jiffies(INIT_RETRY_DELAY_MS);

		schedule_delayed_work(&gcpm->init_work, jif);
	} else {
		pr_info("google_cpm init_work done %d/%d pps=%d wlc_dc=%d\n",
			found, gcpm->chg_psy_count,
			!!gcpm->tcpm_psy, !!gcpm->wlc_dc_psy);

		gcpm->dc_init_complete = true;

	}

	/* might run along set_property() */
	mutex_unlock(&gcpm->chg_psy_lock);
}

/* ------------------------------------------------------------------------ */

static int gcpm_debug_get_active(void *data, u64 *val)
{
	struct gcpm_drv *gcpm = data;

	mutex_lock(&gcpm->chg_psy_lock);
	*val = gcpm->dc_index;
	mutex_unlock(&gcpm->chg_psy_lock);
	return 0;
}

static int gcpm_debug_set_active(void *data, u64 val)
{
	struct gcpm_drv *gcpm = data;
	const int intval = val;

	if (intval != -1 && (intval < 0 || intval >= gcpm->chg_psy_count))
		return -ERANGE;
	if (intval != -1 && !gcpm->chg_psy_avail[intval])
		return -EINVAL;

	mutex_lock(&gcpm->chg_psy_lock);
	gcpm->force_active = val;
	mod_delayed_work(system_wq, &gcpm->select_work, 0);
	mutex_unlock(&gcpm->chg_psy_lock);

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(gcpm_debug_active_fops, gcpm_debug_get_active,
			gcpm_debug_set_active, "%lld\n");

static int gcpm_debug_dc_limit_demand_show(void *data, u64 *val)
{
	struct gcpm_drv *gcpm = data;

	*val = gcpm->dc_limit_demand;
	return 0;
}

static int gcpm_debug_dc_limit_demand_set(void *data, u64 val)
{
	struct gcpm_drv *gcpm = data;
	const int intval = val;

	mutex_lock(&gcpm->chg_psy_lock);
	if (gcpm->dc_limit_demand != intval) {
		gcpm->dc_limit_demand = intval;
		gcpm->new_dc_limit = true;
	}

	mutex_unlock(&gcpm->chg_psy_lock);
	return 0;
}


DEFINE_SIMPLE_ATTRIBUTE(gcpm_debug_dc_limit_demand_fops,
			gcpm_debug_dc_limit_demand_show,
                        gcpm_debug_dc_limit_demand_set,
			"%llu\n");


static int gcpm_debug_pps_stage_get(void *data, u64 *val)
{
	struct gcpm_drv *gcpm = data;
	struct pd_pps_data *pps_data;

	mutex_lock(&gcpm->chg_psy_lock);
	pps_data = gcpm_pps_data(gcpm);
	if (pps_data)
		*val = pps_data->stage;
	mutex_unlock(&gcpm->chg_psy_lock);
	return 0;
}

static int gcpm_debug_pps_stage_set(void *data, u64 val)
{
	struct gcpm_drv *gcpm = data;
	const int intval = (int)val;
	struct pd_pps_data *pps_data;

	if (intval < PPS_DISABLED || intval > PPS_ACTIVE)
		return -EINVAL;

	mutex_lock(&gcpm->chg_psy_lock);
	pps_data = gcpm_pps_data(gcpm);
	if (pps_data)
		pps_data->stage = intval;
	gcpm->force_pps = !pps_is_disabled(intval);
	mod_delayed_work(system_wq, &gcpm->pps_work, 0);
	mutex_unlock(&gcpm->chg_psy_lock);

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(gcpm_debug_pps_stage_fops, gcpm_debug_pps_stage_get,
			gcpm_debug_pps_stage_set, "%llu\n");

static int gcpm_debug_dc_state_get(void *data, u64 *val)
{
	struct gcpm_drv *gcpm = data;

	mutex_lock(&gcpm->chg_psy_lock);
	*val = gcpm->dc_state;
	mutex_unlock(&gcpm->chg_psy_lock);
	return 0;
}

static int gcpm_debug_dc_state_set(void *data, u64 val)
{
	struct gcpm_drv *gcpm = data;
	const int intval = (int)val;

	if (intval < DC_DISABLED || intval > DC_PASSTHROUGH)
		return -EINVAL;

	mutex_lock(&gcpm->chg_psy_lock);
	gcpm->dc_state = intval;
	mod_delayed_work(system_wq, &gcpm->select_work, 0);
	mutex_unlock(&gcpm->chg_psy_lock);

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(gcpm_debug_dc_state_fops, gcpm_debug_dc_state_get,
			gcpm_debug_dc_state_set, "%llu\n");

static struct dentry *gcpm_init_fs(struct gcpm_drv *gcpm)
{
	struct dentry *de;

	de = debugfs_create_dir("google_cpm", 0);
	if (IS_ERR_OR_NULL(de))
		return NULL;

	debugfs_create_file("dc_state", 0644, de, gcpm, &gcpm_debug_dc_state_fops);
	debugfs_create_file("active", 0644, de, gcpm, &gcpm_debug_active_fops);
	debugfs_create_file("dc_limit_demand", 0644, de, gcpm,
			    &gcpm_debug_dc_limit_demand_fops);
	debugfs_create_file("pps_stage", 0644, de, gcpm,
			    &gcpm_debug_pps_stage_fops);

	return de;
}

/* ------------------------------------------------------------------------ */

static int gcpm_probe_psy_names(struct gcpm_drv *gcpm)
{
	struct device *dev = gcpm->device;
	int i, count, ret;

	if (!gcpm->device)
		return -EINVAL;

	count = of_property_count_strings(dev->of_node,
					  "google,chg-power-supplies");
	if (count <= 0 || count > GCPM_MAX_CHARGERS)
		return -ERANGE;

	ret = of_property_read_string_array(dev->of_node,
					    "google,chg-power-supplies",
					    (const char**)&gcpm->chg_psy_names,
					    count);
	if (ret != count)
		return -ERANGE;

	for (i = 0; i < count; i++)
		dev_info(gcpm->device, "%d:%s\n", i, gcpm->chg_psy_names[i]);

	return count;
}

/* -------------------------------------------------------------------------
 *  Use to abstract the PPS adapter if needed.
 */

static int gcpm_pps_psy_set_property(struct power_supply *psy,
				    enum power_supply_property prop,
				    const union power_supply_propval *val)
{
	struct gcpm_drv *gcpm = power_supply_get_drvdata(psy);
	struct pd_pps_data *pps_data;
	int ret = 0;

	mutex_lock(&gcpm->chg_psy_lock);

	pps_data = gcpm_pps_data(gcpm);
	if (!pps_data || !pps_data->pps_psy) {
		pr_debug("%s: no target prop=%d ret=%d\n", __func__, prop, ret);
		mutex_unlock(&gcpm->chg_psy_lock);
		return -EAGAIN;
	}

	switch (prop) {
	default:
		ret = power_supply_set_property(pps_data->pps_psy, prop, val);
		break;
	}

	mutex_unlock(&gcpm->chg_psy_lock);
	pr_debug("%s: prop=%d val=%d ret=%d\n", __func__,
		 prop, val->intval, ret);

	return ret;
}

static int gcpm_pps_psy_get_property(struct power_supply *psy,
				    enum power_supply_property prop,
				    union power_supply_propval *val)
{
	struct gcpm_drv *gcpm = power_supply_get_drvdata(psy);
	struct pd_pps_data *pps_data;
	int ret = 0;

	mutex_lock(&gcpm->chg_psy_lock);

	pps_data = gcpm_pps_data(gcpm);
	if (pps_data && pps_data->pps_psy) {
		ret = power_supply_get_property(pps_data->pps_psy, prop, val);
		pr_debug("%s: prop=%d val=%d ret=%d\n", __func__,
			 prop, val->intval, ret);
		goto done;
	}

	switch (prop) {
	case POWER_SUPPLY_PROP_USB_TYPE:
		val->intval = POWER_SUPPLY_USB_TYPE_UNKNOWN;
		break;
	default:
		val->intval = 0;
		break;
	}

done:
	mutex_unlock(&gcpm->chg_psy_lock);
	return ret;
}

/* check pps_is_avail(), pps_prog_online() and pps_check_type() */
static enum power_supply_property gcpm_pps_psy_properties[] = {
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_VOLTAGE_MIN,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,	/* 17 */
	POWER_SUPPLY_PROP_ONLINE,	/* 4 */
	POWER_SUPPLY_PROP_PRESENT,	/* 3 */
	POWER_SUPPLY_PROP_TYPE,		/* */
	POWER_SUPPLY_PROP_USB_TYPE,	/* */
	POWER_SUPPLY_PROP_VOLTAGE_NOW,	/* */
};

static int gcpm_pps_psy_is_writeable(struct power_supply *psy,
				   enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
	case POWER_SUPPLY_PROP_ONLINE:
	case POWER_SUPPLY_PROP_CURRENT_NOW:
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		return 1;
	default:
		break;
	}

	return 0;
}

static enum power_supply_usb_type gcpm_pps_usb_types[] = {
	POWER_SUPPLY_USB_TYPE_UNKNOWN,
	POWER_SUPPLY_USB_TYPE_PD_PPS
};

static const struct power_supply_desc gcpm_pps_psy_desc = {
	.name		= "gcpm_pps",
	.type		= POWER_SUPPLY_TYPE_UNKNOWN,
	.get_property	= gcpm_pps_psy_get_property,
	.set_property 	= gcpm_pps_psy_set_property,
	.properties	= gcpm_pps_psy_properties,
	.property_is_writeable = gcpm_pps_psy_is_writeable,
	.num_properties	= ARRAY_SIZE(gcpm_pps_psy_properties),

	/* POWER_SUPPLY_PROP_USB_TYPE requires an array of these */
	.usb_types	= gcpm_pps_usb_types,
	.num_usb_types	= ARRAY_SIZE(gcpm_pps_usb_types),
};

/* ------------------------------------------------------------------------- */

#define LOG_PSY_RATELIMIT_CNT	200

static int google_cpm_probe(struct platform_device *pdev)
{
	struct power_supply_config gcpm_pps_psy_cfg = { 0 };
	struct power_supply_config psy_cfg = { 0 };
	const char *tmp_name = NULL;
	struct gcpm_drv *gcpm;
	int ret;

	gcpm = devm_kzalloc(&pdev->dev, sizeof(*gcpm), GFP_KERNEL);
	if (!gcpm)
		return -ENOMEM;

	gcpm->device = &pdev->dev;
	gcpm->force_active = -1;
	gcpm->log_psy_ratelimit = LOG_PSY_RATELIMIT_CNT;
	gcpm->chg_psy_retries = 10; /* chg_psy_retries *  INIT_RETRY_DELAY_MS */
	gcpm->out_uv = -1;
	gcpm->out_ua = -1;
	INIT_DELAYED_WORK(&gcpm->pps_work, gcpm_pps_wlc_dc_work);
	INIT_DELAYED_WORK(&gcpm->select_work, gcpm_chg_select_work);
	INIT_DELAYED_WORK(&gcpm->init_work, gcpm_init_work);
	mutex_init(&gcpm->chg_psy_lock);

	/* this is my name */
	ret = of_property_read_string(pdev->dev.of_node, "google,psy-name",
				      &tmp_name);
	if (ret == 0) {
		gcpm_psy_desc.name = devm_kstrdup(&pdev->dev, tmp_name,
						  GFP_KERNEL);
		if (!gcpm_psy_desc.name)
			return -ENOMEM;
	}

	/* subs power supply names */
	gcpm->chg_psy_count = gcpm_probe_psy_names(gcpm);
	if (gcpm->chg_psy_count <= 0)
		return -ENODEV;

	/* DC/PPS needs at least one power supply of this type */
	ret = of_property_read_u32(pdev->dev.of_node,
				   "google,tcpm-power-supply",
				   &gcpm->tcpm_phandle);
	if (ret < 0)
		pr_warn("google,tcpm-power-supply not defined\n");

	ret = of_property_read_string(pdev->dev.of_node,
				      "google,wlc_dc-power-supply",
				      &tmp_name);
	if (ret == 0) {
		gcpm->wlc_dc_name = devm_kstrdup(&pdev->dev, tmp_name,
						     GFP_KERNEL);
		if (!gcpm->wlc_dc_name)
			return -ENOMEM;
	}

	/* GCPM might need a gpio to enable/disable DC/PPS */
	gcpm->dcen_gpio = of_get_named_gpio(pdev->dev.of_node,
					    "google,dc-en", 0);
	if (gcpm->dcen_gpio >= 0) {
		of_property_read_u32(pdev->dev.of_node, "google,dc-en-value",
				     &gcpm->dcen_gpio_default);

		/* make sure that the DC is DISABLED before doing this */
		ret = gpio_direction_output(gcpm->dcen_gpio,
					    gcpm->dcen_gpio_default);
		pr_info("google,dc-en value = %d ret=%d\n",
			gcpm->dcen_gpio_default, ret);
	}

	/* Triggers to enable dc charging */
	ret = of_property_read_u32(pdev->dev.of_node, "google,dc_limit-demand",
				   &gcpm->dc_limit_demand);
	if (ret < 0)
		gcpm->dc_limit_demand = GCPM_DEFAULT_DC_LIMIT_DEMAND;

	/* voltage lower bound */
	ret = of_property_read_u32(pdev->dev.of_node, "google,dc_limit-vbatt_min",
				   &gcpm->dc_limit_vbatt_min);
	if (ret < 0)
		gcpm->dc_limit_vbatt_min = GCPM_DEFAULT_DC_LIMIT_VBATT_MIN;
	ret = of_property_read_u32(pdev->dev.of_node, "google,dc_limit-vbatt_low",
				   &gcpm->dc_limit_vbatt_low);
	if (ret < 0)
		gcpm->dc_limit_vbatt_low = gcpm->dc_limit_vbatt_min -
					   GCPM_DEFAULT_DC_LIMIT_DELTA_LOW;
	if (gcpm->dc_limit_vbatt_low > gcpm->dc_limit_vbatt_min)
		gcpm->dc_limit_vbatt_low = gcpm->dc_limit_vbatt_min;

	/* voltage upper bound */
	ret = of_property_read_u32(pdev->dev.of_node, "google,dc_limit-vbatt_max",
				   &gcpm->dc_limit_vbatt_max);
	if (ret < 0)
		gcpm->dc_limit_vbatt_max = GCPM_DEFAULT_DC_LIMIT_VBATT_MAX;
	ret = of_property_read_u32(pdev->dev.of_node, "google,dc_limit-vbatt_high",
				   &gcpm->dc_limit_vbatt_high);
	if (ret < 0)
		gcpm->dc_limit_vbatt_high = gcpm->dc_limit_vbatt_max -
					    GCPM_DEFAULT_DC_LIMIT_DELTA_HIGH;
	if (gcpm->dc_limit_vbatt_high > gcpm->dc_limit_vbatt_max)
		gcpm->dc_limit_vbatt_high = gcpm->dc_limit_vbatt_max;

	/* sysfs & debug */
	gcpm->debug_entry = gcpm_init_fs(gcpm);
	if (!gcpm->debug_entry)
		pr_warn("No debug control\n");

	platform_set_drvdata(pdev, gcpm);

	psy_cfg.drv_data = gcpm;
	psy_cfg.of_node = pdev->dev.of_node;
	gcpm->psy = devm_power_supply_register(gcpm->device,
					       &gcpm_psy_desc,
					       &psy_cfg);
	if (IS_ERR(gcpm->psy)) {
		ret = PTR_ERR(gcpm->psy);
		if (ret == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		dev_err(gcpm->device, "Couldn't register gcpm, (%d)\n", ret);
		return -ENODEV;
	}

	/* gcpm_pps_psy_cfg.of_node is used to find out the snk_pdos */
	gcpm_pps_psy_cfg.drv_data = gcpm;
	gcpm_pps_psy_cfg.of_node = pdev->dev.of_node;
	gcpm->pps_psy = devm_power_supply_register(gcpm->device,
						   &gcpm_pps_psy_desc,
						   &gcpm_pps_psy_cfg);
	if (IS_ERR(gcpm->pps_psy)) {
		ret = PTR_ERR(gcpm->pps_psy);
		if (ret == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		dev_err(gcpm->device, "Couldn't register gcpm_pps (%d)\n", ret);
		return -ENODEV;
	}

	/* give time to fg driver to start */
	schedule_delayed_work(&gcpm->init_work,
			      msecs_to_jiffies(INIT_DELAY_MS));

	return 0;
}

static int google_cpm_remove(struct platform_device *pdev)
{
	struct gcpm_drv *gcpm = platform_get_drvdata(pdev);
	int i;

	if (!gcpm)
		return 0;

	for (i = 0; i < gcpm->chg_psy_count; i++) {
		if (!gcpm->chg_psy_avail[i])
			continue;

		power_supply_put(gcpm->chg_psy_avail[i]);
		gcpm->chg_psy_avail[i] = NULL;
	}

	if (gcpm->wlc_dc_psy)
		power_supply_put(gcpm->wlc_dc_psy);

	return 0;
}

static const struct of_device_id google_cpm_of_match[] = {
	{.compatible = "google,cpm"},
	{},
};
MODULE_DEVICE_TABLE(of, google_cpm_of_match);


static struct platform_driver google_cpm_driver = {
	.driver = {
		   .name = "google_cpm",
		   .owner = THIS_MODULE,
		   .of_match_table = google_cpm_of_match,
		   .probe_type = PROBE_PREFER_ASYNCHRONOUS,
#ifdef SUPPORT_PM_SLEEP
		   /* .pm = &gcpm_pm_ops, */
#endif
		   },
	.probe = google_cpm_probe,
	.remove = google_cpm_remove,
};

module_platform_driver(google_cpm_driver);

MODULE_DESCRIPTION("Google Charging Policy Manager");
MODULE_AUTHOR("AleX Pelosi <apelosi@google.com>");
MODULE_LICENSE("GPL");

#if 0

/* NOTE: call with a lock around gcpm->chg_psy_lock */
static int gcpm_dc_charging(struct gcpm_drv *gcpm)
{
	struct power_supply *dc_psy;
	int vchg, ichg, status;

	dc_psy = gcpm_chg_get_active(gcpm);
	if (!dc_psy) {
		pr_err("DC_CHG: invalid charger\n");
		return -ENODEV;
	}

	vchg = GPSY_GET_PROP(dc_psy, POWER_SUPPLY_PROP_VOLTAGE_NOW);
	ichg = GPSY_GET_PROP(dc_psy, POWER_SUPPLY_PROP_CURRENT_NOW);
	status = GPSY_GET_PROP(dc_psy, POWER_SUPPLY_PROP_STATUS);

	pr_err("DC_CHG: vchg=%d, ichg=%d status=%d\n",
	       vchg, ichg, status);

	return 0;
}

static void gcpm_pps_dc_charging(struct gcpm_drv *gcpm)
{
	struct pd_pps_data *pps_data = &gcpm->pps_data;
	struct power_supply *pps_psy = gcpm->tcpm_psy;
	const int pre_out_ua = pps_data->op_ua;
	const int pre_out_uv = pps_data->out_uv;
	int ret, pps_ui = -ENODEV;

	if (gcpm->dc_state == DC_ENABLE) {
		struct pd_pps_data *pps_data = &gcpm->pps_data;
		bool pwr_ok;

		/* must run at the end of PPS negotiation */
		if (gcpm->out_ua == -1)
			gcpm->out_ua = min(gcpm->cc_max, pps_data->max_ua);
		if (gcpm->out_uv == -1) {
			struct power_supply *chg_psy =
						gcpm_chg_get_active(gcpm);
			unsigned long ta_max_v, value;
			int vbatt = -1;

			ta_max_v = pps_data->max_ua * pps_data->max_uv;
			ta_max_v /= gcpm->out_ua;
			if (ta_max_v > DC_TA_VMAX_MV)
				ta_max_v = DC_TA_VMAX_MV;

			if (chg_psy)
				vbatt = GPSY_GET_PROP(chg_psy,
						POWER_SUPPLY_PROP_VOLTAGE_NOW);
			if (vbatt < 0)
				vbatt = gcpm->fv_uv;
			if (vbatt < 0)
				vbatt = 0;

			/* good for pca9468 */
			value = 2 * vbatt + DC_VBATT_HEADROOM_MV;
			if (value < DC_TA_VMIN_MV)
				value = DC_TA_VMIN_MV;

			/* PPS voltage in 20mV steps */
			gcpm->out_uv = value - value % 20000;
		}

		pr_info("CHG_CHK: max_uv=%d,max_ua=%d  out_uv=%d,out_ua=%d\n",
			pps_data->max_uv, pps_data->max_ua,
			gcpm->out_uv, gcpm->out_ua);

		pps_ui = pps_update_adapter(pps_data, gcpm->out_uv,
					    gcpm->out_ua, pps_psy);
		if (pps_ui < 0)
			pps_ui = PPS_ERROR_RETRY_MS;

		/* wait until adapter is at or over request */
		pwr_ok = pps_data->out_uv == gcpm->out_uv &&
				pps_data->op_ua == gcpm->out_ua;
		if (pwr_ok) {
			ret = gcpm_chg_offline(gcpm);
			if (ret == 0)
				ret = gcpm_dc_start(gcpm, gcpm->dc_index);
			if (ret == 0) {
				gcpm->dc_state = DC_RUNNING;
				pps_ui = DC_RUN_DELAY_MS;
			}  else if (pps_ui > DC_ERROR_RETRY_MS) {
				pps_ui = DC_ERROR_RETRY_MS;
			}
		}

		/*
			* TODO: add retries and switch to DC_ENABLE again or to
			* DC_DISABLED on timeout.
			*/

		pr_info("PPS_DC: dc_state=%d out_uv=%d %d->%d, out_ua=%d %d->%d\n",
			gcpm->dc_state,
			pps_data->out_uv, pre_out_uv, gcpm->out_uv,
			pps_data->op_ua, pre_out_ua, gcpm->out_ua);
	} else if (gcpm->dc_state == DC_RUNNING)  {

		ret = gcpm_chg_ping(gcpm, 0, 0);
		if (ret < 0)
			pr_err("PPS_DC: ping failed with %d\n", ret);

		/* update gcpm->out_uv, gcpm->out_ua */
		pr_info("PPS_DC: dc_state=%d out_uv=%d %d->%d out_ua=%d %d->%d\n",
			gcpm->dc_state,
			pps_data->out_uv, pre_out_uv, gcpm->out_uv,
			pps_data->op_ua, pre_out_ua, gcpm->out_ua);

		ret = gcpm_dc_charging(gcpm);
		if (ret < 0)
			pps_ui = DC_ERROR_RETRY_MS;

		ret = pps_update_adapter(&gcpm->pps_data,
						gcpm->out_uv, gcpm->out_ua,
						pps_psy);
		if (ret < 0)
			pps_ui = PPS_ERROR_RETRY_MS;
	}

	return pps_ui;
}
#endif