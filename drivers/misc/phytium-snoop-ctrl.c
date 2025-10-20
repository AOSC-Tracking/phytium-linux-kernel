// SPDX-License-Identifier: GPL-2.0
/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * Provides a simple driver to control the PHYTIUM snoop ctrl interface which
 * allows the BMC to listen on and save the data written by
 * the host to an arbitrary I/O port.
 *
 * Typically used by the BMC to "watch" host boot progress via port
 * 0x80 writes made by the BIOS during the boot process.
 *
 * Copyright (c) 2019-2023, Phytium Technology Co., Ltd.
 *
 */

#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/kfifo.h>
#include <linux/mfd/syscon.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/regmap.h>

#define DEVICE_NAME	"phytium-snoop-ctrl"

#define NUM_SNOOP_CHANNELS 2
#define SNOOP_FIFO_SIZE 2048

#define snp_enable_reg			0x150
#define snp_enable_reg_snp1_en		BIT(0)
#define snp_enable_reg_snp1_int_en	BIT(1)
#define snp_enable_reg_snp2_en		BIT(2)
#define snp_enable_reg_snp2_int_en	BIT(3)

#define snp_status_reg			0x154
#define snp_status_reg_snp1_int		BIT(0)
#define snp_status_reg_snp2_int		BIT(1)

#define snp_addr_reg			0x158
#define snp_addr_reg_snp1_addr		GENMASK(15, 0)
#define snp_addr_reg_snp1_shift		0
#define snp_addr_reg_snp2_addr		GENMASK(31, 16)
#define snp_addr_reg_snp2_shift		16

#define snp_data_reg			0x15c
#define snp_data_reg_snp1_data_reg	GENMASK(7, 0)
#define snp_data_reg_snp1_shift		0
#define snp_data_reg_snp2_data_reg	GENMASK(15, 8)
#define snp_data_reg_snp2_shift		8

#define SNOOP_DRIVER_VERSION "1.1.1"

struct phytium_snoop_ctrl_channel {
	struct kfifo		fifo;
	wait_queue_head_t	wq;
	struct miscdevice	miscdev;
};

struct phytium_snoop_ctrl {
	struct regmap		*regmap;
	int			irq;
	struct phytium_snoop_ctrl_channel chan[NUM_SNOOP_CHANNELS];
};

static struct phytium_snoop_ctrl_channel *snoop_file_to_chan(struct file *file)
{
	return container_of(file->private_data,
			    struct phytium_snoop_ctrl_channel,
			    miscdev);
}

static ssize_t snoop_file_read(struct file *file, char __user *buffer,
				size_t count, loff_t *ppos)
{
	struct phytium_snoop_ctrl_channel *chan = snoop_file_to_chan(file);
	unsigned int copied;
	int ret = 0;

	if (kfifo_is_empty(&chan->fifo)) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(chan->wq,
				!kfifo_is_empty(&chan->fifo));
		if (ret == -ERESTARTSYS)
			return -EINTR;
	}
	ret = kfifo_to_user(&chan->fifo, buffer, count, &copied);

	return ret ? ret : copied;
}

static unsigned int snoop_file_poll(struct file *file,
				    struct poll_table_struct *pt)
{
	struct phytium_snoop_ctrl_channel *chan = snoop_file_to_chan(file);

	poll_wait(file, &chan->wq, pt);
	return !kfifo_is_empty(&chan->fifo) ? POLLIN : 0;
}

static const struct file_operations snoop_fops = {
	.owner  = THIS_MODULE,
	.read   = snoop_file_read,
	.poll   = snoop_file_poll,
	.llseek = noop_llseek,
};

/* Save a byte to a FIFO and discard the oldest byte if FIFO is full */
static void put_fifo_with_discard(struct phytium_snoop_ctrl_channel *chan, u8 val)
{
	if (!kfifo_initialized(&chan->fifo))
		return;
	if (kfifo_is_full(&chan->fifo))
		kfifo_skip(&chan->fifo);
	kfifo_put(&chan->fifo, val);
	wake_up_interruptible(&chan->wq);
}

