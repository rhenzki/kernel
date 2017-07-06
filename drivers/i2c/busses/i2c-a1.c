/* drivers/i2c/busses/i2c-msm.c
 *
 * Copyright (C) 2007 Google, Inc.
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

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <mach/system.h>
#include <mach/gpio.h>
#include <mach/board.h>
#include <linux/remote_spinlock.h>
#include <linux/pm_qos_params.h>

#define DEBUG 0
static int has_been_suspended = 0;
atomic_t during_suspend = ATOMIC_INIT(0);

enum {
	I2C_WRITE_DATA          = 0x00,
	I2C_CLK_CTL             = 0x04,
	I2C_STATUS              = 0x08,
	I2C_READ_DATA           = 0x0c,
	I2C_INTERFACE_SELECT    = 0x10,

	I2C_WRITE_DATA_DATA_BYTE            = 0xff,
	I2C_WRITE_DATA_ADDR_BYTE            = 1U << 8,
	I2C_WRITE_DATA_LAST_BYTE            = 1U << 9,

	I2C_CLK_CTL_FS_DIVIDER_VALUE        = 0xff,
	I2C_CLK_CTL_HS_DIVIDER_VALUE        = 7U << 8,

	I2C_STATUS_WR_BUFFER_FULL           = 1U << 0,
	I2C_STATUS_RD_BUFFER_FULL           = 1U << 1,
	I2C_STATUS_BUS_ERROR                = 1U << 2,
	I2C_STATUS_PACKET_NACKED            = 1U << 3,
	I2C_STATUS_ARB_LOST                 = 1U << 4,
	I2C_STATUS_INVALID_WRITE            = 1U << 5,
	I2C_STATUS_FAILED                   = 3U << 6,
	I2C_STATUS_BUS_ACTIVE               = 1U << 8,
	I2C_STATUS_BUS_MASTER               = 1U << 9,
	I2C_STATUS_ERROR_MASK               = 0xfc,

	I2C_INTERFACE_SELECT_INTF_SELECT    = 1U << 0,
	I2C_INTERFACE_SELECT_SCL            = 1U << 8,
	I2C_INTERFACE_SELECT_SDA            = 1U << 9,
};

struct msm_i2c_dev {
	struct device      *dev;
	void __iomem       *base;		/* virtual */
	int                 irq;
	struct clk         *clk;
	struct i2c_adapter  adapter;

	spinlock_t          lock;

	struct i2c_msg      *msg;
	int                 rem;
	int                 pos;
	int                 cnt;
	int                 ret;
	bool                need_flush;
	int                 flush_cnt;
	void                *complete;
	remote_spinlock_t   *smem_lock;
	int                 *mutex;
	int                 initiated;
};


static void i2c_get_mutex(struct msm_i2c_dev *dev);
static void i2c_release_mutex(struct msm_i2c_dev *dev);
void *smem_alloc(unsigned id, unsigned size);
static remote_spinlock_t i2c_smem_lock;
atomic_t finished = ATOMIC_INIT(1);

#if DEBUG
static void
dump_status(uint32_t status)
{
	printk("STATUS (0x%.8x): ", status);
	if (status & I2C_STATUS_BUS_MASTER)
		printk("MST ");
	if (status & I2C_STATUS_BUS_ACTIVE)
		printk("ACT ");
	if (status & I2C_STATUS_INVALID_WRITE)
		printk("INV_WR ");
	if (status & I2C_STATUS_ARB_LOST)
		printk("ARB_LST ");
	if (status & I2C_STATUS_PACKET_NACKED)
		printk("NAK ");
	if (status & I2C_STATUS_BUS_ERROR)
		printk("BUS_ERR ");
	if (status & I2C_STATUS_RD_BUFFER_FULL)
		printk("RD_FULL ");
	if (status & I2C_STATUS_WR_BUFFER_FULL)
		printk("WR_FULL ");
	if (status & I2C_STATUS_FAILED)
		printk("FAIL 0x%x", (status & I2C_STATUS_FAILED));
	printk("\n");
}
#endif

