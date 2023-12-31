/*
 * Andestech SPI controller driver
 *
 * Author: Nylon Chen
 *	nylon7@andestech.com
 *
 * 2020 (c) Andes Technology Corporation

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/io.h>
#include <linux/spi/spi.h>
#include <linux/of.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>

#define SPI_XFER_BEGIN		(1 << 0)
#define SPI_XFER_END		(1 << 1)
#define SPI_XFER_ONCE		(SPI_XFER_BEGIN | SPI_XFER_END)
#define SPI_XFER_SHIFT		0

#define SPI_NAME		"atcspi200"
#define SPI_MAX_HZ		50000000
#define MAX_TRANSFER_LEN	512
#define CHUNK_SIZE		1
#define SPI_TIMEOUT		0x100000
#define NSPI_MAX_CS_NUM		1
#define DATA_LENGTH(x)		((x-1)<<8)

/* SPI Transfer Control Register  */
#define ATCSPI200_TRANSFMT_OFFSET 		24
#define ATCSPI200_TRANSFMT_MASK			(0x0F<<ATCSPI200_TRANSFMT_OFFSET)
#define ATCSPI200_TRANSMODE_WR_SYNC 		(0<<ATCSPI200_TRANSFMT_OFFSET)
#define ATCSPI200_TRANSMODE_W_ONLY      	(1<<ATCSPI200_TRANSFMT_OFFSET)
#define ATCSPI200_TRANSMODE_R_ONLY      	(2<<ATCSPI200_TRANSFMT_OFFSET)
#define ATCSPI200_TRANSMODE_WR      		(3<<ATCSPI200_TRANSFMT_OFFSET)
#define ATCSPI200_TRANSCTRL_WRTRANCNT_OFFSET	12
#define ATCSPI200_TRANSCTRL_WRTRANCNT_MASK	(0x1FF<<ATCSPI200_TRANSCTRL_WRTRANCNT_OFFSET)
#define ATCSPI200_TRANSCTRL_RDTRANCNT_OFFSET	0
#define ATCSPI200_TRANSCTRL_RDTRANCNT_MASK	(0x1FF<<ATCSPI200_TRANSCTRL_RDTRANCNT_OFFSET)

/* SPI Control Register */
#define ATCSPI200_CTRL_TXFIFORST_MASK		(1<<2)
#define ATCSPI200_CTRL_RXFIFORST_MASK		(1<<1)
#define ATCSPI200_CTRL_SPIRST_MASK		(1<<0)

/* SPI Transfer Format Register */
#define ATCSPI200_TRANSFMT_CPHA_MASK		(1UL << 0)
#define ATCSPI200_TRANSFMT_CPOL_MASK		(1UL << 1)

/* SPI Status Register */
#define ATCSPI200_STATUS_TXEMPTY_OFFSET		(1<<22)
#define ATCSPI200_STATUS_RXEMPTY_OFFSET		(1<<14)
#define ATCSPI200_STATUS_TXNUM_LOWER_OFFSET	(16)
#define ATCSPI200_STATUS_TXNUM_LOWER_MASK	(0x1F<<ATCSPI200_STATUS_TXNUM_LOWER_OFFSET)
#define ATCSPI200_STATUS_RXNUM_LOWER_OFFSET	(8)
#define ATCSPI200_STATUS_RXNUM_LOWER_MASK	(0x1F<<ATCSPI200_STATUS_RXNUM_LOWER_OFFSET)
#define ATCSPI200_STATUS_SPIACTIVE_MASK		(1<<0)

/* SPI Interface timing Setting */
#define ATCSPI200_TIMING_SCLK_DIV_MASK		0xFF

/* ATCSPI200 registers  */
#define IDRev		0x00	// ID and Revision Register
#define TransFmt  	0x10    // SPI Transfer Format Register
#define DirectIO	0x14	// SPI Direct IO Control Register
#define TransCtrl	0x20	// SPI Transfer Control Register
#define Cmd		0x24	// SPI Command Register
#define Addr		0x28	// SPI Address Register
#define Data		0x2C	// SPI Data Register
#define Ctrl		0x30	// SPI Control Register
#define Status		0x34	// SPI Status Register
#define IntrEn		0x38	// SPI Interrupt Enable Register
#define IntrSt		0x3C	// SPI Interrupt Status Registe
#define Timing		0x40	// SPI Interface timing Register

