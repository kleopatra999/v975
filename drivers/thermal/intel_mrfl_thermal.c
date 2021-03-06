/*
 * intel_mrfl_thermal.c - Intel Merrifield Platform Thermal Driver
 *
 *
 * Copyright (C) 2011 Intel Corporation
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * Author: Durgadoss R <durgadoss.r@intel.com>
 *
 * DEVICE_NAME: Intel Merrifield platform - PMIC: Thermal Monitor
 */

#define pr_fmt(fmt)  "intel_mrfl_thermal: " fmt

#include <linux/pm.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/rpmsg.h>
#include <linux/module.h>
#include <linux/thermal.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>

#include <asm/intel_scu_pmic.h>
#include <asm/intel_mid_rpmsg.h>
#include <asm/intel_basincove_gpadc.h>

#include "../staging/iio/consumer.h"

#define DRIVER_NAME "bcove_thrm"
#define DEVICE_NAME "mrfl_pmic_thermal"

/* Number of Thermal sensors on the PMIC */
#define PMIC_THERMAL_SENSORS	4

/* Registers that govern Thermal Monitoring */
#define THRMMONCFG	0xB3
#define THRMMONCTL	0xB4
#define THRMIRQ		0x04
#define MTHRMIRQ	0x0F
#define STHRMIRQ	0xB2
#define IRQLVL1		0x01
#define MIRQLVL1	0x0C
#define IRQ_MASK_ALL	0x0F

/* PMIC SRAM base address and offset for Thermal register */
#define PMIC_SRAM_BASE_ADDR	0xFFFFF610
#define PMIC_SRAM_THRM_OFFSET	0x03
#define IOMAP_SIZE		0x04

#define PMICALRT	(1 << 3)
#define SYS2ALRT	(1 << 2)
#define SYS1ALRT	(1 << 1)
#define SYS0ALRT	(1 << 0)
#define THERM_EN	(1 << 0)
#define THERM_ALRT	(1 << 2)

/* ADC to Temperature conversion table length */
#define TABLE_LENGTH	34
#define TEMP_INTERVAL	5

/* Default _max 85 C */
#define DEFAULT_MAX_TEMP	85

/* Constants defined in BasinCove PMIC spec */
#define PMIC_DIE_SENSOR		0
#define PMIC_DIE_ADC_MIN	395
#define PMIC_DIE_ADC_MAX	661
#define PMIC_DIE_TEMP_MIN	-40
#define PMIC_DIE_TEMP_MAX	125
#define ADC_VAL_27C		503
#define ADC_COEFFICIENT		619
#define TEMP_OFFSET		27000

/* 'enum' of Thermal sensors */
enum thermal_sensors { SYS0, SYS1, SYS2, PMIC_DIE, _COUNT };

/* ADC channels for the sensors. Order: SYS0 SYS1 SYS2 PMIC_DIE */
static const int adc_channels[] = { GPADC_SYSTEMP0, GPADC_SYSTEMP1,
				GPADC_SYSTEMP2, GPADC_PMICTEMP };
/*
 * Alert registers store the 'alert' temperature for each sensor,
 * as 10 bit ADC code. The higher two bits are stored in bits[0:1] of
 * alert_regs_h. The lower eight bits are stored in alert_regs_l.
 * The hysteresis value is stored in bits[2:6] of alert_regs_h.
 * Order: SYS0 SYS1 SYS2 PMIC_DIE
 *
 * static const int alert_regs_l[] = { 0xB7, 0xB9, 0xBB, 0xC1 };
 */
static const int alert_regs_h[] = { 0xB6, 0xB8, 0xBA, 0xC0 };

/*
 * ADC code vs Temperature table
 * This table will be different for different thermistors
 * Row 0: ADC code
 * Row 1: Temperature (in degree celsius)
 */
static const int adc_code[2][TABLE_LENGTH] = {
	{952, 932, 906, 877, 843, 804, 761, 714, 665, 614,
	563, 512, 462, 415, 370, 329, 291, 257, 226, 199,
	174, 153, 135, 119, 104, 92, 81, 72, 64, 56,
	50, 45, 40, 36},
	{-40, -35, -30, -25, -20, -15, -10, -5, 0, 5,
	10, 15, 20, 25, 30, 35, 40, 45, 50, 55,
	60, 65, 70, 75, 80, 85, 90, 95, 100, 105,
	110, 115, 120, 125},
	};

