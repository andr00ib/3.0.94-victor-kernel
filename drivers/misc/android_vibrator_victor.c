/*
 * android vibrator driver (msm7x27, Motor IC)
 *
 * Copyright (C) 2009 LGE, Inc.
 *
 * Author: Jinkyu Choi <jinkyu@lge.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#include <linux/platform_device.h>
#include <mach/gpio.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/err.h>
#include <mach/board_lge.h>
#include <mach/timed_output.h>

#define PWM_DUTY_1 		0x01
#define PWM_DUTY_2 		0x02
#define PWM_DUTY_3 		0x03
#define VIB_OFF 		0x00

struct lge_vibrator_platform_data *vibe_data = 0;
static atomic_t vibe_gain = ATOMIC_INIT(60); /* default max gain value is 64 */

struct timed_vibrator_data {
	struct timed_output_dev dev;
	struct hrtimer timer;
	struct work_struct vib_work;
	spinlock_t lock;
	unsigned 	gpio;
	unsigned 	value;
	unsigned 	state;
	unsigned    overdrv;
	int 		max_timeout;
	u8 		active_low;
};

static uint32_t debug_mask;
module_param_named(debug, debug_mask, uint, 0664);

static int android_vibrator_intialize(void)
{
	/* Disable IC  */
	if (vibe_data->ic_enable_set(0) < 0) {
		printk(KERN_ERR "%s IC enable set failed\n", __FUNCTION__);
		return -1;
	}

	/* Initializ and disable PWM */
	if (vibe_data->pwm_set(0, 0) < 0) {
		printk(KERN_ERR "%s PWM set failed\n", __FUNCTION__);
		return -1;
	}

	return 0;
}

static int android_vibrator_force_set(int nForce)
{
	/* Check the Force value with Max and Min force value */
	if (nForce > 63) nForce = 63;
	if (nForce < 0) nForce = 0;

	/* TODO: control the gain of vibrator */

	if (nForce == 0) {
		vibe_data->power_set(0); /* should be checked for vibrator response time */
		vibe_data->pwm_set(0, nForce);
		vibe_data->ic_enable_set(0);
	} else {
		vibe_data->pwm_set(1, nForce);
		vibe_data->power_set(1); /* should be checked for vibrator response time */
		vibe_data->ic_enable_set(1);
	}
	return 0;
}

static void vib_work_func(struct work_struct *vib_work)
{
    int gain = atomic_read(&vibe_gain);
	struct timed_vibrator_data *data = (void *)NULL;
	data = container_of(vib_work, struct timed_vibrator_data, vib_work);

	if (debug_mask)
		printk("[vib] state %d\n", data->state);

	switch(data->state) {
		case PWM_DUTY_1:
			android_vibrator_force_set(gain);
			if (data->overdrv > 0)
				data->state = PWM_DUTY_2;
			else
				data->state = VIB_OFF;
			break;
		case PWM_DUTY_2:
			android_vibrator_force_set(52);
			data->state = PWM_DUTY_3;
			break;
		case PWM_DUTY_3:
			android_vibrator_force_set(2);
			data->state = VIB_OFF;
			break;
		case VIB_OFF:
		default:
			android_vibrator_force_set(0);
			data->state = VIB_OFF;
			break;
	}
}

static void gen_pwm(struct timed_vibrator_data *data)
{
	schedule_work(&data->vib_work);

	if (data->state != VIB_OFF) {
		hrtimer_start(&data->timer,
				ktime_set(data->overdrv  / 1000, (data->overdrv % 1000) * 1000000),
				HRTIMER_MODE_REL);
	}
}

static enum hrtimer_restart vibrator_timer_func(struct hrtimer *timer)
{
	struct timed_vibrator_data *data;
	data = container_of(timer, struct timed_vibrator_data, timer);
	gen_pwm(data);

	return HRTIMER_NORESTART;
}

static int vibrator_get_time(struct timed_output_dev *dev)
{
	struct timed_vibrator_data *data;
	data = container_of(dev, struct timed_vibrator_data, dev);

	if (hrtimer_active(&data->timer)) {
		ktime_t r = hrtimer_get_remaining(&data->timer);
		return r.tv.sec * 1000 + r.tv.nsec / 1000000;
	} else
		return 0;
}

