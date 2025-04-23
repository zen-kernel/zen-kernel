// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for Asus ROG laptops and Ally
 *
 *  Copyright (c) 2023 Luke Jones <luke@ljones.dev>
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/types.h>

#include "asus-ally.h"
#include "../hid-ids.h"

enum btn_map_type {
	BTN_TYPE_NONE = 0,
	BTN_TYPE_PAD = 0x01,
	BTN_TYPE_KB = 0x02,
	BTN_TYPE_MOUSE = 0x03,
	BTN_TYPE_MEDIA = 0x05,
};

struct btn_code_map {
	unsigned char type;
	unsigned char value;
	const char *name;
};

static const struct btn_code_map ally_btn_codes[] = {
	{ BTN_TYPE_NONE, 0x00, "NONE" },
	/* Gamepad button codes */
	{ BTN_TYPE_PAD, 0x01, "PAD_A" },
	{ BTN_TYPE_PAD, 0x02, "PAD_B" },
	{ BTN_TYPE_PAD, 0x03, "PAD_X" },
	{ BTN_TYPE_PAD, 0x04, "PAD_Y" },
	{ BTN_TYPE_PAD, 0x05, "PAD_LB" },
	{ BTN_TYPE_PAD, 0x06, "PAD_RB" },
	{ BTN_TYPE_PAD, 0x07, "PAD_LS" },
	{ BTN_TYPE_PAD, 0x08, "PAD_RS" },
	{ BTN_TYPE_PAD, 0x09, "PAD_DPAD_UP" },
	{ BTN_TYPE_PAD, 0x0A, "PAD_DPAD_DOWN" },
	{ BTN_TYPE_PAD, 0x0B, "PAD_DPAD_LEFT" },
	{ BTN_TYPE_PAD, 0x0C, "PAD_DPAD_RIGHT" },
	{ BTN_TYPE_PAD, 0x0D, "PAD_LT" },
	{ BTN_TYPE_PAD, 0x0E, "PAD_RT" },
	{ BTN_TYPE_PAD, 0x11, "PAD_VIEW" },
	{ BTN_TYPE_PAD, 0x12, "PAD_MENU" },
	{ BTN_TYPE_PAD, 0x13, "PAD_XBOX" },

	/* Keyboard button codes */
	{ BTN_TYPE_KB, 0x8E, "KB_M2" },
	{ BTN_TYPE_KB, 0x8F, "KB_M1" },
	{ BTN_TYPE_KB, 0x76, "KB_ESC" },
	{ BTN_TYPE_KB, 0x50, "KB_F1" },
	{ BTN_TYPE_KB, 0x60, "KB_F2" },
	{ BTN_TYPE_KB, 0x40, "KB_F3" },
	{ BTN_TYPE_KB, 0x0C, "KB_F4" },
	{ BTN_TYPE_KB, 0x03, "KB_F5" },
	{ BTN_TYPE_KB, 0x0B, "KB_F6" },
	{ BTN_TYPE_KB, 0x80, "KB_F7" },
	{ BTN_TYPE_KB, 0x0A, "KB_F8" },
	{ BTN_TYPE_KB, 0x01, "KB_F9" },
	{ BTN_TYPE_KB, 0x09, "KB_F10" },
	{ BTN_TYPE_KB, 0x78, "KB_F11" },
	{ BTN_TYPE_KB, 0x07, "KB_F12" },
	{ BTN_TYPE_KB, 0x18, "KB_F14" },
	{ BTN_TYPE_KB, 0x10, "KB_F15" },
	{ BTN_TYPE_KB, 0x0E, "KB_BACKTICK" },
	{ BTN_TYPE_KB, 0x16, "KB_1" },
	{ BTN_TYPE_KB, 0x1E, "KB_2" },
	{ BTN_TYPE_KB, 0x26, "KB_3" },
	{ BTN_TYPE_KB, 0x25, "KB_4" },
	{ BTN_TYPE_KB, 0x2E, "KB_5" },
	{ BTN_TYPE_KB, 0x36, "KB_6" },
	{ BTN_TYPE_KB, 0x3D, "KB_7" },
	{ BTN_TYPE_KB, 0x3E, "KB_8" },
	{ BTN_TYPE_KB, 0x46, "KB_9" },
	{ BTN_TYPE_KB, 0x45, "KB_0" },
	{ BTN_TYPE_KB, 0x4E, "KB_HYPHEN" },
	{ BTN_TYPE_KB, 0x55, "KB_EQUALS" },
	{ BTN_TYPE_KB, 0x66, "KB_BACKSPACE" },
	{ BTN_TYPE_KB, 0x0D, "KB_TAB" },
	{ BTN_TYPE_KB, 0x15, "KB_Q" },
	{ BTN_TYPE_KB, 0x1D, "KB_W" },
	{ BTN_TYPE_KB, 0x24, "KB_E" },
	{ BTN_TYPE_KB, 0x2D, "KB_R" },
	{ BTN_TYPE_KB, 0x2C, "KB_T" },
	{ BTN_TYPE_KB, 0x35, "KB_Y" },
	{ BTN_TYPE_KB, 0x3C, "KB_U" },
	{ BTN_TYPE_KB, 0x44, "KB_O" },
	{ BTN_TYPE_KB, 0x4D, "KB_P" },
	{ BTN_TYPE_KB, 0x54, "KB_LBRACKET" },
	{ BTN_TYPE_KB, 0x5B, "KB_RBRACKET" },
	{ BTN_TYPE_KB, 0x5D, "KB_BACKSLASH" },
	{ BTN_TYPE_KB, 0x58, "KB_CAPS" },
	{ BTN_TYPE_KB, 0x1C, "KB_A" },
	{ BTN_TYPE_KB, 0x1B, "KB_S" },
	{ BTN_TYPE_KB, 0x23, "KB_D" },
	{ BTN_TYPE_KB, 0x2B, "KB_F" },
	{ BTN_TYPE_KB, 0x34, "KB_G" },
	{ BTN_TYPE_KB, 0x33, "KB_H" },
	{ BTN_TYPE_KB, 0x3B, "KB_J" },
	{ BTN_TYPE_KB, 0x42, "KB_K" },
	{ BTN_TYPE_KB, 0x4B, "KB_L" },
	{ BTN_TYPE_KB, 0x4C, "KB_SEMI" },
	{ BTN_TYPE_KB, 0x52, "KB_QUOTE" },
	{ BTN_TYPE_KB, 0x5A, "KB_RET" },
	{ BTN_TYPE_KB, 0x88, "KB_LSHIFT" },
	{ BTN_TYPE_KB, 0x1A, "KB_Z" },
	{ BTN_TYPE_KB, 0x22, "KB_X" },
	{ BTN_TYPE_KB, 0x21, "KB_C" },
	{ BTN_TYPE_KB, 0x2A, "KB_V" },
	{ BTN_TYPE_KB, 0x32, "KB_B" },
	{ BTN_TYPE_KB, 0x31, "KB_N" },
	{ BTN_TYPE_KB, 0x3A, "KB_M" },
	{ BTN_TYPE_KB, 0x41, "KB_COMMA" },
	{ BTN_TYPE_KB, 0x49, "KB_PERIOD" },
	{ BTN_TYPE_KB, 0x89, "KB_RSHIFT" },
	{ BTN_TYPE_KB, 0x8C, "KB_LCTL" },
	{ BTN_TYPE_KB, 0x82, "KB_META" },
	{ BTN_TYPE_KB, 0x8A, "KB_LALT" },
	{ BTN_TYPE_KB, 0x29, "KB_SPACE" },
	{ BTN_TYPE_KB, 0x8B, "KB_RALT" },
	{ BTN_TYPE_KB, 0x84, "KB_MENU" },
	{ BTN_TYPE_KB, 0x8D, "KB_RCTL" },
	{ BTN_TYPE_KB, 0xC3, "KB_PRNTSCN" },
	{ BTN_TYPE_KB, 0x7E, "KB_SCRLCK" },
	{ BTN_TYPE_KB, 0x91, "KB_PAUSE" },
	{ BTN_TYPE_KB, 0xC2, "KB_INS" },
	{ BTN_TYPE_KB, 0x94, "KB_HOME" },
	{ BTN_TYPE_KB, 0x96, "KB_PGUP" },
	{ BTN_TYPE_KB, 0xC0, "KB_DEL" },
	{ BTN_TYPE_KB, 0x95, "KB_END" },
	{ BTN_TYPE_KB, 0x97, "KB_PGDWN" },
	{ BTN_TYPE_KB, 0x98, "KB_UP_ARROW" },
	{ BTN_TYPE_KB, 0x99, "KB_DOWN_ARROW" },
	{ BTN_TYPE_KB, 0x91, "KB_LEFT_ARROW" },
	{ BTN_TYPE_KB, 0x9B, "KB_RIGHT_ARROW" },

	/* Numpad button codes */
	{ BTN_TYPE_KB, 0x77, "NUMPAD_LOCK" },
	{ BTN_TYPE_KB, 0x90, "NUMPAD_FWDSLASH" },
	{ BTN_TYPE_KB, 0x7C, "NUMPAD_ASTERISK" },
	{ BTN_TYPE_KB, 0x7B, "NUMPAD_HYPHEN" },
	{ BTN_TYPE_KB, 0x70, "NUMPAD_0" },
	{ BTN_TYPE_KB, 0x69, "NUMPAD_1" },
	{ BTN_TYPE_KB, 0x72, "NUMPAD_2" },
	{ BTN_TYPE_KB, 0x7A, "NUMPAD_3" },
	{ BTN_TYPE_KB, 0x6B, "NUMPAD_4" },
	{ BTN_TYPE_KB, 0x73, "NUMPAD_5" },
	{ BTN_TYPE_KB, 0x74, "NUMPAD_6" },
	{ BTN_TYPE_KB, 0x6C, "NUMPAD_7" },
	{ BTN_TYPE_KB, 0x75, "NUMPAD_8" },
	{ BTN_TYPE_KB, 0x7D, "NUMPAD_9" },
	{ BTN_TYPE_KB, 0x79, "NUMPAD_PLUS" },
	{ BTN_TYPE_KB, 0x81, "NUMPAD_ENTER" },
	{ BTN_TYPE_KB, 0x71, "NUMPAD_PERIOD" },

	/* Mouse button codes */
	{ BTN_TYPE_MOUSE, 0x01, "MOUSE_LCLICK" },
	{ BTN_TYPE_MOUSE, 0x02, "MOUSE_RCLICK" },
	{ BTN_TYPE_MOUSE, 0x03, "MOUSE_MCLICK" },
	{ BTN_TYPE_MOUSE, 0x04, "MOUSE_WHEEL_UP" },
	{ BTN_TYPE_MOUSE, 0x05, "MOUSE_WHEEL_DOWN" },

	/* Media button codes */
	{ BTN_TYPE_MEDIA, 0x16, "MEDIA_SCREENSHOT" },
	{ BTN_TYPE_MEDIA, 0x19, "MEDIA_SHOW_KEYBOARD" },
	{ BTN_TYPE_MEDIA, 0x1C, "MEDIA_SHOW_DESKTOP" },
	{ BTN_TYPE_MEDIA, 0x1E, "MEDIA_START_RECORDING" },
	{ BTN_TYPE_MEDIA, 0x01, "MEDIA_MIC_OFF" },
	{ BTN_TYPE_MEDIA, 0x02, "MEDIA_VOL_DOWN" },
	{ BTN_TYPE_MEDIA, 0x03, "MEDIA_VOL_UP" },
};

