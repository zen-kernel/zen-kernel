// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Copyright(c) 2018-2019  Realtek Corporation
 */

#include <linux/module.h>
#include <linux/usb.h>
#include "main.h"
#include "rtw8822bu.h"
#include "usb.h"

#define RTW_USB_VENDER_ID_EDIMAX	0x7392

static const struct usb_device_id rtw_8822bu_id_table[] = {
	{ USB_DEVICE_AND_INTERFACE_INFO(RTW_USB_VENDOR_ID_REALTEK,
				       RTW_USB_PRODUCT_ID_REALTEK_8812B,
				       0xff, 0xff, 0xff),
	  .driver_info = (kernel_ulong_t)&(rtw8822b_hw_spec) },
	{ USB_DEVICE_AND_INTERFACE_INFO(RTW_USB_VENDOR_ID_REALTEK,
				       RTW_USB_PRODUCT_ID_REALTEK_8822B,
				       0xff, 0xff, 0xff),
	  .driver_info = (kernel_ulong_t)&(rtw8822b_hw_spec) },
	{ USB_DEVICE_AND_INTERFACE_INFO(RTW_USB_VENDER_ID_EDIMAX,
					0xB822,
					0xff, 0xff, 0xff),
	  .driver_info = (kernel_ulong_t)&(rtw8822b_hw_spec) },
	{ USB_DEVICE_AND_INTERFACE_INFO(RTW_USB_VENDER_ID_EDIMAX,
					0xC822,
					0xff, 0xff, 0xff),
	  .driver_info = (kernel_ulong_t)&(rtw8822b_hw_spec) },
	{ USB_DEVICE(0x0b05, 0x184c),	/* ASUS AC53 Nano */
	  .driver_info = (kernel_ulong_t)&(rtw8822b_hw_spec) },
	{ USB_DEVICE(0x0b05, 0x1841),	/* ASUS AC55 B1 */
	  .driver_info = (kernel_ulong_t)&(rtw8822b_hw_spec) },
	{ USB_DEVICE(0x2001, 0x331c),	/* D-Link DWA-182 rev D1 */
	  .driver_info = (kernel_ulong_t)&(rtw8822b_hw_spec) },
	{ USB_DEVICE(0x13b1, 0x0043),	/* Linksys WUSB6400M */
	  .driver_info = (kernel_ulong_t)&(rtw8822b_hw_spec) },
	{ USB_DEVICE(0x2357, 0x012D),	/* TP-Link AC1300 T3U */
	  .driver_info = (kernel_ulong_t)&(rtw8822b_hw_spec) },
	{ USB_DEVICE(0x2357, 0x0138),	/* TP-Link AC1300 T3U */
	  .driver_info = (kernel_ulong_t)&(rtw8822b_hw_spec) },
	{},
};
MODULE_DEVICE_TABLE(usb, rtw_8822bu_id_table);

static int rtw8822bu_probe(struct usb_interface *intf,
			    const struct usb_device_id *id)
{
	return rtw_usb_probe(intf, id);
}

static struct usb_driver rtw_8822bu_driver = {
	.name = "rtw_8822bu",
	.id_table = rtw_8822bu_id_table,
	.probe = rtw8822bu_probe,
	.disconnect = rtw_usb_disconnect,
};
module_usb_driver(rtw_8822bu_driver);

MODULE_AUTHOR("Realtek Corporation");
MODULE_DESCRIPTION("Realtek 802.11ac wireless 8822bu driver");
MODULE_LICENSE("Dual BSD/GPL");
