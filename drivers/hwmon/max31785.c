/*
 * max31785.c - Part of lm_sensors, Linux kernel modules for hardware
 *             monitoring.
 *
 * (C) 2016 Raptor Engineering, LLC
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
 */

#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/slab.h>

/* MAX31785 device IDs */
#define MAX31785_MFR_ID				0x4d
#define MAX31785_MFR_MODEL			0x53

/* MAX31785 registers */
#define MAX31785_REG_PAGE			0x00
#define MAX31785_PAGE_FAN_CONFIG(ch)		(0x00 + (ch))
#define MAX31785_REG_FAN_CONFIG_1_2		0x3a
#define MAX31785_REG_FAN_COMMAND_1		0x3b
#define MAX31785_REG_STATUS_FANS_1_2		0x81
#define MAX31785_REG_FAN_SPEED_1		0x90
#define MAX31785_REG_MFR_ID			0x99
#define MAX31785_REG_MFR_MODEL			0x9a
#define MAX31785_REG_MFR_FAN_CONFIG		0xf1
#define MAX31785_REG_READ_FAN_PWM		0xf3

/* Fan Config register bits */
#define MAX31785_FAN_CFG_PWM_ENABLE		0x80
#define MAX31785_FAN_CFG_CONTROL_MODE_RPM	0x40
#define MAX31785_FAN_CFG_PULSE_MASK		0x30
#define MAX31785_FAN_CFG_PULSE_SHIFT		4
#define MAX31785_FAN_CFG_PULSE_OFFSET		1

/* Fan Status register bits */
#define MAX31785_FAN_STATUS_FAULT_MASK		0x80

/* Fan Command constants */
#define MAX31785_FAN_COMMAND_PWM_RATIO		40

#define NR_CHANNEL				6

/* Addresses to scan */
static const unsigned short normal_i2c[] = { 0x52, 0x53, 0x54, 0x55,
							I2C_CLIENT_END };

/*
 * Client data (each client gets its own)
 */
struct max31785_data {
	struct i2c_client *client;
	struct mutex device_lock;
	bool valid; /* zero until following fields are valid */
	unsigned long last_updated; /* in jiffies */

	/* register values */
	u8 fan_config[NR_CHANNEL];
	u16 fan_command[NR_CHANNEL];
	u8 mfr_fan_config[NR_CHANNEL];
	u8 fault_status[NR_CHANNEL];
	u16 tach_rpm[NR_CHANNEL];
	u16 pwm[NR_CHANNEL];
};

static int max31785_set_page(struct i2c_client *client,
				u8 page)
{
	return i2c_smbus_write_byte_data(client,
			MAX31785_REG_PAGE,
			page);
}

static int max31785_read_fan_data(struct i2c_client *client,
				u8 fan, u8 reg, u8 byte_mode)
{
	int rv;

	rv = max31785_set_page(client, MAX31785_PAGE_FAN_CONFIG(fan));
	if (rv < 0)
		return rv;

	if (byte_mode)
		rv = i2c_smbus_read_byte_data(client, reg);
	else
		rv = i2c_smbus_read_word_data(client, reg);

	return rv;
}

static int max31785_write_fan_data(struct i2c_client *client,
				u8 fan, u8 reg, u16 data,
				u8 byte_mode)
{
	int err;

	err = max31785_set_page(client, MAX31785_PAGE_FAN_CONFIG(fan));
	if (err < 0)
		return err;

	if (byte_mode)
		err = i2c_smbus_write_byte_data(client, reg, data);
	else
		err = i2c_smbus_write_word_data(client, reg, data);

	if (err < 0)
		return err;

	return 0;
}

static bool is_automatic_control_mode(struct max31785_data *data,
			int index)
{
	if (data->fan_command[index] > 0x7fff)
		return true;
	else
		return false;
}

static struct max31785_data *max31785_update_device(struct device *dev)
{
	struct max31785_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	struct max31785_data *ret = data;
	int i;
	int rv;

	mutex_lock(&data->device_lock);