static const size_t keymap_len = ARRAY_SIZE(ally_btn_codes);

/* Button pair indexes for mapping commands */
enum btn_pair_index {
	BTN_PAIR_DPAD_UPDOWN    = 0x01,
	BTN_PAIR_DPAD_LEFTRIGHT = 0x02,
	BTN_PAIR_STICK_LR       = 0x03,
	BTN_PAIR_BUMPER_LR      = 0x04,
	BTN_PAIR_AB             = 0x05,
	BTN_PAIR_XY             = 0x06,
	BTN_PAIR_VIEW_MENU      = 0x07,
	BTN_PAIR_M1M2           = 0x08,
	BTN_PAIR_TRIGGER_LR     = 0x09,
};

struct button_map {
	struct btn_code_map *remap;
	struct btn_code_map *macro;
};

struct button_pair_map {
	enum btn_pair_index pair_index;
	struct button_map first;
	struct button_map second;
};

/* Store button mapping per gamepad mode */
struct ally_button_mapping {
	struct button_pair_map button_pairs[9]; /* 9 button pairs */
};

/* Find a button code map by its name */
static const struct btn_code_map *find_button_by_name(const char *name)
{
	int i;

	for (i = 0; i < keymap_len; i++) {
		if (strcmp(ally_btn_codes[i].name, name) == 0)
			return &ally_btn_codes[i];
	}

	return NULL;
}

