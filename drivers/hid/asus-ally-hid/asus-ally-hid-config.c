// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for Asus ROG laptops and Ally
 *
 *  Copyright (c) 2023 Luke Jones <luke@ljones.dev>
 */

#include <linux/device.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/types.h>

#include "asus-ally.h"
#include "../hid-ids.h"

/**
 * ally_request_response - Send a command and get response
 * @hdev: HID device
 * @hidbuf: Buffer containing the command (will be overwritten with response)
 *
 * Returns 0 on success, negative error code on failure
 * The response will be written to hidbuf
 */
static int ally_request_response(struct hid_device *hdev, u8 *hidbuf)
{
	int ret;

	hidbuf[0] = HID_ALLY_SET_REPORT_ID;
	hidbuf[1] = HID_ALLY_FEATURE_CODE_PAGE;

	ret = asus_dev_set_report(hdev, hidbuf, HID_ALLY_REPORT_SIZE);
	if (ret < 0) {
		hid_err(hdev, "Command failed set report: %d\n", ret);
		return ret;
	}

	memset(hidbuf, 0, HID_ALLY_REPORT_SIZE);

	ret = asus_dev_get_report(hdev, hidbuf, HID_ALLY_REPORT_SIZE);
	if (ret < 0) {
		hid_err(hdev, "Failed to get command response: %d\n", ret);
		return ret;
	}

	return 0;
}

/**
 * ally_check_capability - Check if a specific capability is supported
 * @hdev: HID device
 * @flag_code: Capability flag code to check
 *
 * Returns true if capability is supported, false otherwise
 */
static bool ally_check_capability(struct hid_device *hdev, u8 flag_code)
{
	bool result = false;
	u8 *hidbuf;
	int ret;

	hidbuf = kzalloc(HID_ALLY_REPORT_SIZE, GFP_KERNEL);
	if (!hidbuf)
		return false;

	hidbuf[0] = HID_ALLY_SET_REPORT_ID;
	hidbuf[1] = HID_ALLY_FEATURE_CODE_PAGE;
	hidbuf[2] = flag_code;
	hidbuf[3] = 0x01;

	ret = ally_request_response(hdev, hidbuf);
	if (ret < 0)
		goto cleanup;

	if (hidbuf[1] == HID_ALLY_FEATURE_CODE_PAGE && hidbuf[2] == flag_code)
		result = (hidbuf[4] == 0x01);

cleanup:
	kfree(hidbuf);
	return result;
}

static int ally_detect_capabilities(struct hid_device *hdev,
				    struct ally_config *cfg)
{
	if (!hdev || !cfg)
		return -EINVAL;

	mutex_lock(&cfg->config_mutex);
	cfg->is_ally_x =
		(hdev->product == USB_DEVICE_ID_ASUSTEK_ROG_NKEY_ALLY_X);

	cfg->xbox_controller_support =
		ally_check_capability(hdev, CMD_CHECK_XBOX_SUPPORT);
	cfg->user_cal_support =
		ally_check_capability(hdev, CMD_CHECK_USER_CAL_SUPPORT);
	cfg->turbo_support =
		ally_check_capability(hdev, CMD_CHECK_TURBO_SUPPORT);
	cfg->resp_curve_support =
		ally_check_capability(hdev, CMD_CHECK_RESP_CURVE_SUPPORT);
	cfg->dir_to_btn_support =
		ally_check_capability(hdev, CMD_CHECK_DIR_TO_BTN_SUPPORT);
	cfg->gyro_support =
		ally_check_capability(hdev, CMD_CHECK_GYRO_TO_JOYSTICK);
	cfg->anti_deadzone_support =
		ally_check_capability(hdev, CMD_CHECK_ANTI_DEADZONE);
	mutex_unlock(&cfg->config_mutex);

	hid_dbg(
		hdev,
		"Ally capabilities: %s, Xbox: %d, UserCal: %d, Turbo: %d, RespCurve: %d, DirToBtn: %d, Gyro: %d, AntiDZ: %d",
		cfg->is_ally_x ? "Ally X" : "Ally",
		cfg->xbox_controller_support, cfg->user_cal_support,
		cfg->turbo_support, cfg->resp_curve_support,
		cfg->dir_to_btn_support, cfg->gyro_support,
		cfg->anti_deadzone_support);

	return 0;
}

static int ally_set_xbox_controller(struct hid_device *hdev,
				    struct ally_config *cfg, bool enabled)
{
	u8 buffer[64] = { 0 };
	int ret;

	if (!cfg || !cfg->xbox_controller_support)
		return -ENODEV;

