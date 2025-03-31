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
 * ally_check_capability - Check if a specific capability is supported
 * @hdev: HID device
 * @flag_code: Capability flag code to check
 *
 * Returns true if capability is supported, false otherwise
 */
static bool ally_check_capability(struct hid_device *hdev, u8 flag_code)
{
	struct ally_handheld *ally = hid_get_drvdata(hdev);
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

	ret = ally_gamepad_send_receive_packet(ally, hdev, hidbuf, HID_ALLY_REPORT_SIZE);
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
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	u8 buffer[64] = { 0 };
	int ret;

	if (!cfg || !cfg->xbox_controller_support)
		return -ENODEV;

	buffer[0] = HID_ALLY_SET_REPORT_ID;
	buffer[1] = HID_ALLY_FEATURE_CODE_PAGE;
	buffer[2] = CMD_SET_XBOX_CONTROLLER;
	buffer[3] = 0x01;
	buffer[4] = enabled ? 0x01 : 0x00;

	ret = ally_gamepad_send_one_byte_packet(
		ally, hdev, CMD_SET_XBOX_CONTROLLER,
		enabled ? 0x01 : 0x00);
	if (ret < 0) return ret;

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

/**
 * ally_set_vibration_intensity - Set vibration intensity values
 * @hdev: HID device
 * @cfg: Ally config
 * @left: Left motor intensity (0-100)
 * @right: Right motor intensity (0-100)
 *
 * Returns 0 on success, negative error code on failure
 */
static int ally_set_vibration_intensity(struct hid_device *hdev,
					struct ally_config *cfg, u8 left,
					u8 right)
{
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	u8 buffer[64] = { 0 };
	int ret;

	if (!cfg)
		return -ENODEV;

	buffer[0] = HID_ALLY_SET_REPORT_ID;
	buffer[1] = HID_ALLY_FEATURE_CODE_PAGE;
	buffer[2] = CMD_SET_VIBRATION_INTENSITY;
	buffer[3] = 0x02; /* Length */
	buffer[4] = left;
	buffer[5] = right;

	ret = ally_gamepad_send_two_byte_packet(
		ally, hdev, CMD_SET_VIBRATION_INTENSITY, left, right);
	if (ret < 0)
		return ret;

	mutex_lock(&cfg->config_mutex);
	cfg->vibration_intensity_left = left;
	cfg->vibration_intensity_right = right;
	mutex_unlock(&cfg->config_mutex);

	return 0;
}

static ssize_t vibration_intensity_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	struct ally_config *cfg;

	if (!ally || !ally->config)
		return -ENODEV;

	cfg = ally->config;

	return sprintf(buf, "%u,%u\n", cfg->vibration_intensity_left,
		       cfg->vibration_intensity_right);
}

static ssize_t vibration_intensity_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	struct ally_config *cfg;
	u8 left, right;
	int ret;

	if (!ally || !ally->config)
		return -ENODEV;

	cfg = ally->config;

	ret = sscanf(buf, "%hhu %hhu", &left, &right);
	if (ret != 2 || left > 100 || right > 100)
		return -EINVAL;

	ret = ally_set_vibration_intensity(hdev, cfg, left, right);
	if (ret < 0)
		return ret;

	return count;
}

static DEVICE_ATTR_RW(vibration_intensity);

/**
 * ally_set_dzot_ranges - Generic function to set joystick or trigger ranges
 * @hdev: HID device
 * @cfg: Ally config struct
 * @command: Command to use (CMD_SET_JOYSTICK_DEADZONE or CMD_SET_TRIGGER_RANGE)
 * @param1: First parameter
 * @param2: Second parameter
 * @param3: Third parameter
 * @param4: Fourth parameter
 *
 * Returns 0 on success, negative error code on failure
 */
static int ally_set_dzot_ranges(struct hid_device *hdev,
					       struct ally_config *cfg,
					       u8 command, u8 param1, u8 param2,
					       u8 param3, u8 param4)
{
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	u8 packet[HID_ALLY_REPORT_SIZE] = { 0 };
	int ret;

	packet[0] = HID_ALLY_SET_REPORT_ID;
	packet[1] = HID_ALLY_FEATURE_CODE_PAGE;
	packet[2] = command;
	packet[3] = 0x04; /* Length */
	packet[4] = param1;
	packet[5] = param2;
	packet[6] = param3;
	packet[7] = param4;

	ret = ally_gamepad_send_packet(ally, hdev, packet,
				       HID_ALLY_REPORT_SIZE);
	return ret;
}

