// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2008-2017 Andes Technology Corporation

/*
 * Andes RTC Support
 *
 * Copyright (C) 2006, 2007, 2008  Paul Mundt
 * Copyright (C) 2006  Jamie Lenehan
 * Copyright (C) 2008  Angelo Castello
 * Copyright (C) 2008  Roy Lee
 *
 * Based on the old arch/sh/kernel/cpu/rtc.c by:
 *
 *  Copyright (C) 2000  Philipp Rumpf <prumpf@tux.org>
 *  Copyright (C) 1999  Tetsuya Okada & Niibe Yutaka
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/rtc.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/uaccess.h>

#define DRV_NAME		"atcrtc100"
#define RTC_REG(off)		\
		(*(volatile unsigned int *)(rtc->regbase + (off)))
#define RTC_ID			RTC_REG(0x00)	/* ID and Revision */
#define ID_OFF			12
#define ID_MSK			(0xfffff << ID_OFF)
#define ATCRTC100ID		(0x03011 << ID_OFF)
#define RTC_RSV			RTC_REG(0x4)	/* Reserve */

#define RTC_CNT			RTC_REG(0x10)	/* Counter */
#define RTC_ALM			RTC_REG(0x14)	/* Alarm */
#define DAY_OFF			17
#define DAY_MSK			(0x7fff)
#define HOR_OFF			12
#define HOR_MSK			(0x1f)
#define MIN_OFF			6
#define MIN_MSK			(0x3f)
#define SEC_OFF			0
#define SEC_MSK			(0x3f)
#define RTC_SECOND		\
			((RTC_CNT >> SEC_OFF) & SEC_MSK) /* RTC sec */
#define RTC_MINUTE		\
			((RTC_CNT >> MIN_OFF) & MIN_MSK) /* RTC min */
#define RTC_HOUR		\
			((RTC_CNT >> HOR_OFF) & HOR_MSK) /* RTC hour */
#define RTC_DAYS		\
			((RTC_CNT >> DAY_OFF) & DAY_MSK) /* RTC day */
#define RTC_ALM_SECOND		\
			((RTC_ALM >> SEC_OFF) & SEC_MSK) /* RTC alarm sec */
#define RTC_ALM_MINUTE		\
			((RTC_ALM >> MIN_OFF) & MIN_MSK) /* RTC alarm min */
#define RTC_ALM_HOUR		\
			((RTC_ALM >> HOR_OFF) & HOR_MSK) /* RTC alarm hour */
#define RTC_CR			RTC_REG(0x18)	/* Control */
#define RTC_EN			(0x1UL << 0)
#define ALARM_WAKEUP		(0x1UL << 1)
#define ALARM_INT		(0x1UL << 2)
#define DAY_INT			(0x1UL << 3)
#define HOR_INT			(0x1UL << 4)
#define MIN_INT			(0x1UL << 5)
#define SEC_INT			(0x1UL << 6)
#define HSEC_INT		(0x1UL << 7)
#define RTC_STA			RTC_REG(0x1c)	/* Status */
#define WD			(0x1UL << 16)

/* CHeck if day is configured as  15 */
#define CHECK_DAY_15	0

struct atc_rtc {
	void __iomem *regbase;
	struct resource *res;
	unsigned int alarm_irq;
	unsigned int interrupt_irq;
	struct rtc_device *rtc_dev;
	spinlock_t		lock;		/* Protects this structure */
};

struct atc_rtc rtc_platform_data;

static irqreturn_t rtc_interrupt(int irq, void *dev_id)
{
	struct atc_rtc *rtc = dev_id;

	if (RTC_STA & SEC_INT) {
		RTC_STA |= SEC_INT;
		rtc_update_irq(rtc->rtc_dev, 1, RTC_UF | RTC_IRQF);
		return IRQ_HANDLED;
	}
	return IRQ_NONE;
}

static irqreturn_t rtc_alarm(int irq, void *dev_id)
{
	struct atc_rtc *rtc = dev_id;

	if (RTC_STA & ALARM_INT) {
		RTC_CR &= ~ALARM_INT;
		RTC_STA |= ALARM_INT;
		rtc_update_irq(rtc->rtc_dev, 1, RTC_AF | RTC_IRQF);
		return IRQ_HANDLED;
	}
	return IRQ_NONE;
}

static int atc_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	struct atc_rtc *rtc = dev_get_drvdata(dev);

	spin_lock_irq(&rtc->lock);
	if (enabled)
		RTC_CR |= ALARM_INT;
	else
		RTC_CR &= ~ALARM_INT;
	spin_unlock_irq(&rtc->lock);
	return 0;
}

