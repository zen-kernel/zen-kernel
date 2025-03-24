// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for Asus ROG laptops and Ally
 *
 *  Copyright (c) 2025 Luke Jones <luke@ljones.dev>
 */

#include "asus-ally.h"
#include "linux/delay.h"

static const u8 EC_MODE_LED_APPLY[] = { 0x5A, 0xB4, 0, 0, 0, 0, 0, 0, 0,
					0,    0,    0, 0, 0, 0, 0, 0 };
static const u8 EC_MODE_LED_SET[] = { 0x5A, 0xB5, 0, 0, 0, 0, 0, 0, 0,
				      0,    0,	  0, 0, 0, 0, 0, 0 };

static struct ally_rgb_resume_data resume_data;

static void ally_rgb_schedule_work(struct ally_rgb_dev *led)
{
	unsigned long flags;

	if (!led)
		return;

	spin_lock_irqsave(&led->lock, flags);
	if (!led->removed)
		schedule_work(&led->work);
	spin_unlock_irqrestore(&led->lock, flags);
}

/*
 * The RGB still has the basic 0-3 level brightness. Since the multicolour
 * brightness is being used in place, set this to max
 */
static int ally_rgb_set_bright_base_max(struct hid_device *hdev, struct ally_handheld *ally)
{
	u8 buf[] = { HID_ALLY_SET_RGB_REPORT_ID, 0xba, 0xc5, 0xc4, 0x02 };

	return ally_gamepad_send_packet(ally, hdev, buf, sizeof(buf));
}

static void ally_rgb_do_work(struct work_struct *work)
{
	struct ally_rgb_dev *led = container_of(work, struct ally_rgb_dev, work);
	unsigned long flags;
	int ret;

	bool update_needed = false;
	u8 red[4], green[4], blue[4];
	const int data_size = 12; /* 4 RGB zones × 3 colors */

	u8 buf[16] = { [0] = HID_ALLY_SET_REPORT_ID,
		       [1] = HID_ALLY_FEATURE_CODE_PAGE,
		       [2] = CMD_LED_CONTROL,
		       [3] = data_size };

	if (!led || !led->hdev)
		return;

	spin_lock_irqsave(&led->lock, flags);
	if (led->removed) {
		spin_unlock_irqrestore(&led->lock, flags);
		return;
	}

	if (led->update_rgb) {
		memcpy(red, led->red, sizeof(red));
		memcpy(green, led->green, sizeof(green));
		memcpy(blue, led->blue, sizeof(blue));
		led->update_rgb = false;
		update_needed = true;
	}
	spin_unlock_irqrestore(&led->lock, flags);

	if (!update_needed)
		return;

	for (int i = 0; i < 4; i++) {
		buf[5 + i * 3] = green[i];
		buf[6 + i * 3] = blue[i];
		buf[4 + i * 3] = red[i];
	}

	ret = ally_gamepad_send_packet(led->ally, led->hdev, buf, sizeof(buf));
	if (ret < 0)
		hid_err(led->hdev, "Ally failed to set gamepad backlight: %d\n",
			ret);
}

static void ally_rgb_set(struct led_classdev *cdev,
			 enum led_brightness brightness)
{
	struct led_classdev_mc *mc_cdev;
	struct ally_rgb_dev *led;
	int intensity, bright;
	unsigned long flags;

	mc_cdev = lcdev_to_mccdev(cdev);
	if (!mc_cdev)
		return;

	led = container_of(mc_cdev, struct ally_rgb_dev, led_rgb_dev);
	if (!led)
		return;

	led_mc_calc_color_components(mc_cdev, brightness);

	spin_lock_irqsave(&led->lock, flags);

	led->update_rgb = true;
	bright = mc_cdev->led_cdev.brightness;

	for (int i = 0; i < 4; i++) {
		intensity = mc_cdev->subled_info[i].intensity;
		led->red[i] = (((intensity >> 16) & 0xFF) * bright) / 255;
		led->green[i] = (((intensity >> 8) & 0xFF) * bright) / 255;
		led->blue[i] = ((intensity & 0xFF) * bright) / 255;
	}