static void vibrator_enable(struct timed_output_dev *dev, int value)
{
	struct timed_vibrator_data *data = (void *)NULL;
	unsigned long	flags;

	/* Add protection code for detect null point error */
	if (dev == (void *)NULL) {
		printk(KERN_INFO "%s:dev is null\n",__FUNCTION__);
		return;
	}

	data = container_of(dev, struct timed_vibrator_data, dev);
	if (data == (void *)NULL) {
		printk(KERN_INFO "%s:data is null\n",__FUNCTION__);
		return;
	}

	if (debug_mask)
		printk("[%s] time = %d msec\n", __FUNCTION__, value);

	spin_lock_irqsave(&data->lock, flags);
	hrtimer_cancel(&data->timer);

	if (value == 0) {
		data->state = VIB_OFF;
		schedule_work(&data->vib_work);
	} else {
#ifndef CONFIG_LGE_DOMESTIC
		if (value > data->max_timeout)
			value = data->max_timeout;
#endif
		if (value > 30) {
			data->value = value - 20;
			data->overdrv = 10;
		} else {
			data->value = value;
			data->overdrv = 0;
		}
		data->state = PWM_DUTY_1;
		schedule_work(&data->vib_work);
		hrtimer_start(&data->timer, ktime_set(data->value / 1000, (data->value % 1000) * 1000000), HRTIMER_MODE_REL);
	}
	spin_unlock_irqrestore(&data->lock, flags);
}

/* Interface for Android Platform */
struct timed_vibrator_data android_vibrator_data = {
	.dev.name = "vibrator",
	.dev.enable = vibrator_enable,
	.dev.get_time = vibrator_get_time,
	.max_timeout = 30000, /* max time for vibrator enable 30 sec. */
};

static ssize_t vibrator_amp_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    int gain = atomic_read(&vibe_gain);
    return sprintf(buf, "%d\n", gain);
}

static ssize_t vibrator_amp_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
    int gain;

    sscanf(buf, "%d", &gain);
	if (gain > 64 || gain < 0) {
		printk(KERN_ERR "%s invalid value: should be 0 ~ +63\n", __FUNCTION__);
		return -EINVAL;
	}
    atomic_set(&vibe_gain, gain);

    return size;
}
static DEVICE_ATTR(amp, S_IRUGO | S_IWUSR, vibrator_amp_show, vibrator_amp_store);

static int android_vibrator_probe(struct platform_device *pdev)
{
	int ret = 0;

	vibe_data = (struct lge_vibrator_platform_data *)pdev->dev.platform_data;
	atomic_set(&vibe_gain, vibe_data->amp_value);
	vibe_data->pwm_set(1, 32);

	if (android_vibrator_intialize() < 0) {
		printk(KERN_ERR "Android Vibrator Initialization was failed\n");
		return -1;
	}

	android_vibrator_force_set(0); /* disable vibrator */

	hrtimer_init(&android_vibrator_data.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	android_vibrator_data.timer.function = vibrator_timer_func;
	INIT_WORK(&android_vibrator_data.vib_work, vib_work_func);
	spin_lock_init(&android_vibrator_data.lock);

	ret = timed_output_dev_register(&android_vibrator_data.dev);
	if (ret < 0) {
		timed_output_dev_unregister(&android_vibrator_data.dev);
		return -ENODEV;
	}

	ret = device_create_file(android_vibrator_data.dev.dev, &dev_attr_amp);
	if (ret < 0) {
		timed_output_dev_unregister(&android_vibrator_data.dev);
		device_remove_file(android_vibrator_data.dev.dev, &dev_attr_amp);
		return -ENODEV;
	}

	printk(KERN_INFO "LGE: Android Vibrator Initialization was done\n");
	return 0;
}

static int android_vibrator_remove(struct platform_device *dev)
{
	vibe_data->power_set(0);
	vibe_data->ic_enable_set(0);
	vibe_data->pwm_set(0, 0);
	timed_output_dev_unregister(&android_vibrator_data.dev);

	return 0;
}

static void android_vibrator_shutdown(struct platform_device *pdev)
{
	android_vibrator_force_set(0);
	vibe_data->power_set(0);
}

static struct platform_driver android_vibrator_driver = {
	.probe = android_vibrator_probe,
	.remove = android_vibrator_remove,
	.shutdown = android_vibrator_shutdown,
	.suspend = NULL,
	.resume = NULL,
	.driver = {
		.name = "android-vibrator",
	},
};

static int __init android_vibrator_init(void)
{
	printk(KERN_INFO "LGE: ktb Android Vibrator Driver Init\n");
	return platform_driver_register(&android_vibrator_driver);
}

static void __exit android_vibrator_exit(void)
{
	printk(KERN_INFO "LGE: Android Vibrator Driver Exit\n");
	platform_driver_unregister(&android_vibrator_driver);
}

module_init(android_vibrator_init);
module_exit(android_vibrator_exit);

MODULE_AUTHOR("LG Electronics Inc.");
MODULE_DESCRIPTION("Android Common Vibrator Driver");
MODULE_LICENSE("GPL");