static DEFINE_MUTEX(thrm_update_lock);

struct thermal_device_info {
	int sensor_index;
	/* If 'direct', no table lookup is required */
	bool is_direct;
};

struct thermal_data {
	struct platform_device *pdev;
	struct iio_channel *iio_chan;
	struct thermal_zone_device *tzd[PMIC_THERMAL_SENSORS];
	void *thrm_addr;
	unsigned int irq;
	/* Caching information */
	bool is_initialized;
	unsigned long last_updated;
	int cached_vals[PMIC_THERMAL_SENSORS];
};
static struct thermal_data *tdata;

static inline int adc_to_pmic_die_temp(unsigned int val)
{
	/* return temperature in mC */
	return (val - ADC_VAL_27C) * ADC_COEFFICIENT + TEMP_OFFSET;
}

static inline int pmic_die_temp_to_adc(int temp)
{
	/* 'temp' is in C, convert to mC and then do calculations */
	return ((temp * 1000) - TEMP_OFFSET) / ADC_COEFFICIENT + ADC_VAL_27C;
}

/**
 * find_adc_code - searches the ADC code using binary search
 * @val: value to find in the array
 *
 * This function does binary search on an array sorted in 'descending' order
 * Can sleep
 */
static int find_adc_code(uint16_t val)
{
	int left = 0;
	int right = TABLE_LENGTH - 1;
	int mid;
	while (left <= right) {
		mid = (left + right)/2;
		if (val == adc_code[0][mid] ||
			(mid > 0 &&
			val > adc_code[0][mid] && val < adc_code[0][mid-1]))
			return mid;
		else if (val > adc_code[0][mid])
			right = mid - 1;
		else if (val < adc_code[0][mid])
			left = mid + 1;
	}
	return -EINVAL;
}

/**
 * adc_to_temp - converts the ADC code to temperature in mC
 * @direct: true if the sensor uses direct conversion
 * @adc_val: the ADC code to be converted
 * @tp: temperature return value
 *
 * Can sleep
 */
static int adc_to_temp(int direct, uint16_t adc_val, unsigned long *tp)
{
	int x0, x1, y0, y1;
	int nr, dr;		/* Numerator & Denominator */
	int indx;
	int x = adc_val;

	/* Direct conversion for pmic die temperature */
	if (direct) {
		if (adc_val < PMIC_DIE_ADC_MIN || adc_val > PMIC_DIE_ADC_MAX)
			return -EINVAL;

		*tp = adc_to_pmic_die_temp(adc_val);
		return 0;
	}

	indx = find_adc_code(adc_val);
	if (indx < 0)
		return -EINVAL;

	if (adc_code[0][indx] == adc_val) {
		*tp = adc_code[1][indx] * 1000;
		return 0;
	}

	/*
	 * The ADC code is in between two values directly defined in the
	 * table. So, do linear interpolation to calculate the temperature.
	 */
	x0 = adc_code[0][indx];
	x1 = adc_code[0][indx - 1];
	y0 = adc_code[1][indx];
	y1 = adc_code[1][indx - 1];

	/*
	 * Find y:
	 * Of course, we can avoid these variables, but keep them
	 * for readability and maintainability.
	 */
	nr = (x-x0)*y1 + (x1-x)*y0;
	dr =  x1-x0;

	if (!dr)
		return -EINVAL;
	/*
	 * We have to report the temperature in milli degree celsius.
	 * So, to reduce the loss of precision, do (Nr*1000)/Dr, instead
	 * of (Nr/Dr)*1000.
	 */
	*tp = (nr * 1000)/dr;

	return 0;
}

/**
 * temp_to_adc - converts the temperature(in C) to ADC code
 * @direct: true if the sensor uses direct conversion
 * @temp: the temperature to be converted
 * @adc_val: ADC code return value
 *
 * Can sleep
 */
