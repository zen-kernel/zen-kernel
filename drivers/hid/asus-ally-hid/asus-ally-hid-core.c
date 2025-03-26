// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for Asus ROG laptops and Ally
 *
 *  Copyright (c) 2023 Luke Jones <luke@ljones.dev>
 */

#include "linux/mutex.h"
#include "linux/stddef.h"
#include "linux/types.h"
#include <linux/usb.h>

#include "../hid-ids.h"
#include "asus-ally.h"

#define READY_MAX_TRIES 3

static const u8 EC_INIT_STRING[] = { 0x5A, 'A', 'S', 'U', 'S', ' ', 'T', 'e','c', 'h', '.', 'I', 'n', 'c', '.', '\0' };
static const u8 FORCE_FEEDBACK_OFF[] = { 0x0D, 0x0F, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xEB };

static const struct hid_device_id rog_ally_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK, USB_DEVICE_ID_ASUSTEK_ROG_NKEY_ALLY) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK, USB_DEVICE_ID_ASUSTEK_ROG_NKEY_ALLY_X) },
	{}
};

/* Changes to ally_drvdata must lock */
static DEFINE_MUTEX(ally_data_mutex);
static struct ally_handheld ally_drvdata = {
    .cfg_hdev = NULL,
    .led_rgb_dev = NULL,
    .ally_x_input = NULL,
};

static inline int asus_dev_set_report(struct hid_device *hdev, const u8 *buf, size_t len)
{
	unsigned char *dmabuf;
	int ret;

	dmabuf = kmemdup(buf, len, GFP_KERNEL);
	if (!dmabuf)
		return -ENOMEM;

	ret = hid_hw_raw_request(hdev, buf[0], dmabuf, len, HID_FEATURE_REPORT,
					HID_REQ_SET_REPORT);
	kfree(dmabuf);

	return ret;
}

