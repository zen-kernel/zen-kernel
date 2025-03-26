/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  HID driver for Asus ROG laptops and Ally
 *
 *  Copyright (c) 2023 Luke Jones <luke@ljones.dev>
 */

 #ifndef __ASUS_ALLY_H
 #define __ASUS_ALLY_H

#include <linux/hid.h>
#include <linux/types.h>

#define HID_ALLY_INTF_CFG_IN 0x83

#define HID_ALLY_REPORT_SIZE 64
#define HID_ALLY_GET_REPORT_ID 0x0D
#define HID_ALLY_SET_REPORT_ID 0x5A
#define HID_ALLY_FEATURE_CODE_PAGE 0xD1

#define HID_ALLY_X_INPUT_REPORT 0x0B

enum ally_command_codes {
    CMD_SET_GAMEPAD_MODE            = 0x01,
    CMD_SET_MAPPING                 = 0x02,
    CMD_SET_JOYSTICK_MAPPING        = 0x03,
    CMD_SET_JOYSTICK_DEADZONE       = 0x04,
    CMD_SET_TRIGGER_RANGE           = 0x05,
    CMD_SET_VIBRATION_INTENSITY     = 0x06,
    CMD_LED_CONTROL                 = 0x08,
    CMD_CHECK_READY                 = 0x0A,
    CMD_SET_XBOX_CONTROLLER         = 0x0B,
    CMD_CHECK_XBOX_SUPPORT          = 0x0C,
    CMD_USER_CAL_DATA               = 0x0D,
    CMD_CHECK_USER_CAL_SUPPORT      = 0x0E,
    CMD_SET_TURBO_PARAMS            = 0x0F,
    CMD_CHECK_TURBO_SUPPORT         = 0x10,
    CMD_CHECK_RESP_CURVE_SUPPORT    = 0x12,
    CMD_SET_RESP_CURVE              = 0x13,
    CMD_CHECK_DIR_TO_BTN_SUPPORT    = 0x14,
    CMD_SET_GYRO_PARAMS             = 0x15,
    CMD_CHECK_GYRO_TO_JOYSTICK      = 0x16,
    CMD_CHECK_ANTI_DEADZONE         = 0x17,
    CMD_SET_ANTI_DEADZONE           = 0x18,
};

struct ally_handheld {
	/* All read/write to IN interfaces must lock */
	struct mutex intf_mutex;
	struct hid_device *cfg_hdev;
};

int ally_gamepad_send_packet(struct ally_handheld *ally,
			     struct hid_device *hdev, const u8 *buf,
			     size_t len);
int ally_gamepad_send_receive_packet(struct ally_handheld *ally,
				     struct hid_device *hdev, u8 *buf,
				     size_t len);
int ally_gamepad_send_one_byte_packet(struct ally_handheld *ally,
				      struct hid_device *hdev,
				      enum ally_command_codes command,
				      u8 param);
int ally_gamepad_send_two_byte_packet(struct ally_handheld *ally,
				      struct hid_device *hdev,
				      enum ally_command_codes command,
				      u8 param1, u8 param2);
int ally_gamepad_check_ready(struct hid_device *hdev);
u8 get_endpoint_address(struct hid_device *hdev);

#endif /* __ASUS_ALLY_H */