uint16_t get_address(struct msm_i2c_dev *dev)
{
	uint16_t addr;

	if(!dev->msg)
		return 0;
	else {
		addr = dev->msg->addr << 1;
		if (dev->msg->addr & I2C_M_RD)
			addr |= 1;
		return addr;
	}
}

static bool msm_i2c_fill_write_buffer(struct msm_i2c_dev *dev)
{
	uint16_t val;
	if (dev->pos < 0) {
		val = I2C_WRITE_DATA_ADDR_BYTE | dev->msg->addr << 1;
		if (dev->msg->flags & I2C_M_RD)
			val |= 1;
		if (dev->rem == 1 && dev->msg->len == 0)
			val |= I2C_WRITE_DATA_LAST_BYTE;
		writel(val, dev->base + I2C_WRITE_DATA);
		dev->pos++;
		return true;
	}

	if (dev->msg->flags & I2C_M_RD)
		return false;

	if (!dev->cnt)
		return false;

	/* Ready to take a byte */
	val = dev->msg->buf[dev->pos];
	if (dev->cnt == 1 && dev->rem == 1)
		val |= I2C_WRITE_DATA_LAST_BYTE;

	writel(val, dev->base + I2C_WRITE_DATA);
	dev->pos++;
	dev->cnt--;
	return true;
}

static void msm_i2c_read_buffer(struct msm_i2c_dev *dev)
{
	/*
	 * Theres something in the FIFO.
	 * Are we expecting data or flush crap?
	 */
	if ((dev->msg->flags & I2C_M_RD) && dev->pos >= 0 && dev->cnt) {
		switch (dev->cnt) {
		case 1:
			if (dev->pos != 0)
				break;
			dev->need_flush = true;
			/* fall-trough */
		case 2:
			writel(I2C_WRITE_DATA_LAST_BYTE,
			       dev->base + I2C_WRITE_DATA);
		}
		dev->msg->buf[dev->pos] = readl(dev->base + I2C_READ_DATA);
		dev->cnt--;
		dev->pos++;
	} else { /* FLUSH */
		if (dev->flush_cnt & 1) {
			/*
			* Stop requests are sometimes ignored, but writing
			* more than one request generates a write error.
			*/
			writel(I2C_WRITE_DATA_LAST_BYTE,
				dev->base + I2C_WRITE_DATA);
		}
		readl(dev->base + I2C_READ_DATA);
		if (dev->need_flush)
			dev->need_flush = false;
		else
			dev->flush_cnt++;
	}
}

static void msm_i2c_interrupt_locked(struct msm_i2c_dev *dev)
{
	uint32_t status	= readl(dev->base + I2C_STATUS);
	bool not_done = true;

#if DEBUG
	dump_status(status);
#endif
	if ((!dev->msg) || (!dev->complete)) {
		dev_err(dev->dev,
			"(0x%x) IRQ but nothing to do!, status %x\n",
			get_address(dev), status);
		return;
	}
	if (status & I2C_STATUS_ERROR_MASK) {
		printk("i2c error: STATUS_ERROR_MASK\n");
		goto out_err;
	}

	if (!(status & I2C_STATUS_WR_BUFFER_FULL))
		not_done = msm_i2c_fill_write_buffer(dev);
	if (status & I2C_STATUS_RD_BUFFER_FULL)
		msm_i2c_read_buffer(dev);

	if (dev->pos >= 0 && dev->cnt == 0) {
		if (dev->rem > 1) {
			dev->rem--;
			dev->msg++;
			dev->pos = -1;
			dev->cnt = dev->msg->len;
		} else if (!not_done && !dev->need_flush)
			goto out_complete;
	}
	return;

out_err:
	dev_err(dev->dev, "(0x%x) error, status %x\n", get_address(dev), status);
	dev->ret = -EIO;
out_complete:
	complete(dev->complete);
}