static inline int asus_dev_get_report(struct hid_device *hdev, u8 *out, size_t len)
{
	return hid_hw_raw_request(hdev, HID_ALLY_GET_REPORT_ID, out, len,
		HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
}

/**
 * ally_gamepad_send_packet - Send a raw packet to the gamepad device.
 *
 * @ally: ally handheld structure
 * @hdev: hid device
 * @buf: Buffer containing the packet data
 * @len: Length of data to send
 *
 * Return: count of data transferred, negative if error
 */
int ally_gamepad_send_packet(struct ally_handheld *ally,
			     struct hid_device *hdev, const u8 *buf, size_t len)
{
	int ret;

	mutex_lock(&ally->intf_mutex);
	ret = asus_dev_set_report(hdev, buf, len);
	mutex_unlock(&ally->intf_mutex);

	return ret;
}

/**
 * ally_gamepad_send_receive_packet - Send packet and receive response.
 *
 * @ally: ally handheld structure
 * @hdev: hid device
 * @buf: Buffer containing the packet data to send and receive response in
 * @len: Length of buffer
 *
 * Return: count of data transferred, negative if error
 */
int ally_gamepad_send_receive_packet(struct ally_handheld *ally,
				     struct hid_device *hdev, u8 *buf,
				     size_t len)
{
	int ret;

	mutex_lock(&ally->intf_mutex);
	ret = asus_dev_set_report(hdev, buf, len);
	if (ret >= 0) {
		memset(buf, 0, len);
		ret = asus_dev_get_report(hdev, buf, len);
	}
	mutex_unlock(&ally->intf_mutex);

	return ret;
}

/**
 * ally_gamepad_send_one_byte_packet - Send a one-byte payload packet.
 *
 * @ally: ally handheld structure
 * @hdev: hid device
 * @command: Command code
 * @param: Parameter byte
 *
 * Return: count of data transferred, negative if error
 */
int ally_gamepad_send_one_byte_packet(struct ally_handheld *ally,
				      struct hid_device *hdev,
				      enum ally_command_codes command, u8 param)
{
	u8 *packet;
	int ret;

	packet = kzalloc(HID_ALLY_REPORT_SIZE, GFP_KERNEL);
	if (!packet)
		return -ENOMEM;

	packet[0] = HID_ALLY_SET_REPORT_ID;
	packet[1] = HID_ALLY_FEATURE_CODE_PAGE;
	packet[2] = command;
	packet[3] = 0x01; /* Length */
	packet[4] = param;

	ret = ally_gamepad_send_packet(ally, hdev, packet,
				       HID_ALLY_REPORT_SIZE);
	kfree(packet);
	return ret;
}

/**
 * ally_gamepad_send_two_byte_packet - Send a two-byte payload packet.
 *
 * @ally: ally handheld structure
 * @hdev: hid device
 * @command: Command code
 * @param1: First parameter byte
 * @param2: Second parameter byte
 *
 * Return: count of data transferred, negative if error
 */
int ally_gamepad_send_two_byte_packet(struct ally_handheld *ally,
				      struct hid_device *hdev,
				      enum ally_command_codes command,
				      u8 param1, u8 param2)
{
	u8 *packet;
	int ret;

	packet = kzalloc(HID_ALLY_REPORT_SIZE, GFP_KERNEL);
	if (!packet)
		return -ENOMEM;

	packet[0] = HID_ALLY_SET_REPORT_ID;
	packet[1] = HID_ALLY_FEATURE_CODE_PAGE;
	packet[2] = command;
	packet[3] = 0x02; /* Length */
	packet[4] = param1;
	packet[5] = param2;

	ret = ally_gamepad_send_packet(ally, hdev, packet,
				       HID_ALLY_REPORT_SIZE);
	kfree(packet);
	return ret;
}

/*
 * This should be called before any remapping attempts, and on driver init/resume.
 */
int ally_gamepad_check_ready(struct hid_device *hdev)
{
	int ret, count;
	u8 *hidbuf;
	struct ally_handheld *ally = hid_get_drvdata(hdev);

	hidbuf = kzalloc(HID_ALLY_REPORT_SIZE, GFP_KERNEL);
	if (!hidbuf)
		return -ENOMEM;

	ret = 0;
	for (count = 0; count < READY_MAX_TRIES; count++) {
		hidbuf[0] = HID_ALLY_SET_REPORT_ID;
		hidbuf[1] = HID_ALLY_FEATURE_CODE_PAGE;
		hidbuf[2] = CMD_CHECK_READY;
		hidbuf[3] = 01;

		ret = ally_gamepad_send_receive_packet(ally, hdev, hidbuf,
						       HID_ALLY_REPORT_SIZE);
		if (ret < 0) {
			hid_err(hdev, "ROG Ally check failed: %d\n", ret);
			continue;
		}

		ret = hidbuf[2] == CMD_CHECK_READY;
		if (ret)
			break;
		usleep_range(1000, 2000);
	}

	if (count == READY_MAX_TRIES)
		hid_warn(hdev, "ROG Ally never responded with a ready\n");

	kfree(hidbuf);
	return ret;
}

u8 get_endpoint_address(struct hid_device *hdev)
{
	struct usb_host_endpoint *ep;
	struct usb_interface *intf;

	intf = to_usb_interface(hdev->dev.parent);
	if (!intf || !intf->cur_altsetting)
		return -ENODEV;

	ep = intf->cur_altsetting->endpoint;
	if (!ep)
		return -ENODEV;

	return ep->desc.bEndpointAddress;
}

/**************************************************************************************************/
/* ROG Ally driver init                                                                           */
/**************************************************************************************************/

static int cad_sequence_state = 0;
static unsigned long cad_last_event_time = 0;

/* Ally left buton emits a sequence of events: ctrl+alt+del. Capture this and emit only a single code */
static bool handle_ctrl_alt_del(struct hid_device *hdev, u8 *data, int size)
{
	if (size < 16 || data[0] != 0x01)
		return false;

	if (cad_sequence_state > 0 && time_after(jiffies, cad_last_event_time + msecs_to_jiffies(100)))
		cad_sequence_state = 0;

	cad_last_event_time = jiffies;

	switch (cad_sequence_state) {
	case 0:
		if (data[1] == 0x01 && data[2] == 0x00 && data[3] == 0x00) {
			cad_sequence_state = 1;
			data[1] = 0x00;
			return true;
		}
		break;
	case 1:
		if (data[1] == 0x05 && data[2] == 0x00 && data[3] == 0x00) {
			cad_sequence_state = 2;
			data[1] = 0x00;
			return true;
		}
		break;
	case 2:
		if (data[1] == 0x05 && data[2] == 0x00 && data[3] == 0x4c) {
			cad_sequence_state = 3;
			data[1] = 0x00;
			data[3] = 0x6F; // F20;
			return true;
		}
		break;
	case 3:
		if (data[1] == 0x04 && data[2] == 0x00 && data[3] == 0x4c) {
			cad_sequence_state = 4;
			data[1] = 0x00;
			data[1] = data[3] = 0x00;
			return true;
		}
		break;
	case 4:
		if (data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x4c) {
			cad_sequence_state = 5;
			data[3] = 0x00;
			return true;
		}
		break;
	}
	cad_sequence_state = 0;
	return false;
}

static bool handle_ally_event(struct hid_device *hdev, u8 *data, int size)
{
	struct input_dev *keyboard_input;
	int keycode = 0;

	if (data[0] == 0x5A) {
		switch (data[1]) {
		case 0x38:
			keycode = KEY_F19;
			break;
		case 0xA6:
			keycode = KEY_F16;
			break;
		case 0xA7:
			keycode = KEY_F17;
			break;
		case 0xA8:
			keycode = KEY_F18;
			break;
		default:
			return false;
		}

		keyboard_input = ally_drvdata.keyboard_input;
		if (keyboard_input) {
			input_report_key(keyboard_input, keycode, 1);
			input_sync(keyboard_input);
			input_report_key(keyboard_input, keycode, 0);
			input_sync(keyboard_input);
			return true;
		}

		memset(data, 0, size);
	}
	return false;
}

static int ally_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data,
					int size)
{
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	struct ally_x_input *ally_x;
	int ep;

	if (!ally)
		return -ENODEV;

	ep = get_endpoint_address(hdev);
	if (ep != HID_ALLY_INTF_CFG_IN && ep != HID_ALLY_X_INTF_IN && ep != HID_ALLY_KEYBOARD_INTF_IN)
		return 0;

	ally_x = ally->ally_x_input;
	if (ally_x) {
		if ((hdev->bus == BUS_USB && report->id == HID_ALLY_X_INPUT_REPORT &&
		   size == HID_ALLY_X_INPUT_REPORT_SIZE) ||
		   (data[0] == 0x5A)) {
			if (ally_x_raw_event(ally_x, report, data, size))
				return 0;
		}
	}

	switch (ep) {
	case HID_ALLY_INTF_CFG_IN:
		if (handle_ally_event(hdev, data, size))
			return 0;
		break;
	case HID_ALLY_KEYBOARD_INTF_IN:
		if (handle_ctrl_alt_del(hdev, data, size))
			return 0;
		break;
	}

	return 0;
}