struct atcspi200_spi {
	void __iomem	*regs;
	struct clk	*clk;
	size_t		trans_len;
	size_t		data_len;
	size_t		cmd_len;
	u32		clk_rate;
	u8		cmd_buf[16];
	u8		*din;
	u8		*dout;
	unsigned int	max_transfer_length;
	unsigned int	freq;
	unsigned int	mode;
	unsigned int	mtiming;
	int		timeout;
	spinlock_t	lock;
};

static void atcspi200_spi_write(struct atcspi200_spi *spi, int offset, u32 value)
{
	iowrite32(value, spi->regs + offset);
}

static u32 atcspi200_spi_read(struct atcspi200_spi *spi, int offset)
{
	return ioread32(spi->regs + offset);
}

static int atcspi200_spi_setup(struct atcspi200_spi *spi)
{
	unsigned int	format_val;
	u32	timing;
	u8	div;
	int ctrl_val = atcspi200_spi_read(spi,Ctrl);
	ctrl_val |= (ATCSPI200_CTRL_TXFIFORST_MASK|ATCSPI200_CTRL_RXFIFORST_MASK|ATCSPI200_CTRL_SPIRST_MASK);
	atcspi200_spi_write(spi,Ctrl,ctrl_val);
	while (((atcspi200_spi_read(spi,Ctrl))&(ATCSPI200_CTRL_TXFIFORST_MASK|ATCSPI200_CTRL_RXFIFORST_MASK|ATCSPI200_CTRL_SPIRST_MASK))&&(spi->timeout--))
		if (!spi->timeout)
			return -EINVAL;

	spi->cmd_len = 0;
	format_val = spi->mode|DATA_LENGTH(8);
	format_val |= ATCSPI200_TRANSFMT_CPHA_MASK;
	format_val |= ATCSPI200_TRANSFMT_CPOL_MASK;
	atcspi200_spi_write(spi,TransFmt,format_val);

	timing = atcspi200_spi_read(spi,Timing);
	timing &= ~ATCSPI200_TIMING_SCLK_DIV_MASK;

	if (spi->freq >= spi->clk_rate)
		div = ATCSPI200_TIMING_SCLK_DIV_MASK;
	else {
		for (div=0; div <ATCSPI200_TIMING_SCLK_DIV_MASK; div++) {
			if(spi->freq >= spi->clk_rate / (2 * (div + 1)))
				break;
		}
	}
	timing |= div;
	atcspi200_spi_write(spi,Timing,timing);
	return 0;
}

static int atcspi200_spi_stop(struct atcspi200_spi *spi)
{
	atcspi200_spi_write(spi,Timing,spi->mtiming);
	while ((atcspi200_spi_read(spi,Status) & ATCSPI200_STATUS_SPIACTIVE_MASK)&&(spi->timeout--))
		if (!spi->timeout)
			return -EINVAL;
	return 0;
}

static int atcspi200_spi_start(struct atcspi200_spi *spi,struct spi_transfer *t)
{
	int i,trans_len=0;
	int tc = atcspi200_spi_read(spi,TransCtrl);

	tc &= ~(ATCSPI200_TRANSCTRL_WRTRANCNT_MASK|ATCSPI200_TRANSCTRL_RDTRANCNT_MASK|ATCSPI200_TRANSFMT_MASK);
	if ((spi->dout)&&(spi->cmd_len))
		tc |= ATCSPI200_TRANSMODE_WR;
	else if (spi->dout)
		tc |= ATCSPI200_TRANSMODE_R_ONLY;
	else
		tc |= ATCSPI200_TRANSMODE_W_ONLY;

	if (spi->din)
		trans_len = spi->trans_len;
	tc |= (spi->cmd_len+trans_len-1) << ATCSPI200_TRANSCTRL_WRTRANCNT_OFFSET;

	if (spi->dout)
		tc |= (spi->trans_len-1) << ATCSPI200_TRANSCTRL_RDTRANCNT_OFFSET;

	atcspi200_spi_write(spi,TransCtrl,tc);
	atcspi200_spi_write(spi,Cmd,1);

	for (i=0;i<spi->cmd_len;i++)
		atcspi200_spi_write(spi,Data,spi->cmd_buf[i]);

	return 0;
}

static void atcspi200_spi_tx(struct atcspi200_spi *spi, const u8 *dout)
{
	atcspi200_spi_write(spi,Data,*dout);
}