static int temp_to_adc(int direct, int temp, int *adc_val)
{
	int indx;
	int x0, x1, y0, y1;
	int nr, dr;		/* Numerator & Denominator */
	int x = temp;

	/* Direct conversion for pmic die temperature */
	if (direct) {
		if (temp < PMIC_DIE_TEMP_MIN || temp > PMIC_DIE_TEMP_MAX)
			return -EINVAL;

		*adc_val = pmic_die_temp_to_adc(temp);
		return 0;
	}

	if (temp < adc_code[1][0] || temp > adc_code[1][TABLE_LENGTH - 1])
		return -EINVAL;


	/* Find the 'indx' of this 'temp' in the table */
	indx = (temp - adc_code[1][0]) / TEMP_INTERVAL;

	if (temp == adc_code[1][indx]) {
		*adc_val = adc_code[0][indx];
		return 0;
	}

	/*
	 * Temperature is not a multiple of 'TEMP_INTERVAL'. So,
	 * do linear interpolation to obtain a better ADC code.
	 */
	x0 = adc_code[1][indx];
	x1 = adc_code[1][indx + 1];
	y0 = adc_code[0][indx];
	y1 = adc_code[0][indx + 1];

	nr = (x-x0)*y1 + (x1-x)*y0;
	dr =  x1-x0;

	if (!dr)
		return -EINVAL;

	*adc_val = nr/dr;

	return 0;
}

/**
 * set_tmax - sets the given 'adc_val' to the 'alert_reg'
 * @alert_reg: register address
 * @adc_val: ADC value to be programmed
 *
 * Not protected. Calling function should handle synchronization.
 * Can sleep
 */
static int set_tmax(int alert_reg, int adc_val)
{
	int ret;

	/* Set bits[0:1] of alert_reg_h to bits[8:9] of 'adc_val' */
	ret = intel_scu_ipc_update_register(alert_reg, (adc_val >> 8), 0x03);
	if (ret)
		return ret;

	/* Extract bits[0:7] of 'adc_val' and write them into alert_reg_l */
	return intel_scu_ipc_iowrite8(alert_reg + 1, adc_val & 0xFF);
}

/**
 * program_tmax - programs a default _max value for each sensor
 * @dev: device pointer
 *
 * Can sleep
 */
static int program_tmax(struct device *dev)
{
	int i, ret;
	int pmic_die_val, adc_val;

	ret = temp_to_adc(0, DEFAULT_MAX_TEMP, &adc_val);
	if (ret)
		return ret;

	ret = temp_to_adc(1, DEFAULT_MAX_TEMP, &pmic_die_val);
	if (ret)
		return ret;

	for (i = 0; i < PMIC_THERMAL_SENSORS - 1; i++) {
		ret = set_tmax(alert_regs_h[i], adc_val);
		if (ret)
			goto exit_err;
	}

	/* Set _max for pmic die sensor */
	ret = set_tmax(alert_regs_h[i], pmic_die_val);
	if (ret)
		goto exit_err;

	return ret;

exit_err:
	dev_err(dev, "set_tmax for channel %d failed:%d\n", i, ret);
	return ret;
}

static ssize_t store_trip_hyst(struct thermal_zone_device *tzd,
				int trip, unsigned long hyst)
{
	int ret;
	uint8_t data;
	struct thermal_device_info *td_info = tzd->devdata;
	int alert_reg = alert_regs_h[td_info->sensor_index];

	/* Hysteresis value is 5 bits wide */
	if (hyst > 31)
		return -EINVAL;

	mutex_lock(&thrm_update_lock);

	ret = intel_scu_ipc_ioread8(alert_reg, &data);
	if (ret)
		goto ipc_fail;

	/* Set bits [2:6] to value of hyst */
	data = (data & 0x83) | (hyst << 2);

	ret = intel_scu_ipc_iowrite8(alert_reg, data);

ipc_fail:
	mutex_unlock(&thrm_update_lock);
	return ret;
}

static ssize_t show_trip_hyst(struct thermal_zone_device *tzd,
				int trip, unsigned long *hyst)
{
	int ret;
	uint8_t data;
	struct thermal_device_info *td_info = tzd->devdata;
	int alert_reg = alert_regs_h[td_info->sensor_index];

	mutex_lock(&thrm_update_lock);

	ret = intel_scu_ipc_ioread8(alert_reg, &data);
	if (!ret)
		*hyst = (data >> 2) & 0x1F; /* Extract bits[2:6] of data */

	mutex_unlock(&thrm_update_lock);

	return ret;
}