static int ally_validate_joystick_dzot(u8 left_dz, u8 left_ot, u8 right_dz,
				       u8 right_ot)
{
	if (left_dz > 50 || right_dz > 50)
		return -EINVAL;

	if (left_ot < 70 || left_ot > 100 || right_ot < 70 || right_ot > 100)
		return -EINVAL;

	return 0;
}

static int ally_set_joystick_dzot(struct hid_device *hdev,
				  struct ally_config *cfg, u8 left_dz,
				  u8 left_ot, u8 right_dz, u8 right_ot)
{
	int ret;

	ret = ally_validate_joystick_dzot(left_dz, left_ot, right_dz, right_ot);
	if (ret < 0)
		return ret;

	ret = ally_set_dzot_ranges(hdev, cfg,
				  CMD_SET_JOYSTICK_DEADZONE,
				  left_dz, left_ot, right_dz,
				  right_ot);
	if (ret < 0)
		return ret;

	mutex_lock(&cfg->config_mutex);
	cfg->left_deadzone = left_dz;
	cfg->left_outer_threshold = left_ot;
	cfg->right_deadzone = right_dz;
	cfg->right_outer_threshold = right_ot;
	mutex_unlock(&cfg->config_mutex);

	return 0;
}

static ssize_t joystick_deadzone_show(struct device *dev,
				      struct device_attribute *attr, char *buf,
				      u8 deadzone, u8 outer_threshold)
{
	return sprintf(buf, "%u %u\n", deadzone, outer_threshold);
}

static ssize_t joystick_deadzone_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count,
				       bool is_left, struct ally_config *cfg)
{
	struct hid_device *hdev = to_hid_device(dev);
	u8 dz, ot;
	int ret;

	ret = sscanf(buf, "%hhu %hhu", &dz, &ot);
	if (ret != 2)
		return -EINVAL;

	if (is_left) {
		ret = ally_set_joystick_dzot(hdev, cfg, dz, ot,
					     cfg->right_deadzone,
					     cfg->right_outer_threshold);
	} else {
		ret = ally_set_joystick_dzot(hdev, cfg, cfg->left_deadzone,
					     cfg->left_outer_threshold, dz, ot);
	}

	if (ret < 0)
		return ret;

	return count;
}

static ssize_t joystick_left_deadzone_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	struct ally_config *cfg = ally->config;

	return joystick_deadzone_show(dev, attr, buf, cfg->left_deadzone,
				      cfg->left_outer_threshold);
}

static ssize_t joystick_left_deadzone_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);

	return joystick_deadzone_store(dev, attr, buf, count, true,
				       ally->config);
}

static ssize_t joystick_right_deadzone_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	struct ally_config *cfg = ally->config;

	return joystick_deadzone_show(dev, attr, buf, cfg->right_deadzone,
				      cfg->right_outer_threshold);
}

static ssize_t joystick_right_deadzone_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);

	return joystick_deadzone_store(dev, attr, buf, count, false,
				       ally->config);
}

ALLY_DEVICE_CONST_ATTR_RO(js_deadzone_index, deadzone_index, "inner outer\n");
ALLY_DEVICE_CONST_ATTR_RO(js_deadzone_inner_min, deadzone_inner_min, "0\n");
ALLY_DEVICE_CONST_ATTR_RO(js_deadzone_inner_max, deadzone_inner_max, "50\n");
ALLY_DEVICE_CONST_ATTR_RO(js_deadzone_outer_min, deadzone_outer_min, "70\n");
ALLY_DEVICE_CONST_ATTR_RO(js_deadzone_outer_max, deadzone_outer_max, "100\n");

ALLY_DEVICE_ATTR_RW(joystick_left_deadzone, deadzone);
ALLY_DEVICE_ATTR_RW(joystick_right_deadzone, deadzone);

/**
 * ally_set_anti_deadzone - Set anti-deadzone values for joysticks
 * @ally: ally handheld structure
 * @left_adz: Left joystick anti-deadzone value (0-100)
 * @right_adz: Right joystick anti-deadzone value (0-100)
 *
 * Return: 0 on success, negative on failure
 */
