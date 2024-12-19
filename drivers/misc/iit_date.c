/*
 *           IIT-date Linux driver.
 *
 * Copyright (c) October 2024 
 * Istituto Italiano di Tecnologia
 * Electronic Design Laboratory
 *
 */

#include <asm/io.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <linux/types.h>

/* names */
#define IITDATE_NAME "iit-date"
#define IITDATE_DRIVER_NAME IITDATE_NAME"-driver"

/* registers */
#define IITDATE_BITSTREAM		0x0

#define IITDATE_DAY_MSK			0xF8000000
#define IITDATE_DAY_SH		  	(27)
#define IITDATE_MONTH_MSK		0x07800000
#define IITDATE_MONTH_SH	  	(23)
#define IITDATE_YEAR_MSK		0x007E0000
#define IITDATE_YEAR_SH		  	(17)
#define IITDATE_HOUR_MSK		0x0001F000
#define IITDATE_HOUR_SH		  	(12)
#define IITDATE_MINUTES_MSK		0x00000FC0
#define IITDATE_MINUTES_SH		(6)
#define IITDATE_SECONDS_MSK		0x0000003F
#define IITDATE_SECONDS_SH		(0)


static struct debugfs_reg32 iitdate_regs[] = {
	{"IITDATE_BITSTREAM",			0x00},
};

struct iitdate_device {
	struct platform_device *pdev;
	void __iomem *regs;
	struct dentry *debugfsdir;

	unsigned long day;
	unsigned long month;
	unsigned long year;
	unsigned long hour;
	unsigned long minutes;
	unsigned long seconds;
	char timestamp[128];
	
};

static struct dentry *iitdate_debugfsdir = NULL;

static u32 iitdate_reg_read(struct iitdate_device *iitdate, int offs)
{
	u32 val;
	val = readl(iitdate->regs + offs);
	return val;
}

void ttm_parse_date (char * str, u32 date) 
{
	unsigned long day;
	unsigned long month;
	unsigned long year;
	unsigned long hour;
	unsigned long minutes;
	unsigned long seconds;

	day   		=  (date & IITDATE_DAY_MSK)     >> IITDATE_DAY_SH;
	month 		=  (date & IITDATE_MONTH_MSK)   >> IITDATE_MONTH_SH;
	year  		= ((date & IITDATE_YEAR_MSK)    >> IITDATE_YEAR_SH) + 2000;
	hour  		=  (date & IITDATE_HOUR_MSK)    >> IITDATE_HOUR_SH;
	minutes		=  (date & IITDATE_MINUTES_MSK) >> IITDATE_MINUTES_SH;
	seconds 	=  (date & IITDATE_SECONDS_MSK) >> IITDATE_SECONDS_SH; // Seconds

	sprintf(str, "FPGA bitstream: %ld/%ld/%ld @ %ld:%02ld:%02ld\n", day, month, year,
												hour, minutes, seconds);
}

static int debugfs_ttm_show_regset32(struct seq_file *s, void *data)
{	
	u32 bitstream_stamp;

	struct iitdate_device *iitdate  = s->private;
	bitstream_stamp = iitdate_reg_read(iitdate, IITDATE_BITSTREAM);
	ttm_parse_date(iitdate->timestamp, bitstream_stamp);

	seq_printf(s, "%s", iitdate->timestamp);

	return 0;
}

static int debugfs_ttm_open_regset32(struct inode *inode, struct file *file)
{
	return single_open(file, debugfs_ttm_show_regset32, inode->i_private);
}

static const struct file_operations ttm_fops_regset32 = {
	.open =		debugfs_ttm_open_regset32,
	.read =		seq_read,
	.llseek =	seq_lseek,
	.release =	single_release,
};

static int iitdate_probe(struct platform_device *pdev)
{
	struct iitdate_device *iitdate;
	struct resource *res;
	struct debugfs_regset32 *regset;
	u32 bitstream_stamp;
	char buf[128];

	dev_dbg(&pdev->dev, "Probing iit_date\n");
	iitdate_debugfsdir = debugfs_create_dir("iitdate", NULL);

	iitdate = devm_kzalloc(&pdev->dev, sizeof(*iitdate), GFP_KERNEL);
	if (!iitdate) {
		dev_err(&pdev->dev, "Can't alloc iitdate mem\n");
		return -ENOMEM;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	iitdate->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(iitdate->regs)) {
		dev_err(&pdev->dev, "IIT Date has no regs in DT\n");
		kfree(iitdate);
		return PTR_ERR(iitdate->regs);
	}

	iitdate->pdev = pdev;

	bitstream_stamp = iitdate_reg_read(iitdate, IITDATE_BITSTREAM);
	ttm_parse_date(iitdate->timestamp, bitstream_stamp);
	dev_info(&pdev->dev, "%s\n", iitdate->timestamp);

	if (iitdate_debugfsdir) {
		sprintf(buf, "iit-date.%llx", ( long long unsigned int) res->start);
		iitdate->debugfsdir = debugfs_create_dir(buf, iitdate_debugfsdir);
	}

	if (iitdate->debugfsdir) {
		regset = devm_kzalloc(&pdev->dev, sizeof(*regset), GFP_KERNEL);
		if (!regset)
			return 0;
		regset->regs = iitdate_regs;
		regset->nregs = ARRAY_SIZE(iitdate_regs);
		regset->base = iitdate->regs;
		debugfs_create_regset32("regdump", 0444, iitdate->debugfsdir, regset);
		debugfs_create_file("timestamp", 0444, iitdate->debugfsdir, iitdate, &ttm_fops_regset32);
	}

	platform_set_drvdata(pdev, iitdate);

	return 0;
}

static int iitdate_remove(struct platform_device *pdev)
{
	debugfs_remove_recursive(iitdate_debugfsdir);
	return 0;
}

static struct of_device_id iitdate_of_match[] = {
	{.compatible = "iit,date-1.0",},
	{}
};

MODULE_DEVICE_TABLE(of, iitdate_of_match);

static struct platform_driver iitdate_driver = {
	.probe = iitdate_probe,
	.remove = iitdate_remove,
	.driver = {
		.name = IITDATE_DRIVER_NAME,
		.of_match_table = iitdate_of_match,
	},
};

module_platform_driver(iitdate_driver);

MODULE_ALIAS("platform:iit-date");
MODULE_DESCRIPTION("IIT Date driver");
MODULE_AUTHOR("Francesco Diotalevi <francesco.diotalevi@iit.it>");
MODULE_LICENSE("GPL v2");