/* Set button mapping for a button pair */
static int ally_set_button_mapping(struct hid_device *hdev, struct ally_handheld *ally,
				  struct button_pair_map *mapping)
{
	unsigned char packet[64] = { 0 };

	if (!mapping)
		return -EINVAL;

	packet[0] = HID_ALLY_SET_REPORT_ID;
	packet[1] = HID_ALLY_FEATURE_CODE_PAGE;
	packet[2] = CMD_SET_MAPPING;
	packet[3] = mapping->pair_index;
	packet[4] = 0x2C; /* Length */

	/* First button mapping */
	packet[5] = mapping->first.remap->type;
	/* Fill in bytes 6-14 with button code */
	if (mapping->first.remap->type) {
		unsigned char btn_bytes[10] = {0};
		btn_bytes[0] = mapping->first.remap->type;

		switch (mapping->first.remap->type) {
		case BTN_TYPE_NONE:
			break;
		case BTN_TYPE_PAD:
		case BTN_TYPE_KB:
		case BTN_TYPE_MEDIA:
			btn_bytes[2] = mapping->first.remap->value;
			break;
		case BTN_TYPE_MOUSE:
			btn_bytes[4] = mapping->first.remap->value;
			break;
		}
		memcpy(&packet[5], btn_bytes, 10);
	}

	/* Macro mapping for first button if any */
	packet[15] = mapping->first.macro->type;
	if (mapping->first.macro->type) {
		unsigned char macro_bytes[11] = {0};
		macro_bytes[0] = mapping->first.macro->type;

		switch (mapping->first.macro->type) {
		case BTN_TYPE_NONE:
			break;
		case BTN_TYPE_PAD:
		case BTN_TYPE_KB:
		case BTN_TYPE_MEDIA:
			macro_bytes[2] = mapping->first.macro->value;
			break;
		case BTN_TYPE_MOUSE:
			macro_bytes[4] = mapping->first.macro->value;
			break;
		}
		memcpy(&packet[15], macro_bytes, 11);
	}

	/* Second button mapping */
	packet[27] = mapping->second.remap->type;
	/* Fill in bytes 28-36 with button code */
	if (mapping->second.remap->type) {
		unsigned char btn_bytes[10] = {0};
		btn_bytes[0] = mapping->second.remap->type;

		switch (mapping->second.remap->type) {
		case BTN_TYPE_NONE:
			break;
		case BTN_TYPE_PAD:
		case BTN_TYPE_KB:
		case BTN_TYPE_MEDIA:
			btn_bytes[2] = mapping->second.remap->value;
			break;
		case BTN_TYPE_MOUSE:
			btn_bytes[4] = mapping->second.remap->value;
			break;
		}
		memcpy(&packet[27], btn_bytes, 10);
	}

	/* Macro mapping for second button if any */
	packet[37] = mapping->second.macro->type;
	if (mapping->second.macro->type) {
		unsigned char macro_bytes[11] = {0};
		macro_bytes[0] = mapping->second.macro->type;

		switch (mapping->second.macro->type) {
		case BTN_TYPE_NONE:
			break;
		case BTN_TYPE_PAD:
		case BTN_TYPE_KB:
		case BTN_TYPE_MEDIA:
			macro_bytes[2] = mapping->second.macro->value;
			break;
		case BTN_TYPE_MOUSE:
			macro_bytes[4] = mapping->second.macro->value;
			break;
		}
		memcpy(&packet[37], macro_bytes, 11);
	}

	return ally_gamepad_send_packet(ally, hdev, packet, sizeof(packet));
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

/* Map from string name to enum value */
static int get_gamepad_mode_from_name(const char *name)
{
	int i;

	for (i = ALLY_GAMEPAD_MODE_GAMEPAD; i <= ALLY_GAMEPAD_MODE_KEYBOARD;
	     i++) {
		if (gamepad_mode_names[i] &&
		    strcmp(name, gamepad_mode_names[i]) == 0)
			return i;
	}

	return -1;
}

/**
 * ally_set_gamepad_mode - Set the gamepad operating mode
 * @ally: ally handheld structure
 * @hdev: HID device
 * @mode: Gamepad mode to set
 *
 * Returns: 0 on success, negative on failure
 */
static int ally_set_gamepad_mode(struct ally_handheld *ally,
				 struct hid_device *hdev, u8 mode)
{
	struct ally_config *cfg = ally->config;
	int ret;

	if (!cfg)
		return -EINVAL;

	if (mode < ALLY_GAMEPAD_MODE_GAMEPAD ||
	    mode > ALLY_GAMEPAD_MODE_KEYBOARD) {
		hid_err(hdev, "Invalid gamepad mode: %u\n", mode);
		return -EINVAL;
	}

	ret = ally_gamepad_send_one_byte_packet(ally, hdev,
						CMD_SET_GAMEPAD_MODE, mode);
	if (ret < 0) {
		hid_err(hdev, "Failed to set gamepad mode: %d\n", ret);
		return ret;
	}