	if (time_after(jiffies, data->last_updated + HZ) || !data->valid) {
		for (i = 0; i < NR_CHANNEL; i++) {
			rv = max31785_read_fan_data(client, i,
					MAX31785_REG_STATUS_FANS_1_2, 1);
			if (rv < 0)
				goto abort;
			data->fault_status[i] = rv;

			rv = max31785_read_fan_data(client, i,
					MAX31785_REG_FAN_SPEED_1, 0);
			if (rv < 0)
				goto abort;
			data->tach_rpm[i] = rv;

			if ((data->fan_config[i]
				& MAX31785_FAN_CFG_CONTROL_MODE_RPM)
				|| is_automatic_control_mode(data, i)) {
				rv = max31785_read_fan_data(client, i,
						MAX31785_REG_READ_FAN_PWM, 0);
				if (rv < 0)
					goto abort;
				data->pwm[i] = rv;
			}

			if (!is_automatic_control_mode(data, i)) {
				/* Poke watchdog for manual fan control */
				rv = max31785_write_fan_data(client,
					i, MAX31785_REG_FAN_COMMAND_1,
					data->fan_command[i], 0);
				if (rv < 0)
					goto abort;
			}
		}

		data->last_updated = jiffies;
		data->valid = true;
	}
	goto done;

abort:
	data->valid = false;
	ret = ERR_PTR(rv);

done:
	mutex_unlock(&data->device_lock);

	return ret;
}

static ssize_t get_fan(struct device *dev,
		       struct device_attribute *devattr, char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct max31785_data *data = max31785_update_device(dev);

	if (IS_ERR(data))
		return PTR_ERR(data);

	return sprintf(buf, "%d\n", data->tach_rpm[attr->index]);
}

static ssize_t get_fan_target(struct device *dev,
			      struct device_attribute *devattr, char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct max31785_data *data = max31785_update_device(dev);
	int rpm;

	if (IS_ERR(data))
		return PTR_ERR(data);

	if (data->fan_config[attr->index]
		& MAX31785_FAN_CFG_CONTROL_MODE_RPM)
		rpm = data->fan_command[attr->index];
	else
		rpm = data->fan_command[attr->index]
					/ MAX31785_FAN_COMMAND_PWM_RATIO;

	return sprintf(buf, "%d\n", rpm);
}

static ssize_t set_fan_target(struct device *dev,
			      struct device_attribute *devattr,
			      const char *buf, size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct max31785_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	unsigned long rpm;
	int err;

	err = kstrtoul(buf, 10, &rpm);
	if (err)
		return err;

	if (rpm > 0x7fff)
		return -EINVAL;

	mutex_lock(&data->device_lock);

	/* Write new RPM value */
	data->fan_command[attr->index] = rpm;
	err = max31785_write_fan_data(client, attr->index,
				MAX31785_REG_FAN_COMMAND_1,
				data->fan_command[attr->index], 0);

	mutex_unlock(&data->device_lock);

	if (err < 0)
		return err;

	return count;
}

static ssize_t get_fan_pulses(struct device *dev,
			      struct device_attribute *devattr, char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct max31785_data *data = max31785_update_device(dev);
	int pulses;

	if (IS_ERR(data))
		return PTR_ERR(data);

	pulses = ((data->fan_config[attr->index] & MAX31785_FAN_CFG_PULSE_MASK)
			>> MAX31785_FAN_CFG_PULSE_SHIFT)
			+ MAX31785_FAN_CFG_PULSE_OFFSET;

	return sprintf(buf, "%d\n", pulses);
}