static ssize_t store_trip_temp(struct thermal_zone_device *tzd,
				int trip, unsigned long trip_temp)
{
	int ret, adc_val;
	struct thermal_device_info *td_info = tzd->devdata;
	int alert_reg = alert_regs_h[td_info->sensor_index];

	if (trip_temp < 1000) {
		dev_err(&tzd->device, "Temperature should be in mC\n");
		return -EINVAL;
	}

	mutex_lock(&thrm_update_lock);

	/* Convert from mC to C */
	trip_temp /= 1000;

	ret = temp_to_adc(td_info->is_direct, (int)trip_temp, &adc_val);
	if (ret)
		goto exit;

	ret =  set_tmax(alert_reg, adc_val);
exit:
	mutex_unlock(&thrm_update_lock);
	return ret;
}

static ssize_t show_trip_temp(struct thermal_zone_device *tzd,
				int trip, unsigned long *trip_temp)
{
	int ret, adc_val;
	uint8_t l, h;
	struct thermal_device_info *td_info = tzd->devdata;
	int alert_reg = alert_regs_h[td_info->sensor_index];

	mutex_lock(&thrm_update_lock);

	ret = intel_scu_ipc_ioread8(alert_reg, &h);
	if (ret)
		goto exit;

	ret = intel_scu_ipc_ioread8(alert_reg + 1, &l);
	if (ret)
		goto exit;

	/* Concatenate 'h' and 'l' to get 10-bit ADC code */
	adc_val = ((h & 0x03) << 8) | l;

	ret = adc_to_temp(td_info->is_direct, adc_val, trip_temp);
exit:
	mutex_unlock(&thrm_update_lock);
	return ret;
}

static ssize_t show_trip_type(struct thermal_zone_device *tzd,
			int trip, enum thermal_trip_type *trip_type)
{
	/* All are passive trip points */
	*trip_type = THERMAL_TRIP_PASSIVE;

	return 0;
}

static ssize_t show_temp(struct thermal_zone_device *tzd, unsigned long *temp)
{
	int ret;
	struct thermal_device_info *td_info = tzd->devdata;
	int indx = td_info->sensor_index;

	if (!tdata->iio_chan)
		return -EINVAL;

	mutex_lock(&thrm_update_lock);

	if (!tdata->is_initialized ||
			time_after(jiffies, tdata->last_updated + HZ)) {
		ret = iio_st_read_channel_all_raw(tdata->iio_chan,
						tdata->cached_vals);
		if (ret) {
			dev_err(&tzd->device, "ADC sampling failed:%d\n", ret);
			goto exit;
		}
		tdata->last_updated = jiffies;
		tdata->is_initialized = true;
	}

	ret = adc_to_temp(td_info->is_direct, tdata->cached_vals[indx], temp);
exit:
	mutex_unlock(&thrm_update_lock);
	return ret;
}

static int enable_tm(void)
{
	int ret;
	uint8_t data;

	mutex_lock(&thrm_update_lock);

	ret = intel_scu_ipc_ioread8(THRMMONCTL, &data);
	if (ret)
		goto ipc_fail;

	ret = intel_scu_ipc_iowrite8(THRMMONCTL, data | THERM_EN);

ipc_fail:
	mutex_unlock(&thrm_update_lock);
	return ret;
}

static struct thermal_device_info *initialize_sensor(int index)
{
	struct thermal_device_info *td_info =
		kzalloc(sizeof(struct thermal_device_info), GFP_KERNEL);

	if (!td_info)
		return NULL;

	td_info->sensor_index = index;

	if (index == PMIC_DIE_SENSOR)
		td_info->is_direct = true;

	return td_info;
}