	mutex_lock(&cfg->config_mutex);
	cfg->gamepad_mode = mode;
	mutex_unlock(&cfg->config_mutex);

	hid_info(hdev, "Set gamepad mode to %s\n", gamepad_mode_names[mode]);
	return 0;
}

static ssize_t gamepad_mode_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	struct ally_config *cfg;

	if (!ally || !ally->config)
		return -ENODEV;

	cfg = ally->config;

	if (cfg->gamepad_mode >= ALLY_GAMEPAD_MODE_GAMEPAD &&
	    cfg->gamepad_mode <= ALLY_GAMEPAD_MODE_KEYBOARD) {
		return sprintf(buf, "%s\n",
			       gamepad_mode_names[cfg->gamepad_mode]);
	} else {
		return sprintf(buf, "unknown (%u)\n", cfg->gamepad_mode);
	}
}

static ssize_t gamepad_mode_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	char mode_name[16];
	int mode;
	int ret;

	if (!ally || !ally->config)
		return -ENODEV;

	if (sscanf(buf, "%15s", mode_name) != 1)
		return -EINVAL;

	mode = get_gamepad_mode_from_name(mode_name);
	if (mode < 0) {
		hid_err(hdev, "Unknown gamepad mode: %s\n", mode_name);
		return -EINVAL;
	}

	ret = ally_set_gamepad_mode(ally, hdev, mode);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t gamepad_modes_available_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	int i;
	int len = 0;

	for (i = ALLY_GAMEPAD_MODE_GAMEPAD; i <= ALLY_GAMEPAD_MODE_KEYBOARD;
	     i++) {
		len += sprintf(buf + len, "%s ", gamepad_mode_names[i]);
	}

	/* Replace the last space with a newline */
	if (len > 0)
		buf[len - 1] = '\n';

	return len;
}

DEVICE_ATTR_RW(gamepad_mode);
DEVICE_ATTR_RO(gamepad_modes_available);

static int ally_set_default_gamepad_mode(struct hid_device *hdev,
					 struct ally_config *cfg)
{
	struct ally_handheld *ally = hid_get_drvdata(hdev);

	cfg->gamepad_mode = ALLY_GAMEPAD_MODE_GAMEPAD;

	return ally_set_gamepad_mode(ally, hdev, cfg->gamepad_mode);
}

static struct attribute *ally_config_attrs[] = {
	&dev_attr_xbox_controller.attr,
	&dev_attr_vibration_intensity.attr,
	&dev_attr_gamepad_mode.attr,
	&dev_attr_gamepad_modes_available.attr,
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
 * ally_get_turbo_params - Get turbo parameters for a specific button
 * @cfg: Ally config structure
 * @button_id: Button identifier from ally_button_id enum
 *
 * Returns: Pointer to the button's turbo parameters, or NULL if invalid
 */
static struct button_turbo_params *ally_get_turbo_params(struct ally_config *cfg,
                                                       enum ally_button_id button_id)
{
	struct turbo_config *turbo;

	if (!cfg || button_id >= ALLY_BTN_MAX)
		return NULL;

	turbo = &cfg->turbo;

	switch (button_id) {
	case ALLY_BTN_A:
		return &turbo->btn_a;
	case ALLY_BTN_B:
		return &turbo->btn_b;
	case ALLY_BTN_X:
		return &turbo->btn_x;
	case ALLY_BTN_Y:
		return &turbo->btn_y;
	case ALLY_BTN_LB:
		return &turbo->btn_lb;
	case ALLY_BTN_RB:
		return &turbo->btn_rb;
	case ALLY_BTN_DU:
		return &turbo->btn_du;
	case ALLY_BTN_DD:
		return &turbo->btn_dd;
	case ALLY_BTN_DL:
		return &turbo->btn_dl;
	case ALLY_BTN_DR:
		return &turbo->btn_dr;
	case ALLY_BTN_J0B:
		return &turbo->btn_j0b;
	case ALLY_BTN_J1B:
		return &turbo->btn_j1b;
	case ALLY_BTN_MENU:
		return &turbo->btn_menu;
	case ALLY_BTN_VIEW:
		return &turbo->btn_view;
	case ALLY_BTN_M1:
		return &turbo->btn_m1;
	case ALLY_BTN_M2:
		return &turbo->btn_m2;
	default:
		return NULL;
	}
}

/**
 * ally_set_turbo_params - Set turbo parameters for all buttons
 * @hdev: HID device
 * @cfg: Ally config structure
 *
 * Returns: 0 on success, negative on failure
 */
static int ally_set_turbo_params(struct hid_device *hdev, struct ally_config *cfg)
{
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	struct turbo_config *turbo = &cfg->turbo;
	u8 packet[HID_ALLY_REPORT_SIZE] = { 0 };
	int ret;

	if (!cfg->turbo_support) {
		hid_dbg(hdev, "Turbo functionality not supported on this device\n");
		return -EOPNOTSUPP;
	}

	packet[0] = HID_ALLY_SET_REPORT_ID;
	packet[1] = HID_ALLY_FEATURE_CODE_PAGE;
	packet[2] = CMD_SET_TURBO_PARAMS;
	packet[3] = 0x20; /* Length - 32 bytes for 16 buttons with 2 values each */

	packet[4] = turbo->btn_du.turbo;
	packet[5] = turbo->btn_du.toggle;
	packet[6] = turbo->btn_dd.turbo;
	packet[7] = turbo->btn_dd.toggle;
	packet[8] = turbo->btn_dl.turbo;
	packet[9] = turbo->btn_dl.toggle;
	packet[10] = turbo->btn_dr.turbo;
	packet[11] = turbo->btn_dr.toggle;
	packet[12] = turbo->btn_j0b.turbo;
	packet[13] = turbo->btn_j0b.toggle;
	packet[14] = turbo->btn_j1b.turbo;
	packet[15] = turbo->btn_j1b.toggle;
	packet[16] = turbo->btn_lb.turbo;
	packet[17] = turbo->btn_lb.toggle;
	packet[18] = turbo->btn_rb.turbo;
	packet[19] = turbo->btn_rb.toggle;
	packet[20] = turbo->btn_a.turbo;
	packet[21] = turbo->btn_a.toggle;
	packet[22] = turbo->btn_b.turbo;
	packet[23] = turbo->btn_b.toggle;
	packet[24] = turbo->btn_x.turbo;
	packet[25] = turbo->btn_x.toggle;
	packet[26] = turbo->btn_y.turbo;
	packet[27] = turbo->btn_y.toggle;
	packet[28] = turbo->btn_view.turbo;
	packet[29] = turbo->btn_view.toggle;
	packet[30] = turbo->btn_menu.turbo;
	packet[31] = turbo->btn_menu.toggle;
	packet[32] = turbo->btn_m2.turbo;
	packet[33] = turbo->btn_m2.toggle;
	packet[34] = turbo->btn_m1.turbo;
	packet[35] = turbo->btn_m1.toggle;

	ret = ally_gamepad_send_packet(ally, hdev, packet, HID_ALLY_REPORT_SIZE);
	if (ret < 0) {
		hid_err(hdev, "Failed to set turbo parameters: %d\n", ret);
		return ret;
	}

	return 0;
}

struct button_turbo_attr {
	struct device_attribute dev_attr;
	int button_id;
};

#define to_button_turbo_attr(x) container_of(x, struct button_turbo_attr, dev_attr)

static ssize_t button_turbo_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	struct button_turbo_attr *btn_attr = to_button_turbo_attr(attr);
	struct button_turbo_params *params;

