/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  HID driver for Asus ROG laptops and Ally
 *
 *  Copyright (c) 2023 Luke Jones <luke@ljones.dev>
 */

 #ifndef __ASUS_ALLY_H
 #define __ASUS_ALLY_H

#include <linux/hid.h>
#include <linux/led-class-multicolor.h>
#include <linux/types.h>

#define HID_ALLY_KEYBOARD_INTF_IN 0x81
#define HID_ALLY_MOUSE_INTF_IN 0x82
#define HID_ALLY_INTF_CFG_IN 0x83
#define HID_ALLY_X_INTF_IN 0x87

#define HID_ALLY_REPORT_SIZE 64
#define HID_ALLY_GET_REPORT_ID 0x0D
#define HID_ALLY_SET_REPORT_ID 0x5A
#define HID_ALLY_SET_RGB_REPORT_ID 0x5D
#define HID_ALLY_FEATURE_CODE_PAGE 0xD1

#define HID_ALLY_X_INPUT_REPORT 0x0B
#define HID_ALLY_X_INPUT_REPORT_SIZE 16

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

enum ally_gamepad_mode {
	ALLY_GAMEPAD_MODE_GAMEPAD = 0x01,
	ALLY_GAMEPAD_MODE_KEYBOARD = 0x02,
};

static const char *const gamepad_mode_names[] = {
	[ALLY_GAMEPAD_MODE_GAMEPAD] = "gamepad",
	[ALLY_GAMEPAD_MODE_KEYBOARD] = "keyboard"
};

/* Button identifiers for the attribute system */
enum ally_button_id {
	ALLY_BTN_A,
	ALLY_BTN_B,
	ALLY_BTN_X,
	ALLY_BTN_Y,
	ALLY_BTN_LB,
	ALLY_BTN_RB,
	ALLY_BTN_DU,
	ALLY_BTN_DD,
	ALLY_BTN_DL,
	ALLY_BTN_DR,
	ALLY_BTN_J0B,
	ALLY_BTN_J1B,
	ALLY_BTN_MENU,
	ALLY_BTN_VIEW,
	ALLY_BTN_M1,
	ALLY_BTN_M2,
	ALLY_BTN_MAX
};

/* Names for the button directories in sysfs */
static const char *const ally_button_names[ALLY_BTN_MAX] = {
	[ALLY_BTN_A] = "btn_a",
	[ALLY_BTN_B] = "btn_b",
	[ALLY_BTN_X] = "btn_x",
	[ALLY_BTN_Y] = "btn_y",
	[ALLY_BTN_LB] = "btn_lb",
	[ALLY_BTN_RB] = "btn_rb",
	[ALLY_BTN_DU] = "dpad_up",
	[ALLY_BTN_DD] = "dpad_down",
	[ALLY_BTN_DL] = "dpad_left",
	[ALLY_BTN_DR] = "dpad_right",
	[ALLY_BTN_J0B] = "btn_l3",
	[ALLY_BTN_J1B] = "btn_r3",
	[ALLY_BTN_MENU] = "btn_menu",
	[ALLY_BTN_VIEW] = "btn_view",
	[ALLY_BTN_M1] = "btn_m1",
	[ALLY_BTN_M2] = "btn_m2",
};

struct ally_rgb_resume_data {
	uint8_t brightness;
	uint8_t red[4];
	uint8_t green[4];
	uint8_t blue[4];
	bool initialized;
};

struct ally_rgb_dev {
	struct ally_handheld *ally;
	struct hid_device *hdev;
	struct led_classdev_mc led_rgb_dev;
	struct work_struct work;
	bool output_worker_initialized;
	spinlock_t lock;

	bool removed;
	bool update_rgb;
	uint8_t red[4];
	uint8_t green[4];
	uint8_t blue[4];
};

/* rumble packet structure */
struct ff_data {
	u8 enable;
	u8 magnitude_left;
	u8 magnitude_right;
	u8 magnitude_strong;
	u8 magnitude_weak;
	u8 pulse_sustain_10ms;
	u8 pulse_release_10ms;
	u8 loop_count;
} __packed;

struct ff_report {
	u8 report_id;
	struct ff_data ff;
} __packed;

struct ally_x_input {
	struct ally_handheld *ally;
	struct input_dev *input;
	struct hid_device *hdev;
	spinlock_t lock;

	struct work_struct output_worker;
	bool output_worker_initialized;

	/* Set if the left QAM emits Guide/Mode and right QAM emits Home + A chord */
	bool qam_mode;
	/* Prevent multiple queued event due to the enforced delay in worker */
	bool update_qam_chord;

	struct ff_report *ff_packet;
	bool update_ff;
};

struct resp_curve_param {
	u8 move;
	u8 resp;
} __packed;

struct joystick_resp_curve {
	struct resp_curve_param entry_1;
	struct resp_curve_param entry_2;
	struct resp_curve_param entry_3;
	struct resp_curve_param entry_4;
} __packed;

