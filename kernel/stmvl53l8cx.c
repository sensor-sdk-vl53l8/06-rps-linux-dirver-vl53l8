/**
  *
  * Copyright (c) 2023 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/miscdevice.h>
#include <linux/version.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/fs.h>


#define VL53L8CX_COMMS_CHUNK_SIZE 1024

#define ST_TOF_IOCTL_TRANSFER 		_IOWR('a',0x1, struct stmvl53l8cx_comms_struct)
#define ST_TOF_IOCTL_WAIT_FOR_INTERRUPT	_IO('a',0x2)


struct stmvl53l8cx_drvdata {
	struct i2c_client *client;
	struct spi_device *pdev;
	int irq;
	struct miscdevice misc;
	uint8_t * reg_buf; /*[0-1]: register, [2...]: data*/
	atomic_t intr_ready_flag;
	wait_queue_head_t wq;
	int dev_num;
};

struct stmvl53l8cx_comms_struct {
	__u16   len;
	__u16   reg_index;
	__u8    write_not_read;
	__u8    padding[3]; /* 64bits alignment */
	__u64   bufptr;
};

static int stmvl53l8cx_i2c_read(struct stmvl53l8cx_drvdata *drvdata, uint32_t count)
{
	int ret = 0;
	struct i2c_client *client = drvdata->client;
	uint8_t * reg_buf = drvdata->reg_buf;
	uint8_t * data_buf = drvdata->reg_buf + 2;
	struct i2c_msg msg[2];

	msg[0].addr = client->addr;
	msg[0].flags = client->flags;
	msg[0].buf = reg_buf;
	msg[0].len = 2;

	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD | client->flags;
	msg[1].buf = data_buf;
	msg[1].len = count;

	ret = i2c_transfer(client->adapter, msg, 2);
	if (ret != 2) {
		pr_err("%s: err[%d]\n", __func__, ret);
		ret = -1;
	}
	else {
		ret = 0;
	}

	return ret;
}

static int stmvl53l8cx_i2c_write(struct stmvl53l8cx_drvdata *drvdata, uint32_t count)
{
	int ret = 0;
	struct i2c_client *client = drvdata->client;
	struct i2c_msg msg;

	msg.addr = client->addr;
	msg.flags = client->flags;
	msg.buf = drvdata->reg_buf;
	msg.len = count + 2;

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret != 1) {
		pr_err("%s: err[%d]\n", __func__, ret);
		ret = -1;
	}
	else {
		ret = 0;
	}

	return ret;
}

static int stmvl53l8cx_spi_read(struct stmvl53l8cx_drvdata *drvdata, uint32_t count)
{
	int ret = 0;
	struct spi_device *pdev = drvdata->pdev;
	uint8_t * reg_buf = drvdata->reg_buf;
	uint8_t * data_buf = drvdata->reg_buf + 2;
	struct spi_message m;
	struct spi_transfer t[2];

	spi_message_init(&m);
	memset(&t, 0, sizeof(t));

	t[0].tx_buf = reg_buf;
	t[0].len = 2;

	t[1].rx_buf = data_buf;
	t[1].len = (unsigned int)count;

	spi_message_add_tail(&t[0], &m);
	spi_message_add_tail(&t[1], &m);

	ret = spi_sync(pdev, &m);
	if (ret != 0)
	{
		pr_err("%s: err[%d]\n", __func__, ret);
	}
	return ret;
}

static int stmvl53l8cx_spi_write(struct stmvl53l8cx_drvdata *drvdata, uint32_t count)
{
	int ret = 0;
	struct spi_device *pdev = drvdata->pdev;
	uint8_t * reg_buf = drvdata->reg_buf;
	struct spi_message m;
	struct spi_transfer t;

	spi_message_init(&m);
	memset(&t, 0, sizeof(t));

	t.tx_buf = reg_buf;
	t.len = (unsigned int)(count + 2);

	spi_message_add_tail(&t, &m);

	ret = spi_sync(pdev, &m);
	if (ret != 0)
	{
		pr_err("%s: err[%d]\n", __func__, ret);
	}
	return ret;
}