static int ally_hid_init(struct hid_device *hdev)
{
	int ret;
	struct ally_handheld *ally = hid_get_drvdata(hdev);

	ret = ally_gamepad_send_packet(ally, hdev, EC_INIT_STRING, sizeof(EC_INIT_STRING));
	if (ret < 0) {
		hid_err(hdev, "Ally failed to send init command: %d\n", ret);
		goto cleanup;
	}

	/* All gamepad configuration commands must go after the ally_gamepad_check_ready() */
	ret = ally_gamepad_check_ready(hdev);
	if (ret < 0)
		goto cleanup;

	ret = ally_gamepad_send_packet(ally, hdev, FORCE_FEEDBACK_OFF, sizeof(FORCE_FEEDBACK_OFF));
	if (ret < 0)
		hid_err(hdev, "Ally failed to init force-feedback off: %d\n", ret);

cleanup:
	return ret;
}

static int ally_hid_probe(struct hid_device *hdev, const struct hid_device_id *_id)
{
	int ret, ep;

	ep = get_endpoint_address(hdev);
	if (ep < 0)
		return ep;

	/*** CRITICAL START ***/
	mutex_lock(&ally_data_mutex);
	if (ep == HID_ALLY_INTF_CFG_IN)
		ally_drvdata.cfg_hdev = hdev;
	if (ep == HID_ALLY_KEYBOARD_INTF_IN)
		ally_drvdata.keyboard_hdev = hdev;
	mutex_unlock(&ally_data_mutex);
	/*** CRITICAL END ***/

	hid_set_drvdata(hdev, &ally_drvdata);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "Parse failed\n");
		return ret;
	}

	if (ep == HID_ALLY_INTF_CFG_IN || ep == HID_ALLY_X_INTF_IN) {
		ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	} else if (ep == HID_ALLY_KEYBOARD_INTF_IN) {
		ret = hid_hw_start(hdev, HID_CONNECT_HIDINPUT | HID_CONNECT_HIDRAW);
		if (!list_empty(&hdev->inputs)) {
			struct hid_input *hidinput = list_first_entry(&hdev->inputs, struct hid_input, list);
			ally_drvdata.keyboard_input = hidinput->input;
		}
		hid_info(hdev, "Connected keyboard interface with input events\n");
	} else {
		ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
		hid_info(hdev, "Passing through HID events for endpoint: 0x%02x\n", ep);
		return 0;
	}

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "Failed to open HID device\n");
		goto err_stop;
	}

	/* Initialize MCU even before alloc */
	ret = ally_hid_init(hdev);
	if (ret < 0)
		goto err_close;

	if (ep == HID_ALLY_INTF_CFG_IN) {
		ret = ally_rgb_create(hdev, &ally_drvdata);
		if (ret < 0)
			hid_err(hdev, "Failed to create Ally gamepad LEDs.\n");
		else
			hid_info(hdev, "Created Ally RGB LED controls.\n");
	}

	if (ep == HID_ALLY_X_INTF_IN) {
		ret = ally_x_create(hdev, &ally_drvdata);
		if (ret < 0) {
			hid_err(hdev, "Failed to create Ally X gamepad device.\n");
			ally_drvdata.ally_x_input = NULL;
			goto err_close;
		} else {
			hid_info(hdev, "Created Ally X gamepad device.\n");
		}
		// Not required since we send this inputs ep through the gamepad input dev
		// if (drvdata.gamepad_cfg && drvdata.gamepad_cfg->input) {
		// 	input_unregister_device(drvdata.gamepad_cfg->input);
		// 	hid_info(hdev, "Ally X removed unrequired input dev.\n");
		// }
	}

	return 0;

