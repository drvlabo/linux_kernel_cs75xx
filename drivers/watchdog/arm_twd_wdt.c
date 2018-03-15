/*
 * Copyright (c) Cortina-Systems Limited 2010.  All rights reserved.
 *
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
 *
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/miscdevice.h>
#include <linux/watchdog.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/of.h>
#include <asm/io.h>


#define OFFS_LOAD	0x00
#define OFFS_COUNTER	0x04
#define OFFS_CONTROL	0x08
#define OFFS_INTSTAT	0x0C
#define OFFS_RESETSTAT	0x10
#define OFFS_DISABLE	0x14


struct twd_wdt {
	unsigned long	timer_alive;
	struct device	*dev;
	void __iomem	*base;
	int		irq;
	unsigned int	perturb;
	char		expect_close;
	struct twd_wdt * __percpu *pwdt;
};

static struct platform_device *twd_wdt_dev;
static spinlock_t wdt_lock;
static unsigned int twd_wdt_timer_rate = (100*1024*1024);

#define TIMER_MARGIN	30
static int twd_wdt_margin = TIMER_MARGIN;
module_param(twd_wdt_margin, int, 0);
MODULE_PARM_DESC(twd_wdt_margin,
	"TWD WDT timer margin in seconds. (0 < twd_wdt_margin < 65536, default="
	__MODULE_STRING(TIMER_MARGIN) ")");

static int nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, int, 0);
MODULE_PARM_DESC(nowayout,
	"Watchdog cannot be stopped once started (default="
	__MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

#define ONLY_TESTING	0
static int twd_wdt_noboot = ONLY_TESTING;
module_param(twd_wdt_noboot, int, 0);
MODULE_PARM_DESC(twd_wdt_noboot, "TWD watchdog action, "
	"set to 1 to ignore reboots, 0 to reboot (default="
	__MODULE_STRING(ONLY_TESTING) ")");


/*
 *	This is the interrupt handler.  Note that we only use this
 *	in testing mode, so don't actually do a reboot here.
 */
static irqreturn_t twd_wdt_fire(int irq, void *arg)
{
	const struct twd_wdt *wdt = *(void **)arg;

	/* Check it really was our interrupt */
	if (readl(wdt->base + OFFS_INTSTAT)) {
		dev_printk(KERN_CRIT, wdt->dev,
			"Triggered - Reboot ignored.\n");
		/* Clear the interrupt on the watchdog */
		writel(1, wdt->base + OFFS_INTSTAT);
		return IRQ_HANDLED;
	}
	return IRQ_NONE;
}

/*
 *	twd_wdt_keepalive - reload the timer
 *
 *	Note that the spec says a DIFFERENT value must be written to the reload
 *	register each time.  The "perturb" variable deals with this by adding 1
 *	to the count every other time the function is called.
 */
static void twd_wdt_keepalive(void *info)
{
	struct twd_wdt *wdt = info;
	unsigned int count=0;

	/* printk("%s : cpu id = %d ......\n",__func__,get_cpu()); */

	/* Assume prescale is set to 256 */
	count = (twd_wdt_timer_rate / 256) * twd_wdt_margin;

	/* Reload the counter */
	writel(count + wdt->perturb, wdt->base + OFFS_LOAD);
	wdt->perturb = wdt->perturb ? 0 : 1;
}

static void twd_wdt_stop(void *info)
{
	struct twd_wdt *wdt = info;

	spin_lock(&wdt_lock);
	/* switch from watchdog mode to timer mode */
	writel(0x12345678, wdt->base + OFFS_DISABLE);
	writel(0x87654321, wdt->base + OFFS_DISABLE);
	/* watchdog is disabled */
	writel(0x0, wdt->base + OFFS_CONTROL);
	spin_unlock(&wdt_lock);
}

static void twd_wdt_start(void *info)
{
	struct twd_wdt* wdt = info;

	spin_lock(&wdt_lock);

	/* This loads the count register but does NOT start the count yet */
	twd_wdt_keepalive(wdt);

	if (twd_wdt_noboot) {
		/* Enable watchdog - prescale=256, watchdog mode=0, enable=1 */
		writel(0x0000FF01, wdt->base + OFFS_CONTROL);
	} else {
		/* Enable watchdog - prescale=256, watchdog mode=1, enable=1 */
		writel(0x0000FF09, wdt->base + OFFS_CONTROL);
	}
	spin_unlock(&wdt_lock);
}

static int twd_wdt_set_heartbeat(int t)
{

	if (t < 0x0001 || t > 0xFFFF)
		return -EINVAL;

	twd_wdt_margin = t;
	return 0;
}

/*
 *	/dev/watchdog handling
 */
static int twd_wdt_open(struct inode *inode, struct file *file)
{
	struct twd_wdt *wdt = platform_get_drvdata(twd_wdt_dev);


	if (test_and_set_bit(0, &wdt->timer_alive))
		return -EBUSY;

	if (nowayout)
		__module_get(THIS_MODULE);

	file->private_data = wdt;

	/*
	 *	Activate timer
	 */
   	smp_call_function(twd_wdt_start, wdt, 1);
	twd_wdt_start(wdt);

	return nonseekable_open(inode, file);
}

