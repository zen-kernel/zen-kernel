// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for Asus ROG laptops and Ally
 *
 *  Copyright (c) 2023 Luke Jones <luke@ljones.dev>
 */

#include "linux/delay.h"
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

static void ally_x_work(struct work_struct *work)
{
	struct ally_x_input *ally_x = container_of(work, struct ally_x_input, output_worker);
	struct ff_report *ff_report = NULL;
	bool update_qam_chord = false;
	bool update_ff = false;
	unsigned long flags;

	spin_lock_irqsave(&ally_x->lock, flags);
	update_qam_chord = ally_x->update_qam_chord;

	update_ff = ally_x->update_ff;
	if (ally_x->update_ff) {
		ff_report = kmemdup(ally_x->ff_packet, sizeof(*ally_x->ff_packet), GFP_KERNEL);
		ally_x->update_ff = false;
	}
	spin_unlock_irqrestore(&ally_x->lock, flags);

	if (update_ff && ff_report) {
		ff_report->ff.magnitude_left = ff_report->ff.magnitude_strong;
		ff_report->ff.magnitude_right = ff_report->ff.magnitude_weak;
		ally_gamepad_send_packet(ally_x->ally, ally_x->hdev,
					 (u8 *)ff_report, sizeof(*ff_report));
	}
	kfree(ff_report);

	if (update_qam_chord) {
		/*
		 * The sleeps here are required to allow steam to register the button combo.
		 */
		input_report_key(ally_x->input, BTN_MODE, 1);
		input_sync(ally_x->input);
		msleep(150);
		input_report_key(ally_x->input, BTN_A, 1);
		input_sync(ally_x->input);
		input_report_key(ally_x->input, BTN_A, 0);
		input_sync(ally_x->input);
		input_report_key(ally_x->input, BTN_MODE, 0);
		input_sync(ally_x->input);

		spin_lock_irqsave(&ally_x->lock, flags);
		ally_x->update_qam_chord = false;
		spin_unlock_irqrestore(&ally_x->lock, flags);
	}
}

static int ally_x_play_effect(struct input_dev *idev, void *data, struct ff_effect *effect)
{
	struct hid_device *hdev = input_get_drvdata(idev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	struct ally_x_input *ally_x = ally->ally_x_input;
	unsigned long flags;

	if (effect->type != FF_RUMBLE)
		return 0;

	spin_lock_irqsave(&ally_x->lock, flags);
	ally_x->ff_packet->ff.magnitude_strong = effect->u.rumble.strong_magnitude / 512;
	ally_x->ff_packet->ff.magnitude_weak = effect->u.rumble.weak_magnitude / 512;
	ally_x->update_ff = true;
	spin_unlock_irqrestore(&ally_x->lock, flags);

	if (ally_x->output_worker_initialized)
		schedule_work(&ally_x->output_worker);

	return 0;
}

/* Return true if event was handled, otherwise false */
bool ally_x_raw_event(struct ally_x_input *ally_x, struct hid_report *report, u8 *data,
			    int size)
{
	struct ally_x_input_report *in_report;
	unsigned long flags;
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
	/*
	 * The MCU used on Ally provides many devices: gamepad, keyboord, mouse, other.
	 * The AC and QAM buttons route through another interface making it difficult to
	 * use the events unless we grab those and use them here. Only works for Ally X.
	 */
	else if (data[0] == 0x5A) {
		if (ally_x->right_qam_steam_mode) {
			spin_lock_irqsave(&ally_x->lock, flags);
			if (data[1] == 0x38 && !ally_x->update_qam_chord) {
				ally_x->update_qam_chord = true;
				if (ally_x->output_worker_initialized)
					schedule_work(&ally_x->output_worker);
			}
			spin_unlock_irqrestore(&ally_x->lock, flags);
		} else {
			input_report_key(ally_x->input, KEY_F19, data[1] == 0x38);
		}
		/* Left/XBox button */
		input_report_key(ally_x->input, BTN_MODE, data[1] == 0xA6);
		/* QAM long press */
		input_report_key(ally_x->input, KEY_F17, data[1] == 0xA7);
		/* QAM long press released */
		input_report_key(ally_x->input, KEY_F18, data[1] == 0xA8);
		input_sync(ally_x->input);

		return data[1] == 0xA6 || data[1] == 0xA7 || data[1] == 0xA8 || data[1] == 0x38;
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

static ssize_t ally_x_qam_mode_show(struct device *dev, struct device_attribute *attr,
								char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	struct ally_x_input *ally_x = ally->ally_x_input;

	if (!ally_x)
		return -ENODEV;

	return sysfs_emit(buf, "%d\n", ally_x->right_qam_steam_mode);
}

static ssize_t ally_x_qam_mode_store(struct device *dev, struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	struct ally_x_input *ally_x = ally->ally_x_input;
	bool val;
	int ret;

	if (!ally_x)
		return -ENODEV;

	ret = kstrtobool(buf, &val);
	if (ret < 0)
		return ret;

	ally_x->right_qam_steam_mode = val;

	return count;
}
ALLY_DEVICE_ATTR_RW(ally_x_qam_mode, qam_mode);

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

	input_set_capability(input, EV_FF, FF_RUMBLE);
	input_ff_create_memless(input, NULL, ally_x_play_effect);

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
	struct ff_report *ff_report;
	int ret;

	ally_x = devm_kzalloc(&hdev->dev, sizeof(*ally_x), GFP_KERNEL);
	if (!ally_x)
		return -ENOMEM;

	ally_x->hdev = hdev;
	ally_x->ally = ally;
	ally->ally_x_input = ally_x;

	max_output_report_size = sizeof(struct ally_x_input_report);

	ret = ally_x_setup_input(hdev, ally_x);
	if (ret)
		goto free_ally_x;

	INIT_WORK(&ally_x->output_worker, ally_x_work);
	spin_lock_init(&ally_x->lock);
	ally_x->output_worker_initialized = true;
	ally_x->right_qam_steam_mode =
		true;

	ff_report = devm_kzalloc(&hdev->dev, sizeof(*ff_report), GFP_KERNEL);
	if (!ff_report) {
		ret = -ENOMEM;
		goto free_ally_x;
	}

	/* None of these bytes will change for the FF command for now */
	ff_report->report_id = 0x0D;
	ff_report->ff.enable = 0x0F; /* Enable all by default */
	ff_report->ff.pulse_sustain_10ms = 0xFF; /* Duration */
	ff_report->ff.pulse_release_10ms = 0x00; /* Start Delay */
	ff_report->ff.loop_count = 0xEB; /* Loop Count */
	ally_x->ff_packet = ff_report;

	if (sysfs_create_file(&hdev->dev.kobj, &dev_attr_ally_x_qam_mode.attr)) {
		ret = -ENODEV;
		goto unregister_input;
	}

	hid_info(hdev, "Registered Ally X controller using %s\n",
			dev_name(&ally_x->input->dev));

	return 0;

unregister_input:
	input_unregister_device(ally_x->input);
free_ally_x:
	devm_kfree(&hdev->dev, ally_x);
	return ret;
}

void ally_x_remove(struct hid_device *hdev, struct ally_handheld *ally)
{
	if (ally->ally_x_input) {
		sysfs_remove_file(&hdev->dev.kobj, &dev_attr_ally_x_qam_mode.attr);

		if (ally->ally_x_input->output_worker_initialized)
			cancel_work_sync(&ally->ally_x_input->output_worker);

		ally->ally_x_input = NULL;
	}
}