static int ally_set_anti_deadzone(struct ally_handheld *ally, u8 left_adz,
				  u8 right_adz)
{
	struct hid_device *hdev = ally->cfg_hdev;
	int ret;

	if (!ally->config->anti_deadzone_support) {
		hid_dbg(hdev, "Anti-deadzone not supported on this device\n");
		return -EOPNOTSUPP;
	}

	if (left_adz > 100 || right_adz > 100)
		return -EINVAL;

	ret = ally_gamepad_send_two_byte_packet(
		ally, hdev, CMD_SET_ANTI_DEADZONE, left_adz, right_adz);
	if (ret < 0) {
		hid_err(hdev, "Failed to set anti-deadzone values: %d\n", ret);
		return ret;
	}

	ally->config->left_anti_deadzone = left_adz;
	ally->config->right_anti_deadzone = right_adz;
	hid_dbg(hdev, "Set joystick anti-deadzone: left=%d, right=%d\n",
		left_adz, right_adz);

	return 0;
}

static ssize_t anti_deadzone_show(struct device *dev,
				 struct device_attribute *attr, char *buf,
				 u8 anti_deadzone)
{
	return sprintf(buf, "%u\n", anti_deadzone);
}

static ssize_t anti_deadzone_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count, bool is_left,
				  struct ally_handheld *ally)
{
	u8 adz;
	int ret;

	if (!ally || !ally->config)
		return -ENODEV;

	if (!ally->config->anti_deadzone_support)
		return -EOPNOTSUPP;

	ret = kstrtou8(buf, 10, &adz);
	if (ret)
		return ret;

	if (adz > 100)
		return -EINVAL;

	if (is_left)
		ret = ally_set_anti_deadzone(ally, adz, ally->config->right_anti_deadzone);
	else
		ret = ally_set_anti_deadzone(ally, ally->config->left_anti_deadzone, adz);

	if (ret < 0)
		return ret;

	return count;
}

static ssize_t js_left_anti_deadzone_show(struct device *dev,
						struct device_attribute *attr,
						char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);

	if (!ally || !ally->config)
		return -ENODEV;

	return anti_deadzone_show(dev, attr, buf,
				  ally->config->left_anti_deadzone);
}

static ssize_t js_left_anti_deadzone_store(struct device *dev,
						 struct device_attribute *attr,
						 const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);

	return anti_deadzone_store(dev, attr, buf, count, true, ally);
}

static ssize_t js_right_anti_deadzone_show(struct device *dev,
						 struct device_attribute *attr,
						 char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);

	if (!ally || !ally->config)
		return -ENODEV;

	return anti_deadzone_show(dev, attr, buf,
				  ally->config->right_anti_deadzone);
}

static ssize_t js_right_anti_deadzone_store(struct device *dev,
						  struct device_attribute *attr,
						  const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);

	return anti_deadzone_store(dev, attr, buf, count, false, ally);
}

ALLY_DEVICE_ATTR_RW(js_left_anti_deadzone, anti_deadzone);
ALLY_DEVICE_ATTR_RW(js_right_anti_deadzone, anti_deadzone);
ALLY_DEVICE_CONST_ATTR_RO(js_anti_deadzone_min, js_anti_deadzone_min, "0\n");
ALLY_DEVICE_CONST_ATTR_RO(js_anti_deadzone_max, js_anti_deadzone_max, "100\n");

/**
 * ally_set_joystick_resp_curve - Set joystick response curve parameters
 * @ally: ally handheld structure
 * @hdev: HID device
 * @side: Which joystick side (0=left, 1=right)
 * @curve: Response curve parameter structure
 *
 * Return: 0 on success, negative on failure
 */
static int ally_set_joystick_resp_curve(struct ally_handheld *ally,
					struct hid_device *hdev, u8 side,
					struct joystick_resp_curve *curve)
{
	u8 packet[HID_ALLY_REPORT_SIZE] = { 0 };
	int ret;
	struct ally_config *cfg = ally->config;

	if (!cfg || !cfg->resp_curve_support) {
		hid_dbg(hdev, "Response curve not supported on this device\n");
		return -EOPNOTSUPP;
	}

	if (side > 1) {
		return -EINVAL;
	}