static int twd_wdt_release(struct inode *inode, struct file *file)
{
	struct twd_wdt *wdt = file->private_data;

	/*
	 *	Shut off the timer.
	 * 	Lock it in if it's a module and we set nowayout
	 */
	if (wdt->expect_close == 42) {
   		smp_call_function(twd_wdt_stop, wdt, 1);
		twd_wdt_stop(wdt);
	} else {
		dev_printk(KERN_CRIT, wdt->dev,
			"unexpected close, not stopping watchdog!\n");
   		smp_call_function(twd_wdt_keepalive, wdt, 1);
		twd_wdt_keepalive(wdt);
	}
	clear_bit(0, &wdt->timer_alive);
	wdt->expect_close = 0;
	return 0;
}

static ssize_t twd_wdt_write(struct file *file, const char *data,
	size_t len, loff_t *ppos)
{
	struct twd_wdt *wdt = file->private_data;

	/*
	 *	Refresh the timer.
	 */
	if (len) {
		if (!nowayout) {
			size_t i;

			/* In case it was set long ago */
			wdt->expect_close = 0;

			for (i = 0; i != len; i++) {
				char c;

				if (get_user(c, data + i))
					return -EFAULT;
				if (c == 'V')
					wdt->expect_close = 42;
			}
		}
   		smp_call_function(twd_wdt_keepalive, wdt, 1);
		twd_wdt_keepalive(wdt);
	}
	return len;
}

static struct watchdog_info ident = {
	.options		= WDIOF_SETTIMEOUT |
				  WDIOF_KEEPALIVEPING |
				  WDIOF_MAGICCLOSE,
	.identity		= "ARM TWD Watchdog",
};

static long twd_wdt_ioctl(struct file *file, unsigned int cmd,
	unsigned long arg)
{
	struct twd_wdt *wdt = file->private_data;
	int ret;
	union {
		struct watchdog_info ident;
		int i;
	} uarg;


	if (_IOC_DIR(cmd) && _IOC_SIZE(cmd) > sizeof(uarg))
		return -ENOTTY;

	if (_IOC_DIR(cmd) & _IOC_WRITE) {
		ret = copy_from_user(&uarg, (void __user *)arg, _IOC_SIZE(cmd));
		if (ret)
			return -EFAULT;
	}

	switch (cmd) {
	case WDIOC_GETSUPPORT:
		uarg.ident = ident;
		ret = 0;
		break;

	case WDIOC_GETSTATUS:
	case WDIOC_GETBOOTSTATUS:
		uarg.i = 0;
		ret = 0;
		break;

	case WDIOC_SETOPTIONS:
		ret = -EINVAL;
		if (uarg.i & WDIOS_DISABLECARD) {
        		smp_call_function(twd_wdt_stop, wdt, 1);
			twd_wdt_stop(wdt);
			ret = 0;
		}
		if (uarg.i & WDIOS_ENABLECARD) {
        		smp_call_function(twd_wdt_start, wdt, 1);
			twd_wdt_start(wdt);
			ret = 0;
		}
		break;

	case WDIOC_KEEPALIVE:
       		smp_call_function(twd_wdt_keepalive, wdt, 1);
        	twd_wdt_keepalive(wdt);
		ret = 0;
		break;

	case WDIOC_SETTIMEOUT:
		ret = twd_wdt_set_heartbeat(uarg.i);
		if (ret)
			break;

        	smp_call_function(twd_wdt_keepalive, wdt, 1);
		twd_wdt_keepalive(wdt);
		break;
		
	case WDIOC_GETTIMEOUT:
		uarg.i = twd_wdt_margin;
		ret = 0;
		break;

	default:
		return -ENOTTY;
	}

	if (ret == 0 && _IOC_DIR(cmd) & _IOC_READ) {
		ret = copy_to_user((void __user *)arg, &uarg, _IOC_SIZE(cmd));
		if (ret)
			ret = -EFAULT;
	}
	return ret;
}

/*
 *	System shutdown handler.  Turn off the watchdog if we're
 *	restarting or halting the system.
 */
static void twd_wdt_shutdown(struct platform_device *dev)
{
	struct twd_wdt *wdt = platform_get_drvdata(dev);

	if (system_state == SYSTEM_RESTART || system_state == SYSTEM_HALT) {
        	smp_call_function(twd_wdt_stop, wdt, 1);
		twd_wdt_stop(wdt);
	}
}

/*
 *	Kernel Interfaces
 */
static const struct file_operations twd_wdt_fops = {
	.owner			= THIS_MODULE,
	.llseek			= no_llseek,
	.write			= twd_wdt_write,
	.unlocked_ioctl		= twd_wdt_ioctl,
	.open			= twd_wdt_open,
	.release		= twd_wdt_release,
};