static int atc_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct atc_rtc *rtc = dev_get_drvdata(dev);
	unsigned long time = RTC_DAYS * 86400 + RTC_HOUR * 3600
				+ RTC_MINUTE * 60 + RTC_SECOND;

	rtc_time_to_tm(time, tm);
	if (rtc_valid_tm(tm) < 0) {
		dev_err(dev, "invalid date\n");
		rtc_time_to_tm(0, tm);
	}
	return 0;
}

static int atc_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct atc_rtc *rtc = dev_get_drvdata(dev);
	unsigned long time = 0;
	u32 cnt = 0;

	rtc_tm_to_time(tm, &time);
	cnt |= (((time / 86400) & DAY_MSK) << DAY_OFF);
	time %= 86400;
	cnt |= (((time / 3600) & HOR_MSK) << HOR_OFF);
	time %= 3600;
	cnt |= (((time / 60) & MIN_MSK) << MIN_OFF);
	time %= 60;
	cnt |= ((time & SEC_MSK) << SEC_OFF);

	spin_lock_irq(&rtc->lock);
	RTC_CNT = cnt;
	spin_unlock_irq(&rtc->lock);

	/* synchronization progress of RTC register updates */
	while ((RTC_STA & WD) != WD)
		continue;

	return 0;
}

static int atc_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *wkalrm)
{
	struct atc_rtc *rtc = dev_get_drvdata(dev);
	struct rtc_time *tm = &wkalrm->time;

	tm->tm_sec	= RTC_ALM_SECOND;
	tm->tm_min	= RTC_ALM_MINUTE;
	tm->tm_hour	= RTC_ALM_HOUR;
	wkalrm->enabled = (RTC_CR & ALARM_INT) ? 1 : 0;
	return 0;
}

static int atc_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *wkalrm)
{
	u32 alm = 0;
	struct atc_rtc *rtc = dev_get_drvdata(dev);
	struct rtc_time *tm = &wkalrm->time;
	int err = rtc_valid_tm(tm);

	if (err < 0) {
		dev_err(dev, "invalid alarm value\n");
		return err;
	}
	/* disable alarm interrupt and clear the alarm flag */
	RTC_CR &= ~ALARM_INT;
	/* set alarm time */
	alm |= ((tm->tm_sec & SEC_MSK) << SEC_OFF);
	alm |= ((tm->tm_min & MIN_MSK) << MIN_OFF);
	alm |= ((tm->tm_hour & HOR_MSK) << HOR_OFF);

	spin_lock_irq(&rtc->lock);
	RTC_ALM = alm;

	while ((RTC_STA & WD) != WD)
		continue;

	if (wkalrm->enabled)
		RTC_CR |= ALARM_INT;
	spin_unlock_irq(&rtc->lock);

	return 0;
}

const static struct rtc_class_ops rtc_ops = {
	.alarm_irq_enable = atc_alarm_irq_enable,
	.read_time	= atc_rtc_read_time,
	.set_time	= atc_rtc_set_time,
	.read_alarm	= atc_rtc_read_alarm,
	.set_alarm	= atc_rtc_set_alarm,
};

static int atc_rtc_probe(struct platform_device *pdev)
{
	int (*read_fixup)(void __iomem *addr, unsigned int val,
		unsigned int shift_bits);
	struct atc_rtc *rtc = &rtc_platform_data;
	int ret = -ENOENT;

	spin_lock_init(&rtc->lock);

	rtc->alarm_irq = platform_get_irq(pdev, 1);
	if (rtc->alarm_irq < 0)
		goto err_exit;

	rtc->interrupt_irq = platform_get_irq(pdev, 0);
	if (rtc->interrupt_irq < 0)
		goto err_exit;

	rtc->res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!rtc->res)
		goto err_exit;

	ret = -EBUSY;

	rtc->res = request_mem_region(rtc->res->start,
				      rtc->res->end - rtc->res->start + 1,
				      pdev->name);
	if (!rtc->res)
		goto err_request_region;

	ret = -EINVAL;

	rtc->regbase = ioremap_nocache(rtc->res->start,
				       rtc->res->end - rtc->res->start + 1);
	if (!rtc->regbase)
		goto err_ioremap1;

	/* Check ID and Revision register 0x030110*/
	read_fixup = symbol_get(readl_fixup);
	ret = read_fixup(rtc->regbase, 0x030110, 8);
	symbol_put(readl_fixup);
	if (!ret){
		dev_err(&pdev->dev, "failed read ID register, bitmap not support atcrtc100\n");
		return -ENOENT;
	}

	if ((RTC_ID & ID_MSK) != ATCRTC100ID)
		return -ENOENT;

