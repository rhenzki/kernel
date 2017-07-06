/*
 * Copyright (c) 2009 ACER, INC.
 * 
 * All source code in this file is licensed under the following license
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can find it at http://www.fsf.org
 */

#include "msm_fb.h"

/* FIXME CONFIG_FB_MDDI_CATCH_LCDC_PRISM is incorrect. */

#ifdef CONFIG_FB_MSM_TRY_MDDI_CATCH_LCDC_PRISM
#include "mddihosti.h"
#endif

/* TODO Implement sharp panel on/off, backlight functions */
static int lcdc_sharp_panel_on(struct platform_device *pdev)
{
	pr_debug("%s\n", __func__);
	return 0;
}

static int lcdc_sharp_panel_off(struct platform_device *pdev)
{
	pr_debug("%s\n", __func__);
	return 0;
}

static void lcdc_sharp_lcd_set_backlight(struct msm_fb_data_type *mfd)
{
	pr_debug("%s\n", __func__);
}

static struct msm_fb_panel_data lcdc_sharp_panel_data = {
	.on = lcdc_sharp_panel_on,
	.off = lcdc_sharp_panel_off,
	.set_backlight = lcdc_sharp_lcd_set_backlight,
};

static struct msm_panel_info pinfo;

static int __init lcdc_sharp_init(void)
{
	int ret;

#ifdef CONFIG_FB_MSM_TRY_MDDI_CATCH_LCDC_PRISM
	ret = msm_fb_detect_client("lcdc_sharp_wvga");
	if (ret == -ENODEV)
		return 0;

	if (ret && (mddi_get_client_id() != 0))
		return 0;
#endif
	pinfo.xres = 480;
	pinfo.yres = 800;
	pinfo.type = LCDC_PANEL;
	pinfo.pdest = DISPLAY_1;
	pinfo.wait_cycle = 0;
	pinfo.bpp = 16;
	pinfo.fb_num = 2;
	pinfo.clk_rate = 38400000;

	pinfo.lcdc.h_back_porch = 0x1e;
	pinfo.lcdc.h_front_porch = 0x10;
	pinfo.lcdc.h_pulse_width = 0xa;
	pinfo.lcdc.v_back_porch = 0x4;
	pinfo.lcdc.v_front_porch = 0x4;
	pinfo.lcdc.v_pulse_width = 0x2;
	pinfo.lcdc.border_clr = 0;		/* blk */
	pinfo.lcdc.underflow_clr = 0xff;	/* blue */
	pinfo.lcdc.hsync_skew = 0;

	ret = lcdc_device_register(&pinfo, &lcdc_sharp_panel_data);
	if (ret)
		printk(KERN_ERR "%s: failed to register device!\n", __func__);

	return ret;
}

module_init(lcdc_sharp_init);