	resume_data.initialized = true;

	spin_unlock_irqrestore(&led->lock, flags);

	ally_rgb_schedule_work(led);
}

static int ally_rgb_set_static_from_multi(struct hid_device *hdev,
					  struct ally_handheld *ally, u8 r,
					  u8 g, u8 b)
{
	u8 buf[17] = { HID_ALLY_SET_RGB_REPORT_ID, 0xb3 };
	int ret;

	/*
	 * Set single zone single colour based on the first LED of EC software mode.
	 * buf[2] = zone, buf[3] = mode
	 */
	buf[4] = r;
	buf[5] = g;
	buf[6] = b;

	ret = ally_gamepad_send_packet(ally, hdev, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	ret = ally_gamepad_send_packet(ally, hdev, EC_MODE_LED_APPLY,
				       sizeof(EC_MODE_LED_APPLY));
	if (ret < 0)
		return ret;

	return ally_gamepad_send_packet(ally, hdev, EC_MODE_LED_SET,
					sizeof(EC_MODE_LED_SET));
}

/*
 * Store the RGB values for restoring on resume, and set the static mode to the first LED colour
*/
void ally_rgb_store_settings(struct ally_handheld *ally)
{
	struct ally_rgb_dev *led_rgb;
	int arr_size;
	u8 r = 0, g = 0, b = 0;

	led_rgb = ally->led_rgb_dev;
	if (!led_rgb || !led_rgb->hdev)
		return;

	arr_size = sizeof(resume_data.red);

	/* Take a snapshot of current settings with locking */
	spin_lock_irq(&led_rgb->lock);
	resume_data.brightness = led_rgb->led_rgb_dev.led_cdev.brightness;
	memcpy(resume_data.red, led_rgb->red, arr_size);
	memcpy(resume_data.green, led_rgb->green, arr_size);
	memcpy(resume_data.blue, led_rgb->blue, arr_size);
	r = resume_data.red[0];
	g = resume_data.green[0];
	b = resume_data.blue[0];
	spin_unlock_irq(&led_rgb->lock);

	ally_rgb_set_static_from_multi(led_rgb->hdev, ally, r, g, b);
}

static void ally_rgb_restore_settings(struct ally_handheld *ally,
				      struct led_classdev *led_cdev,
				      struct mc_subled *mc_led_info)
{
	struct ally_rgb_dev *led_rgb_dev;
	unsigned long flags;
	int arr_size;

	led_rgb_dev = ally->led_rgb_dev;
	if (!led_rgb_dev)
		return;

	arr_size = sizeof(resume_data.red);

	spin_lock_irqsave(&led_rgb_dev->lock, flags);

	memcpy(led_rgb_dev->red, resume_data.red, arr_size);
	memcpy(led_rgb_dev->green, resume_data.green, arr_size);
	memcpy(led_rgb_dev->blue, resume_data.blue, arr_size);

	for (int i = 0; i < 4; i++) {
		mc_led_info[i].intensity = (resume_data.red[i] << 16) |
					   (resume_data.green[i] << 8) |
					   resume_data.blue[i];
	}
	led_cdev->brightness = resume_data.brightness;

	spin_unlock_irqrestore(&led_rgb_dev->lock, flags);
}

/* Set LEDs. Call after any setup. */
void ally_rgb_resume(struct ally_handheld *ally)
{
	struct ally_rgb_dev *led_rgb;
	struct led_classdev *led_cdev;
	struct mc_subled *mc_led_info;

	led_rgb = ally->led_rgb_dev;
	if (!led_rgb)
		return;

	led_cdev = &led_rgb->led_rgb_dev.led_cdev;
	mc_led_info = led_rgb->led_rgb_dev.subled_info;
	if (!led_cdev || !mc_led_info)
		return;

	if (resume_data.initialized) {
		ally_rgb_restore_settings(ally, led_cdev, mc_led_info);

		spin_lock_irq(&led_rgb->lock);
		led_rgb->update_rgb = true;
		spin_unlock_irq(&led_rgb->lock);

		ally_rgb_schedule_work(led_rgb);
		ally_rgb_set_bright_base_max(led_rgb->hdev, ally);
	}
}

static int ally_rgb_register(struct hid_device *hdev,
			     struct ally_rgb_dev *led_rgb)
{
	struct mc_subled *mc_led_info;
	struct led_classdev *led_cdev;
	int ret;

	if (!hdev || !led_rgb)
		return -EINVAL;

	mc_led_info = devm_kmalloc_array(&hdev->dev, 4, sizeof(*mc_led_info),
					 GFP_KERNEL | __GFP_ZERO);
	if (!mc_led_info)
		return -ENOMEM;

	for (int i = 0; i < 4; i++) {
		mc_led_info[i].color_index = LED_COLOR_ID_RGB;
	}

	led_rgb->led_rgb_dev.subled_info = mc_led_info;
	led_rgb->led_rgb_dev.num_colors = 4;

	led_cdev = &led_rgb->led_rgb_dev.led_cdev;
	led_cdev->brightness = 128;
	led_cdev->name = "ally:rgb:joystick_rings";
	led_cdev->max_brightness = 255;
	led_cdev->brightness_set = ally_rgb_set;

	ret = devm_led_classdev_multicolor_register(&hdev->dev,
						    &led_rgb->led_rgb_dev);
	if (ret < 0)
		hid_err(hdev, "Failed to register RGB LED device: %d\n", ret);

	return ret;
}

int ally_rgb_create(struct hid_device *hdev, struct ally_handheld *ally)
{
	struct ally_rgb_dev *led_rgb;
	int ret;

	led_rgb = devm_kzalloc(&hdev->dev, sizeof(struct ally_rgb_dev),
			       GFP_KERNEL);
	if (!led_rgb)
		return -ENOMEM;

	led_rgb->ally = ally;
	led_rgb->hdev = hdev;
	led_rgb->removed = false;
	INIT_WORK(&led_rgb->work, ally_rgb_do_work);
	spin_lock_init(&led_rgb->lock);

	/* Set the pointer in ally structure BEFORE doing any operations that might use it */
	ally->led_rgb_dev = led_rgb;

	ret = ally_rgb_register(hdev, led_rgb);
	if (ret < 0) {
		hid_err(hdev, "Failed to register RGB LED device: %d\n", ret);
		cancel_work_sync(&led_rgb->work);
		ally->led_rgb_dev = NULL; /* Reset pointer on failure */
		devm_kfree(&hdev->dev, led_rgb);
		return ret;
	}

	led_rgb->output_worker_initialized = true;

	ret = ally_rgb_set_bright_base_max(hdev, ally);
	if (ret < 0)
		hid_warn(hdev, "Failed to set maximum base brightness: %d\n",
			 ret);

	if (resume_data.initialized) {
		msleep(1500);
		spin_lock_irq(&led_rgb->lock);
		led_rgb->update_rgb = true;
		spin_unlock_irq(&led_rgb->lock);
		ally_rgb_schedule_work(led_rgb);
	}

	return 0;
}

void ally_rgb_remove(struct hid_device *hdev, struct ally_handheld *ally)
{
	struct ally_rgb_dev *led_rgb;
	unsigned long flags;
	int ep;

	ep = get_endpoint_address(hdev);
	if (ep != HID_ALLY_INTF_CFG_IN)
		return;

	led_rgb = ally->led_rgb_dev;
	if (!led_rgb)
		return;

	/* Mark as removed to prevent new work from being scheduled */
	spin_lock_irqsave(&led_rgb->lock, flags);
	if (led_rgb->removed) {
		spin_unlock_irqrestore(&led_rgb->lock, flags);
		return;
	}
	led_rgb->removed = true;
	led_rgb->output_worker_initialized = false;
	spin_unlock_irqrestore(&led_rgb->lock, flags);

	cancel_work_sync(&led_rgb->work);

	devm_led_classdev_multicolor_unregister(&hdev->dev,
						&led_rgb->led_rgb_dev);

	ally->led_rgb_dev = NULL;

	hid_info(hdev, "Removed Ally RGB interface");
}