#if CHECK_DAY_15
	RTC_CNT = DAY_MSK << DAY_OFF;
	while ((RTC_STA & WD) != WD)
		continue;

	if (DAY_MSK << DAY_OFF != RTC_CNT) {
		pr_err("rtc initialize fail\n");
		return -ENOENT;
	}
#endif
	platform_set_drvdata(pdev, rtc);

	if (of_property_read_bool(pdev->dev.of_node, "wakeup-source"))
		device_init_wakeup(&pdev->dev, true);

	rtc->rtc_dev = devm_rtc_allocate_device(&pdev->dev);
	rtc->rtc_dev->ops = &rtc_ops;

	if (IS_ERR(rtc->rtc_dev)) {
		ret = PTR_ERR(rtc->rtc_dev);
		goto err_unmap;
	}

	rtc->rtc_dev->max_user_freq = 256;
	rtc->rtc_dev->irq_freq = 1;

	ret = rtc_register_device(rtc->rtc_dev);
	if (ret)
		goto err_unmap;

	ret = request_irq(rtc->alarm_irq, rtc_alarm, 0,
			  "RTC Alarm : atcrtc100", rtc);
	if (ret)
		goto err_exit;

	ret = request_irq(rtc->interrupt_irq, rtc_interrupt,
			  0, "RTC Interrupt : atcrtc100", rtc);
	if (ret)
		goto err_interrupt_irq;

	RTC_CR |= RTC_EN;

	return 0;

err_unmap:
	iounmap(rtc->regbase);
err_ioremap1:
	release_resource(rtc->res);
err_request_region:
	free_irq(rtc->interrupt_irq, rtc);
err_interrupt_irq:
	free_irq(rtc->alarm_irq, rtc);
err_exit:
	return ret;
}

static int atc_rtc_remove(struct platform_device *pdev)
{
	struct atc_rtc *rtc = platform_get_drvdata(pdev);

	/*
	 * Because generic rtc will not execute rtc_device_release()
	 * when call rtc_device_unregister(),
	 * rtc id will increase when unloading a rtc driver.
	 * This can work around to recycle rtc id.
	 * But if kernel fix this issue, it shell be removed away.
	 */
	rtc->rtc_dev->dev.release(&rtc->rtc_dev->dev);
	RTC_CR &= ~(RTC_EN | SEC_INT | ALARM_INT);
	free_irq(rtc->alarm_irq, rtc);
	free_irq(rtc->interrupt_irq, rtc);
	iounmap(rtc->regbase);
	release_resource(rtc->res);
	platform_set_drvdata(pdev, NULL);
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int atc_rtc_resume(struct device *dev)
{
	struct atc_rtc *rtc_dd = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		disable_irq_wake(rtc_dd->alarm_irq);

	return 0;
}

static int atc_rtc_suspend(struct device *dev)
{
	struct atc_rtc *rtc_dd = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		enable_irq_wake(rtc_dd->alarm_irq);

	return 0;
}
#endif
static SIMPLE_DEV_PM_OPS(atc_rtc_pm_ops, atc_rtc_suspend, atc_rtc_resume);

#ifdef CONFIG_OF
static const struct of_device_id atc_rtc_dt_match[] = {
	{.compatible = "andestech,atcrtc100" },
	{},
};
MODULE_DEVICE_TABLE(of, atc_rtc_dt_match);
#endif

static struct platform_driver atc_rtc_platform_driver = {
	.driver		= {
		.name	= DRV_NAME,
		.owner	= THIS_MODULE,
		.of_match_table	= of_match_ptr(atc_rtc_dt_match),
		.pm = &atc_rtc_pm_ops,
	},
	.probe		= atc_rtc_probe,
	.remove		= atc_rtc_remove,
};

module_platform_driver(atc_rtc_platform_driver);
MODULE_DESCRIPTION("SuperH on-chip RTC driver");
MODULE_AUTHOR("Paul Mundt <lethal@linux-sh.org>, "
	      "Jamie Lenehan <lenehan@twibble.org>, "
	      "Angelo Castello <angelo.castello@st.com>, "
	      "Nick Hu <nickhu@andestech.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
