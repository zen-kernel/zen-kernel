// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for Asus ROG laptops and Ally
 *
 *  Copyright (c) 2023 Luke Jones <luke@ljones.dev>
 */

#include "linux/input-event-codes.h"
#include <linux/hid.h>
#include <linux/types.h>

#include "asus-ally.h"

struct ally_x_input_report {
	uint16_t x, y;
	uint16_t rx, ry;
	uint16_t z, rz;
	uint8_t buttons[4];
} __packed;

/* The hatswitch outputs integers, we use them to index this X|Y pair */
static const int hat_values[][2] = {
	{ 0, 0 }, { 0, -1 }, { 1, -1 }, { 1, 0 },   { 1, 1 },
	{ 0, 1 }, { -1, 1 }, { -1, 0 }, { -1, -1 },
};

/* Return true if event was handled, otherwise false */
bool ally_x_raw_event(struct ally_x_input *ally_x, struct hid_report *report, u8 *data,
			    int size)
{
	struct ally_x_input_report *in_report;
	u8 byte;

	if (data[0] == 0x0B) {
		in_report = (struct ally_x_input_report *)&data[1];

		input_report_abs(ally_x->input, ABS_X, in_report->x - 32768);
		input_report_abs(ally_x->input, ABS_Y, in_report->y - 32768);
		input_report_abs(ally_x->input, ABS_RX, in_report->rx - 32768);
		input_report_abs(ally_x->input, ABS_RY, in_report->ry - 32768);
		input_report_abs(ally_x->input, ABS_Z, in_report->z);
		input_report_abs(ally_x->input, ABS_RZ, in_report->rz);

		byte = in_report->buttons[0];
		input_report_key(ally_x->input, BTN_A, byte & BIT(0));
		input_report_key(ally_x->input, BTN_B, byte & BIT(1));
		input_report_key(ally_x->input, BTN_X, byte & BIT(2));
		input_report_key(ally_x->input, BTN_Y, byte & BIT(3));
		input_report_key(ally_x->input, BTN_TL, byte & BIT(4));
		input_report_key(ally_x->input, BTN_TR, byte & BIT(5));
		input_report_key(ally_x->input, BTN_SELECT, byte & BIT(6));
		input_report_key(ally_x->input, BTN_START, byte & BIT(7));

		byte = in_report->buttons[1];
		input_report_key(ally_x->input, BTN_THUMBL, byte & BIT(0));
		input_report_key(ally_x->input, BTN_THUMBR, byte & BIT(1));
		input_report_key(ally_x->input, BTN_MODE, byte & BIT(2));

		byte = in_report->buttons[2];
		input_report_abs(ally_x->input, ABS_HAT0X, hat_values[byte][0]);
		input_report_abs(ally_x->input, ABS_HAT0Y, hat_values[byte][1]);
		input_sync(ally_x->input);

		return true;
	}

	return false;
}

static struct input_dev *ally_x_alloc_input_dev(struct hid_device *hdev,
						const char *name_suffix)
{
	struct input_dev *input_dev;

	input_dev = devm_input_allocate_device(&hdev->dev);
	if (!input_dev)
		return ERR_PTR(-ENOMEM);

	input_dev->id.bustype = hdev->bus;
	input_dev->id.vendor = hdev->vendor;
	input_dev->id.product = hdev->product;
	input_dev->id.version = hdev->version;
	input_dev->uniq = hdev->uniq;
	input_dev->name = "ASUS ROG Ally X Gamepad";

	input_set_drvdata(input_dev, hdev);

	return input_dev;
}

static int ally_x_setup_input(struct hid_device *hdev, struct ally_x_input *ally_x)
{
	struct input_dev *input;
	int ret;

	input = ally_x_alloc_input_dev(hdev, NULL);
	if (IS_ERR(input))
		return PTR_ERR(input);

	input_set_abs_params(input, ABS_X, -32768, 32767, 0, 0);
	input_set_abs_params(input, ABS_Y, -32768, 32767, 0, 0);
	input_set_abs_params(input, ABS_RX, -32768, 32767, 0, 0);
	input_set_abs_params(input, ABS_RY, -32768, 32767, 0, 0);
	input_set_abs_params(input, ABS_Z, 0, 1023, 0, 0);
	input_set_abs_params(input, ABS_RZ, 0, 1023, 0, 0);
	input_set_abs_params(input, ABS_HAT0X, -1, 1, 0, 0);
	input_set_abs_params(input, ABS_HAT0Y, -1, 1, 0, 0);
	input_set_capability(input, EV_KEY, BTN_A);
	input_set_capability(input, EV_KEY, BTN_B);
	input_set_capability(input, EV_KEY, BTN_X);
	input_set_capability(input, EV_KEY, BTN_Y);
	input_set_capability(input, EV_KEY, BTN_TL);
	input_set_capability(input, EV_KEY, BTN_TR);
	input_set_capability(input, EV_KEY, BTN_SELECT);
	input_set_capability(input, EV_KEY, BTN_START);
	input_set_capability(input, EV_KEY, BTN_MODE);
	input_set_capability(input, EV_KEY, BTN_THUMBL);
	input_set_capability(input, EV_KEY, BTN_THUMBR);

	input_set_capability(input, EV_KEY, KEY_PROG1);
	input_set_capability(input, EV_KEY, KEY_PROG2);
	input_set_capability(input, EV_KEY, KEY_F16);
	input_set_capability(input, EV_KEY, KEY_F17);
	input_set_capability(input, EV_KEY, KEY_F18);
	input_set_capability(input, EV_KEY, BTN_TRIGGER_HAPPY);
	input_set_capability(input, EV_KEY, BTN_TRIGGER_HAPPY1);

	ret = input_register_device(input);
	if (ret) {
		input_unregister_device(input);
		return ret;
	}

	ally_x->input = input;

	return 0;
}

int ally_x_create(struct hid_device *hdev, struct ally_handheld *ally)
{
	uint8_t max_output_report_size;
	struct ally_x_input *ally_x;
	int ret;

	ally_x = devm_kzalloc(&hdev->dev, sizeof(*ally_x), GFP_KERNEL);
	if (!ally_x)
		return -ENOMEM;

	ally_x->hdev = hdev;
	ally->ally_x_input = ally_x;

	max_output_report_size = sizeof(struct ally_x_input_report);

	ret = ally_x_setup_input(hdev, ally_x);
	if (ret)
		goto free_ally_x;

	hid_info(hdev, "Registered Ally X controller using %s\n",
		 dev_name(&ally_x->input->dev));

	return 0;

free_ally_x:
	devm_kfree(&hdev->dev, ally_x);
	return ret;
}

void ally_x_remove(struct hid_device *hdev, struct ally_handheld *ally)
{
	ally->ally_x_input = NULL;
}