/*
 * Button turbo parameters structure
 * Each button can have:
 * - turbo: Turbo press interval in multiple of 50ms (0 = disabled, 1-20 = 50ms-1000ms)
 * - toggle: Toggle interval (0 = disabled)
 */
struct button_turbo_params {
	u8 turbo;
	u8 toggle;
} __packed;

/* Collection of all button turbo settings */
struct turbo_config {
	struct button_turbo_params btn_du;   /* D-pad Up */
	struct button_turbo_params btn_dd;   /* D-pad Down */
	struct button_turbo_params btn_dl;   /* D-pad Left */
	struct button_turbo_params btn_dr;   /* D-pad Right */
	struct button_turbo_params btn_j0b;  /* Left joystick button */
	struct button_turbo_params btn_j1b;  /* Right joystick button */
	struct button_turbo_params btn_lb;   /* Left bumper */
	struct button_turbo_params btn_rb;   /* Right bumper */
	struct button_turbo_params btn_a;    /* A button */
	struct button_turbo_params btn_b;    /* B button */
	struct button_turbo_params btn_x;    /* X button */
	struct button_turbo_params btn_y;    /* Y button */
	struct button_turbo_params btn_view; /* View button */
	struct button_turbo_params btn_menu; /* Menu button */
	struct button_turbo_params btn_m2;   /* M2 button */
	struct button_turbo_params btn_m1;   /* M1 button */
};

struct ally_config {
	struct hid_device *hdev;
	/* Must be locked if the data is being changed */
	struct mutex config_mutex;
	bool initialized;

	/* Device capabilities flags */
	bool is_ally_x;
	bool xbox_controller_support;
	bool user_cal_support;
	bool turbo_support;
	bool resp_curve_support;
	bool dir_to_btn_support;
	bool gyro_support;
	bool anti_deadzone_support;

	/* Current settings */
	bool xbox_controller_enabled;
	u8 gamepad_mode;
	u8 left_deadzone;
	u8 left_outer_threshold;
	u8 right_deadzone;
	u8 right_outer_threshold;
	u8 left_anti_deadzone;
	u8 right_anti_deadzone;
	u8 left_trigger_min;
	u8 left_trigger_max;
	u8 right_trigger_min;
	u8 right_trigger_max;

	/* Vibration settings */
	u8 vibration_intensity_left;
	u8 vibration_intensity_right;
	bool vibration_active;

	struct turbo_config turbo;
	struct button_sysfs_entry *button_entries;
	void *button_mappings; /* ally_button_mapping array indexed by gamepad_mode */

	struct joystick_resp_curve left_curve;
	struct joystick_resp_curve right_curve;
};

struct ally_handheld {
	/* All read/write to IN interfaces must lock */
	struct mutex intf_mutex;
	struct hid_device *cfg_hdev;

	struct ally_rgb_dev *led_rgb_dev;

	struct ally_x_input *ally_x_input;

	struct hid_device *keyboard_hdev;
	struct input_dev *keyboard_input;

	struct ally_config *config;
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

int ally_rgb_create(struct hid_device *hdev, struct ally_handheld *ally);
void ally_rgb_remove(struct hid_device *hdev, struct ally_handheld *ally);
void ally_rgb_store_settings(struct ally_handheld *ally);
void ally_rgb_resume(struct ally_handheld *ally);

int ally_x_create(struct hid_device *hdev, struct ally_handheld *ally);
void ally_x_remove(struct hid_device *hdev, struct ally_handheld *ally);
bool ally_x_raw_event(struct ally_x_input *ally_x, struct hid_report *report, u8 *data,
			    int size);

int ally_config_create(struct hid_device *hdev, struct ally_handheld *ally);
void ally_config_remove(struct hid_device *hdev, struct ally_handheld *ally);

#define ALLY_DEVICE_ATTR_RW(_name, _sysfs_name)    \
	struct device_attribute dev_attr_##_name = \
		__ATTR(_sysfs_name, 0644, _name##_show, _name##_store)

#define ALLY_DEVICE_ATTR_RO(_name, _sysfs_name)    \
	struct device_attribute dev_attr_##_name = \
		__ATTR(_sysfs_name, 0444, _name##_show, NULL)

#define ALLY_DEVICE_ATTR_WO(_name, _sysfs_name)    \
	struct device_attribute dev_attr_##_name = \
		__ATTR(_sysfs_name, 0200, NULL, _name##_store)

#define ALLY_DEVICE_CONST_ATTR_RO(fname, sysfs_name, value)			\
	static ssize_t fname##_show(struct device *dev,				\
				   struct device_attribute *attr, char *buf)	\
	{									\
		return sprintf(buf, value);					\
	}									\
	struct device_attribute dev_attr_##fname =				\
		__ATTR(sysfs_name, 0444, fname##_show, NULL)

#endif /* __ASUS_ALLY_H */