static struct miscdevice twd_wdt_miscdev = {
	.minor		= WATCHDOG_MINOR,
	.name		= "watchdog",
	.fops		= &twd_wdt_fops,
};

static int __init twd_wdt_probe(struct platform_device *dev)
{
	struct twd_wdt * __percpu *pwdt;
	struct twd_wdt *wdt;
	struct resource *res;
	int ret;
	int i;

	res = platform_get_resource(dev, IORESOURCE_MEM, 0);
	if (!res) {
		ret = -ENODEV;
		goto err_out;
	}

	pwdt = alloc_percpu(struct twd_wdt *);
	if (!pwdt) {
		ret = -ENOMEM;
		goto err_out;
	}

	wdt = kzalloc(sizeof(struct twd_wdt), GFP_KERNEL);
	if (!wdt) {
		ret = -ENOMEM;
		goto err_free_percpu;
	}

	wdt->pwdt = pwdt;
	wdt->dev = &dev->dev;
	wdt->irq = platform_get_irq(dev, 0);
	if (wdt->irq < 0) {
		ret = -ENXIO;
		goto err_free;
	}
	wdt->base = ioremap(res->start, res->end - res->start + 1);
	if (!wdt->base) {
		ret = -ENOMEM;
		goto err_free;
	}

	twd_wdt_miscdev.parent = &dev->dev;
	ret = misc_register(&twd_wdt_miscdev);
	if (ret) {
		dev_err(&dev->dev, "cannot register miscdev on minor=%d (%d)\n",
			WATCHDOG_MINOR, ret);
		goto err_misc;
	}

	for_each_possible_cpu(i)
		*per_cpu_ptr(pwdt, i) = wdt;

	ret = request_percpu_irq(wdt->irq, twd_wdt_fire, "twd_wdt", pwdt);
	if (ret) {
		dev_err(&dev->dev,
			"cannot register IRQ%d for watchdog\n", wdt->irq);
		goto err_irq;
	}

	printk("%s: base address =%x  IRQ=%d...\n",__func__,
		(unsigned int)wdt->base,wdt->irq);

	twd_wdt_stop(wdt);
	platform_set_drvdata(dev, wdt);
	twd_wdt_dev = dev;

	return 0;

err_irq:
	misc_deregister(&twd_wdt_miscdev);
err_misc:
	iounmap(wdt->base);
err_free:
	kfree(wdt);
err_free_percpu:
	free_percpu(pwdt);
err_out:
	return ret;
}

static int __exit twd_wdt_remove(struct platform_device *dev)
{
	struct twd_wdt *wdt = platform_get_drvdata(dev);
	struct twd_wdt * __percpu *pwdt = wdt->pwdt;

	platform_set_drvdata(dev, NULL);

	misc_deregister(&twd_wdt_miscdev);

	twd_wdt_dev = NULL;

	free_percpu_irq(wdt->irq, pwdt);
	iounmap(wdt->base);
	kfree(wdt);
	free_percpu(pwdt);
	return 0;
}

/* work with hotplug and coldplug */
MODULE_ALIAS("platform:arm_twd_wdt");

#ifdef CONFIG_OF
static const struct of_device_id twd_wdt_of_match_table[] = {
	{ .compatible = "arm,cortex-a9-twd-wdt" },
	{ .compatible = "arm,cortex-a5-twd-wdt" },
	{},
};
MODULE_DEVICE_TABLE(of,twd_wdt_of_match_table);
#endif

static struct platform_driver twd_wdt_driver = {
	.probe		= twd_wdt_probe,
	.remove		= __exit_p(twd_wdt_remove),
	.shutdown	= twd_wdt_shutdown,
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= "twd-wdt",
#ifdef CONFIG_OF
		.of_match_table = twd_wdt_of_match_table,
#endif
	},
};

static char banner[] __initdata = KERN_INFO "ARM TWD Watchdog Timer: 0.1. "
		"twd_wdt_noboot=%d twd_wdt_margin=%d sec (nowayout= %d)\n";

static int __init arm_twd_wdt_init(void)
{
	/*
	 * Check that the margin value is within it's range;
	 * if not reset to the default
	 */
	if (twd_wdt_set_heartbeat(twd_wdt_margin)) {
		twd_wdt_set_heartbeat(TIMER_MARGIN);
		printk(KERN_INFO "twd_wdt_margin value must be 0<twd_wdt_margin<65536, \
			 using %d\n",TIMER_MARGIN);
	}

	spin_lock_init(&wdt_lock);

	printk(banner, twd_wdt_noboot, twd_wdt_margin, nowayout);

	return platform_driver_register(&twd_wdt_driver);
}

static void __exit arm_twd_wdt_exit(void)
{
	platform_driver_unregister(&twd_wdt_driver);
}

module_init(arm_twd_wdt_init);
module_exit(arm_twd_wdt_exit);

MODULE_AUTHOR("Cortina-Systems Limited");
MODULE_DESCRIPTION("ARM TWD Watchdog Device Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS_MISCDEV(WATCHDOG_MINOR);
