/*
 * Acer Headset device detection driver
 *
 *
 * Copyright (C) 2009 acer Corporation.
 *
 * Authors:
 *    Lawrence Hou <Lawrence_Hou@acer.com.tw>
 */

#ifndef __ACER_HEADSET_H
#define __ACER_HEADSET_H


#include <linux/switch.h>

#define HPH_AMP_EN 39
#define EXPIRES 5

struct hs_res {
	struct switch_dev sdev;
	unsigned int det;
	unsigned int irq;
	unsigned int mic_bias_en;
	unsigned int hph_amp_en;
	struct hrtimer timer;
	ktime_t debounce_time;
};
extern void hs_amp(bool enable);
#endif