	buffer[0] = HID_ALLY_SET_REPORT_ID;
	buffer[1] = HID_ALLY_FEATURE_CODE_PAGE;
	buffer[2] = CMD_SET_XBOX_CONTROLLER;
	buffer[3] = 0x01;
	buffer[4] = enabled ? 0x01 : 0x00;

	ret = asus_dev_set_report(hdev, buffer, sizeof(buffer));
	if (ret < 0)
		return ret;

	cfg->xbox_controller_enabled = enabled;
	return 0;
}

static ssize_t xbox_controller_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	struct ally_config *cfg;

	if (!ally || !ally->config)
		return -ENODEV;

	cfg = ally->config;

	if (!cfg->xbox_controller_support)
		return sprintf(buf, "Unsupported\n");

	return sprintf(buf, "%d\n", cfg->xbox_controller_enabled);
}

static ssize_t xbox_controller_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	struct ally_config *cfg;
	bool enabled;
	int ret;

	cfg = ally->config;
	if (!cfg->xbox_controller_support)
		return -ENODEV;

	ret = kstrtobool(buf, &enabled);
	if (ret)
		return ret;

	ret = ally_set_xbox_controller(hdev, cfg, enabled);

	if (ret < 0)
		return ret;

	return count;
}

static DEVICE_ATTR_RW(xbox_controller);

static struct attribute *ally_config_attrs[] = {
	&dev_attr_xbox_controller.attr,
	NULL
};

static const struct attribute_group ally_attr_groups[] = {
    {
        .attrs = ally_config_attrs,
    },
};

/**
 * ally_config_create - Initialize configuration and create sysfs entries
 * @hdev: HID device
 * @ally: Ally device data
 *
 * Returns 0 on success, negative error code on failure
 */
int ally_config_create(struct hid_device *hdev, struct ally_handheld *ally)
{
	struct ally_config *cfg;
	int ret, i;

	if (!hdev || !ally)
		return -EINVAL;

	if (get_endpoint_address(hdev) != HID_ALLY_INTF_CFG_IN)
		return 0;

	cfg = devm_kzalloc(&hdev->dev, sizeof(*cfg), GFP_KERNEL);
	if (!cfg)
		return -ENOMEM;

	cfg->hdev = hdev;

	ally->config = cfg;

	ret = ally_detect_capabilities(hdev, cfg);
	if (ret < 0) {
		hid_err(hdev, "Failed to detect Ally capabilities: %d\n", ret);
		goto err_free;
	}

	/* Create all attribute groups */
	for (i = 0; i < ARRAY_SIZE(ally_attr_groups); i++) {
		ret = sysfs_create_group(&hdev->dev.kobj, &ally_attr_groups[i]);
		if (ret < 0) {
			hid_err(hdev, "Failed to create sysfs group '%s': %d\n",
				ally_attr_groups[i].name, ret);
			/* Remove any groups already created */
			while (--i >= 0)
				sysfs_remove_group(&hdev->dev.kobj,
						   &ally_attr_groups[i]);
			goto err_free;
		}
	}

	cfg->gamepad_mode = 0x01;
	cfg->left_deadzone = 10;
	cfg->left_outer_threshold = 90;
	cfg->right_deadzone = 10;
	cfg->right_outer_threshold = 90;

	// ONLY FOR ALLY 1
	if (cfg->xbox_controller_support) {
		ret = ally_set_xbox_controller(hdev, cfg, true);
		if (ret < 0)
			hid_warn(
				hdev,
				"Failed to set default Xbox controller mode: %d\n",
				ret);
	}

	cfg->initialized = true;
	hid_info(hdev, "Ally configuration system initialized successfully\n");

	return 0;

err_free:
	ally->config = NULL;
	devm_kfree(&hdev->dev, cfg);
	return ret;
}

/**
 * ally_config_remove - Clean up configuration resources
 * @hdev: HID device
 * @ally: Ally device data
 */
void ally_config_remove(struct hid_device *hdev, struct ally_handheld *ally)
{
	struct ally_config *cfg;
	int i;

	if (!ally)
		return;

	cfg = ally->config;
	if (!cfg || !cfg->initialized)
		return;

	if (get_endpoint_address(hdev) != HID_ALLY_INTF_CFG_IN)
		return;

	/* Remove all attribute groups in reverse order */
	for (i = ARRAY_SIZE(ally_attr_groups) - 1; i >= 0; i--)
		sysfs_remove_group(&hdev->dev.kobj, &ally_attr_groups[i]);

	ally->config = NULL;

	hid_info(hdev, "Ally configuration system removed\n");
}