static int stmvl53l8cx_read_regs(struct stmvl53l8cx_drvdata *drvdata, uint16_t reg_index,
									 uint8_t *pdata, uint32_t count, char __user *useraddr)
{
	int ret = 0;
	uint8_t * reg_buf = drvdata->reg_buf;
	uint8_t * data_buf = drvdata->reg_buf + 2;

	reg_buf[0] = (reg_index >> 8) & 0xFF;
	reg_buf[1] = reg_index & 0xFF;

	if (drvdata->client) {
		ret = stmvl53l8cx_i2c_read(drvdata, count);
	}
	else if (drvdata->pdev) {
		reg_buf[0] &= 0x7F; /* set spi read bit*/
		ret = stmvl53l8cx_spi_read(drvdata, count);
	}
	else {
		pr_err("%s:%d input wrong params\n", __func__, __LINE__);
		ret = -1;
		return ret;
	}

	if (ret == 0) {
		if (useraddr) {
			ret = copy_to_user(useraddr, data_buf, count);
			if (ret) {
				pr_err("%s:%d error[%d]\n", __func__, __LINE__, ret);
				return ret;
			}		
		}
		else if (pdata) {
			memcpy(pdata, data_buf, count);
		}
		else {
			ret = -1;
			pr_err("%s: input null data buffer\n", __func__);
			return ret;
		}
	}
	return ret;
}

static int stmvl53l8cx_write_regs(struct stmvl53l8cx_drvdata *drvdata, uint16_t reg_index,
									  uint8_t *pdata, uint32_t count, char __user *useraddr)
{
	int ret = 0;
	uint8_t * reg_buf = drvdata->reg_buf;
	uint8_t * data_buf = drvdata->reg_buf + 2;

	reg_buf[0] = (reg_index >> 8) & 0xFF;
	reg_buf[1] = reg_index & 0xFF;
	if (useraddr) {
		ret = copy_from_user(data_buf, useraddr, count);
		if (ret) {
			pr_err("%s:%d error[%d]\n", __func__, __LINE__, ret);
			return ret;
		}		
	}
	else if (pdata) {
		memcpy(data_buf, pdata, count);
	}
	else {
		ret = -1;
		pr_err("%s: input null data buffer\n", __func__);
		return ret;
	}

	if (drvdata->client) {
		ret = stmvl53l8cx_i2c_write(drvdata, count);
	}
	else if (drvdata->pdev) {
		reg_buf[0] |= 0x80; /* set spi write bit*/
		ret = stmvl53l8cx_spi_write(drvdata, count);
	}
	else {
		ret = -1;
	}
	return 0;
}

static int stmvl53l8cx_read_write(struct stmvl53l8cx_drvdata *drvdata, uint16_t reg_index,
							 char __user *useraddr, uint32_t count, uint8_t write_not_read) 
{
	int ret = 0;
	uint16_t chunks = count / VL53L8CX_COMMS_CHUNK_SIZE;
	uint16_t offset = 0;

	while (chunks > 0) {
				
		if (write_not_read) {
			ret = stmvl53l8cx_write_regs(drvdata, reg_index+offset, NULL, 
									VL53L8CX_COMMS_CHUNK_SIZE, useraddr+offset);
		}
		else {			
			ret = stmvl53l8cx_read_regs(drvdata, reg_index+offset, NULL,
								  VL53L8CX_COMMS_CHUNK_SIZE, useraddr+offset);			
		}

		if (ret) {
			pr_err("%s:%d r/w[%d], err[%d]\n", __func__, __LINE__, write_not_read, ret);
			return ret;
		}
		
		offset += VL53L8CX_COMMS_CHUNK_SIZE;
		chunks--;
	}
	if (count > offset) {
		if (write_not_read) {
			ret = stmvl53l8cx_write_regs(drvdata, reg_index+offset, NULL, 
											count-offset, useraddr+offset);
		}
		else {
			ret = stmvl53l8cx_read_regs(drvdata, reg_index+offset, NULL,
								  			count-offset, useraddr+offset);
		}
		if (ret) {
			pr_err("%s:%d r/w[%d], err[%d]\n", __func__, __LINE__, write_not_read, ret);
		}
	}
	return ret;
}