static int atcspi200_spi_rx(struct atcspi200_spi *spi, u8 *din, unsigned int bytes)
{
	u32 tmp_data = atcspi200_spi_read(spi,Data);
	*din = tmp_data;
	return bytes;
}
static int atcspi200_spi_transfer(struct spi_device *atcspi200_spi, struct spi_transfer *t, unsigned long flags)
{
	struct atcspi200_spi *spi = spi_master_get_devdata(atcspi200_spi->master);

	unsigned int event, rx_bytes;
	const u8* dout = NULL;
        u8 *din = NULL;
        int num_blks, num_chunks, max_trans_len, trans_len;
        int num_bytes;
        u8 *cmd_buf = spi->cmd_buf;
        size_t cmd_len = spi->cmd_len;
        unsigned long data_len = t->len;
        int rf_cnt;
        int ret = 0;

        max_trans_len = spi->max_transfer_length;
	switch (flags) {
	case SPI_XFER_SHIFT:
		spi->cmd_len += t->len;
		return 0;
	case SPI_XFER_BEGIN:
		cmd_len = spi->cmd_len = data_len;
		memcpy(cmd_buf,t->tx_buf,cmd_len);
		return 0;
	case SPI_XFER_END:
		if(data_len ==0) {
			return 0;
		}
		spi->data_len =data_len;
		spi->din =(u8 *)t->tx_buf;
		spi->dout =(u8 *)t->rx_buf;
		break;

	case SPI_XFER_BEGIN | SPI_XFER_END:
		spi->data_len = 0;
		spi->din = 0;
		spi->dout=0;
		cmd_len = spi->cmd_len = data_len;
		memcpy(cmd_buf,t->tx_buf,cmd_len);
		t->tx_buf = 0;
		data_len = 0;
		atcspi200_spi_start(spi,t);
		break;
	}
	num_chunks = DIV_ROUND_UP(data_len, max_trans_len);
	din = t->rx_buf;
	dout = t->tx_buf;
	while(num_chunks--) {
		trans_len = min((size_t)data_len,(size_t)max_trans_len);
		spi->trans_len = trans_len;
		num_blks = DIV_ROUND_UP(trans_len , CHUNK_SIZE);
		num_bytes = (trans_len) % CHUNK_SIZE;
		if(num_bytes == 0)
			num_bytes = CHUNK_SIZE;
		atcspi200_spi_start(spi,t);
		while (num_blks) {
			event = atcspi200_spi_read(spi,Status);
			if ((event & ATCSPI200_STATUS_TXEMPTY_OFFSET ) && (t->tx_buf)) {
				atcspi200_spi_tx(spi,dout);
				num_blks -= CHUNK_SIZE;
				dout += CHUNK_SIZE;
			}
			if ((event&ATCSPI200_STATUS_RXNUM_LOWER_MASK)&&(t->rx_buf)) {
				rf_cnt = ((event & ATCSPI200_STATUS_RXNUM_LOWER_MASK)>> ATCSPI200_STATUS_RXNUM_LOWER_OFFSET);
				if (rf_cnt >= CHUNK_SIZE)
					rx_bytes = CHUNK_SIZE;
				else if(num_blks == 1 && rf_cnt == num_bytes)
					rx_bytes = num_bytes;
				else
					continue;

				if(atcspi200_spi_rx(spi,din,rx_bytes) == rx_bytes)
				{
					num_blks -= CHUNK_SIZE;
					din = (unsigned char*)din + rx_bytes;
				}
			}
		}
		data_len -= trans_len;
		if(data_len) {
			spi->cmd_buf[1] += ((trans_len>>16)&0xff);
			spi->cmd_buf[2] += ((trans_len>>8)&0xff);
			spi->cmd_buf[3] += ((trans_len)&0xff);
			spi->data_len = data_len;
		}
		ret = atcspi200_spi_stop(spi);
	}
	ret = atcspi200_spi_stop(spi);
	return ret;
}
static int atcspi200_spi_transfer_one_message(struct spi_master *master,struct spi_message *m)
{
	struct atcspi200_spi *spi = spi_master_get_devdata(master);
	struct spi_transfer *t;
	unsigned long spi_flags;
	unsigned long flags;
	int ret=0;

	m->actual_length = 0;

	spi_flags = SPI_XFER_BEGIN;
	list_for_each_entry(t, &m->transfers, transfer_list) {
		if (!t->tx_buf && !t->rx_buf)
			spi_flags |= SPI_XFER_ONCE;

		if (list_is_last(&t->transfer_list, &m->transfers))
			spi_flags |= SPI_XFER_END;

		spin_lock_irqsave(&spi->lock, flags);
		ret = atcspi200_spi_transfer(m->spi, t, spi_flags);
		spin_unlock_irqrestore(&spi->lock, flags);
		if (ret)
			break;
		m->actual_length += t->len;
		spi_flags = 0;
	}
	m->status = ret;
	spi_finalize_current_message(master);

	return 0;
}
static int atcspi200_spi_probe(struct platform_device *pdev)
{
	int (*read_fixup)(void __iomem *addr, unsigned int val,
		unsigned int shift_bits);
	struct resource *res;
	struct spi_master *master;
	struct atcspi200_spi *spi;
	int ret;
	u32 num_cs = NSPI_MAX_CS_NUM;

	master = spi_alloc_master(&pdev->dev, sizeof(struct atcspi200_spi));
	if (!master) {
		dev_err(&pdev->dev, "spi_allooc_master error\n");
		return -ENOMEM;
	}

	spi = spi_master_get_devdata(master);
	platform_set_drvdata(pdev, master);

	/* get base addr  */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	spi->regs = devm_ioremap_resource(&pdev->dev, res);

	if (IS_ERR(spi->regs)) {
		dev_err(&pdev->dev, "Unable to map IO resources\n");
		ret = PTR_ERR(spi->regs);
		goto put_master;
	}

	/* check ID and Revision register */
	read_fixup = symbol_get(readl_fixup);
	ret = read_fixup(spi->regs, 0x020020, 8);
	symbol_put(readl_fixup);
	if (!ret){
		dev_err(&pdev->dev, "Fail to read ID and Revision register, bitmap not support spi200\n");
		goto put_master;
	}

	spi->timeout = SPI_TIMEOUT;
	spi->max_transfer_length = MAX_TRANSFER_LEN;
	spi->mtiming = atcspi200_spi_read(spi,Timing);

	/* get clock */
	spi->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(spi->clk)) {
		dev_err(&pdev->dev, "Unable to find bus clock\n");
		ret = PTR_ERR(spi->clk);
		goto put_master;
	}

	/* Optional parameters */
	ret = of_property_read_u32(pdev->dev.of_node, "spi-max-frequency",&master->max_speed_hz);
	if(ret) {
		master->max_speed_hz = SPI_MAX_HZ; /* 50MHz */
		spi->freq = SPI_MAX_HZ;
	}

	/* Spin up the bus clock before hitting registers */
	ret = clk_prepare_enable(spi->clk);
	if(ret) {
		dev_err(&pdev->dev, "Unable to enable bus clock\n");
		goto put_master;
	}
	spi->clk_rate = clk_get_rate(spi->clk);
	if (!spi->clk_rate) {
		dev_err(&pdev->dev, "clk rate = 0\n");
		ret = -EINVAL;
		goto put_master;
	}
	/* probe the number of CS lines */
	ret = of_property_read_u32(pdev->dev.of_node, "num-cs",&num_cs);
	if (ret) {
		dev_err(&pdev->dev, "could not find num-cs\n");
		ret = -ENXIO;
		goto put_master;
	}
	if (num_cs > NSPI_MAX_CS_NUM) {
		dev_warn(&pdev->dev, "unsupported number of cs (%i), reducing to 1\n",num_cs);
		num_cs = NSPI_MAX_CS_NUM;
	}

	/* Define our master */
	master->bus_num = pdev->id;
	master->mode_bits = SPI_CPOL | SPI_CPHA;
	master->dev.of_node = pdev->dev.of_node;
	master->num_chipselect = num_cs;
	master->transfer_one_message = atcspi200_spi_transfer_one_message;

	/* Configure the SPI master hardware */
	atcspi200_spi_setup(spi);

	ret = devm_spi_register_master(&pdev->dev, master);
	if (ret < 0) {
		dev_err(&pdev->dev, "spi_register_master failed\n");
		goto put_master;
	}
	dev_info(&pdev->dev, "Andes SPI driver.\n");

	return 0;

put_master:
	spi_master_put(master);

	return ret;
}

static const struct of_device_id atcspi200_spi_of_match[] = {
	{ .compatible = "andestech,atcspi200", },
	{}
};
MODULE_DEVICE_TABLE(of, atcspi200_spi_of_match);

static struct platform_driver atcspi200_spi_driver = {
	.probe = atcspi200_spi_probe,
	.driver = {
		.name = SPI_NAME,
		.of_match_table = atcspi200_spi_of_match,
	},
};
module_platform_driver(atcspi200_spi_driver);

MODULE_AUTHOR("Nylon Chen. <nylon7@andestech.com>");
MODULE_DESCRIPTION("Andes SPI driver");
MODULE_LICENSE("GPL");