static irqreturn_t thermal_intrpt(int irq, void *dev_data)
{
	int ret, sensor, event_type;
	uint8_t irq_status;
	unsigned int irq_data;
	struct thermal_data *tdata = (struct thermal_data *)dev_data;

	if (!tdata)
		return IRQ_NONE;

	mutex_lock(&thrm_update_lock);

	irq_data = ioread8(tdata->thrm_addr + PMIC_SRAM_THRM_OFFSET);

	ret = intel_scu_ipc_ioread8(STHRMIRQ, &irq_status);
	if (ret)
		goto ipc_fail;

	dev_dbg(&tdata->pdev->dev, "STHRMIRQ: %.2x\n", irq_status);

	/*
	 * -1 for invalid interrupt
	 * 1 for LOW to HIGH temperature alert
	 * 0 for HIGH to LOW temperature alert
	 */
	event_type = -1;

	/* Check which interrupt occured and for what event */
	if (irq_data & PMICALRT) {
		event_type = !!(irq_status & PMICALRT);
		sensor = PMIC_DIE;
	} else if (irq_data & SYS2ALRT) {
		event_type = !!(irq_status & SYS2ALRT);
		sensor = SYS2;
	} else if (irq_data & SYS1ALRT) {
		event_type = !!(irq_status & SYS1ALRT);
		sensor = SYS1;
	} else if (irq_data & SYS0ALRT) {
		event_type = !!(irq_status & SYS0ALRT);
		sensor = SYS0;
	} else {
		dev_err(&tdata->pdev->dev, "Invalid Interrupt\n");
		ret = IRQ_HANDLED;
		goto ipc_fail;
	}

	if (event_type != -1) {
		dev_info(&tdata->pdev->dev,
				"%s interrupt for thermal sensor %d\n",
				event_type ? "HIGH" : "LOW", sensor);
	}

	/* Notify using UEvent */
	kobject_uevent(&tdata->pdev->dev.kobj, KOBJ_CHANGE);

	/* Unmask Thermal Interrupt in the mask register */
	ret = intel_scu_ipc_update_register(MIRQLVL1, 0xFF, THERM_ALRT);
	if (ret)
		goto ipc_fail;

	ret = IRQ_HANDLED;

ipc_fail:
	mutex_unlock(&thrm_update_lock);
	return ret;
}

static struct thermal_zone_device_ops tzd_ops = {
	.get_temp = show_temp,
	.get_trip_type = show_trip_type,
	.get_trip_temp = show_trip_temp,
	.set_trip_temp = store_trip_temp,
	.get_trip_hyst = show_trip_hyst,
	.set_trip_hyst = store_trip_hyst,
};

static int mrfl_thermal_probe(struct platform_device *pdev)
{
	int ret, i;
	static char *name[PMIC_THERMAL_SENSORS] = {
			"PMICDIE", "SYSTHERM0", "SYSTHERM1", "SYSTHERM2" };

	tdata = kzalloc(sizeof(struct thermal_data), GFP_KERNEL);
	if (!tdata) {
		dev_err(&pdev->dev, "kzalloc failed\n");
		return -ENOMEM;
	}

	tdata->pdev = pdev;
	tdata->irq = platform_get_irq(pdev, 0);
	platform_set_drvdata(pdev, tdata);

	/* Program a default _max value for each sensor */
	ret = program_tmax(&pdev->dev);
	if (ret) {
		dev_err(&pdev->dev, "Programming _max failed:%d\n", ret);
		goto exit_free;
	}

	/* Register with IIO to sample temperature values */
	tdata->iio_chan = iio_st_channel_get_all("THERMAL");
	if (tdata->iio_chan == NULL) {
		dev_err(&pdev->dev, "tdata->iio_chan is null\n");
		ret = -EINVAL;
		goto exit_free;
	}

	/* Check whether we got all the four channels */
	ret = iio_st_channel_get_num(tdata->iio_chan);
	if (ret != PMIC_THERMAL_SENSORS) {
		dev_err(&pdev->dev, "incorrect number of channels:%d\n", ret);
		ret = -EFAULT;
		goto exit_iio;
	}

	/* Register each sensor with the generic thermal framework */
	for (i = 0; i < PMIC_THERMAL_SENSORS; i++) {
		tdata->tzd[i] = thermal_zone_device_register(name[i],
					1, 1, initialize_sensor(i), &tzd_ops,
					0, 0, 0, 0);
		if (IS_ERR(tdata->tzd[i])) {
			ret = PTR_ERR(tdata->tzd[i]);
			dev_err(&pdev->dev,
				"registering thermal sensor %s failed: %d\n",
				name[i], ret);
			goto exit_reg;
		}
	}

	tdata->thrm_addr = ioremap_nocache(PMIC_SRAM_BASE_ADDR, IOMAP_SIZE);
	if (!tdata->thrm_addr) {
		ret = -ENOMEM;
		dev_err(&pdev->dev, "ioremap_nocache failed\n");
		goto exit_reg;
	}

	/* Register for Interrupt Handler */
	ret = request_threaded_irq(tdata->irq, NULL, thermal_intrpt,
						IRQF_TRIGGER_RISING,
						DRIVER_NAME, tdata);
	if (ret) {
		dev_err(&pdev->dev, "request_threaded_irq failed:%d\n", ret);
		goto exit_ioremap;
	}

	/* Enable Thermal Monitoring */
	ret = enable_tm();
	if (ret) {
		dev_err(&pdev->dev, "Enabling TM failed:%d\n", ret);
		goto exit_irq;
	}

	return 0;

exit_irq:
	free_irq(tdata->irq, tdata);
exit_ioremap:
	iounmap(tdata->thrm_addr);
exit_reg:
	while (--i >= 0)
		thermal_zone_device_unregister(tdata->tzd[i]);
exit_iio:
	iio_st_channel_release_all(tdata->iio_chan);
exit_free:
	kfree(tdata);
	return ret;
}