static long stmvl53l8cx_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	struct stmvl53l8cx_drvdata *drvdata = container_of(file->private_data, 
    										struct stmvl53l8cx_drvdata, misc);
	struct stmvl53l8cx_comms_struct comms_struct = {0};
	void __user *data_ptr = NULL;

	pr_debug("stmvl53l8cx_ioctl : cmd = %u\n", cmd);
	switch (cmd) {
		case ST_TOF_IOCTL_WAIT_FOR_INTERRUPT:
			pr_debug("%s(%d)\n", __func__, __LINE__);
			ret = wait_event_interruptible(drvdata->wq, atomic_read(&drvdata->intr_ready_flag) != 0);
			atomic_set(&drvdata->intr_ready_flag, 0);
			if (ret) {
				pr_info("%s: wait_event_interruptible err=%d\n", __func__, ret);				
				return -EINTR;
			}
			break;
		case ST_TOF_IOCTL_TRANSFER:
			ret = copy_from_user(&comms_struct, (void __user *)arg, sizeof(comms_struct));
			if (ret) {
				pr_err("%s:%d err[%d]\n", __func__, __LINE__, ret);
				return -EINVAL;
			}
			pr_debug("[0x%x,%d,%d]\n", comms_struct.reg_index, comms_struct.len, comms_struct.write_not_read);
			if (!comms_struct.write_not_read) {
				data_ptr = (u8 __user *)(uintptr_t)(comms_struct.bufptr);
				ret = stmvl53l8cx_read_write(drvdata, comms_struct.reg_index, data_ptr,
									comms_struct.len, comms_struct.write_not_read);
			}
			else {
				ret = stmvl53l8cx_read_write(drvdata, comms_struct.reg_index, (char *)(uintptr_t)comms_struct.bufptr,
										comms_struct.len, comms_struct.write_not_read);
			}
			if (ret) {
				pr_err("%s:%d err[%d]\n", __func__, __LINE__, ret);
				return -EFAULT;
			}
			break;

		default:
			return -EINVAL;

	}
	return 0;
}

#ifdef CONFIG_COMPAT
static long stmvl53l8cx_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return stmvl53l8cx_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static const struct file_operations stmvl53l8cx_fops = {
	.owner 			= THIS_MODULE,
	.unlocked_ioctl		= stmvl53l8cx_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl		= stmvl53l8cx_compat_ioctl,
#endif
};

/* Interrupt handler */
static irqreturn_t stmvl53l8cx_intr_handler(int irq, void *dev_id)
{
	struct stmvl53l8cx_drvdata *drvdata = (struct stmvl53l8cx_drvdata *)dev_id;
	atomic_set(&drvdata->intr_ready_flag, 1);
	wake_up_interruptible(&drvdata->wq);
	return IRQ_HANDLED;
}

static int stmvl53l8cx_parse_dt(struct device *dev, struct stmvl53l8cx_drvdata *drvdata)
{
	int ret = 0;
	struct gpio_desc *gpio_int, *gpio_pwr;
	
	gpio_pwr = devm_gpiod_get_optional(dev, "pwr", GPIOD_OUT_HIGH);
	if (!gpio_pwr || IS_ERR(gpio_pwr)) {
		ret = PTR_ERR(gpio_pwr);
		dev_info(dev, "failed to request power GPIO: %d.\n", ret);
	}

	gpio_int = devm_gpiod_get_optional(dev, "irq", GPIOD_IN);
	if (!gpio_int || IS_ERR(gpio_int)) {
		ret = PTR_ERR(gpio_int);
		dev_err(dev, "failed to request interrupt GPIO: %d\n", ret);
		return ret;
	}

	drvdata->irq = gpiod_to_irq(gpio_int);
	if (drvdata->irq < 0) {
		ret = drvdata->irq;
		dev_err(dev, "failed to get irq: %d\n", ret);
		return ret;
	}

	init_waitqueue_head(&drvdata->wq);
	ret = devm_request_threaded_irq(dev, drvdata->irq, NULL,
			stmvl53l8cx_intr_handler, IRQF_TRIGGER_FALLING | IRQF_ONESHOT, "vl53l8cx_intr", drvdata);
	if (ret) {
		dev_err(dev, "failed to register plugin det irq (%d)\n", ret);
	}

	if (of_find_property(dev->of_node, "dev_num", NULL)) {
		of_property_read_s32(dev->of_node, "dev_num", &drvdata->dev_num);
		dev_info(dev, "dev_num = %d\n", drvdata->dev_num);
	}
	else {
		drvdata->dev_num = -1;
	}

	return ret;
}