static irqreturn_t
msm_i2c_interrupt(int irq, void *devid)
{
	struct msm_i2c_dev *dev = devid;

	if (dev->mutex != NULL) {
		if (!dev->initiated) {
			return IRQ_HANDLED;
		}
	}
	pm_qos_update_requirement(PM_QOS_CPU_DMA_LATENCY, "msm_i2c", 501);

	spin_lock(&dev->lock);
	msm_i2c_interrupt_locked(dev);
	spin_unlock(&dev->lock);

	return IRQ_HANDLED;
}

static int
msm_i2c_poll_notbusy(struct msm_i2c_dev *dev)
{
	uint32_t retries = 0;
	uint32_t status;

	while (retries != 200) {
		status = readl(dev->base + I2C_STATUS);

		if (!(status & I2C_STATUS_BUS_ACTIVE)) {
			if (retries > 50)
				dev_warn(dev->dev,
					"(0x%x) Status(0x%x) Warning bus was busy (%d)\n",
					get_address(dev), status, retries);
			return 0;
		}
		if (has_been_suspended) {
			has_been_suspended = 0;
			return -ETIMEDOUT;
		}
		if (retries++ > 100)
			msleep(10);
	}
	dev_err(dev->dev, "(0x%x) Error waiting for notbusy status : 0x%x\n",
	    get_address(dev), status);
	return -ETIMEDOUT;
}

static int
msm_i2c_recover_bus_busy(struct msm_i2c_dev *dev)
{
	int i;
	uint32_t status = readl(dev->base + I2C_STATUS);
	int gpio_clk, gpio_dat;
	bool gpio_clk_status = false;

	if (!(status & (I2C_STATUS_BUS_ACTIVE | I2C_STATUS_WR_BUFFER_FULL)))
		return 0;

	msm_set_i2c_mux(true, &gpio_clk, &gpio_dat);

	if (status & I2C_STATUS_RD_BUFFER_FULL) {
		dev_warn(dev->dev, "(0x%x) Read buffer full, status %x, intf %x\n",
			 get_address(dev), status, readl(dev->base + I2C_INTERFACE_SELECT));
		writel(I2C_WRITE_DATA_LAST_BYTE, dev->base + I2C_WRITE_DATA);
		readl(dev->base + I2C_READ_DATA);
	}
	else if (status & I2C_STATUS_BUS_MASTER) {
		dev_warn(dev->dev, "(0x%x) Still the bus master, status %x, intf %x\n",
			 get_address(dev), status, readl(dev->base + I2C_INTERFACE_SELECT));
		writel(I2C_WRITE_DATA_LAST_BYTE | 0xff,
		       dev->base + I2C_WRITE_DATA);
	}

	for (i = 0; i < 9; i++) {
		if (gpio_get_value(gpio_dat) && gpio_clk_status)
			break;
		gpio_direction_output(gpio_clk, 0);
		udelay(5);
		gpio_direction_output(gpio_dat, 0);
		udelay(5);
		gpio_direction_input(gpio_clk);
		udelay(5);
		if (!gpio_get_value(gpio_clk))
			udelay(20);
		if (!gpio_get_value(gpio_clk))
			msleep(10);
		gpio_clk_status = gpio_get_value(gpio_clk);
		gpio_direction_input(gpio_dat);
		udelay(5);
	}
	msm_set_i2c_mux(false, NULL, NULL);
	udelay(10);

	status = readl(dev->base + I2C_STATUS);
	if (!(status & I2C_STATUS_BUS_ACTIVE)) {
		dev_info(dev->dev, "(0x%x) Bus busy cleared after %d clock cycles, "
			 "status %x, intf %x\n",
			 get_address(dev), i, status, readl(dev->base + I2C_INTERFACE_SELECT));
		return 0;
	}

	dev_warn(dev->dev, "(0x%x) Bus still busy, status %x, intf %x\n",
		 get_address(dev), status, readl(dev->base + I2C_INTERFACE_SELECT));
	return -EBUSY;
}