static irqreturn_t phytium_snoop_ctrl_irq(int irq, void *arg)
{
	struct phytium_snoop_ctrl *snoop_ctrl = arg;
	u32 reg, data;

	if (regmap_read(snoop_ctrl->regmap, snp_status_reg, &reg))
		return IRQ_NONE;

	/* Check if one of the snoop channels is interrupting */
	reg &= (snp_status_reg_snp1_int | snp_status_reg_snp2_int);
	if (!reg)
		return IRQ_NONE;

	/* Ack pending IRQs */
	regmap_write(snoop_ctrl->regmap, snp_status_reg, reg);

	/* Read and save most recent snoop'ed data byte to FIFO */
	regmap_read(snoop_ctrl->regmap, snp_data_reg, &data);

	if (reg & snp_status_reg_snp1_int) {
		u8 val = (data & snp_data_reg_snp1_data_reg) >> snp_data_reg_snp1_shift;

		put_fifo_with_discard(&snoop_ctrl->chan[0], val);
	}
	if (reg & snp_status_reg_snp2_int) {
		u8 val = (data & snp_data_reg_snp2_data_reg) >> snp_data_reg_snp2_shift;

		put_fifo_with_discard(&snoop_ctrl->chan[1], val);
	}

	return IRQ_HANDLED;
}

static int phytium_snoop_ctrl_config_irq(struct phytium_snoop_ctrl *snoop_ctrl,
				       struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int rc;

	snoop_ctrl->irq = platform_get_irq(pdev, 0);
	if (!snoop_ctrl->irq)
		return -ENODEV;

	rc = devm_request_irq(dev, snoop_ctrl->irq,
			      phytium_snoop_ctrl_irq, IRQF_SHARED,
			      DEVICE_NAME, snoop_ctrl);
	if (rc < 0) {
		dev_warn(dev, "Unable to request IRQ %d\n", snoop_ctrl->irq);
		snoop_ctrl->irq = 0;
		return rc;
	}

	return 0;
}

static int phytium_enable_snoop(struct phytium_snoop_ctrl *snoop_ctrl,
				   struct device *dev,
				   int channel, u16 snoop_port)
{
	int rc = 0;
	u32 snp_enable_reg_en, snp_addr_reg_mask, snp_addr_reg_shift;

	init_waitqueue_head(&snoop_ctrl->chan[channel].wq);
	/* Create FIFO datastructure */
	rc = kfifo_alloc(&snoop_ctrl->chan[channel].fifo,
			 SNOOP_FIFO_SIZE, GFP_KERNEL);
	if (rc)
		return rc;

	snoop_ctrl->chan[channel].miscdev.minor = MISC_DYNAMIC_MINOR;
	snoop_ctrl->chan[channel].miscdev.name =
		devm_kasprintf(dev, GFP_KERNEL, "%s%d", DEVICE_NAME, channel);
	snoop_ctrl->chan[channel].miscdev.fops = &snoop_fops;
	snoop_ctrl->chan[channel].miscdev.parent = dev;
	rc = misc_register(&snoop_ctrl->chan[channel].miscdev);
	if (rc)
		return rc;

	/* Enable snoop channel at requested port */
	switch (channel) {
	case 0:
		snp_enable_reg_en = snp_enable_reg_snp1_en | snp_enable_reg_snp1_int_en;
		snp_addr_reg_mask = snp_addr_reg_snp1_addr;
		snp_addr_reg_shift = snp_addr_reg_snp1_shift;
		break;
	case 1:
		snp_enable_reg_en = snp_enable_reg_snp2_en | snp_enable_reg_snp2_int_en;
		snp_addr_reg_mask = snp_addr_reg_snp2_addr;
		snp_addr_reg_shift = snp_addr_reg_snp2_shift;
		break;
	default:
		return -EINVAL;
	}

	regmap_update_bits(snoop_ctrl->regmap,
			snp_enable_reg, snp_enable_reg_en, snp_enable_reg_en);
	regmap_update_bits(snoop_ctrl->regmap, snp_addr_reg, snp_addr_reg_mask,
			snoop_port << snp_addr_reg_shift);
	return rc;
}