static int stmvl53l8cx_detect(struct stmvl53l8cx_drvdata *drvdata)
{
	int ret = 0;
	uint8_t page = 0, revision_id = 0, device_id = 0;
	char devname[16];

	ret = stmvl53l8cx_write_regs(drvdata, 0x7FFF, &page, 1, NULL);
	ret |= stmvl53l8cx_read_regs(drvdata, 0x00, &device_id, 1, NULL);
	ret |= stmvl53l8cx_read_regs(drvdata, 0x01, &revision_id, 1, NULL);

	if ((device_id != 0xF0) || (revision_id != 0x0C)) {
		pr_err("stmvl53l8cx: Error. Could not read device and revision id registers\n");
		return -ENODEV;
	}
	pr_info("stmvl53l8cx: device_id : 0x%x. revision_id : 0x%x\n", device_id, revision_id);

	drvdata->misc.minor = MISC_DYNAMIC_MINOR;
	drvdata->misc.name = "stmvl53l8cx";
	drvdata->misc.fops = &stmvl53l8cx_fops;
	if (drvdata->dev_num >= 0) {
		snprintf(devname, sizeof(devname), "stmvl53l8cx%d", drvdata->dev_num);
		drvdata->misc.name = devname;		
	}
	
	ret = misc_register(&drvdata->misc);
	return ret;	
}

static int stmvl53l8cx_i2c_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int ret;
	struct stmvl53l8cx_drvdata *drvdata;
	
	drvdata = devm_kzalloc(&client->dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->reg_buf = devm_kzalloc(&client->dev, VL53L8CX_COMMS_CHUNK_SIZE+2, GFP_DMA | GFP_KERNEL);
	if (drvdata->reg_buf == NULL)
		 return -ENOMEM;

	drvdata->client = client;

	pr_info("%s: i2c name=%s, addr=0x%x\n", __func__, client->adapter->name, client->addr);

	ret = stmvl53l8cx_parse_dt(&client->dev, drvdata);
	if (ret) {
		dev_err(&client->dev, "parse dts failed: %d\n", ret);
		return ret;
	}

	ret = stmvl53l8cx_detect(drvdata);
	if (ret) {
		dev_err(&client->dev, "sensor detect failed: %d\n", ret);
		return ret;
	}

	i2c_set_clientdata(client, drvdata);
	return ret;
}

#if KERNEL_VERSION(6, 1, 0) > LINUX_VERSION_CODE
static int stmvl53l8cx_i2c_remove(struct i2c_client *client)
#else
static void stmvl53l8cx_i2c_remove(struct i2c_client *client)
#endif
{
	struct stmvl53l8cx_drvdata *drvdata;

	drvdata = i2c_get_clientdata(client);
	if (!drvdata) {
		pr_err("%s: can't remove %p", __func__, client);
	}
	else {
		misc_deregister(&drvdata->misc);
	}

	#if KERNEL_VERSION(6, 1, 0) > LINUX_VERSION_CODE
	return 0;
	#endif
}