static int
msm_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg msgs[], int num)
{
	DECLARE_COMPLETION_ONSTACK(complete);
	struct msm_i2c_dev *dev = i2c_get_adapdata(adap);
	int ret;
	long timeout;
	unsigned long flags;

	while(atomic_read(&during_suspend))
		msleep(1);
	pm_qos_update_requirement(PM_QOS_CPU_DMA_LATENCY, "msm_i2c", 501);
	atomic_set(&finished, 0);
	i2c_get_mutex(dev);
	dev->initiated = 1;

	dev->msg = msgs;
	ret = msm_i2c_poll_notbusy(dev);
	if (ret) {
		ret = msm_i2c_recover_bus_busy(dev);
		if (ret) {
			dev->initiated = 0;
			i2c_release_mutex(dev);
			atomic_set(&finished, 1);
			pm_qos_update_requirement(PM_QOS_CPU_DMA_LATENCY, "msm_i2c", 20001);

			return ret;
		}
	}

	spin_lock_irqsave(&dev->lock, flags);
	if (dev->flush_cnt) {
		dev_warn(dev->dev, "(0x%x) %d unrequested bytes read\n",
			 get_address(dev), dev->flush_cnt);
	}
	dev->msg = msgs;
	dev->rem = num;
	dev->pos = -1;
	dev->ret = num;
	dev->need_flush = false;
	dev->flush_cnt = 0;
	dev->cnt = msgs->len;
	dev->complete = &complete;

	msm_i2c_interrupt_locked(dev);
	spin_unlock_irqrestore(&dev->lock, flags);

	/*
	 * Now that we've setup the xfer, the ISR will transfer the data
	 * and wake us up with dev->err set if there was an error
	 */

	timeout = wait_for_completion_timeout(&complete, HZ);
	msm_i2c_poll_notbusy(dev); /* Read may not have stopped in time */

	spin_lock_irqsave(&dev->lock, flags);
	if (dev->flush_cnt) {
		dev_warn(dev->dev, "(0x%x) %d unrequested bytes read\n",
			 get_address(dev), dev->flush_cnt);
	}
	ret = dev->ret;
	dev->complete = NULL;
	dev->msg = NULL;
	dev->rem = 0;
	dev->pos = 0;
	dev->ret = 0;
	dev->flush_cnt = 0;
	dev->cnt = 0;
	spin_unlock_irqrestore(&dev->lock, flags);

	if (!timeout) {
		dev_err(dev->dev, "(0x%x) Transaction timed out\n", get_address(dev));
		ret = -ETIMEDOUT;
	}

	if (ret < 0) {
		dev_err(dev->dev, "(0x%x) Error during data xfer (%d)\n",
			get_address(dev), ret);
		msm_i2c_recover_bus_busy(dev);
	}

	dev->initiated = 0;
	i2c_release_mutex(dev);
	atomic_set(&finished, 1);
	pm_qos_update_requirement(PM_QOS_CPU_DMA_LATENCY, "msm_i2c", 20001);

	return ret;
}

static u32
msm_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | (I2C_FUNC_SMBUS_EMUL & ~I2C_FUNC_SMBUS_QUICK);
}

static const struct i2c_algorithm msm_i2c_algo = {
	.master_xfer	= msm_i2c_xfer,
	.functionality	= msm_i2c_func,
};


#define SMEM_SPINLOCK_I2C            6

static void i2c_get_mutex(struct msm_i2c_dev *dev)
{
	unsigned long flags;
	int gotit = 0;
	if (dev->mutex == NULL) {
		printk("i2c-a1: mutex is null \n");
		return;
	}

	do {
		remote_spin_lock_irqsave(dev->smem_lock, flags);
		if (*(dev->mutex) == 0) {
			*(dev->mutex) = 1;
			gotit=1;
		}
		remote_spin_unlock_irqrestore(dev->smem_lock, flags);
	} while (!gotit);
}

static void i2c_release_mutex(struct msm_i2c_dev *dev)
{
	unsigned long flags;
	if (dev->mutex == NULL)
		return;

	remote_spin_lock_irqsave(dev->smem_lock, flags);
	if (*(dev->mutex) == 2) {
		remote_spin_unlock_irqrestore(dev->smem_lock, flags);
		return;
	}
	*(dev->mutex) = 0;
	remote_spin_unlock_irqrestore(dev->smem_lock, flags);
}