	packet[0] = HID_ALLY_SET_REPORT_ID;
	packet[1] = HID_ALLY_FEATURE_CODE_PAGE;
	packet[2] = CMD_SET_RESP_CURVE;
	packet[3] = 0x09; /* Length */
	packet[4] = side;

	packet[5] = curve->entry_1.move;
	packet[6] = curve->entry_1.resp;
	packet[7] = curve->entry_2.move;
	packet[8] = curve->entry_2.resp;
	packet[9] = curve->entry_3.move;
	packet[10] = curve->entry_3.resp;
	packet[11] = curve->entry_4.move;
	packet[12] = curve->entry_4.resp;

	ret = ally_gamepad_send_packet(ally, hdev, packet,
				       HID_ALLY_REPORT_SIZE);
	if (ret < 0) {
		hid_err(hdev, "Failed to set joystick response curve: %d\n",
			ret);
		return ret;
	}

	mutex_lock(&cfg->config_mutex);
	if (side == 0) {
		memcpy(&cfg->left_curve, curve, sizeof(*curve));
	} else {
		memcpy(&cfg->right_curve, curve, sizeof(*curve));
	}
	mutex_unlock(&cfg->config_mutex);

	hid_dbg(hdev, "Set joystick response curve for side %d\n", side);
	return 0;
}

static int response_curve_apply(struct hid_device *hdev, struct ally_handheld *ally, bool is_left)
{
	struct ally_config *cfg = ally->config;
	struct joystick_resp_curve *curve = is_left ? &cfg->left_curve : &cfg->right_curve;

	if (!(curve->entry_1.move < curve->entry_2.move &&
	      curve->entry_2.move < curve->entry_3.move &&
	      curve->entry_3.move < curve->entry_4.move)) {
		return -EINVAL;
	}

	return ally_set_joystick_resp_curve(ally, hdev, is_left ? 0 : 1, curve);
}

static ssize_t response_curve_apply_left_store(struct device *dev,
					      struct device_attribute *attr,
					      const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	int ret;
	bool apply;

	if (!ally->config->resp_curve_support)
		return -EOPNOTSUPP;

	ret = kstrtobool(buf, &apply);
	if (ret)
		return ret;

	if (!apply)
		return count;  /* Only apply on "1" or "true" value */

	ret = response_curve_apply(hdev, ally, true);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t response_curve_apply_right_store(struct device *dev,
					       struct device_attribute *attr,
					       const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	int ret;
	bool apply;

	if (!ally->config->resp_curve_support)
		return -EOPNOTSUPP;

	ret = kstrtobool(buf, &apply);
	if (ret)
		return ret;

	if (!apply)
		return count;  /* Only apply on "1" or "true" value */

	ret = response_curve_apply(hdev, ally, false);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t response_curve_pct_show(struct device *dev,
				      struct device_attribute *attr, char *buf,
				      struct joystick_resp_curve *curve, int idx)
{
	switch (idx) {
	case 1: return sprintf(buf, "%u\n", curve->entry_1.resp);
	case 2: return sprintf(buf, "%u\n", curve->entry_2.resp);
	case 3: return sprintf(buf, "%u\n", curve->entry_3.resp);
	case 4: return sprintf(buf, "%u\n", curve->entry_4.resp);
	default: return -EINVAL;
	}
}

static ssize_t response_curve_move_show(struct device *dev,
				      struct device_attribute *attr, char *buf,
				      struct joystick_resp_curve *curve, int idx)
{
	switch (idx) {
	case 1: return sprintf(buf, "%u\n", curve->entry_1.move);
	case 2: return sprintf(buf, "%u\n", curve->entry_2.move);
	case 3: return sprintf(buf, "%u\n", curve->entry_3.move);
	case 4: return sprintf(buf, "%u\n", curve->entry_4.move);
	default: return -EINVAL;
	}
}

static ssize_t response_curve_pct_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count,
				       bool is_left, struct ally_handheld *ally,
				       int idx)
{
	struct ally_config *cfg = ally->config;
	struct joystick_resp_curve *curve = is_left ? &cfg->left_curve : &cfg->right_curve;
	u8 value;
	int ret;

	if (!cfg->resp_curve_support)
		return -EOPNOTSUPP;

	ret = kstrtou8(buf, 10, &value);
	if (ret)
		return ret;

	if (value > 100)
		return -EINVAL;

	mutex_lock(&cfg->config_mutex);
	switch (idx) {
	case 1: curve->entry_1.resp = value; break;
	case 2: curve->entry_2.resp = value; break;
	case 3: curve->entry_3.resp = value; break;
	case 4: curve->entry_4.resp = value; break;
	default: ret = -EINVAL;
	}
	mutex_unlock(&cfg->config_mutex);

	if (ret < 0)
		return ret;

	return count;
}

static ssize_t response_curve_move_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count,
				       bool is_left, struct ally_handheld *ally,
				       int idx)
{
	struct ally_config *cfg = ally->config;
	struct joystick_resp_curve *curve = is_left ? &cfg->left_curve : &cfg->right_curve;
	u8 value;
	int ret;

	if (!cfg->resp_curve_support)
		return -EOPNOTSUPP;

	ret = kstrtou8(buf, 10, &value);
	if (ret)
		return ret;

	if (value > 100)
		return -EINVAL;

	mutex_lock(&cfg->config_mutex);
	switch (idx) {
	case 1: curve->entry_1.move = value; break;
	case 2: curve->entry_2.move = value; break;
	case 3: curve->entry_3.move = value; break;
	case 4: curve->entry_4.move = value; break;
	default: ret = -EINVAL;
	}
	mutex_unlock(&cfg->config_mutex);

	if (ret < 0)
		return ret;

	return count;
}