err_close:
	hid_hw_close(hdev);
err_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void ally_hid_remove(struct hid_device *hdev)
{
	struct ally_handheld *ally = hid_get_drvdata(hdev);

	if (!ally)
		goto out;

	if (ally->led_rgb_dev)
		ally_rgb_remove(hdev, ally);

	if (ally->ally_x_input)
		ally_x_remove(hdev, ally);

out:
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static int ally_hid_reset_resume(struct hid_device *hdev)
{
	struct ally_handheld *ally = hid_get_drvdata(hdev);
	int ret;

	if (!ally)
		return -EINVAL;

	int ep = get_endpoint_address(hdev);
	if (ep != HID_ALLY_INTF_CFG_IN)
		return 0;

	ret = ally_hid_init(hdev);
	if (ret < 0)
		return ret;

	ally_rgb_resume(ally);

	return 0;
}

static int ally_pm_thaw(struct device *dev)
{
	struct hid_device *hdev = to_hid_device(dev);

	if (!hdev)
		return -EINVAL;

	return ally_hid_reset_resume(hdev);
}

static int ally_pm_prepare(struct device *dev)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct ally_handheld *ally = hid_get_drvdata(hdev);

	if (ally->led_rgb_dev) {
		ally_rgb_store_settings(ally);
	}

	return 0;
}

static const struct dev_pm_ops ally_pm_ops = {
	.thaw = ally_pm_thaw,
	.prepare = ally_pm_prepare,
};

MODULE_DEVICE_TABLE(hid, rog_ally_devices);

static struct hid_driver rog_ally_cfg = { .name = "asus_rog_ally",
		.id_table = rog_ally_devices,
		.probe = ally_hid_probe,
		.remove = ally_hid_remove,
		.raw_event = ally_raw_event,
		/* ALLy 1 requires this to reset device state correctly */
		.reset_resume = ally_hid_reset_resume,
		.driver = {
			.pm = &ally_pm_ops,
		}
};

static int __init rog_ally_init(void)
{
	mutex_init(&ally_drvdata.intf_mutex);
	return hid_register_driver(&rog_ally_cfg);
}

static void __exit rog_ally_exit(void)
{
	mutex_destroy(&ally_drvdata.intf_mutex);
	hid_unregister_driver(&rog_ally_cfg);
}

module_init(rog_ally_init);
module_exit(rog_ally_exit);

MODULE_AUTHOR("Luke D. Jones");
MODULE_DESCRIPTION("HID Driver for ASUS ROG Ally handeheld.");
MODULE_LICENSE("GPL");