static int stmvl53l8cx_spi_driver_probe(struct spi_device *pdev)
{
	int ret = 0;
	struct stmvl53l8cx_drvdata *drvdata;
	
	drvdata = devm_kzalloc(&pdev->dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->reg_buf = devm_kzalloc(&pdev->dev, VL53L8CX_COMMS_CHUNK_SIZE+2, GFP_DMA | GFP_KERNEL);
	if (drvdata->reg_buf == NULL)
		 return -ENOMEM;

	drvdata->pdev = pdev;

	pr_info("%s: spi mode=%d, cs=%d, bits_per_word=%d, speed=%d, modalias=%s", 
						__func__, pdev->mode, pdev->chip_select, pdev->bits_per_word, 
						pdev->max_speed_hz, pdev->modalias);

	
	ret = stmvl53l8cx_parse_dt(&pdev->dev, drvdata);
	if (ret) {
		dev_err(&pdev->dev, "parse dts failed: %d\n", ret);
		return ret;
	}

	ret = stmvl53l8cx_detect(drvdata);
	if (ret) {
		dev_err(&pdev->dev, "sensor detect failed: %d\n", ret);
		return ret;
	}

	spi_set_drvdata(pdev, drvdata);
	return ret;
}

#if KERNEL_VERSION(6, 1, 0) > LINUX_VERSION_CODE
static int stmvl53l8cx_spi_driver_remove(struct spi_device *pdev)
#else
static void stmvl53l8cx_spi_driver_remove(struct spi_device *pdev)
#endif
{
  struct stmvl53l8cx_drvdata *drvdata;

  drvdata = spi_get_drvdata(pdev);
  if (!drvdata) {
    pr_err("%s: can't remove %p", __func__, pdev);
  }
  else {
	misc_deregister(&drvdata->misc);
  }

#if KERNEL_VERSION(6, 1, 0) > LINUX_VERSION_CODE
  return 0;
#endif
}

static const struct of_device_id stmvl53l8cx_dt_ids[] = {
	{ .compatible = "st,stmvl53l8cx" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, stmvl53l8cx_dt_ids);

static const struct i2c_device_id stmvl53l8cx_i2c_id[] = {
	{ "stmvl53l8cx", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, stmvl53l8cx_i2c_id);

static struct i2c_driver stmvl53l8cx_i2c_driver = {
	.driver = {
		.name = "stmvl53l8cx",
		.owner = THIS_MODULE,
		.of_match_table = stmvl53l8cx_dt_ids,
	},
	.probe = stmvl53l8cx_i2c_probe,
	.remove = stmvl53l8cx_i2c_remove,
	.id_table = stmvl53l8cx_i2c_id,
};

static const struct of_device_id stmvl53l8cx_spi_dt_ids[] = {
	{ .compatible = "st,stmvl53l8cx" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, stmvl53l8cx_spi_dt_ids);

static const struct spi_device_id stmvl53l8cx_spi_id[] = {
	{ "stmvl53l8cx", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, stmvl53l8cx_spi_id);

static struct spi_driver stmvl53l8cx_spi_driver = {
  .driver = {
    .name = "stmvl53l8cx",
    .owner = THIS_MODULE,
    .of_match_table = stmvl53l8cx_spi_dt_ids,
  },
  .probe = stmvl53l8cx_spi_driver_probe,
  .remove = stmvl53l8cx_spi_driver_remove,
  .id_table = stmvl53l8cx_spi_id,
};

static int __init stmvl53l8cx_init(void)
{
	int ret = 0;

	pr_debug("stmvl53l8cx: module init\n");

	/* register as a i2c client device */
	ret = i2c_add_driver(&stmvl53l8cx_i2c_driver);

	if (ret) {
		i2c_del_driver(&stmvl53l8cx_i2c_driver);
		printk("stmvl53l8cx: could not add i2c driver\n");
		return ret;
	}

	ret = spi_register_driver(&stmvl53l8cx_spi_driver);
	if (ret) {
		pr_err("spi_register_driver failed => %d", ret);
		return ret;
	}

	return ret;
}

static void __exit stmvl53l8cx_exit(void)
{
	pr_debug("stmvl53l8cx : module exit\n");
	spi_unregister_driver(&stmvl53l8cx_spi_driver);
	i2c_del_driver(&stmvl53l8cx_i2c_driver);
}

module_init(stmvl53l8cx_init);
module_exit(stmvl53l8cx_exit);

MODULE_AUTHOR("Pengfei Mao <peng-fei.mao@st.com>");
MODULE_DESCRIPTION("VL53L8CX dTOF lightweight driver");
MODULE_LICENSE("Dual BSD/GPL");