#define DEFINE_JS_CURVE_PCT_FOPS(region, side)                             \
	static ssize_t response_curve_pct_##region##_##side##_show(              \
		struct device *dev, struct device_attribute *attr, char *buf) \
	{                                                                     \
		struct hid_device *hdev = to_hid_device(dev);                 \
		struct ally_handheld *ally = hid_get_drvdata(hdev);           \
		return response_curve_pct_show(                               \
			dev, attr, buf, &ally->config->side##_curve, region);    \
	}                                                                     \
                                                                              \
	static ssize_t response_curve_pct_##region##_##side##_store(             \
		struct device *dev, struct device_attribute *attr,            \
		const char *buf, size_t count)                                \
	{                                                                     \
		struct hid_device *hdev = to_hid_device(dev);                 \
		struct ally_handheld *ally = hid_get_drvdata(hdev);           \
		return response_curve_pct_store(dev, attr, buf, count,        \
						side##_is_left, ally, region);   \
	}

#define DEFINE_JS_CURVE_MOVE_FOPS(region, side)                            \
	static ssize_t response_curve_move_##region##_##side##_show(             \
		struct device *dev, struct device_attribute *attr, char *buf) \
	{                                                                     \
		struct hid_device *hdev = to_hid_device(dev);                 \
		struct ally_handheld *ally = hid_get_drvdata(hdev);           \
		return response_curve_move_show(                              \
			dev, attr, buf, &ally->config->side##_curve, region);    \
	}                                                                     \
                                                                              \
	static ssize_t response_curve_move_##region##_##side##_store(            \
		struct device *dev, struct device_attribute *attr,            \
		const char *buf, size_t count)                                \
	{                                                                     \
		struct hid_device *hdev = to_hid_device(dev);                 \
		struct ally_handheld *ally = hid_get_drvdata(hdev);           \
		return response_curve_move_store(dev, attr, buf, count,       \
						 side##_is_left, ally, region);  \
	}

#define DEFINE_JS_CURVE_ATTRS(region, side)                                 \
	DEFINE_JS_CURVE_PCT_FOPS(region, side)                              \
		DEFINE_JS_CURVE_MOVE_FOPS(region, side)                     \
			ALLY_DEVICE_ATTR_RW(response_curve_pct_##region##_##side, \
					    response_curve_pct_##region);        \
	ALLY_DEVICE_ATTR_RW(response_curve_move_##region##_##side,                \
			    response_curve_move_##region)

/* Helper defines for "is_left" parameter */
#define left_is_left true
#define right_is_left false

DEFINE_JS_CURVE_ATTRS(1, left);
DEFINE_JS_CURVE_ATTRS(2, left);
DEFINE_JS_CURVE_ATTRS(3, left);
DEFINE_JS_CURVE_ATTRS(4, left);

DEFINE_JS_CURVE_ATTRS(1, right);
DEFINE_JS_CURVE_ATTRS(2, right);
DEFINE_JS_CURVE_ATTRS(3, right);
DEFINE_JS_CURVE_ATTRS(4, right);

ALLY_DEVICE_ATTR_WO(response_curve_apply_left, response_curve_apply);
ALLY_DEVICE_ATTR_WO(response_curve_apply_right, response_curve_apply);

static ssize_t deadzone_left_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	struct ally_config *cfg = ally->config;

	return sprintf(buf, "%u %u\n", cfg->left_deadzone, cfg->left_outer_threshold);
}

static ssize_t deadzone_right_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	struct ally_config *cfg = ally->config;

	return sprintf(buf, "%u %u\n", cfg->right_deadzone, cfg->right_outer_threshold);
}

DEVICE_ATTR_RO(deadzone_left);
DEVICE_ATTR_RO(deadzone_right);
ALLY_DEVICE_CONST_ATTR_RO(deadzone_index, deadzone_index, "inner outer\n");

static struct attribute *axis_xy_left_attrs[] = {
	&dev_attr_joystick_left_deadzone.attr,
	&dev_attr_js_deadzone_index.attr,
	&dev_attr_js_deadzone_inner_min.attr,
	&dev_attr_js_deadzone_inner_max.attr,
	&dev_attr_js_deadzone_outer_min.attr,
	&dev_attr_js_deadzone_outer_max.attr,
	&dev_attr_js_left_anti_deadzone.attr,
	&dev_attr_js_anti_deadzone_min.attr,
	&dev_attr_js_anti_deadzone_max.attr,
	&dev_attr_response_curve_pct_1_left.attr,
	&dev_attr_response_curve_pct_2_left.attr,
	&dev_attr_response_curve_pct_3_left.attr,
	&dev_attr_response_curve_pct_4_left.attr,
	&dev_attr_response_curve_move_1_left.attr,
	&dev_attr_response_curve_move_2_left.attr,
	&dev_attr_response_curve_move_3_left.attr,
	&dev_attr_response_curve_move_4_left.attr,
	&dev_attr_response_curve_apply_left.attr,
	NULL
};

static struct attribute *axis_xy_right_attrs[] = {
	&dev_attr_joystick_right_deadzone.attr,
	&dev_attr_js_deadzone_index.attr,
	&dev_attr_js_deadzone_inner_min.attr,
	&dev_attr_js_deadzone_inner_max.attr,
	&dev_attr_js_deadzone_outer_min.attr,
	&dev_attr_js_deadzone_outer_max.attr,
	&dev_attr_js_right_anti_deadzone.attr,
	&dev_attr_js_anti_deadzone_min.attr,
	&dev_attr_js_anti_deadzone_max.attr,
	&dev_attr_response_curve_pct_1_right.attr,
	&dev_attr_response_curve_pct_2_right.attr,
	&dev_attr_response_curve_pct_3_right.attr,
	&dev_attr_response_curve_pct_4_right.attr,
	&dev_attr_response_curve_move_1_right.attr,
	&dev_attr_response_curve_move_2_right.attr,
	&dev_attr_response_curve_move_3_right.attr,
	&dev_attr_response_curve_move_4_right.attr,
	&dev_attr_response_curve_apply_right.attr,
	NULL
};

/**
 * ally_set_trigger_range - Set trigger range values
 * @hdev: HID device
 * @cfg: Ally config
 * @left_min: Left trigger minimum (0-255)
 * @left_max: Left trigger maximum (0-255)
 * @right_min: Right trigger minimum (0-255)
 * @right_max: Right trigger maximum (0-255)
 *
 * Returns 0 on success, negative error code on failure
 */
static int ally_set_trigger_range(struct hid_device *hdev,
				  struct ally_config *cfg, u8 left_min,
				  u8 left_max, u8 right_min, u8 right_max)
{
	int ret;

	if (left_min >= left_max || right_min >= right_max)
		return -EINVAL;

	ret = ally_set_dzot_ranges(hdev, cfg,
						  CMD_SET_TRIGGER_RANGE,
						  left_min, left_max, right_min,
						  right_max);
	if (ret < 0)
		return ret;

	mutex_lock(&cfg->config_mutex);
	cfg->left_trigger_min = left_min;
	cfg->left_trigger_max = left_max;
	cfg->right_trigger_min = right_min;
	cfg->right_trigger_max = right_max;
	mutex_unlock(&cfg->config_mutex);

	return 0;
}

static ssize_t trigger_range_show(struct device *dev,
				  struct device_attribute *attr, char *buf,
				  u8 min_val, u8 max_val)
{
	return sprintf(buf, "%u %u\n", min_val, max_val);
}

static ssize_t trigger_range_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count, bool is_left,
				   struct ally_config *cfg)
{
	struct hid_device *hdev = to_hid_device(dev);
	u8 min_val, max_val;
	int ret;