	if (!ally->config->turbo_support)
		return sprintf(buf, "Unsupported\n");

	params = ally_get_turbo_params(ally->config, btn_attr->button_id);
	if (!params)
		return -EINVAL;

	/* Format: turbo_interval_ms[,toggle_interval_ms] */
	if (params->toggle)
		return sprintf(buf, "%d,%d\n", params->turbo * 50, params->toggle * 50);
	else
		return sprintf(buf, "%d\n", params->turbo * 50);
}

static ssize_t button_turbo_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	struct button_turbo_attr *btn_attr = to_button_turbo_attr(attr);
	struct button_turbo_params *params;
	unsigned int turbo_ms, toggle_ms = 0;
	int ret;

	if (!ally->config->turbo_support)
		return -EOPNOTSUPP;

	params = ally_get_turbo_params(ally->config, btn_attr->button_id);
	if (!params)
		return -EINVAL;

	/* Parse input: turbo_interval_ms[,toggle_interval_ms] */
	ret = sscanf(buf, "%u,%u", &turbo_ms, &toggle_ms);
	if (ret < 1)
		return -EINVAL;

	if (turbo_ms != 0 && (turbo_ms < 50 || turbo_ms > 1000))
		return -EINVAL;

	if (ret > 1 && toggle_ms > 0 && (toggle_ms < 50 || toggle_ms > 1000))
		return -EINVAL;

	mutex_lock(&ally->config->config_mutex);

	params->turbo = turbo_ms / 50;
	params->toggle = toggle_ms / 50;

	ret = ally_set_turbo_params(hdev, ally->config);

	mutex_unlock(&ally->config->config_mutex);

	if (ret < 0)
		return ret;

	return count;
}

/* Helper to create button turbo attribute */
static struct button_turbo_attr *button_turbo_attr_create(int button_id)
{
	struct button_turbo_attr *attr;

	attr = kzalloc(sizeof(*attr), GFP_KERNEL);
	if (!attr)
		return NULL;

	attr->button_id = button_id;
	sysfs_attr_init(&attr->dev_attr.attr);
	attr->dev_attr.attr.name = "turbo";
	attr->dev_attr.attr.mode = 0644;
	attr->dev_attr.show = button_turbo_show;
	attr->dev_attr.store = button_turbo_store;

	return attr;
}

/* Button remap attribute structure */
struct button_remap_attr {
	struct device_attribute dev_attr;
	enum ally_button_id button_id;
	bool is_macro;
};

#define to_button_remap_attr(x) container_of(x, struct button_remap_attr, dev_attr)