static ssize_t set_fan_pulses(struct device *dev,
			      struct device_attribute *devattr,
			      const char *buf, size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct max31785_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	unsigned long pulses;
	int err;

	err = kstrtoul(buf, 10, &pulses);
	if (err)
		return err;

	if (pulses > 4)
		return -EINVAL;

	data->fan_config[attr->index] &= MAX31785_FAN_CFG_PULSE_MASK;
	data->fan_config[attr->index] |=
				((pulses - MAX31785_FAN_CFG_PULSE_OFFSET)
				<< MAX31785_FAN_CFG_PULSE_SHIFT);

	mutex_lock(&data->device_lock);

	/* Write new pulse value */
	data->fan_command[attr->index] = pulses;
	err = max31785_write_fan_data(client, attr->index,
				MAX31785_REG_FAN_CONFIG_1_2,
				data->fan_config[attr->index], 1);

	mutex_unlock(&data->device_lock);

	if (err < 0)
		return err;

	return count;
}

static ssize_t get_pwm(struct device *dev,
		       struct device_attribute *devattr, char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct max31785_data *data = max31785_update_device(dev);
	int pwm;

	if (IS_ERR(data))
		return PTR_ERR(data);

	if ((data->fan_config[attr->index]
		& MAX31785_FAN_CFG_CONTROL_MODE_RPM)
		|| is_automatic_control_mode(data, attr->index))
		pwm = data->pwm[attr->index] / 100;
	else
		pwm = data->fan_command[attr->index]
					/ MAX31785_FAN_COMMAND_PWM_RATIO;

	return sprintf(buf, "%d\n", pwm);
}

static ssize_t set_pwm(struct device *dev,
		       struct device_attribute *devattr,
		       const char *buf, size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct max31785_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	unsigned long pwm;
	int err;

	err = kstrtoul(buf, 10, &pwm);
	if (err)
		return err;

	if (pwm > 255)
		return -EINVAL;

	mutex_lock(&data->device_lock);

	/* Write new PWM value */
	data->fan_command[attr->index] = pwm * MAX31785_FAN_COMMAND_PWM_RATIO;
	err = max31785_write_fan_data(client, attr->index,
				MAX31785_REG_FAN_COMMAND_1,
				data->fan_command[attr->index], 0);

	mutex_unlock(&data->device_lock);

	if (err < 0)
		return err;

	return count;
}

static ssize_t get_pwm_enable(struct device *dev,
			      struct device_attribute *devattr, char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct max31785_data *data = max31785_update_device(dev);
	int mode;

	if (IS_ERR(data))
		return PTR_ERR(data);

	if (!(data->fan_config[attr->index] & MAX31785_FAN_CFG_PWM_ENABLE))
		mode = 0;
	else if (is_automatic_control_mode(data, attr->index))
		mode = 3;
	else if (data->fan_config[attr->index]
		& MAX31785_FAN_CFG_CONTROL_MODE_RPM)
		mode = 2;
	else
		mode = 1;

	return sprintf(buf, "%d\n", mode);
}

static ssize_t set_pwm_enable(struct device *dev,
			      struct device_attribute *devattr,
			      const char *buf, size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct max31785_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	unsigned long mode;
	int err;

	err = kstrtoul(buf, 10, &mode);
	if (err)
		return err;

	switch (mode) {
	case 0:
		data->fan_config[attr->index] =
			data->fan_config[attr->index]
			& ~MAX31785_FAN_CFG_PWM_ENABLE;
		break;
	case 1:
	case 2:
	case 3:
		data->fan_config[attr->index] =
			data->fan_config[attr->index]
			 | MAX31785_FAN_CFG_PWM_ENABLE;
		break;
	default:
		return -EINVAL;
	}

	switch (mode) {
	case 0:
		break;
	case 1:
		data->fan_config[attr->index] =
			data->fan_config[attr->index]
			& ~MAX31785_FAN_CFG_CONTROL_MODE_RPM;
		break;
	case 2:
		data->fan_config[attr->index] =
			data->fan_config[attr->index]
			| MAX31785_FAN_CFG_CONTROL_MODE_RPM;
		break;
	case 3:
		data->fan_command[attr->index] = 0xffff;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&data->device_lock);

	err = max31785_write_fan_data(client, attr->index,
				MAX31785_REG_FAN_CONFIG_1_2,
				data->fan_config[attr->index], 1);

	if (err < 0)
		goto abort;

	err = max31785_write_fan_data(client, attr->index,
				MAX31785_REG_FAN_COMMAND_1,
				data->fan_command[attr->index], 0);

abort:
	mutex_unlock(&data->device_lock);

	if (err < 0)
		return err;

	return count;
}

