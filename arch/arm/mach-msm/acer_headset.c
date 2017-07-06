/*
 * Acer Headset device detection driver.
 *
 *
 * Copyright (C) 2008 acer Corporation.
 *
 * Authors:
 *    Lawrence Hou <Lawrence_Hou@acer.com.tw>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

/*
    For detecting acer headset .

    Headset insertion/removal causes UEvent's to be sent, and
    /sys/class/switch/acer-hs/state to be updated.
*/


#include <linux/module.h>
#include <linux/sysdev.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/input.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/input.h>
#include <asm/gpio.h>
#include <mach/acer_headset.h>

#if 0
#define ACER_HS_DBG(fmt, arg...) printk(KERN_INFO "[ACER_HS]: %s " fmt "\n", __FUNCTION__, ## arg)
#else
#define ACER_HS_DBG(fmt, arg...) do {} while (0)
#endif

#define HS_DET		151
#define MIC_BIAS_EN	152
static void enable_amp_work(struct work_struct *work);
static struct work_struct short_wq;
static DECLARE_DELAYED_WORK(en_amp_wq, enable_amp_work);

enum {
	NO_DEVICE	= 0,
	ACER_HEADSET	= 1,
};

static struct hs_res *hr;

static ssize_t acer_hs_print_name(struct switch_dev *sdev, char *buf)
{
	switch (switch_get_state(&hr->sdev)) {
		case NO_DEVICE:
			return sprintf(buf, "No Device\n");
		case ACER_HEADSET:
			return sprintf(buf, "Headset\n");
	}
	return -EINVAL;
}

static ssize_t acer_hs_print_state(struct switch_dev *sdev, char *buf)
{
	return sprintf(buf, "%s\n", (switch_get_state(&hr->sdev) ? "1" : "0"));
}

static void remove_headset(void)
{
	ACER_HS_DBG(" Remove Headset.\n");

	switch_set_state(&hr->sdev, NO_DEVICE);
}

static void acer_update_state_work(struct work_struct *work)
{
	bool state;

	state = gpio_get_value(hr->det);
	switch_set_state(&hr->sdev, state? NO_DEVICE : ACER_HEADSET);

	if (!state) {
	        msleep(1000);
	        gpio_set_value(hr->mic_bias_en,1);
	        ACER_HS_DBG("mic_bias_en = %d \n",gpio_get_value(hr->mic_bias_en));

	} else {
	        gpio_set_value(hr->mic_bias_en,0);
	        ACER_HS_DBG("mic_bias_en = %d \n",gpio_get_value(hr->mic_bias_en));
	}

}

static void enable_amp_work(struct work_struct *work)
{
	gpio_set_value(hr->hph_amp_en,1);
}

void hs_amp(bool enable)
{
	if(enable)
		schedule_delayed_work(&en_amp_wq, EXPIRES);
	else
		gpio_set_value(hr->hph_amp_en, enable);
}

static enum hrtimer_restart detect_event_timer_func(struct hrtimer *data)
{
	ACER_HS_DBG("");

	schedule_work(&short_wq);
	return HRTIMER_NORESTART;
}

static irqreturn_t hs_det_irq(int irq, void *dev_id)
{
	ACER_HS_DBG("Update Headset state by scheduling a work.\n");
	gpio_set_value(hr->mic_bias_en,0);
	hrtimer_start(&hr->timer, hr->debounce_time, HRTIMER_MODE_REL);

	return IRQ_HANDLED;
}

static int acer_hs_probe(struct platform_device *pdev)
{
	int ret;

	printk(KERN_INFO "[ACER-HS]: Registering ACER headset driver\n");
	hr = kzalloc(sizeof(struct hs_res), GFP_KERNEL);
	if (!hr)
		return -ENOMEM;

	hr->debounce_time = ktime_set(0, 500000000);  /* 500 ms */

	INIT_WORK(&short_wq, acer_update_state_work);
	hr->sdev.name = "acer-hs";
	hr->sdev.print_name = acer_hs_print_name;
	hr->sdev.print_state = acer_hs_print_state;

	ret = switch_dev_register(&hr->sdev);
	if (ret < 0)
	{
		pr_err("switch_dev fail!\n");
		goto err_switch_dev_register;
	}

	hr->det = HS_DET;
	ret = gpio_request(hr->det, "hs_detect");
	if (ret < 0)
	{
		pr_err("request detect gpio fail!\n");
		goto err_request_detect_gpio;
	}

	/* mic_bias_en - mic bias enable*/
	hr->mic_bias_en = MIC_BIAS_EN;
	ret = gpio_request(hr->mic_bias_en, "MIC BIAS EN");
	if (ret) {
		pr_err("GPIO request for MIC BIAS EN failed\n");
		goto err_request_mic_bias_gpio;
	}

	/* hph_en_amp - head phone amplifier enable*/
	hr->hph_amp_en = HPH_AMP_EN;
	ret = gpio_request(hr->hph_amp_en, "HPH AMP EN");
	if (ret) {
		pr_err("GPIO request for HPH AMP EN failed!\n");
		return ret;
	}

	hr->irq = gpio_to_irq(hr->det);
	if (hr->irq < 0) {
		ret = hr->irq;
		pr_err("get hs detect irq num fail!\n");
		goto err_get_hs_detect_irq_num_failed;
	}

	hrtimer_init(&hr->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hr->timer.function = detect_event_timer_func;

	ret = request_irq(hr->irq, hs_det_irq,
			  IRQF_TRIGGER_FALLING, "hs_detect", NULL);
	if (ret < 0)
	{
		pr_err("request detect irq fail!\n");
		goto err_request_detect_irq;
	}

#if 0
	/* open this section for debug usage. */
	ret = set_irq_wake(hr->irq, 1);
	if (ret < 0)
	{
		pr_info("err_request_detect_irq fail!\n");
		goto err_request_detect_irq;
	}
#endif
	ACER_HS_DBG("probe done.\n");

	return 0;

err_request_detect_irq:
	free_irq(hr->irq, 0);
err_get_hs_detect_irq_num_failed:
	gpio_free(hr->det);
err_request_detect_gpio:
	gpio_free(hr->det);
err_request_mic_bias_gpio:
	gpio_free(hr->mic_bias_en);
err_switch_dev_register:
	pr_err("ACER-HS: Failed to register driver\n");

	return ret;
}

static int acer_hs_remove(struct platform_device *pdev)
{
	ACER_HS_DBG("");
	if (switch_get_state(&hr->sdev))
		remove_headset();
	gpio_free(hr->det);
	free_irq(hr->irq, 0);
	switch_dev_unregister(&hr->sdev);

	return 0;
}

static struct platform_device acer_hs_device = {
	.name		= "acer-hs",
};

static struct platform_driver acer_hs_driver = {
	.probe		= acer_hs_probe,
	.remove		= acer_hs_remove,
	.driver		= {
		.name		= "acer-hs",
		.owner		= THIS_MODULE,
	},
};

static int __init acer_hs_init(void)
{
	int ret;
	ACER_HS_DBG("");
	ret = platform_driver_register(&acer_hs_driver);
	if (ret)
		return ret;
	return platform_device_register(&acer_hs_device);
}

static void __exit acer_hs_exit(void)
{
	platform_device_unregister(&acer_hs_device);
	platform_driver_unregister(&acer_hs_driver);
}

module_init(acer_hs_init);
module_exit(acer_hs_exit);

MODULE_AUTHOR("Lawrence Hou <Lawrence_Hou@acer.com.tw>");
MODULE_DESCRIPTION("Acer Headset detection driver");
MODULE_LICENSE("GPL");