/* Get appropriate button pair index and position for a given button */
static int get_button_pair_info(enum ally_button_id button_id,
				enum btn_pair_index *pair_idx,
				bool *is_first)
{
	switch (button_id) {
	case ALLY_BTN_DU:
		*pair_idx = BTN_PAIR_DPAD_UPDOWN;
		*is_first = true;
		break;
	case ALLY_BTN_DD:
		*pair_idx = BTN_PAIR_DPAD_UPDOWN;
		*is_first = false;
		break;
	case ALLY_BTN_DL:
		*pair_idx = BTN_PAIR_DPAD_LEFTRIGHT;
		*is_first = true;
		break;
	case ALLY_BTN_DR:
		*pair_idx = BTN_PAIR_DPAD_LEFTRIGHT;
		*is_first = false;
		break;
	case ALLY_BTN_J0B:
		*pair_idx = BTN_PAIR_STICK_LR;
		*is_first = true;
		break;
	case ALLY_BTN_J1B:
		*pair_idx = BTN_PAIR_STICK_LR;
		*is_first = false;
		break;
	case ALLY_BTN_LB:
		*pair_idx = BTN_PAIR_BUMPER_LR;
		*is_first = true;
		break;
	case ALLY_BTN_RB:
		*pair_idx = BTN_PAIR_BUMPER_LR;
		*is_first = false;
		break;
	case ALLY_BTN_A:
		*pair_idx = BTN_PAIR_AB;
		*is_first = true;
		break;
	case ALLY_BTN_B:
		*pair_idx = BTN_PAIR_AB;
		*is_first = false;
		break;
	case ALLY_BTN_X:
		*pair_idx = BTN_PAIR_XY;
		*is_first = true;
		break;
	case ALLY_BTN_Y:
		*pair_idx = BTN_PAIR_XY;
		*is_first = false;
		break;
	case ALLY_BTN_VIEW:
		*pair_idx = BTN_PAIR_VIEW_MENU;
		*is_first = true;
		break;
	case ALLY_BTN_MENU:
		*pair_idx = BTN_PAIR_VIEW_MENU;
		*is_first = false;
		break;
	case ALLY_BTN_M1:
		*pair_idx = BTN_PAIR_M1M2;
		*is_first = true;
		break;
	case ALLY_BTN_M2:
		*pair_idx = BTN_PAIR_M1M2;
		*is_first = false;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static ssize_t button_remap_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	struct button_remap_attr *btn_attr = to_button_remap_attr(attr);
	struct ally_config *cfg = ally->config;
	enum ally_button_id button_id = btn_attr->button_id;
	enum btn_pair_index pair_idx;
	bool is_first;
	struct button_pair_map *pair;
	struct button_map *btn_map;
	int ret;

	if (!cfg)
		return -ENODEV;

	ret = get_button_pair_info(button_id, &pair_idx, &is_first);
	if (ret < 0)
		return ret;

	mutex_lock(&cfg->config_mutex);
	pair = &((struct ally_button_mapping
			  *)(cfg->button_mappings))[cfg->gamepad_mode]
			.button_pairs[pair_idx - 1];
	btn_map = is_first ? &pair->first : &pair->second;

	if (btn_attr->is_macro) {
		if (btn_map->macro->type == BTN_TYPE_NONE)
			ret = sprintf(buf, "NONE\n");
		else
			ret = sprintf(buf, "%s\n", btn_map->macro->name);
	} else {
		if (btn_map->remap->type == BTN_TYPE_NONE)
			ret = sprintf(buf, "NONE\n");
		else
			ret = sprintf(buf, "%s\n", btn_map->remap->name);
	}
	mutex_unlock(&cfg->config_mutex);

	return ret;
}

static ssize_t button_remap_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	struct button_remap_attr *btn_attr = to_button_remap_attr(attr);
	struct ally_config *cfg = ally->config;
	enum ally_button_id button_id = btn_attr->button_id;
	enum btn_pair_index pair_idx;
	bool is_first;
	struct button_pair_map *pair;
	struct button_map *btn_map;
	char btn_name[32];
	const struct btn_code_map *code;
	int ret;

	if (!cfg)
		return -ENODEV;

	if (sscanf(buf, "%31s", btn_name) != 1)
		return -EINVAL;

	/* Handle "NONE" specially */
	if (strcmp(btn_name, "NONE") == 0) {
		code = &ally_btn_codes[0]; /* NONE entry */
	} else {
		code = find_button_by_name(btn_name);
		if (!code)
			return -EINVAL;
	}

	ret = get_button_pair_info(button_id, &pair_idx, &is_first);
	if (ret < 0)
		return ret;

	mutex_lock(&cfg->config_mutex);
	/* Access the mapping for current gamepad mode */
	pair = &((struct ally_button_mapping
			  *)(cfg->button_mappings))[cfg->gamepad_mode]
			.button_pairs[pair_idx - 1];
	btn_map = is_first ? &pair->first : &pair->second;

	if (btn_attr->is_macro) {
		btn_map->macro = (struct btn_code_map *)code;
	} else {
		btn_map->remap = (struct btn_code_map *)code;
	}

	/* Update pair index */
	pair->pair_index = pair_idx;

	/* Send mapping to device */
	ret = ally_set_button_mapping(hdev, ally, pair);
	mutex_unlock(&cfg->config_mutex);

	if (ret < 0)
		return ret;

	return count;
}

/* Helper to create button remap attribute */
static struct button_remap_attr *button_remap_attr_create(enum ally_button_id button_id, bool is_macro)
{
	struct button_remap_attr *attr;

	attr = kzalloc(sizeof(*attr), GFP_KERNEL);
	if (!attr)
		return NULL;

	attr->button_id = button_id;
	attr->is_macro = is_macro;
	sysfs_attr_init(&attr->dev_attr.attr);
	attr->dev_attr.attr.name = is_macro ? "macro" : "remap";
	attr->dev_attr.attr.mode = 0644;
	attr->dev_attr.show = button_remap_show;
	attr->dev_attr.store = button_remap_store;

	return attr;
}

/* Structure to hold button sysfs information */
struct button_sysfs_entry {
	struct attribute_group group;
	struct attribute *attrs[4]; /* turbo + remap + macro + NULL terminator */
	struct button_turbo_attr *turbo_attr;
	struct button_remap_attr *remap_attr;
	struct button_remap_attr *macro_attr;
};