static int mrfl_thermal_resume(struct device *dev)
{
	dev_info(dev, "resume called.\n");
	return 0;
}

static int mrfl_thermal_suspend(struct device *dev)
{
	dev_info(dev, "suspend called.\n");
	return 0;
}

static int mrfl_thermal_remove(struct platform_device *pdev)
{
	int i;
	struct thermal_data *tdata = platform_get_drvdata(pdev);

	if (!tdata)
		return 0;

	for (i = 0; i < PMIC_THERMAL_SENSORS; i++)
		thermal_zone_device_unregister(tdata->tzd[i]);

	free_irq(tdata->irq, tdata);
	iounmap(tdata->thrm_addr);
	iio_st_channel_release_all(tdata->iio_chan);
	kfree(tdata);
	return 0;
}

/*********************************************************************
 *		Driver initialization and finalization
 *********************************************************************/

static const struct dev_pm_ops thermal_pm_ops = {
	.suspend = mrfl_thermal_suspend,
	.resume = mrfl_thermal_resume,
};

static struct platform_driver mrfl_thermal_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.pm = &thermal_pm_ops,
		},
	.probe = mrfl_thermal_probe,
	.remove = mrfl_thermal_remove,
};

static int mrfl_thermal_module_init(void)
{
	return platform_driver_register(&mrfl_thermal_driver);
}

static void mrfl_thermal_module_exit(void)
{
	platform_driver_unregister(&mrfl_thermal_driver);
}

/* RPMSG related functionality */
static int mrfl_thermal_rpmsg_probe(struct rpmsg_channel *rpdev)
{
	if (!rpdev) {
		pr_err("rpmsg channel not created\n");
		return -ENODEV;
	}

	dev_info(&rpdev->dev, "Probed mrfl_thermal rpmsg device\n");

	return mrfl_thermal_module_init();
}

static void mrfl_thermal_rpmsg_remove(struct rpmsg_channel *rpdev)
{
	mrfl_thermal_module_exit();
	dev_info(&rpdev->dev, "Removed mrfl_thermal rpmsg device\n");
}

static void mrfl_thermal_rpmsg_cb(struct rpmsg_channel *rpdev, void *data,
			int len, void *priv, u32 src)
{
	dev_warn(&rpdev->dev, "unexpected, message\n");

	print_hex_dump(KERN_DEBUG, __func__, DUMP_PREFIX_NONE, 16, 1,
				data, len, true);
}

static struct rpmsg_device_id mrfl_thermal_id_table[] = {
	{ .name = "rpmsg_mrfl_thermal" },
	{ },
};

MODULE_DEVICE_TABLE(rpmsg, mrfl_thermal_id_table);

static struct rpmsg_driver mrfl_thermal_rpmsg = {
	.drv.name	= DRIVER_NAME,
	.drv.owner	= THIS_MODULE,
	.probe		= mrfl_thermal_rpmsg_probe,
	.callback	= mrfl_thermal_rpmsg_cb,
	.remove		= mrfl_thermal_rpmsg_remove,
	.id_table	= mrfl_thermal_id_table,
};

static int __init mrfl_thermal_rpmsg_init(void)
{
	return register_rpmsg_driver(&mrfl_thermal_rpmsg);
}

static void __exit mrfl_thermal_rpmsg_exit(void)
{
	return unregister_rpmsg_driver(&mrfl_thermal_rpmsg);
}

module_init(mrfl_thermal_rpmsg_init);
module_exit(mrfl_thermal_rpmsg_exit);

MODULE_AUTHOR("Durgadoss R <durgadoss.r@intel.com>");
MODULE_DESCRIPTION("Intel Merrifield Platform Thermal Driver");
MODULE_LICENSE("GPL");