static void phytium_disable_snoop(struct phytium_snoop_ctrl *snoop_ctrl,
				     int channel)
{
	switch (channel) {
	case 0:
		regmap_update_bits(snoop_ctrl->regmap, snp_enable_reg,
				   snp_enable_reg_snp1_en | snp_enable_reg_snp1_int_en,
				   0);
		break;
	case 1:
		regmap_update_bits(snoop_ctrl->regmap, snp_enable_reg,
				   snp_enable_reg_snp2_en | snp_enable_reg_snp2_int_en,
				   0);
		break;
	default:
		return;
	}

	kfifo_free(&snoop_ctrl->chan[channel].fifo);
	misc_deregister(&snoop_ctrl->chan[channel].miscdev);
}

static int phytium_snoop_ctrl_probe(struct platform_device *pdev)
{
	struct phytium_snoop_ctrl *snoop_ctrl;
	struct device *dev;
	u32 port;
	int rc;

	dev = &pdev->dev;

	snoop_ctrl = devm_kzalloc(dev, sizeof(*snoop_ctrl), GFP_KERNEL);
	if (!snoop_ctrl)
		return -ENOMEM;

	snoop_ctrl->regmap = syscon_node_to_regmap(
			pdev->dev.of_node);
	if (IS_ERR(snoop_ctrl->regmap)) {
		dev_err(dev, "Couldn't get regmap\n");
		return -ENODEV;
	}

	dev_set_drvdata(&pdev->dev, snoop_ctrl);

	rc = of_property_read_u32_index(dev->of_node, "snoop-ports", 0, &port);
	if (rc) {
		dev_err(dev, "no snoop ports configured\n");
		return -ENODEV;
	}

	regmap_update_bits(snoop_ctrl->regmap, snp_enable_reg,
			snp_enable_reg_snp1_en | snp_enable_reg_snp1_int_en, 0);

	regmap_update_bits(snoop_ctrl->regmap, snp_enable_reg,
			snp_enable_reg_snp2_en | snp_enable_reg_snp2_int_en, 0);

	rc = phytium_snoop_ctrl_config_irq(snoop_ctrl, pdev);
	if (rc)
		return rc;

	rc = phytium_enable_snoop(snoop_ctrl, dev, 0, port);
	if (rc)
		return rc;

	/* Configuration of 2nd snoop channel port is optional */
	if (of_property_read_u32_index(dev->of_node, "snoop-ports",
				       1, &port) == 0) {
		rc = phytium_enable_snoop(snoop_ctrl, dev, 1, port);
		if (rc)
			phytium_disable_snoop(snoop_ctrl, 0);
	}

	return rc;
}

static int phytium_snoop_ctrl_remove(struct platform_device *pdev)
{
	struct phytium_snoop_ctrl *snoop_ctrl = dev_get_drvdata(&pdev->dev);

	/* Disable both snoop channels */
	phytium_disable_snoop(snoop_ctrl, 0);
	phytium_disable_snoop(snoop_ctrl, 1);

	return 0;
}


static const struct of_device_id phytium_snoop_ctrl_match[] = {
	{ .compatible = "phytium,snoop-ctrl"},
	{ },
};

static struct platform_driver phytium_snoop_ctrl_driver = {
	.driver = {
		.name		= DEVICE_NAME,
		.of_match_table = phytium_snoop_ctrl_match,
	},
	.probe = phytium_snoop_ctrl_probe,
	.remove = phytium_snoop_ctrl_remove,
};

module_platform_driver(phytium_snoop_ctrl_driver);

MODULE_DEVICE_TABLE(of, phytium_snoop_ctrl_match);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lan Hengyu lanhengyu1395@phytium.com.cn");
MODULE_DESCRIPTION("Driver to control Phytium snoop controller functionality");
MODULE_VERSION(SNOOP_DRIVER_VERSION);