static void ally_set_default_gamepad_mapping(struct ally_button_mapping *mappings)
{
	struct ally_button_mapping *map = &mappings[ALLY_GAMEPAD_MODE_GAMEPAD];
	int i;

	/* Set all pair indexes and initialize to NONE */
	for (i = 0; i < 9; i++) {
		map->button_pairs[i].pair_index = i + 1;
		map->button_pairs[i].first.remap =
			(struct btn_code_map *)&ally_btn_codes[0];
		map->button_pairs[i].first.macro =
			(struct btn_code_map *)&ally_btn_codes[0];
		map->button_pairs[i].second.remap =
			(struct btn_code_map *)&ally_btn_codes[0];
		map->button_pairs[i].second.macro =
			(struct btn_code_map *)&ally_btn_codes[0];
	}

	/* Set direct mappings using array indices */
	map->button_pairs[BTN_PAIR_AB - 1].first.remap =
		(struct btn_code_map *)&ally_btn_codes[1]; /* PAD_A */
	map->button_pairs[BTN_PAIR_AB - 1].second.remap =
		(struct btn_code_map *)&ally_btn_codes[2]; /* PAD_B */

	map->button_pairs[BTN_PAIR_XY - 1].first.remap =
		(struct btn_code_map *)&ally_btn_codes[3]; /* PAD_X */
	map->button_pairs[BTN_PAIR_XY - 1].second.remap =
		(struct btn_code_map *)&ally_btn_codes[4]; /* PAD_Y */

	map->button_pairs[BTN_PAIR_BUMPER_LR - 1].first.remap =
		(struct btn_code_map *)&ally_btn_codes[5]; /* PAD_LB */
	map->button_pairs[BTN_PAIR_BUMPER_LR - 1].second.remap =
		(struct btn_code_map *)&ally_btn_codes[6]; /* PAD_RB */

	map->button_pairs[BTN_PAIR_STICK_LR - 1].first.remap =
		(struct btn_code_map *)&ally_btn_codes[7]; /* PAD_LS */
	map->button_pairs[BTN_PAIR_STICK_LR - 1].second.remap =
		(struct btn_code_map *)&ally_btn_codes[8]; /* PAD_RS */

	map->button_pairs[BTN_PAIR_DPAD_UPDOWN - 1].first.remap =
		(struct btn_code_map *)&ally_btn_codes[9]; /* PAD_DPAD_UP */
	map->button_pairs[BTN_PAIR_DPAD_UPDOWN - 1].second.remap =
		(struct btn_code_map *)&ally_btn_codes[10]; /* PAD_DPAD_DOWN */

	map->button_pairs[BTN_PAIR_DPAD_LEFTRIGHT - 1].first.remap =
		(struct btn_code_map *)&ally_btn_codes[11]; /* PAD_DPAD_LEFT */
	map->button_pairs[BTN_PAIR_DPAD_LEFTRIGHT - 1].second.remap =
		(struct btn_code_map *)&ally_btn_codes[12]; /* PAD_DPAD_RIGHT */

	map->button_pairs[BTN_PAIR_TRIGGER_LR - 1].first.remap =
		(struct btn_code_map *)&ally_btn_codes[13]; /* PAD_LT */
	map->button_pairs[BTN_PAIR_TRIGGER_LR - 1].second.remap =
		(struct btn_code_map *)&ally_btn_codes[14]; /* PAD_RT */

	map->button_pairs[BTN_PAIR_VIEW_MENU - 1].first.remap =
		(struct btn_code_map *)&ally_btn_codes[15]; /* PAD_VIEW */
	map->button_pairs[BTN_PAIR_VIEW_MENU - 1].second.remap =
		(struct btn_code_map *)&ally_btn_codes[16]; /* PAD_MENU */

	map->button_pairs[BTN_PAIR_M1M2 - 1].first.remap =
		(struct btn_code_map *)&ally_btn_codes[19]; /* KB_M1 */
	map->button_pairs[BTN_PAIR_M1M2 - 1].second.remap =
		(struct btn_code_map *)&ally_btn_codes[18]; /* KB_M2 */
}

static void ally_set_default_keyboard_mapping(struct ally_button_mapping *mappings)
{
	struct ally_button_mapping *map = &mappings[ALLY_GAMEPAD_MODE_KEYBOARD];
	int i;

	/* Set all pair indexes and initialize to NONE */
	for (i = 0; i < 9; i++) {
		map->button_pairs[i].pair_index = i + 1;
		map->button_pairs[i].first.remap =
			(struct btn_code_map *)&ally_btn_codes[0];
		map->button_pairs[i].first.macro =
			(struct btn_code_map *)&ally_btn_codes[0];
		map->button_pairs[i].second.remap =
			(struct btn_code_map *)&ally_btn_codes[0];
		map->button_pairs[i].second.macro =
			(struct btn_code_map *)&ally_btn_codes[0];
	}

	/* Set direct mappings using array indices */
	map->button_pairs[BTN_PAIR_AB - 1].first.remap =
		(struct btn_code_map *)&ally_btn_codes[1]; /* PAD_A */
	map->button_pairs[BTN_PAIR_AB - 1].second.remap =
		(struct btn_code_map *)&ally_btn_codes[2]; /* PAD_B */

	map->button_pairs[BTN_PAIR_XY - 1].first.remap =
		(struct btn_code_map *)&ally_btn_codes[3]; /* PAD_X */
	map->button_pairs[BTN_PAIR_XY - 1].second.remap =
		(struct btn_code_map *)&ally_btn_codes[4]; /* PAD_Y */

	map->button_pairs[BTN_PAIR_BUMPER_LR - 1].first.remap =
		(struct btn_code_map *)&ally_btn_codes[5]; /* PAD_LB */
	map->button_pairs[BTN_PAIR_BUMPER_LR - 1].second.remap =
		(struct btn_code_map *)&ally_btn_codes[6]; /* PAD_RB */

	map->button_pairs[BTN_PAIR_STICK_LR - 1].first.remap =
		(struct btn_code_map *)&ally_btn_codes[7]; /* PAD_LS */
	map->button_pairs[BTN_PAIR_STICK_LR - 1].second.remap =
		(struct btn_code_map *)&ally_btn_codes[8]; /* PAD_RS */

	map->button_pairs[BTN_PAIR_DPAD_UPDOWN - 1].first.remap =
		(struct btn_code_map *)&ally_btn_codes[9]; /* PAD_DPAD_UP */
	map->button_pairs[BTN_PAIR_DPAD_UPDOWN - 1].second.remap =
		(struct btn_code_map *)&ally_btn_codes[10]; /* PAD_DPAD_DOWN */

	map->button_pairs[BTN_PAIR_DPAD_LEFTRIGHT - 1].first.remap =
		(struct btn_code_map *)&ally_btn_codes[11]; /* PAD_DPAD_LEFT */
	map->button_pairs[BTN_PAIR_DPAD_LEFTRIGHT - 1].second.remap =
		(struct btn_code_map *)&ally_btn_codes[12]; /* PAD_DPAD_RIGHT */

	map->button_pairs[BTN_PAIR_TRIGGER_LR - 1].first.remap =
		(struct btn_code_map *)&ally_btn_codes[13]; /* PAD_LT */
	map->button_pairs[BTN_PAIR_TRIGGER_LR - 1].second.remap =
		(struct btn_code_map *)&ally_btn_codes[14]; /* PAD_RT */

	map->button_pairs[BTN_PAIR_VIEW_MENU - 1].first.remap =
		(struct btn_code_map *)&ally_btn_codes[15]; /* PAD_VIEW */
	map->button_pairs[BTN_PAIR_VIEW_MENU - 1].second.remap =
		(struct btn_code_map *)&ally_btn_codes[16]; /* PAD_MENU */

	map->button_pairs[BTN_PAIR_M1M2 - 1].first.remap =
		(struct btn_code_map *)&ally_btn_codes[19]; /* KB_M1 */
	map->button_pairs[BTN_PAIR_M1M2 - 1].second.remap =
		(struct btn_code_map *)&ally_btn_codes[18]; /* KB_M2 */
}