	ret = sscanf(buf, "%hhu %hhu", &min_val, &max_val);
	if (ret != 2)
		return -EINVAL;

	if (is_left) {
		ret = ally_set_trigger_range(hdev, cfg, min_val, max_val,
					     cfg->right_trigger_min,
					     cfg->right_trigger_max);
	} else {
		ret = ally_set_trigger_range(hdev, cfg, cfg->left_trigger_min,
					     cfg->left_trigger_max, min_val,
					     max_val);
	}

	if (ret < 0)
		return ret;

	return count;
}

static ssize_t trigger_left_deadzone_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	struct ally_config *cfg = ally->config;

	return trigger_range_show(dev, attr, buf, cfg->left_trigger_min,
				  cfg->left_trigger_max);
}

static ssize_t trigger_left_deadzone_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);

	return trigger_range_store(dev, attr, buf, count, true, ally->config);
}

static ssize_t trigger_right_deadzone_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	struct ally_config *cfg = ally->config;

	return trigger_range_show(dev, attr, buf, cfg->right_trigger_min,
				  cfg->right_trigger_max);
}

static ssize_t trigger_right_deadzone_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);

	return trigger_range_store(dev, attr, buf, count, false, ally->config);
}

ALLY_DEVICE_CONST_ATTR_RO(tr_deadzone_inner_min, deadzone_inner_min, "0\n");
ALLY_DEVICE_CONST_ATTR_RO(tr_deadzone_inner_max, deadzone_inner_max, "255\n");