static ssize_t get_fan_fault(struct device *dev,
			     struct device_attribute *devattr, char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct max31785_data *data = max31785_update_device(dev);
	int fault;

	if (IS_ERR(data))
		return PTR_ERR(data);

	fault = !!(data->fault_status[attr->index]
			& MAX31785_FAN_STATUS_FAULT_MASK);

	return sprintf(buf, "%d\n", fault);
}

static SENSOR_DEVICE_ATTR(fan1_input, S_IRUGO, get_fan, NULL, 0);
static SENSOR_DEVICE_ATTR(fan2_input, S_IRUGO, get_fan, NULL, 1);
static SENSOR_DEVICE_ATTR(fan3_input, S_IRUGO, get_fan, NULL, 2);
static SENSOR_DEVICE_ATTR(fan4_input, S_IRUGO, get_fan, NULL, 3);
static SENSOR_DEVICE_ATTR(fan5_input, S_IRUGO, get_fan, NULL, 4);
static SENSOR_DEVICE_ATTR(fan6_input, S_IRUGO, get_fan, NULL, 5);

static SENSOR_DEVICE_ATTR(fan1_fault, S_IRUGO, get_fan_fault, NULL, 0);
static SENSOR_DEVICE_ATTR(fan2_fault, S_IRUGO, get_fan_fault, NULL, 1);
static SENSOR_DEVICE_ATTR(fan3_fault, S_IRUGO, get_fan_fault, NULL, 2);
static SENSOR_DEVICE_ATTR(fan4_fault, S_IRUGO, get_fan_fault, NULL, 3);
static SENSOR_DEVICE_ATTR(fan5_fault, S_IRUGO, get_fan_fault, NULL, 4);
static SENSOR_DEVICE_ATTR(fan6_fault, S_IRUGO, get_fan_fault, NULL, 5);

static SENSOR_DEVICE_ATTR(fan1_target, S_IWUSR | S_IRUGO,
		get_fan_target, set_fan_target, 0);
static SENSOR_DEVICE_ATTR(fan2_target, S_IWUSR | S_IRUGO,
		get_fan_target, set_fan_target, 1);
static SENSOR_DEVICE_ATTR(fan3_target, S_IWUSR | S_IRUGO,
		get_fan_target, set_fan_target, 2);
static SENSOR_DEVICE_ATTR(fan4_target, S_IWUSR | S_IRUGO,
		get_fan_target, set_fan_target, 3);
static SENSOR_DEVICE_ATTR(fan5_target, S_IWUSR | S_IRUGO,
		get_fan_target, set_fan_target, 4);
static SENSOR_DEVICE_ATTR(fan6_target, S_IWUSR | S_IRUGO,
		get_fan_target, set_fan_target, 5);

static SENSOR_DEVICE_ATTR(fan1_pulses, S_IWUSR | S_IRUGO,
		get_fan_pulses, set_fan_pulses, 0);
static SENSOR_DEVICE_ATTR(fan2_pulses, S_IWUSR | S_IRUGO,
		get_fan_pulses, set_fan_pulses, 1);
static SENSOR_DEVICE_ATTR(fan3_pulses, S_IWUSR | S_IRUGO,
		get_fan_pulses, set_fan_pulses, 2);
static SENSOR_DEVICE_ATTR(fan4_pulses, S_IWUSR | S_IRUGO,
		get_fan_pulses, set_fan_pulses, 3);
static SENSOR_DEVICE_ATTR(fan5_pulses, S_IWUSR | S_IRUGO,
		get_fan_pulses, set_fan_pulses, 4);
static SENSOR_DEVICE_ATTR(fan6_pulses, S_IWUSR | S_IRUGO,
		get_fan_pulses, set_fan_pulses, 5);