/**
 * ally_create_button_attributes - Create button attributes
 * @hdev: HID device
 * @cfg: Ally config structure
 *
 * Returns: 0 on success, negative on failure
 */
static int ally_create_button_attributes(struct hid_device *hdev,
					 struct ally_config *cfg)
{
	struct button_sysfs_entry *entries;
	int i, j, ret;
	struct ally_button_mapping *mappings;

	entries = devm_kcalloc(&hdev->dev, ALLY_BTN_MAX, sizeof(*entries),
			       GFP_KERNEL);
	if (!entries)
		return -ENOMEM;

	/* Allocate mappings for each gamepad mode (1-based indexing) */
	mappings = devm_kcalloc(&hdev->dev, ALLY_GAMEPAD_MODE_KEYBOARD + 1,
				sizeof(*mappings), GFP_KERNEL);
	if (!mappings) {
		ret = -ENOMEM;
		goto err_free_entries;
	}

	cfg->button_entries = entries;
	cfg->button_mappings = mappings;
	ally_set_default_gamepad_mapping(mappings);
	ally_set_default_keyboard_mapping(mappings);

	for (i = 0; i < ALLY_BTN_MAX; i++) {
		if (cfg->turbo_support) {
			entries[i].turbo_attr = button_turbo_attr_create(i);
			if (!entries[i].turbo_attr) {
				ret = -ENOMEM;
				goto err_cleanup;
			}
		}

		entries[i].remap_attr = button_remap_attr_create(i, false);
		if (!entries[i].remap_attr) {
			ret = -ENOMEM;
			goto err_cleanup;
		}

		entries[i].macro_attr = button_remap_attr_create(i, true);
		if (!entries[i].macro_attr) {
			ret = -ENOMEM;
			goto err_cleanup;
		}

		/* Set up attributes array based on what's supported */
		if (cfg->turbo_support) {
			entries[i].attrs[0] =
				&entries[i].turbo_attr->dev_attr.attr;
			entries[i].attrs[1] =
				&entries[i].remap_attr->dev_attr.attr;
			entries[i].attrs[2] =
				&entries[i].macro_attr->dev_attr.attr;
			entries[i].attrs[3] = NULL;
		} else {
			entries[i].attrs[0] =
				&entries[i].remap_attr->dev_attr.attr;
			entries[i].attrs[1] =
				&entries[i].macro_attr->dev_attr.attr;
			entries[i].attrs[2] = NULL;
		}

		entries[i].group.name = ally_button_names[i];
		entries[i].group.attrs = entries[i].attrs;

		ret = sysfs_create_group(&hdev->dev.kobj, &entries[i].group);
		if (ret < 0) {
			hid_err(hdev,
				"Failed to create sysfs group for %s: %d\n",
				ally_button_names[i], ret);
			goto err_cleanup;
		}
	}

	return 0;

err_cleanup:
	while (--i >= 0) {
		sysfs_remove_group(&hdev->dev.kobj, &entries[i].group);
		if (entries[i].turbo_attr)
			kfree(entries[i].turbo_attr);
		if (entries[i].remap_attr)
			kfree(entries[i].remap_attr);
		if (entries[i].macro_attr)
			kfree(entries[i].macro_attr);
	}

err_free_entries:
	if (mappings)
		devm_kfree(&hdev->dev, mappings);
	devm_kfree(&hdev->dev, entries);
	return ret;
}

/**
 * ally_remove_button_attributes - Remove button attributes
 * @hdev: HID device
 * @cfg: Ally config structure
 */
static void ally_remove_button_attributes(struct hid_device *hdev,
					  struct ally_config *cfg)
{
	struct button_sysfs_entry *entries;
	int i;

	if (!cfg || !cfg->button_entries)
		return;

	entries = cfg->button_entries;

	/* Remove all attribute groups */
	for (i = 0; i < ALLY_BTN_MAX; i++) {
		sysfs_remove_group(&hdev->dev.kobj, &entries[i].group);
		if (entries[i].turbo_attr)
			kfree(entries[i].turbo_attr);
		if (entries[i].remap_attr)
			kfree(entries[i].remap_attr);
		if (entries[i].macro_attr)
			kfree(entries[i].macro_attr);
	}

	if (cfg->button_mappings)
		devm_kfree(&hdev->dev, cfg->button_mappings);
	devm_kfree(&hdev->dev, entries);
}

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

	if (cfg->turbo_support) {
		ret = ally_create_button_attributes(hdev, cfg);
		if (ret < 0) {
			hid_err(hdev, "Failed to create button attributes: %d\n", ret);
			for (i = 0; i < ARRAY_SIZE(ally_attr_groups); i++)
				sysfs_remove_group(&hdev->dev.kobj, &ally_attr_groups[i]);
			goto err_free;
		}
	}

	ret = ally_set_default_gamepad_mode(hdev, cfg);
		if (ret < 0)
			hid_warn(hdev, "Failed to set default gamepad mode: %d\n", ret);

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

	if (cfg->turbo_support && cfg->button_entries)
			ally_remove_button_attributes(hdev, cfg);

	/* Remove all attribute groups in reverse order */
	for (i = ARRAY_SIZE(ally_attr_groups) - 1; i >= 0; i--)
		sysfs_remove_group(&hdev->dev.kobj, &ally_attr_groups[i]);

	ally->config = NULL;

	hid_info(hdev, "Ally configuration system removed\n");
}