static int
msm_i2c_probe(struct platform_device *pdev)
{
	struct msm_i2c_dev	*dev;
	struct resource		*mem, *irq, *ioarea;
	int ret;
	int fs_div;
	int hs_div;
	int i2c_clk;
	int clk_ctl;
	int target_clk;
	struct clk *clk;
	struct msm_i2c_platform_data *pdata;

	printk(KERN_INFO "msm_i2c_probe\n");

	/* NOTE: driver uses the static register mapping */
	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem) {
		dev_err(&pdev->dev, "no mem resource?\n");
		return -ENODEV;
	}
	irq = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!irq) {
		dev_err(&pdev->dev, "no irq resource?\n");
		return -ENODEV;
	}

	ioarea = request_mem_region(mem->start, (mem->end - mem->start) + 1,
			pdev->name);
	if (!ioarea) {
		dev_err(&pdev->dev, "I2C region already claimed\n");
		return -EBUSY;
	}
	clk = clk_get(&pdev->dev, "i2c_clk");
	if (IS_ERR(clk)) {
		dev_err(&pdev->dev, "Could not get clock\n");
		ret = PTR_ERR(clk);
		goto err_clk_get_failed;
	}

	pm_qos_add_requirement(PM_QOS_CPU_DMA_LATENCY, "msm_i2c", PM_QOS_DEFAULT_VALUE);
	pdata = pdev->dev.platform_data;
	if (!pdata) {
		dev_err(&pdev->dev, "platform data not initialized\n");
		ret = -ENOSYS;
		goto err_clk_get_failed;
	}
	if (!pdata->msm_i2c_config_gpio) {
		dev_err(&pdev->dev, "config_gpio function not initialized\n");
		ret = -ENOSYS;
		goto err_clk_get_failed;
	}
	target_clk = pdata->clk_freq;

	dev = kzalloc(sizeof(struct msm_i2c_dev), GFP_KERNEL);
	if (!dev) {
		ret = -ENOMEM;
		goto err_alloc_dev_failed;
	}

	dev->dev = &pdev->dev;
	dev->irq = irq->start;
	dev->clk = clk;
	dev->base = ioremap(mem->start, (mem->end - mem->start) + 1);
	if (!dev->base) {
		ret = -ENOMEM;
		goto err_ioremap_failed;
	}

	dev->mutex = NULL;
	dev->initiated = 0;
	if (pdata->lock_val) {
		dev->mutex = (int *)smem_alloc(pdata->lock_val, 8);
		if (dev->mutex == NULL) {
			printk("i2c probe: fail to allocate smem\n");
			ret = -ENOMEM;
			goto err_ioremap_failed;
		}
	}
	dev->smem_lock = &i2c_smem_lock;
	if (dev->mutex != NULL) {
		remote_spin_lock_init(&i2c_smem_lock, SMEM_SPINLOCK_I2C);
	}

	spin_lock_init(&dev->lock);
	platform_set_drvdata(pdev, dev);

	clk_enable(clk);

	/* I2C_HS_CLK = I2C_CLK/(3*(HS_DIVIDER_VALUE+1) */
	/* I2C_FS_CLK = I2C_CLK/(2*(FS_DIVIDER_VALUE+3) */
	/* FS_DIVIDER_VALUE = ((I2C_CLK / I2C_FS_CLK) / 2) - 3 */
	i2c_clk = 19200000; /* input clock */
	target_clk = 100000;
	/* target_clk = 200000; */
	fs_div = ((i2c_clk / target_clk) / 2) - 3;
	hs_div = 3;
	clk_ctl = ((hs_div & 0x7) << 8) | (fs_div & 0xff);
	writel(clk_ctl, dev->base + I2C_CLK_CTL);
	printk(KERN_INFO "msm_i2c_probe: clk_ctl %x, %d Hz\n",
	       clk_ctl, i2c_clk / (2 * ((clk_ctl & 0xff) + 3)));

	i2c_set_adapdata(&dev->adapter, dev);
	dev->adapter.algo = &msm_i2c_algo;
	strncpy(dev->adapter.name,
		"MSM I2C adapter",
		sizeof(dev->adapter.name));

	dev->adapter.nr = pdev->id;
	ret = i2c_add_numbered_adapter(&dev->adapter);
	if (ret) {
		dev_err(&pdev->dev, "i2c_add_adapter failed\n");
		goto err_i2c_add_adapter_failed;
	}

	ret = request_irq(dev->irq, msm_i2c_interrupt,
			IRQF_TRIGGER_RISING, pdev->name, dev);
	if (ret) {
		dev_err(&pdev->dev, "request_irq failed\n");
		goto err_request_irq_failed;
	}
	return 0;