static SENSOR_DEVICE_ATTR(pwm1, S_IWUSR | S_IRUGO, get_pwm, set_pwm, 0);
static SENSOR_DEVICE_ATTR(pwm2, S_IWUSR | S_IRUGO, get_pwm, set_pwm, 1);
static SENSOR_DEVICE_ATTR(pwm3, S_IWUSR | S_IRUGO, get_pwm, set_pwm, 2);
static SENSOR_DEVICE_ATTR(pwm4, S_IWUSR | S_IRUGO, get_pwm, set_pwm, 3);
static SENSOR_DEVICE_ATTR(pwm5, S_IWUSR | S_IRUGO, get_pwm, set_pwm, 4);
static SENSOR_DEVICE_ATTR(pwm6, S_IWUSR | S_IRUGO, get_pwm, set_pwm, 5);

static SENSOR_DEVICE_ATTR(pwm1_enable, S_IWUSR | S_IRUGO,
		get_pwm_enable, set_pwm_enable, 0);
static SENSOR_DEVICE_ATTR(pwm2_enable, S_IWUSR | S_IRUGO,
		get_pwm_enable, set_pwm_enable, 1);
static SENSOR_DEVICE_ATTR(pwm3_enable, S_IWUSR | S_IRUGO,
		get_pwm_enable, set_pwm_enable, 2);
static SENSOR_DEVICE_ATTR(pwm4_enable, S_IWUSR | S_IRUGO,
		get_pwm_enable, set_pwm_enable, 3);
static SENSOR_DEVICE_ATTR(pwm5_enable, S_IWUSR | S_IRUGO,
		get_pwm_enable, set_pwm_enable, 4);
static SENSOR_DEVICE_ATTR(pwm6_enable, S_IWUSR | S_IRUGO,
		get_pwm_enable, set_pwm_enable, 5);

static struct attribute *max31785_attrs[] = {
	&sensor_dev_attr_fan1_input.dev_attr.attr,
	&sensor_dev_attr_fan2_input.dev_attr.attr,
	&sensor_dev_attr_fan3_input.dev_attr.attr,
	&sensor_dev_attr_fan4_input.dev_attr.attr,
	&sensor_dev_attr_fan5_input.dev_attr.attr,
	&sensor_dev_attr_fan6_input.dev_attr.attr,

	&sensor_dev_attr_fan1_fault.dev_attr.attr,
	&sensor_dev_attr_fan2_fault.dev_attr.attr,
	&sensor_dev_attr_fan3_fault.dev_attr.attr,
	&sensor_dev_attr_fan4_fault.dev_attr.attr,
	&sensor_dev_attr_fan5_fault.dev_attr.attr,
	&sensor_dev_attr_fan6_fault.dev_attr.attr,

	&sensor_dev_attr_fan1_target.dev_attr.attr,
	&sensor_dev_attr_fan2_target.dev_attr.attr,
	&sensor_dev_attr_fan3_target.dev_attr.attr,
	&sensor_dev_attr_fan4_target.dev_attr.attr,
	&sensor_dev_attr_fan5_target.dev_attr.attr,
	&sensor_dev_attr_fan6_target.dev_attr.attr,

	&sensor_dev_attr_fan1_pulses.dev_attr.attr,
	&sensor_dev_attr_fan2_pulses.dev_attr.attr,
	&sensor_dev_attr_fan3_pulses.dev_attr.attr,
	&sensor_dev_attr_fan4_pulses.dev_attr.attr,
	&sensor_dev_attr_fan5_pulses.dev_attr.attr,
	&sensor_dev_attr_fan6_pulses.dev_attr.attr,

	&sensor_dev_attr_pwm1.dev_attr.attr,
	&sensor_dev_attr_pwm2.dev_attr.attr,
	&sensor_dev_attr_pwm3.dev_attr.attr,
	&sensor_dev_attr_pwm4.dev_attr.attr,
	&sensor_dev_attr_pwm5.dev_attr.attr,
	&sensor_dev_attr_pwm6.dev_attr.attr,