ALLY_DEVICE_ATTR_RW(trigger_left_deadzone, deadzone);
ALLY_DEVICE_ATTR_RW(trigger_right_deadzone, deadzone);

static struct attribute *axis_z_left_attrs[] = {
	&dev_attr_trigger_left_deadzone.attr,
	&dev_attr_tr_deadzone_inner_min.attr,
	&dev_attr_tr_deadzone_inner_max.attr,
	NULL
};

static struct attribute *axis_z_right_attrs[] = {
	&dev_attr_trigger_right_deadzone.attr,
	&dev_attr_tr_deadzone_inner_min.attr,
	&dev_attr_tr_deadzone_inner_max.attr,
	NULL
};

static struct attribute *ally_config_attrs[] = {
	&dev_attr_xbox_controller.attr,
	&dev_attr_vibration_intensity.attr,
	NULL
};

static const struct attribute_group ally_attr_groups[] = {
	{
		.attrs = ally_config_attrs,
	},
	{
		.name = "axis_xy_left",
		.attrs = axis_xy_left_attrs,
	},
	{
		.name = "axis_xy_right",
		.attrs = axis_xy_right_attrs,
	},
	{
		.name = "axis_z_left",
		.attrs = axis_z_left_attrs,
	},
	{
		.name = "axis_z_right",
		.attrs = axis_z_right_attrs,
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

	cfg->vibration_intensity_left = 100;
	cfg->vibration_intensity_right = 100;
	cfg->vibration_active = false;

	/* Initialize default response curve values (linear) */
	cfg->left_curve.entry_1.move = 0;
	cfg->left_curve.entry_1.resp = 0;
	cfg->left_curve.entry_2.move = 33;
	cfg->left_curve.entry_2.resp = 33;
	cfg->left_curve.entry_3.move = 66;
	cfg->left_curve.entry_3.resp = 66;
	cfg->left_curve.entry_4.move = 100;
	cfg->left_curve.entry_4.resp = 100;

	cfg->right_curve.entry_1.move = 0;
	cfg->right_curve.entry_1.resp = 0;
	cfg->right_curve.entry_2.move = 33;
	cfg->right_curve.entry_2.resp = 33;
	cfg->right_curve.entry_3.move = 66;
	cfg->right_curve.entry_3.resp = 66;
	cfg->right_curve.entry_4.move = 100;
	cfg->right_curve.entry_4.resp = 100;

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