/*	free_irq(dev->irq, dev); */
err_request_irq_failed:
	i2c_del_adapter(&dev->adapter);
err_i2c_add_adapter_failed:
	clk_disable(clk);
	iounmap(dev->base);
err_ioremap_failed:
	kfree(dev);
err_alloc_dev_failed:
	clk_put(clk);
err_clk_get_failed:
	release_mem_region(mem->start, (mem->end - mem->start) + 1);
	return ret;
}

static int
msm_i2c_remove(struct platform_device *pdev)
{
	struct msm_i2c_dev	*dev = platform_get_drvdata(pdev);
	struct resource		*mem;

	platform_set_drvdata(pdev, NULL);
	free_irq(dev->irq, dev);
	i2c_del_adapter(&dev->adapter);
	clk_disable(dev->clk);
	clk_put(dev->clk);
	iounmap(dev->base);
	kfree(dev);
	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	release_mem_region(mem->start, (mem->end - mem->start) + 1);
	return 0;
}

static int msm_i2c_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct msm_i2c_dev *dev = platform_get_drvdata(pdev);
#if defined (CONFIG_ACER_DEBUG)
	printk( "%s: ++ entering  [I2C]... -ACER- \n",__func__);
#endif
	atomic_set(&during_suspend, 1);
	while(!atomic_read(&finished))
		msleep(1);
	if (dev) {
		i2c_release_mutex(dev);
		clk_disable(dev->clk);
	}
	has_been_suspended = 1;
#if defined (CONFIG_ACER_DEBUG)
	printk( "%s: ++ leaving ...[I2C]  -ACER- \n",__func__);
#endif
	return 0;
}

static int msm_i2c_resume(struct platform_device *pdev)
{
	struct msm_i2c_dev *dev = platform_get_drvdata(pdev);
#if defined (CONFIG_ACER_DEBUG)
	printk( "%s: ++ entering  [I2C]... -ACER- \n",__func__);
#endif
	if (dev)
		clk_enable(dev->clk);
	atomic_set(&during_suspend, 0);
#if defined (CONFIG_ACER_DEBUG)
	printk( "%s: ++ leaving ...[I2C]  -ACER- \n",__func__);
#endif
	return 0;
}

static struct platform_driver msm_i2c_driver = {
	.probe		= msm_i2c_probe,
	.remove		= msm_i2c_remove,
	.suspend	= msm_i2c_suspend,
	.resume		= msm_i2c_resume,
	.driver		= {
		.name	= "msm_i2c",
		.owner	= THIS_MODULE,
	},
};

/* I2C may be needed to bring up other drivers */
static int __init
msm_i2c_init_driver(void)
{
#if defined (CONFIG_ACER_DEBUG)
	int rett;
	printk( "%s: ++ entering  [I2C]... -ACER- \n",__func__);
	rett = platform_driver_register(&msm_i2c_driver);
	printk( "%s: ++ leaving ...[I2C]  -ACER- \n",__func__);
	return rett;
#else
	return platform_driver_register(&msm_i2c_driver);
#endif
}
subsys_initcall(msm_i2c_init_driver);

static void __exit msm_i2c_exit_driver(void)
{
	platform_driver_unregister(&msm_i2c_driver);
}
module_exit(msm_i2c_exit_driver);