	&sensor_dev_attr_pwm1_enable.dev_attr.attr,
	&sensor_dev_attr_pwm2_enable.dev_attr.attr,
	&sensor_dev_attr_pwm3_enable.dev_attr.attr,
	&sensor_dev_attr_pwm4_enable.dev_attr.attr,
	&sensor_dev_attr_pwm5_enable.dev_attr.attr,
	&sensor_dev_attr_pwm6_enable.dev_attr.attr,
	NULL
};

static umode_t max31785_attrs_visible(struct kobject *kobj,
				     struct attribute *a, int n)
{
	return a->mode;
}

static const struct attribute_group max31785_group = {
	.attrs = max31785_attrs,
	.is_visible = max31785_attrs_visible,
};
__ATTRIBUTE_GROUPS(max31785);

static int max31785_init_client(struct i2c_client *client,
				struct max31785_data *data)
{
	int i, rv;

	for (i = 0; i < NR_CHANNEL; i++) {
		rv = max31785_read_fan_data(client, i,
				MAX31785_REG_FAN_CONFIG_1_2, 1);
		if (rv < 0)
			return rv;
		data->fan_config[i] = rv;

		rv = max31785_read_fan_data(client, i,
				MAX31785_REG_FAN_COMMAND_1, 0);
		if (rv < 0)
			return rv;
		data->fan_command[i] = rv;

		rv = max31785_read_fan_data(client, i,
				MAX31785_REG_MFR_FAN_CONFIG, 1);
		if (rv < 0)
			return rv;
		data->mfr_fan_config[i] = rv;

		if (!((data->fan_config[i]
			& MAX31785_FAN_CFG_CONTROL_MODE_RPM)
			|| is_automatic_control_mode(data, i))) {
			data->pwm[i] = 0;
		}
	}

	return rv;
}

/* Return 0 if detection is successful, -ENODEV otherwise */
static int max31785_detect(struct i2c_client *client,
			  struct i2c_board_info *info)
{
	struct i2c_adapter *adapter = client->adapter;
	int rv;

	if (!i2c_check_functionality(adapter,
			I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA))
		return -ENODEV;

	/* Probe manufacturer / model registers */
	rv = i2c_smbus_read_byte_data(client, MAX31785_REG_MFR_ID);
	if (rv < 0)
		return -ENODEV;
	if (rv != MAX31785_MFR_ID)
		return -ENODEV;

	rv = i2c_smbus_read_byte_data(client, MAX31785_REG_MFR_MODEL);
	if (rv < 0)
		return -ENODEV;
	if (rv != MAX31785_MFR_MODEL)
		return -ENODEV;

	strlcpy(info->type, "max31785", I2C_NAME_SIZE);

	return 0;
}

static int max31785_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	struct i2c_adapter *adapter = client->adapter;
	struct device *dev = &client->dev;
	struct max31785_data *data;
	struct device *hwmon_dev;
	int err;

	if (!i2c_check_functionality(adapter,
			I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA))
		return -ENODEV;

	data = devm_kzalloc(dev, sizeof(struct max31785_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	mutex_init(&data->device_lock);

	/*
	 * Initialize the max31785 chip
	 */
	err = max31785_init_client(client, data);
	if (err)
		return err;

	hwmon_dev = devm_hwmon_device_register_with_groups(dev,
			client->name, data, max31785_groups);

	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct i2c_device_id max31785_id[] = {
	{ "max31785", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, max31785_id);

static struct i2c_driver max31785_driver = {
	.class		= I2C_CLASS_HWMON,
	.probe		= max31785_probe,
	.driver = {
		.name	= "max31785",
	},
	.id_table	= max31785_id,
	.detect		= max31785_detect,
	.address_list	= normal_i2c,
};

module_i2c_driver(max31785_driver);

MODULE_AUTHOR("Timothy Pearson <tpearson@raptorengineering.com>");
MODULE_DESCRIPTION("MAX31785 sensor driver");
MODULE_LICENSE("GPL");
