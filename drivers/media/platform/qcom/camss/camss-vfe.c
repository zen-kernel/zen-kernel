// SPDX-License-Identifier: GPL-2.0
/*
 * camss-vfe.c
 *
 * Qualcomm MSM Camera Subsystem - VFE (Video Front End) Module
 *
 * Copyright (c) 2013-2015, The Linux Foundation. All rights reserved.
 * Copyright (C) 2015-2018 Linaro Ltd.
 */
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/iommu.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/spinlock_types.h>
#include <linux/spinlock.h>
#include <media/media-entity.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>

#include "camss-vfe.h"
#include "camss.h"

#define MSM_VFE_NAME "msm_vfe"

/* VFE reset timeout */
#define VFE_RESET_TIMEOUT_MS 50

#define SCALER_RATIO_MAX 16

#define VFE_HW_VERSION		0x0
#define		HW_VERSION_STEPPING	0
#define		HW_VERSION_REVISION	16
#define		HW_VERSION_GENERATION	28

static const struct camss_format_info formats_rdi_8x16[] = {
	{ MEDIA_BUS_FMT_UYVY8_1X16, 8, V4L2_PIX_FMT_UYVY, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 16) },
	{ MEDIA_BUS_FMT_VYUY8_1X16, 8, V4L2_PIX_FMT_VYUY, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 16) },
	{ MEDIA_BUS_FMT_YUYV8_1X16, 8, V4L2_PIX_FMT_YUYV, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 16) },
	{ MEDIA_BUS_FMT_YVYU8_1X16, 8, V4L2_PIX_FMT_YVYU, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 16) },
	{ MEDIA_BUS_FMT_SBGGR8_1X8, 8, V4L2_PIX_FMT_SBGGR8, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 8) },
	{ MEDIA_BUS_FMT_SGBRG8_1X8, 8, V4L2_PIX_FMT_SGBRG8, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 8) },
	{ MEDIA_BUS_FMT_SGRBG8_1X8, 8, V4L2_PIX_FMT_SGRBG8, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 8) },
	{ MEDIA_BUS_FMT_SRGGB8_1X8, 8, V4L2_PIX_FMT_SRGGB8, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 8) },
	{ MEDIA_BUS_FMT_SBGGR10_1X10, 10, V4L2_PIX_FMT_SBGGR10P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 10) },
	{ MEDIA_BUS_FMT_SGBRG10_1X10, 10, V4L2_PIX_FMT_SGBRG10P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 10) },
	{ MEDIA_BUS_FMT_SGRBG10_1X10, 10, V4L2_PIX_FMT_SGRBG10P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 10) },
	{ MEDIA_BUS_FMT_SRGGB10_1X10, 10, V4L2_PIX_FMT_SRGGB10P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 10) },
	{ MEDIA_BUS_FMT_SBGGR12_1X12, 12, V4L2_PIX_FMT_SBGGR12P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 12) },
	{ MEDIA_BUS_FMT_SGBRG12_1X12, 12, V4L2_PIX_FMT_SGBRG12P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 12) },
	{ MEDIA_BUS_FMT_SGRBG12_1X12, 12, V4L2_PIX_FMT_SGRBG12P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 12) },
	{ MEDIA_BUS_FMT_SRGGB12_1X12, 12, V4L2_PIX_FMT_SRGGB12P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 12) },
	{ MEDIA_BUS_FMT_Y10_1X10, 10, V4L2_PIX_FMT_Y10P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 10) },
};

static const struct camss_format_info formats_rdi_8x96[] = {
	{ MEDIA_BUS_FMT_UYVY8_1X16, 8, V4L2_PIX_FMT_UYVY, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 16) },
	{ MEDIA_BUS_FMT_VYUY8_1X16, 8, V4L2_PIX_FMT_VYUY, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 16) },
	{ MEDIA_BUS_FMT_YUYV8_1X16, 8, V4L2_PIX_FMT_YUYV, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 16) },
	{ MEDIA_BUS_FMT_YVYU8_1X16, 8, V4L2_PIX_FMT_YVYU, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 16) },
	{ MEDIA_BUS_FMT_SBGGR8_1X8, 8, V4L2_PIX_FMT_SBGGR8, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 8) },
	{ MEDIA_BUS_FMT_SGBRG8_1X8, 8, V4L2_PIX_FMT_SGBRG8, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 8) },
	{ MEDIA_BUS_FMT_SGRBG8_1X8, 8, V4L2_PIX_FMT_SGRBG8, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 8) },
	{ MEDIA_BUS_FMT_SRGGB8_1X8, 8, V4L2_PIX_FMT_SRGGB8, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 8) },
	{ MEDIA_BUS_FMT_SBGGR10_1X10, 10, V4L2_PIX_FMT_SBGGR10P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 10) },
	{ MEDIA_BUS_FMT_SGBRG10_1X10, 10, V4L2_PIX_FMT_SGBRG10P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 10) },
	{ MEDIA_BUS_FMT_SGRBG10_1X10, 10, V4L2_PIX_FMT_SGRBG10P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 10) },
	{ MEDIA_BUS_FMT_SRGGB10_1X10, 10, V4L2_PIX_FMT_SRGGB10P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 10) },
	{ MEDIA_BUS_FMT_SBGGR10_2X8_PADHI_LE, 16, V4L2_PIX_FMT_SBGGR10, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 16) },
	{ MEDIA_BUS_FMT_SBGGR12_1X12, 12, V4L2_PIX_FMT_SBGGR12P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 12) },
	{ MEDIA_BUS_FMT_SGBRG12_1X12, 12, V4L2_PIX_FMT_SGBRG12P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 12) },
	{ MEDIA_BUS_FMT_SGRBG12_1X12, 12, V4L2_PIX_FMT_SGRBG12P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 12) },
	{ MEDIA_BUS_FMT_SRGGB12_1X12, 12, V4L2_PIX_FMT_SRGGB12P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 12) },
	{ MEDIA_BUS_FMT_SBGGR14_1X14, 14, V4L2_PIX_FMT_SBGGR14P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 14) },
	{ MEDIA_BUS_FMT_SGBRG14_1X14, 14, V4L2_PIX_FMT_SGBRG14P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 14) },
	{ MEDIA_BUS_FMT_SGRBG14_1X14, 14, V4L2_PIX_FMT_SGRBG14P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 14) },
	{ MEDIA_BUS_FMT_SRGGB14_1X14, 14, V4L2_PIX_FMT_SRGGB14P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 14) },
	{ MEDIA_BUS_FMT_Y10_1X10, 10, V4L2_PIX_FMT_Y10P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 10) },
	{ MEDIA_BUS_FMT_Y10_2X8_PADHI_LE, 16, V4L2_PIX_FMT_Y10, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 16) },
};

static const struct camss_format_info formats_rdi_845[] = {
	{ MEDIA_BUS_FMT_UYVY8_1X16, 8, V4L2_PIX_FMT_UYVY, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 16) },
	{ MEDIA_BUS_FMT_VYUY8_1X16, 8, V4L2_PIX_FMT_VYUY, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 16) },
	{ MEDIA_BUS_FMT_YUYV8_1X16, 8, V4L2_PIX_FMT_YUYV, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 16) },
	{ MEDIA_BUS_FMT_YVYU8_1X16, 8, V4L2_PIX_FMT_YVYU, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 16) },
	{ MEDIA_BUS_FMT_SBGGR8_1X8, 8, V4L2_PIX_FMT_SBGGR8, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 8) },
	{ MEDIA_BUS_FMT_SGBRG8_1X8, 8, V4L2_PIX_FMT_SGBRG8, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 8) },
	{ MEDIA_BUS_FMT_SGRBG8_1X8, 8, V4L2_PIX_FMT_SGRBG8, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 8) },
	{ MEDIA_BUS_FMT_SRGGB8_1X8, 8, V4L2_PIX_FMT_SRGGB8, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 8) },
	{ MEDIA_BUS_FMT_SBGGR10_1X10, 10, V4L2_PIX_FMT_SBGGR10P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 10) },
	{ MEDIA_BUS_FMT_SGBRG10_1X10, 10, V4L2_PIX_FMT_SGBRG10P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 10) },
	{ MEDIA_BUS_FMT_SGRBG10_1X10, 10, V4L2_PIX_FMT_SGRBG10P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 10) },
	{ MEDIA_BUS_FMT_SRGGB10_1X10, 10, V4L2_PIX_FMT_SRGGB10P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 10) },
	{ MEDIA_BUS_FMT_SBGGR10_2X8_PADHI_LE, 16, V4L2_PIX_FMT_SBGGR10, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 16) },
	{ MEDIA_BUS_FMT_SBGGR12_1X12, 12, V4L2_PIX_FMT_SBGGR12P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 12) },
	{ MEDIA_BUS_FMT_SGBRG12_1X12, 12, V4L2_PIX_FMT_SGBRG12P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 12) },
	{ MEDIA_BUS_FMT_SGRBG12_1X12, 12, V4L2_PIX_FMT_SGRBG12P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 12) },
	{ MEDIA_BUS_FMT_SRGGB12_1X12, 12, V4L2_PIX_FMT_SRGGB12P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 12) },
	{ MEDIA_BUS_FMT_SBGGR14_1X14, 14, V4L2_PIX_FMT_SBGGR14P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 14) },
	{ MEDIA_BUS_FMT_SGBRG14_1X14, 14, V4L2_PIX_FMT_SGBRG14P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 14) },
	{ MEDIA_BUS_FMT_SGRBG14_1X14, 14, V4L2_PIX_FMT_SGRBG14P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 14) },
	{ MEDIA_BUS_FMT_SRGGB14_1X14, 14, V4L2_PIX_FMT_SRGGB14P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 14) },
	{ MEDIA_BUS_FMT_Y8_1X8, 8, V4L2_PIX_FMT_GREY, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 8) },
	{ MEDIA_BUS_FMT_Y10_1X10, 10, V4L2_PIX_FMT_Y10P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 10) },
	{ MEDIA_BUS_FMT_Y10_2X8_PADHI_LE, 16, V4L2_PIX_FMT_Y10, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 16) },
};

static const struct camss_format_info formats_pix_8x16[] = {
	{ MEDIA_BUS_FMT_YUYV8_1_5X8, 8, V4L2_PIX_FMT_NV12, 1,
	  PER_PLANE_DATA(0, 1, 1, 2, 3, 8) },
	{ MEDIA_BUS_FMT_YVYU8_1_5X8, 8, V4L2_PIX_FMT_NV12, 1,
	  PER_PLANE_DATA(0, 1, 1, 2, 3, 8) },
	{ MEDIA_BUS_FMT_UYVY8_1_5X8, 8, V4L2_PIX_FMT_NV12, 1,
	  PER_PLANE_DATA(0, 1, 1, 2, 3, 8) },
	{ MEDIA_BUS_FMT_VYUY8_1_5X8, 8, V4L2_PIX_FMT_NV12, 1,
	  PER_PLANE_DATA(0, 1, 1, 2, 3, 8) },
	{ MEDIA_BUS_FMT_YUYV8_1_5X8, 8, V4L2_PIX_FMT_NV21, 1,
	  PER_PLANE_DATA(0, 1, 1, 2, 3, 8) },
	{ MEDIA_BUS_FMT_YVYU8_1_5X8, 8, V4L2_PIX_FMT_NV21, 1,
	  PER_PLANE_DATA(0, 1, 1, 2, 3, 8) },
	{ MEDIA_BUS_FMT_UYVY8_1_5X8, 8, V4L2_PIX_FMT_NV21, 1,
	  PER_PLANE_DATA(0, 1, 1, 2, 3, 8) },
	{ MEDIA_BUS_FMT_VYUY8_1_5X8, 8, V4L2_PIX_FMT_NV21, 1,
	  PER_PLANE_DATA(0, 1, 1, 2, 3, 8) },
	{ MEDIA_BUS_FMT_YUYV8_1X16, 8, V4L2_PIX_FMT_NV16, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 2, 8) },
	{ MEDIA_BUS_FMT_YVYU8_1X16, 8, V4L2_PIX_FMT_NV16, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 2, 8) },
	{ MEDIA_BUS_FMT_UYVY8_1X16, 8, V4L2_PIX_FMT_NV16, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 2, 8) },
	{ MEDIA_BUS_FMT_VYUY8_1X16, 8, V4L2_PIX_FMT_NV16, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 2, 8) },
	{ MEDIA_BUS_FMT_YUYV8_1X16, 8, V4L2_PIX_FMT_NV61, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 2, 8) },
	{ MEDIA_BUS_FMT_YVYU8_1X16, 8, V4L2_PIX_FMT_NV61, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 2, 8) },
	{ MEDIA_BUS_FMT_UYVY8_1X16, 8, V4L2_PIX_FMT_NV61, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 2, 8) },
	{ MEDIA_BUS_FMT_VYUY8_1X16, 8, V4L2_PIX_FMT_NV61, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 2, 8) },
};

static const struct camss_format_info formats_pix_8x96[] = {
	{ MEDIA_BUS_FMT_YUYV8_1_5X8, 8, V4L2_PIX_FMT_NV12, 1,
	  PER_PLANE_DATA(0, 1, 1, 2, 3, 8) },
	{ MEDIA_BUS_FMT_YVYU8_1_5X8, 8, V4L2_PIX_FMT_NV12, 1,
	  PER_PLANE_DATA(0, 1, 1, 2, 3, 8) },
	{ MEDIA_BUS_FMT_UYVY8_1_5X8, 8, V4L2_PIX_FMT_NV12, 1,
	  PER_PLANE_DATA(0, 1, 1, 2, 3, 8) },
	{ MEDIA_BUS_FMT_VYUY8_1_5X8, 8, V4L2_PIX_FMT_NV12, 1,
	  PER_PLANE_DATA(0, 1, 1, 2, 3, 8) },
	{ MEDIA_BUS_FMT_YUYV8_1_5X8, 8, V4L2_PIX_FMT_NV21, 1,
	  PER_PLANE_DATA(0, 1, 1, 2, 3, 8) },
	{ MEDIA_BUS_FMT_YVYU8_1_5X8, 8, V4L2_PIX_FMT_NV21, 1,
	  PER_PLANE_DATA(0, 1, 1, 2, 3, 8) },
	{ MEDIA_BUS_FMT_UYVY8_1_5X8, 8, V4L2_PIX_FMT_NV21, 1,
	  PER_PLANE_DATA(0, 1, 1, 2, 3, 8) },
	{ MEDIA_BUS_FMT_VYUY8_1_5X8, 8, V4L2_PIX_FMT_NV21, 1,
	  PER_PLANE_DATA(0, 1, 1, 2, 3, 8) },
	{ MEDIA_BUS_FMT_YUYV8_1X16, 8, V4L2_PIX_FMT_NV16, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 2, 8) },
	{ MEDIA_BUS_FMT_YVYU8_1X16, 8, V4L2_PIX_FMT_NV16, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 2, 8) },
	{ MEDIA_BUS_FMT_UYVY8_1X16, 8, V4L2_PIX_FMT_NV16, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 2, 8) },
	{ MEDIA_BUS_FMT_VYUY8_1X16, 8, V4L2_PIX_FMT_NV16, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 2, 8) },
	{ MEDIA_BUS_FMT_YUYV8_1X16, 8, V4L2_PIX_FMT_NV61, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 2, 8) },
	{ MEDIA_BUS_FMT_YVYU8_1X16, 8, V4L2_PIX_FMT_NV61, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 2, 8) },
	{ MEDIA_BUS_FMT_UYVY8_1X16, 8, V4L2_PIX_FMT_NV61, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 2, 8) },
	{ MEDIA_BUS_FMT_VYUY8_1X16, 8, V4L2_PIX_FMT_NV61, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 2, 8) },
	{ MEDIA_BUS_FMT_UYVY8_1X16, 8, V4L2_PIX_FMT_UYVY, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 16) },
	{ MEDIA_BUS_FMT_VYUY8_1X16, 8, V4L2_PIX_FMT_VYUY, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 16) },
	{ MEDIA_BUS_FMT_YUYV8_1X16, 8, V4L2_PIX_FMT_YUYV, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 16) },
	{ MEDIA_BUS_FMT_YVYU8_1X16, 8, V4L2_PIX_FMT_YVYU, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 16) },
};

const struct camss_formats vfe_formats_rdi_8x16 = {
	.nformats = ARRAY_SIZE(formats_rdi_8x16),
	.formats = formats_rdi_8x16
};

const struct camss_formats vfe_formats_pix_8x16 = {
	.nformats = ARRAY_SIZE(formats_pix_8x16),
	.formats = formats_pix_8x16
};

const struct camss_formats vfe_formats_rdi_8x96 = {
	.nformats = ARRAY_SIZE(formats_rdi_8x96),
	.formats = formats_rdi_8x96
};

const struct camss_formats vfe_formats_pix_8x96 = {
	.nformats = ARRAY_SIZE(formats_pix_8x96),
	.formats = formats_pix_8x96
};

const struct camss_formats vfe_formats_rdi_845 = {
	.nformats = ARRAY_SIZE(formats_rdi_845),
	.formats = formats_rdi_845
};

/* TODO: Replace with pix formats */
const struct camss_formats vfe_formats_pix_845 = {
	.nformats = ARRAY_SIZE(formats_rdi_845),
	.formats = formats_rdi_845
};

static u32 vfe_src_pad_code(struct vfe_line *line, u32 sink_code,
			    unsigned int index, u32 src_req_code)
{
	struct vfe_device *vfe = to_vfe(line);

	switch (vfe->camss->res->version) {
	case CAMSS_8x16:
	case CAMSS_8x53:
		switch (sink_code) {
		case MEDIA_BUS_FMT_YUYV8_1X16:
		{
			u32 src_code[] = {
				MEDIA_BUS_FMT_YUYV8_1X16,
				MEDIA_BUS_FMT_YUYV8_1_5X8,
			};

			return camss_format_find_code(src_code, ARRAY_SIZE(src_code),
						      index, src_req_code);
		}
		case MEDIA_BUS_FMT_YVYU8_1X16:
		{
			u32 src_code[] = {
				MEDIA_BUS_FMT_YVYU8_1X16,
				MEDIA_BUS_FMT_YVYU8_1_5X8,
			};

			return camss_format_find_code(src_code, ARRAY_SIZE(src_code),
						      index, src_req_code);
		}
		case MEDIA_BUS_FMT_UYVY8_1X16:
		{
			u32 src_code[] = {
				MEDIA_BUS_FMT_UYVY8_1X16,
				MEDIA_BUS_FMT_UYVY8_1_5X8,
			};

			return camss_format_find_code(src_code, ARRAY_SIZE(src_code),
						      index, src_req_code);
		}
		case MEDIA_BUS_FMT_VYUY8_1X16:
		{
			u32 src_code[] = {
				MEDIA_BUS_FMT_VYUY8_1X16,
				MEDIA_BUS_FMT_VYUY8_1_5X8,
			};

			return camss_format_find_code(src_code, ARRAY_SIZE(src_code),
						      index, src_req_code);
		}
		default:
			if (index > 0)
				return 0;

			return sink_code;
		}
		break;
	case CAMSS_660:
	case CAMSS_7280:
	case CAMSS_8x96:
	case CAMSS_8250:
	case CAMSS_8280XP:
	case CAMSS_845:
	case CAMSS_8550:
		switch (sink_code) {
		case MEDIA_BUS_FMT_YUYV8_1X16:
		{
			u32 src_code[] = {
				MEDIA_BUS_FMT_YUYV8_1X16,
				MEDIA_BUS_FMT_YVYU8_1X16,
				MEDIA_BUS_FMT_UYVY8_1X16,
				MEDIA_BUS_FMT_VYUY8_1X16,
				MEDIA_BUS_FMT_YUYV8_1_5X8,
			};

			return camss_format_find_code(src_code, ARRAY_SIZE(src_code),
						      index, src_req_code);
		}
		case MEDIA_BUS_FMT_YVYU8_1X16:
		{
			u32 src_code[] = {
				MEDIA_BUS_FMT_YVYU8_1X16,
				MEDIA_BUS_FMT_YUYV8_1X16,
				MEDIA_BUS_FMT_UYVY8_1X16,
				MEDIA_BUS_FMT_VYUY8_1X16,
				MEDIA_BUS_FMT_YVYU8_1_5X8,
			};

			return camss_format_find_code(src_code, ARRAY_SIZE(src_code),
						      index, src_req_code);
		}
		case MEDIA_BUS_FMT_UYVY8_1X16:
		{
			u32 src_code[] = {
				MEDIA_BUS_FMT_UYVY8_1X16,
				MEDIA_BUS_FMT_YUYV8_1X16,
				MEDIA_BUS_FMT_YVYU8_1X16,
				MEDIA_BUS_FMT_VYUY8_1X16,
				MEDIA_BUS_FMT_UYVY8_1_5X8,
			};

			return camss_format_find_code(src_code, ARRAY_SIZE(src_code),
						      index, src_req_code);
		}
		case MEDIA_BUS_FMT_VYUY8_1X16:
		{
			u32 src_code[] = {
				MEDIA_BUS_FMT_VYUY8_1X16,
				MEDIA_BUS_FMT_YUYV8_1X16,
				MEDIA_BUS_FMT_YVYU8_1X16,
				MEDIA_BUS_FMT_UYVY8_1X16,
				MEDIA_BUS_FMT_VYUY8_1_5X8,
			};

			return camss_format_find_code(src_code, ARRAY_SIZE(src_code),
						      index, src_req_code);
		}
		default:
			if (index > 0)
				return 0;

			return sink_code;
		}
		break;
	default:
		WARN(1, "Unsupported HW version: %x\n",
		     vfe->camss->res->version);
		break;
	}
	return 0;
}

/*
 * vfe_hw_version - Process write master done interrupt
 * @vfe: VFE Device
 *
 * Return vfe hw version
 */
u32 vfe_hw_version(struct vfe_device *vfe)
{
	u32 hw_version = readl_relaxed(vfe->base + VFE_HW_VERSION);

	u32 gen = (hw_version >> HW_VERSION_GENERATION) & 0xF;
	u32 rev = (hw_version >> HW_VERSION_REVISION) & 0xFFF;
	u32 step = (hw_version >> HW_VERSION_STEPPING) & 0xFFFF;

	dev_dbg(vfe->camss->dev, "VFE:%d HW Version = %u.%u.%u\n",
		vfe->id, gen, rev, step);

	return hw_version;
}

/*
 * vfe_buf_done - Process write master done interrupt
 * @vfe: VFE Device
 * @wm: Write master id
 */
void vfe_buf_done(struct vfe_device *vfe, int wm)
{
	struct vfe_line *line = &vfe->line[vfe->wm_output_map[wm]];
	const struct vfe_hw_ops *ops = vfe->res->hw_ops;
	struct camss_buffer *ready_buf;
	struct vfe_output *output;
	unsigned long flags;
	u32 index;
	u64 ts = ktime_get_ns();

	spin_lock_irqsave(&vfe->output_lock, flags);

	if (vfe->wm_output_map[wm] == VFE_LINE_NONE) {
		dev_err_ratelimited(vfe->camss->dev,
				    "Received wm done for unmapped index\n");
		goto out_unlock;
	}
	output = &vfe->line[vfe->wm_output_map[wm]].output;

	ready_buf = output->buf[0];
	if (!ready_buf) {
		dev_err_ratelimited(vfe->camss->dev,
				    "Missing ready buf %d!\n", output->state);
		goto out_unlock;
	}

	ready_buf->vb.vb2_buf.timestamp = ts;
	ready_buf->vb.sequence = output->sequence++;

	index = 0;
	output->buf[0] = output->buf[1];
	if (output->buf[0])
		index = 1;

	output->buf[index] = vfe_buf_get_pending(output);

	if (output->buf[index]) {
		ops->vfe_wm_update(vfe, output->wm_idx[0],
				   output->buf[index]->addr[0],
				   line);
		ops->reg_update(vfe, line->id);
	} else {
		output->gen2.active_num--;
	}

	spin_unlock_irqrestore(&vfe->output_lock, flags);

	vb2_buffer_done(&ready_buf->vb.vb2_buf, VB2_BUF_STATE_DONE);

	return;

out_unlock:
	spin_unlock_irqrestore(&vfe->output_lock, flags);
}

int vfe_enable_output_v2(struct vfe_line *line)
{
	struct vfe_device *vfe = to_vfe(line);
	struct vfe_output *output = &line->output;
	const struct vfe_hw_ops *ops = vfe->res->hw_ops;
	struct media_pad *sensor_pad;
	unsigned long flags;
	unsigned int frame_skip = 0;
	unsigned int i;

	sensor_pad = camss_find_sensor_pad(&line->subdev.entity);
	if (sensor_pad) {
		struct v4l2_subdev *subdev =
			media_entity_to_v4l2_subdev(sensor_pad->entity);

		v4l2_subdev_call(subdev, sensor, g_skip_frames, &frame_skip);
		/* Max frame skip is 29 frames */
		if (frame_skip > VFE_FRAME_DROP_VAL - 1)
			frame_skip = VFE_FRAME_DROP_VAL - 1;
	}

	spin_lock_irqsave(&vfe->output_lock, flags);

	ops->reg_update_clear(vfe, line->id);

	if (output->state > VFE_OUTPUT_RESERVED) {
		dev_err(vfe->camss->dev,
			"Output is not in reserved state %d\n",
			output->state);
		spin_unlock_irqrestore(&vfe->output_lock, flags);
		return -EINVAL;
	}

	WARN_ON(output->gen2.active_num);

	output->state = VFE_OUTPUT_ON;

	output->sequence = 0;
	output->wait_reg_update = 0;
	reinit_completion(&output->reg_update);

	ops->vfe_wm_start(vfe, output->wm_idx[0], line);

	for (i = 0; i < 2; i++) {
		output->buf[i] = vfe_buf_get_pending(output);
		if (!output->buf[i])
			break;
		output->gen2.active_num++;
		ops->vfe_wm_update(vfe, output->wm_idx[0],
				   output->buf[i]->addr[0], line);
		ops->reg_update(vfe, line->id);
	}

	spin_unlock_irqrestore(&vfe->output_lock, flags);

	return 0;
}

/*
 * vfe_queue_buffer_v2 - Add empty buffer
 * @vid: Video device structure
 * @buf: Buffer to be enqueued
 *
 * Add an empty buffer - depending on the current number of buffers it will be
 * put in pending buffer queue or directly given to the hardware to be filled.
 *
 * Return 0 on success or a negative error code otherwise
 */
int vfe_queue_buffer_v2(struct camss_video *vid,
			struct camss_buffer *buf)
{
	struct vfe_line *line = container_of(vid, struct vfe_line, video_out);
	struct vfe_device *vfe = to_vfe(line);
	const struct vfe_hw_ops *ops = vfe->res->hw_ops;
	struct vfe_output *output;
	unsigned long flags;

	output = &line->output;

	spin_lock_irqsave(&vfe->output_lock, flags);

	if (output->state == VFE_OUTPUT_ON &&
	    output->gen2.active_num < 2) {
		output->buf[output->gen2.active_num++] = buf;
		ops->vfe_wm_update(vfe, output->wm_idx[0],
				   buf->addr[0], line);
		ops->reg_update(vfe, line->id);
	} else {
		vfe_buf_add_pending(output, buf);
	}

	spin_unlock_irqrestore(&vfe->output_lock, flags);

	return 0;
}

/*
 * vfe_enable_v2 - Enable streaming on VFE line
 * @line: VFE line
 *
 * Return 0 on success or a negative error code otherwise
 */
int vfe_enable_v2(struct vfe_line *line)
{
	struct vfe_device *vfe = to_vfe(line);
	const struct vfe_hw_ops *ops = vfe->res->hw_ops;
	int ret;

	mutex_lock(&vfe->stream_lock);

	if (vfe->res->hw_ops->enable_irq)
		ops->enable_irq(vfe);

	vfe->stream_count++;

	mutex_unlock(&vfe->stream_lock);

	ret = vfe_get_output_v2(line);
	if (ret < 0)
		goto error_get_output;

	ret = vfe_enable_output_v2(line);
	if (ret < 0)
		goto error_enable_output;

	vfe->was_streaming = 1;

	return 0;

error_enable_output:
	vfe_put_output(line);

error_get_output:
	mutex_lock(&vfe->stream_lock);

	vfe->stream_count--;

	mutex_unlock(&vfe->stream_lock);

	return ret;
}

/*
 * vfe_get_output_v2 - Get vfe output port for corresponding VFE line
 * @line: VFE line
 *
 * Return 0 on success or a negative error code otherwise
 */
int vfe_get_output_v2(struct vfe_line *line)
{
	struct vfe_device *vfe = to_vfe(line);
	struct vfe_output *output;
	unsigned long flags;

	spin_lock_irqsave(&vfe->output_lock, flags);

	output = &line->output;
	if (output->state > VFE_OUTPUT_RESERVED) {
		dev_err(vfe->camss->dev, "Output is running\n");
		goto error;
	}

	output->wm_num = 1;

	/* Correspondence between VFE line number and WM number.
	 * line 0 -> RDI 0, line 1 -> RDI1, line 2 -> RDI2, line 3 -> PIX/RDI3
	 * Note this 1:1 mapping will not work for PIX streams.
	 */
	output->wm_idx[0] = line->id;
	vfe->wm_output_map[line->id] = line->id;

	output->drop_update_idx = 0;

	spin_unlock_irqrestore(&vfe->output_lock, flags);

	return 0;

error:
	spin_unlock_irqrestore(&vfe->output_lock, flags);
	output->state = VFE_OUTPUT_OFF;

	return -EINVAL;
}

int vfe_reset(struct vfe_device *vfe)
{
	unsigned long time;

	reinit_completion(&vfe->reset_complete);

	vfe->res->hw_ops->global_reset(vfe);

	time = wait_for_completion_timeout(&vfe->reset_complete,
		msecs_to_jiffies(VFE_RESET_TIMEOUT_MS));
	if (!time) {
		dev_err(vfe->camss->dev, "VFE reset timeout\n");
		return -EIO;
	}

	return 0;
}

static void vfe_init_outputs(struct vfe_device *vfe)
{
	int i;

	for (i = 0; i < vfe->res->line_num; i++) {
		struct vfe_output *output = &vfe->line[i].output;

		output->state = VFE_OUTPUT_OFF;
		output->buf[0] = NULL;
		output->buf[1] = NULL;
		INIT_LIST_HEAD(&output->pending_bufs);
	}
}

static void vfe_reset_output_maps(struct vfe_device *vfe)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(vfe->wm_output_map); i++)
		vfe->wm_output_map[i] = VFE_LINE_NONE;
}

int vfe_reserve_wm(struct vfe_device *vfe, enum vfe_line_id line_id)
{
	int ret = -EBUSY;
	int i;

	for (i = 0; i < ARRAY_SIZE(vfe->wm_output_map); i++) {
		if (vfe->wm_output_map[i] == VFE_LINE_NONE) {
			vfe->wm_output_map[i] = line_id;
			ret = i;
			break;
		}
	}

	return ret;
}

int vfe_release_wm(struct vfe_device *vfe, u8 wm)
{
	if (wm >= ARRAY_SIZE(vfe->wm_output_map))
		return -EINVAL;

	vfe->wm_output_map[wm] = VFE_LINE_NONE;

	return 0;
}

struct camss_buffer *vfe_buf_get_pending(struct vfe_output *output)
{
	struct camss_buffer *buffer = NULL;

	if (!list_empty(&output->pending_bufs)) {
		buffer = list_first_entry(&output->pending_bufs,
					  struct camss_buffer,
					  queue);
		list_del(&buffer->queue);
	}

	return buffer;
}

void vfe_buf_add_pending(struct vfe_output *output,
			 struct camss_buffer *buffer)
{
	INIT_LIST_HEAD(&buffer->queue);
	list_add_tail(&buffer->queue, &output->pending_bufs);
}

/*
 * vfe_buf_flush_pending - Flush all pending buffers.
 * @output: VFE output
 * @state: vb2 buffer state
 */
static void vfe_buf_flush_pending(struct vfe_output *output,
				  enum vb2_buffer_state state)
{
	struct camss_buffer *buf;
	struct camss_buffer *t;

	list_for_each_entry_safe(buf, t, &output->pending_bufs, queue) {
		vb2_buffer_done(&buf->vb.vb2_buf, state);
		list_del(&buf->queue);
	}
}

int vfe_put_output(struct vfe_line *line)
{
	struct vfe_device *vfe = to_vfe(line);
	struct vfe_output *output = &line->output;
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&vfe->output_lock, flags);

	for (i = 0; i < output->wm_num; i++)
		vfe_release_wm(vfe, output->wm_idx[i]);

	output->state = VFE_OUTPUT_OFF;

	spin_unlock_irqrestore(&vfe->output_lock, flags);
	return 0;
}

static int vfe_disable_output(struct vfe_line *line)
{
	struct vfe_device *vfe = to_vfe(line);
	struct vfe_output *output = &line->output;
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&vfe->output_lock, flags);
	for (i = 0; i < output->wm_num; i++)
		vfe->res->hw_ops->vfe_wm_stop(vfe, output->wm_idx[i]);
	output->gen2.active_num = 0;
	spin_unlock_irqrestore(&vfe->output_lock, flags);

	return vfe_reset(vfe);
}

/*
 * vfe_disable - Disable streaming on VFE line
 * @line: VFE line
 *
 * Return 0 on success or a negative error code otherwise
 */
int vfe_disable(struct vfe_line *line)
{
	struct vfe_device *vfe = to_vfe(line);
	int ret;

	ret = vfe_disable_output(line);
	if (ret)
		goto error;

	vfe_put_output(line);

	mutex_lock(&vfe->stream_lock);

	vfe->stream_count--;

	mutex_unlock(&vfe->stream_lock);

error:
	return ret;
}

/**
 * vfe_isr_comp_done() - Process composite image done interrupt
 * @vfe: VFE Device
 * @comp: Composite image id
 */
void vfe_isr_comp_done(struct vfe_device *vfe, u8 comp)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(vfe->wm_output_map); i++)
		if (vfe->wm_output_map[i] == VFE_LINE_PIX) {
			vfe->isr_ops.wm_done(vfe, i);
			break;
		}
}

void vfe_isr_reset_ack(struct vfe_device *vfe)
{
	complete(&vfe->reset_complete);
}

/*
 * vfe_pm_domain_off - Disable power domains specific to this VFE.
 * @vfe: VFE Device
 */
void vfe_pm_domain_off(struct vfe_device *vfe)
{
	if (!vfe->genpd)
		return;

	device_link_del(vfe->genpd_link);
	vfe->genpd_link = NULL;
}

/*
 * vfe_pm_domain_on - Enable power domains specific to this VFE.
 * @vfe: VFE Device
 */
int vfe_pm_domain_on(struct vfe_device *vfe)
{
	struct camss *camss = vfe->camss;

	if (!vfe->genpd)
		return 0;

	vfe->genpd_link = device_link_add(camss->dev, vfe->genpd,
					  DL_FLAG_STATELESS |
					  DL_FLAG_PM_RUNTIME |
					  DL_FLAG_RPM_ACTIVE);
	if (!vfe->genpd_link)
		return -EINVAL;

	return 0;
}

static int vfe_match_clock_names(struct vfe_device *vfe,
				 struct camss_clock *clock)
{
	char vfe_name[7]; /* vfeXXX\0 */
	char vfe_lite_name[12]; /* vfe_liteXXX\0 */

	snprintf(vfe_name, sizeof(vfe_name), "vfe%d", vfe->id);
	snprintf(vfe_lite_name, sizeof(vfe_lite_name), "vfe_lite%d", vfe->id);

	return (!strcmp(clock->name, vfe_name) ||
		!strcmp(clock->name, vfe_lite_name) ||
		!strcmp(clock->name, "vfe_lite"));
}

/*
 * vfe_set_clock_rates - Calculate and set clock rates on VFE module
 * @vfe: VFE device
 *
 * Return 0 on success or a negative error code otherwise
 */
static int vfe_set_clock_rates(struct vfe_device *vfe)
{
	struct device *dev = vfe->camss->dev;
	u64 pixel_clock[VFE_LINE_NUM_MAX];
	int i, j;
	int ret;

	for (i = VFE_LINE_RDI0; i < vfe->res->line_num; i++) {
		ret = camss_get_pixel_clock(&vfe->line[i].subdev.entity,
					    &pixel_clock[i]);
		if (ret)
			pixel_clock[i] = 0;
	}

	for (i = 0; i < vfe->nclocks; i++) {
		struct camss_clock *clock = &vfe->clock[i];

		if (vfe_match_clock_names(vfe, clock)) {
			u64 min_rate = 0;
			long rate;

			for (j = VFE_LINE_RDI0; j < vfe->res->line_num; j++) {
				u32 tmp;
				u8 bpp;

				if (j == VFE_LINE_PIX) {
					tmp = pixel_clock[j];
				} else {
					struct vfe_line *l = &vfe->line[j];

					bpp = camss_format_get_bpp(l->formats,
								   l->nformats,
								   l->fmt[MSM_VFE_PAD_SINK].code);
					tmp = pixel_clock[j] * bpp / 64;
				}

				if (min_rate < tmp)
					min_rate = tmp;
			}

			camss_add_clock_margin(&min_rate);

			for (j = 0; j < clock->nfreqs; j++)
				if (min_rate < clock->freq[j])
					break;

			if (j == clock->nfreqs) {
				dev_err(dev,
					"Pixel clock is too high for VFE");
				return -EINVAL;
			}

			/* if sensor pixel clock is not available */
			/* set highest possible VFE clock rate */
			if (min_rate == 0)
				j = clock->nfreqs - 1;

			rate = clk_round_rate(clock->clk, clock->freq[j]);
			if (rate < 0) {
				dev_err(dev, "clk round rate failed: %ld\n",
					rate);
				return -EINVAL;
			}

			ret = clk_set_rate(clock->clk, rate);
			if (ret < 0) {
				dev_err(dev, "clk set rate failed: %d\n", ret);
				return ret;
			}
		}
	}

	return 0;
}

/*
 * vfe_check_clock_rates - Check current clock rates on VFE module
 * @vfe: VFE device
 *
 * Return 0 if current clock rates are suitable for a new pipeline
 * or a negative error code otherwise
 */
static int vfe_check_clock_rates(struct vfe_device *vfe)
{
	u64 pixel_clock[VFE_LINE_NUM_MAX];
	int i, j;
	int ret;

	for (i = VFE_LINE_RDI0; i < vfe->res->line_num; i++) {
		ret = camss_get_pixel_clock(&vfe->line[i].subdev.entity,
					    &pixel_clock[i]);
		if (ret)
			pixel_clock[i] = 0;
	}

	for (i = 0; i < vfe->nclocks; i++) {
		struct camss_clock *clock = &vfe->clock[i];

		if (vfe_match_clock_names(vfe, clock)) {
			u64 min_rate = 0;
			unsigned long rate;

			for (j = VFE_LINE_RDI0; j < vfe->res->line_num; j++) {
				u32 tmp;
				u8 bpp;

				if (j == VFE_LINE_PIX) {
					tmp = pixel_clock[j];
				} else {
					struct vfe_line *l = &vfe->line[j];

					bpp = camss_format_get_bpp(l->formats,
								   l->nformats,
								   l->fmt[MSM_VFE_PAD_SINK].code);
					tmp = pixel_clock[j] * bpp / 64;
				}

				if (min_rate < tmp)
					min_rate = tmp;
			}

			camss_add_clock_margin(&min_rate);

			rate = clk_get_rate(clock->clk);
			if (rate < min_rate)
				return -EBUSY;
		}
	}

	return 0;
}

/*
 * vfe_get - Power up and reset VFE module
 * @vfe: VFE Device
 *
 * Return 0 on success or a negative error code otherwise
 */
int vfe_get(struct vfe_device *vfe)
{
	int ret;

	mutex_lock(&vfe->power_lock);

	if (vfe->power_count == 0) {
		ret = vfe->res->hw_ops->pm_domain_on(vfe);
		if (ret < 0)
			goto error_pm_domain;

		ret = pm_runtime_resume_and_get(vfe->camss->dev);
		if (ret < 0)
			goto error_domain_off;

		ret = vfe_set_clock_rates(vfe);
		if (ret < 0)
			goto error_pm_runtime_get;

		ret = camss_enable_clocks(vfe->nclocks, vfe->clock,
					  vfe->camss->dev);
		if (ret < 0)
			goto error_pm_runtime_get;

		ret = vfe_reset(vfe);
		if (ret < 0)
			goto error_reset;

		vfe_reset_output_maps(vfe);

		vfe_init_outputs(vfe);

		vfe->res->hw_ops->hw_version(vfe);
	} else {
		ret = vfe_check_clock_rates(vfe);
		if (ret < 0)
			goto error_pm_domain;
	}
	vfe->power_count++;

	mutex_unlock(&vfe->power_lock);

	return 0;

error_reset:
	camss_disable_clocks(vfe->nclocks, vfe->clock);

error_pm_runtime_get:
	pm_runtime_put_sync(vfe->camss->dev);
error_domain_off:
	vfe->res->hw_ops->pm_domain_off(vfe);

error_pm_domain:
	mutex_unlock(&vfe->power_lock);

	return ret;
}

/*
 * vfe_put - Power down VFE module
 * @vfe: VFE Device
 */
void vfe_put(struct vfe_device *vfe)
{
	mutex_lock(&vfe->power_lock);

	if (vfe->power_count == 0) {
		dev_err(vfe->camss->dev, "vfe power off on power_count == 0\n");
		goto exit;
	} else if (vfe->power_count == 1) {
		if (vfe->was_streaming) {
			vfe->was_streaming = 0;
			vfe->res->hw_ops->vfe_halt(vfe);
		}
		camss_disable_clocks(vfe->nclocks, vfe->clock);
		pm_runtime_put_sync(vfe->camss->dev);
		vfe->res->hw_ops->pm_domain_off(vfe);
	}

	vfe->power_count--;

exit:
	mutex_unlock(&vfe->power_lock);
}

/*
 * vfe_flush_buffers - Return all vb2 buffers
 * @vid: Video device structure
 * @state: vb2 buffer state of the returned buffers
 *
 * Return all buffers to vb2. This includes queued pending buffers (still
 * unused) and any buffers given to the hardware but again still not used.
 *
 * Return 0 on success or a negative error code otherwise
 */
int vfe_flush_buffers(struct camss_video *vid,
		      enum vb2_buffer_state state)
{
	struct vfe_line *line = container_of(vid, struct vfe_line, video_out);
	struct vfe_device *vfe = to_vfe(line);
	struct vfe_output *output;
	unsigned long flags;

	output = &line->output;

	spin_lock_irqsave(&vfe->output_lock, flags);

	vfe_buf_flush_pending(output, state);

	if (output->buf[0])
		vb2_buffer_done(&output->buf[0]->vb.vb2_buf, state);

	if (output->buf[1])
		vb2_buffer_done(&output->buf[1]->vb.vb2_buf, state);

	if (output->last_buffer) {
		vb2_buffer_done(&output->last_buffer->vb.vb2_buf, state);
		output->last_buffer = NULL;
	}

	spin_unlock_irqrestore(&vfe->output_lock, flags);

	return 0;
}

/*
 * vfe_set_power - Power on/off VFE module
 * @sd: VFE V4L2 subdevice
 * @on: Requested power state
 *
 * Return 0 on success or a negative error code otherwise
 */
static int vfe_set_power(struct v4l2_subdev *sd, int on)
{
	struct vfe_line *line = v4l2_get_subdevdata(sd);
	struct vfe_device *vfe = to_vfe(line);
	int ret;

	if (on) {
		ret = vfe_get(vfe);
		if (ret < 0)
			return ret;
	} else {
		vfe_put(vfe);
	}

	return 0;
}

/*
 * vfe_set_stream - Enable/disable streaming on VFE module
 * @sd: VFE V4L2 subdevice
 * @enable: Requested streaming state
 *
 * Main configuration of VFE module is triggered here.
 *
 * Return 0 on success or a negative error code otherwise
 */
static int vfe_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct vfe_line *line = v4l2_get_subdevdata(sd);
	struct vfe_device *vfe = to_vfe(line);
	int ret;

	if (enable) {
		line->output.state = VFE_OUTPUT_RESERVED;
		ret = vfe->res->hw_ops->vfe_enable(line);
		if (ret < 0)
			dev_err(vfe->camss->dev,
				"Failed to enable vfe outputs\n");
	} else {
		ret = vfe->res->hw_ops->vfe_disable(line);
		if (ret < 0)
			dev_err(vfe->camss->dev,
				"Failed to disable vfe outputs\n");
	}

	return ret;
}

/*
 * __vfe_get_format - Get pointer to format structure
 * @line: VFE line
 * @sd_state: V4L2 subdev state
 * @pad: pad from which format is requested
 * @which: TRY or ACTIVE format
 *
 * Return pointer to TRY or ACTIVE format structure
 */
static struct v4l2_mbus_framefmt *
__vfe_get_format(struct vfe_line *line,
		 struct v4l2_subdev_state *sd_state,
		 unsigned int pad,
		 enum v4l2_subdev_format_whence which)
{
	if (which == V4L2_SUBDEV_FORMAT_TRY)
		return v4l2_subdev_state_get_format(sd_state, pad);

	return &line->fmt[pad];
}

/*
 * __vfe_get_compose - Get pointer to compose selection structure
 * @line: VFE line
 * @sd_state: V4L2 subdev state
 * @which: TRY or ACTIVE format
 *
 * Return pointer to TRY or ACTIVE compose rectangle structure
 */
static struct v4l2_rect *
__vfe_get_compose(struct vfe_line *line,
		  struct v4l2_subdev_state *sd_state,
		  enum v4l2_subdev_format_whence which)
{
	if (which == V4L2_SUBDEV_FORMAT_TRY)
		return v4l2_subdev_state_get_compose(sd_state,
						     MSM_VFE_PAD_SINK);

	return &line->compose;
}

/*
 * __vfe_get_crop - Get pointer to crop selection structure
 * @line: VFE line
 * @sd_state: V4L2 subdev state
 * @which: TRY or ACTIVE format
 *
 * Return pointer to TRY or ACTIVE crop rectangle structure
 */
static struct v4l2_rect *
__vfe_get_crop(struct vfe_line *line,
	       struct v4l2_subdev_state *sd_state,
	       enum v4l2_subdev_format_whence which)
{
	if (which == V4L2_SUBDEV_FORMAT_TRY)
		return v4l2_subdev_state_get_crop(sd_state, MSM_VFE_PAD_SRC);

	return &line->crop;
}

/*
 * vfe_try_format - Handle try format by pad subdev method
 * @line: VFE line
 * @sd_state: V4L2 subdev state
 * @pad: pad on which format is requested
 * @fmt: pointer to v4l2 format structure
 * @which: wanted subdev format
 */
static void vfe_try_format(struct vfe_line *line,
			   struct v4l2_subdev_state *sd_state,
			   unsigned int pad,
			   struct v4l2_mbus_framefmt *fmt,
			   enum v4l2_subdev_format_whence which)
{
	unsigned int i;
	u32 code;

	switch (pad) {
	case MSM_VFE_PAD_SINK:
		/* Set format on sink pad */

		for (i = 0; i < line->nformats; i++)
			if (fmt->code == line->formats[i].code)
				break;

		/* If not found, use UYVY as default */
		if (i >= line->nformats)
			fmt->code = MEDIA_BUS_FMT_UYVY8_1X16;

		fmt->width = clamp_t(u32, fmt->width, 1, 8191);
		fmt->height = clamp_t(u32, fmt->height, 1, 8191);

		fmt->field = V4L2_FIELD_NONE;
		fmt->colorspace = V4L2_COLORSPACE_SRGB;

		break;

	case MSM_VFE_PAD_SRC:
		/* Set and return a format same as sink pad */
		code = fmt->code;

		*fmt = *__vfe_get_format(line, sd_state, MSM_VFE_PAD_SINK,
					 which);

		fmt->code = vfe_src_pad_code(line, fmt->code, 0, code);

		if (line->id == VFE_LINE_PIX) {
			struct v4l2_rect *rect;

			rect = __vfe_get_crop(line, sd_state, which);

			fmt->width = rect->width;
			fmt->height = rect->height;
		}

		break;
	}

	fmt->colorspace = V4L2_COLORSPACE_SRGB;
}

/*
 * vfe_try_compose - Handle try compose selection by pad subdev method
 * @line: VFE line
 * @sd_state: V4L2 subdev state
 * @rect: pointer to v4l2 rect structure
 * @which: wanted subdev format
 */
static void vfe_try_compose(struct vfe_line *line,
			    struct v4l2_subdev_state *sd_state,
			    struct v4l2_rect *rect,
			    enum v4l2_subdev_format_whence which)
{
	struct v4l2_mbus_framefmt *fmt;

	fmt = __vfe_get_format(line, sd_state, MSM_VFE_PAD_SINK, which);

	if (rect->width > fmt->width)
		rect->width = fmt->width;

	if (rect->height > fmt->height)
		rect->height = fmt->height;

	if (fmt->width > rect->width * SCALER_RATIO_MAX)
		rect->width = (fmt->width + SCALER_RATIO_MAX - 1) /
							SCALER_RATIO_MAX;

	rect->width &= ~0x1;

	if (fmt->height > rect->height * SCALER_RATIO_MAX)
		rect->height = (fmt->height + SCALER_RATIO_MAX - 1) /
							SCALER_RATIO_MAX;

	if (rect->width < 16)
		rect->width = 16;

	if (rect->height < 4)
		rect->height = 4;
}

/*
 * vfe_try_crop - Handle try crop selection by pad subdev method
 * @line: VFE line
 * @sd_state: V4L2 subdev state
 * @rect: pointer to v4l2 rect structure
 * @which: wanted subdev format
 */
static void vfe_try_crop(struct vfe_line *line,
			 struct v4l2_subdev_state *sd_state,
			 struct v4l2_rect *rect,
			 enum v4l2_subdev_format_whence which)
{
	struct v4l2_rect *compose;

	compose = __vfe_get_compose(line, sd_state, which);

	if (rect->width > compose->width)
		rect->width = compose->width;

	if (rect->width + rect->left > compose->width)
		rect->left = compose->width - rect->width;

	if (rect->height > compose->height)
		rect->height = compose->height;

	if (rect->height + rect->top > compose->height)
		rect->top = compose->height - rect->height;

	/* wm in line based mode writes multiple of 16 horizontally */
	rect->left += (rect->width & 0xf) >> 1;
	rect->width &= ~0xf;

	if (rect->width < 16) {
		rect->left = 0;
		rect->width = 16;
	}

	if (rect->height < 4) {
		rect->top = 0;
		rect->height = 4;
	}
}

/*
 * vfe_enum_mbus_code - Handle pixel format enumeration
 * @sd: VFE V4L2 subdevice
 * @sd_state: V4L2 subdev state
 * @code: pointer to v4l2_subdev_mbus_code_enum structure
 *
 * return -EINVAL or zero on success
 */
static int vfe_enum_mbus_code(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *sd_state,
			      struct v4l2_subdev_mbus_code_enum *code)
{
	struct vfe_line *line = v4l2_get_subdevdata(sd);

	if (code->pad == MSM_VFE_PAD_SINK) {
		if (code->index >= line->nformats)
			return -EINVAL;

		code->code = line->formats[code->index].code;
	} else {
		struct v4l2_mbus_framefmt *sink_fmt;

		sink_fmt = __vfe_get_format(line, sd_state, MSM_VFE_PAD_SINK,
					    code->which);

		code->code = vfe_src_pad_code(line, sink_fmt->code,
					      code->index, 0);
		if (!code->code)
			return -EINVAL;
	}

	return 0;
}

/*
 * vfe_enum_frame_size - Handle frame size enumeration
 * @sd: VFE V4L2 subdevice
 * @sd_state: V4L2 subdev state
 * @fse: pointer to v4l2_subdev_frame_size_enum structure
 *
 * Return -EINVAL or zero on success
 */
static int vfe_enum_frame_size(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *sd_state,
			       struct v4l2_subdev_frame_size_enum *fse)
{
	struct vfe_line *line = v4l2_get_subdevdata(sd);
	struct v4l2_mbus_framefmt format;

	if (fse->index != 0)
		return -EINVAL;

	format.code = fse->code;
	format.width = 1;
	format.height = 1;
	vfe_try_format(line, sd_state, fse->pad, &format, fse->which);
	fse->min_width = format.width;
	fse->min_height = format.height;

	if (format.code != fse->code)
		return -EINVAL;

	format.code = fse->code;
	format.width = -1;
	format.height = -1;
	vfe_try_format(line, sd_state, fse->pad, &format, fse->which);
	fse->max_width = format.width;
	fse->max_height = format.height;

	return 0;
}

/*
 * vfe_get_format - Handle get format by pads subdev method
 * @sd: VFE V4L2 subdevice
 * @sd_state: V4L2 subdev state
 * @fmt: pointer to v4l2 subdev format structure
 *
 * Return -EINVAL or zero on success
 */
static int vfe_get_format(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *fmt)
{
	struct vfe_line *line = v4l2_get_subdevdata(sd);
	struct v4l2_mbus_framefmt *format;

	format = __vfe_get_format(line, sd_state, fmt->pad, fmt->which);
	if (format == NULL)
		return -EINVAL;

	fmt->format = *format;

	return 0;
}

static int vfe_set_selection(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state,
			     struct v4l2_subdev_selection *sel);

/*
 * vfe_set_format - Handle set format by pads subdev method
 * @sd: VFE V4L2 subdevice
 * @sd_state: V4L2 subdev state
 * @fmt: pointer to v4l2 subdev format structure
 *
 * Return -EINVAL or zero on success
 */
static int vfe_set_format(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *fmt)
{
	struct vfe_line *line = v4l2_get_subdevdata(sd);
	struct v4l2_mbus_framefmt *format;

	format = __vfe_get_format(line, sd_state, fmt->pad, fmt->which);
	if (format == NULL)
		return -EINVAL;

	vfe_try_format(line, sd_state, fmt->pad, &fmt->format, fmt->which);
	*format = fmt->format;

	if (fmt->pad == MSM_VFE_PAD_SINK) {
		struct v4l2_subdev_selection sel = { 0 };
		int ret;

		/* Propagate the format from sink to source */
		format = __vfe_get_format(line, sd_state, MSM_VFE_PAD_SRC,
					  fmt->which);

		*format = fmt->format;
		vfe_try_format(line, sd_state, MSM_VFE_PAD_SRC, format,
			       fmt->which);

		if (line->id != VFE_LINE_PIX)
			return 0;

		/* Reset sink pad compose selection */
		sel.which = fmt->which;
		sel.pad = MSM_VFE_PAD_SINK;
		sel.target = V4L2_SEL_TGT_COMPOSE;
		sel.r.width = fmt->format.width;
		sel.r.height = fmt->format.height;
		ret = vfe_set_selection(sd, sd_state, &sel);
		if (ret < 0)
			return ret;
	}

	return 0;
}

/*
 * vfe_get_selection - Handle get selection by pads subdev method
 * @sd: VFE V4L2 subdevice
 * @sd_state: V4L2 subdev state
 * @sel: pointer to v4l2 subdev selection structure
 *
 * Return -EINVAL or zero on success
 */
static int vfe_get_selection(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state,
			     struct v4l2_subdev_selection *sel)
{
	struct vfe_line *line = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_format fmt = { 0 };
	struct v4l2_rect *rect;
	int ret;

	if (line->id != VFE_LINE_PIX)
		return -EINVAL;

	if (sel->pad == MSM_VFE_PAD_SINK)
		switch (sel->target) {
		case V4L2_SEL_TGT_COMPOSE_BOUNDS:
			fmt.pad = sel->pad;
			fmt.which = sel->which;
			ret = vfe_get_format(sd, sd_state, &fmt);
			if (ret < 0)
				return ret;

			sel->r.left = 0;
			sel->r.top = 0;
			sel->r.width = fmt.format.width;
			sel->r.height = fmt.format.height;
			break;
		case V4L2_SEL_TGT_COMPOSE:
			rect = __vfe_get_compose(line, sd_state, sel->which);
			if (rect == NULL)
				return -EINVAL;

			sel->r = *rect;
			break;
		default:
			return -EINVAL;
		}
	else if (sel->pad == MSM_VFE_PAD_SRC)
		switch (sel->target) {
		case V4L2_SEL_TGT_CROP_BOUNDS:
			rect = __vfe_get_compose(line, sd_state, sel->which);
			if (rect == NULL)
				return -EINVAL;

			sel->r.left = rect->left;
			sel->r.top = rect->top;
			sel->r.width = rect->width;
			sel->r.height = rect->height;
			break;
		case V4L2_SEL_TGT_CROP:
			rect = __vfe_get_crop(line, sd_state, sel->which);
			if (rect == NULL)
				return -EINVAL;

			sel->r = *rect;
			break;
		default:
			return -EINVAL;
		}

	return 0;
}

/*
 * vfe_set_selection - Handle set selection by pads subdev method
 * @sd: VFE V4L2 subdevice
 * @sd_state: V4L2 subdev state
 * @sel: pointer to v4l2 subdev selection structure
 *
 * Return -EINVAL or zero on success
 */
static int vfe_set_selection(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state,
			     struct v4l2_subdev_selection *sel)
{
	struct vfe_line *line = v4l2_get_subdevdata(sd);
	struct v4l2_rect *rect;
	int ret;

	if (line->id != VFE_LINE_PIX)
		return -EINVAL;

	if (sel->target == V4L2_SEL_TGT_COMPOSE &&
		sel->pad == MSM_VFE_PAD_SINK) {
		struct v4l2_subdev_selection crop = { 0 };

		rect = __vfe_get_compose(line, sd_state, sel->which);
		if (rect == NULL)
			return -EINVAL;

		vfe_try_compose(line, sd_state, &sel->r, sel->which);
		*rect = sel->r;

		/* Reset source crop selection */
		crop.which = sel->which;
		crop.pad = MSM_VFE_PAD_SRC;
		crop.target = V4L2_SEL_TGT_CROP;
		crop.r = *rect;
		ret = vfe_set_selection(sd, sd_state, &crop);
	} else if (sel->target == V4L2_SEL_TGT_CROP &&
		sel->pad == MSM_VFE_PAD_SRC) {
		struct v4l2_subdev_format fmt = { 0 };

		rect = __vfe_get_crop(line, sd_state, sel->which);
		if (rect == NULL)
			return -EINVAL;

		vfe_try_crop(line, sd_state, &sel->r, sel->which);
		*rect = sel->r;

		/* Reset source pad format width and height */
		fmt.which = sel->which;
		fmt.pad = MSM_VFE_PAD_SRC;
		ret = vfe_get_format(sd, sd_state, &fmt);
		if (ret < 0)
			return ret;

		fmt.format.width = rect->width;
		fmt.format.height = rect->height;
		ret = vfe_set_format(sd, sd_state, &fmt);
	} else {
		ret = -EINVAL;
	}

	return ret;
}

/*
 * vfe_init_formats - Initialize formats on all pads
 * @sd: VFE V4L2 subdevice
 * @fh: V4L2 subdev file handle
 *
 * Initialize all pad formats with default values.
 *
 * Return 0 on success or a negative error code otherwise
 */
static int vfe_init_formats(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct v4l2_subdev_format format = {
		.pad = MSM_VFE_PAD_SINK,
		.which = fh ? V4L2_SUBDEV_FORMAT_TRY :
			      V4L2_SUBDEV_FORMAT_ACTIVE,
		.format = {
			.code = MEDIA_BUS_FMT_UYVY8_1X16,
			.width = 1920,
			.height = 1080
		}
	};

	return vfe_set_format(sd, fh ? fh->state : NULL, &format);
}

/*
 * msm_vfe_subdev_init - Initialize VFE device structure and resources
 * @vfe: VFE device
 * @res: VFE module resources table
 *
 * Return 0 on success or a negative error code otherwise
 */
int msm_vfe_subdev_init(struct camss *camss, struct vfe_device *vfe,
			const struct camss_subdev_resources *res, u8 id)
{
	struct device *dev = camss->dev;
	struct platform_device *pdev = to_platform_device(dev);
	int i, j;
	int ret;

	if (!res->vfe.line_num)
		return -EINVAL;

	vfe->res = &res->vfe;
	vfe->res->hw_ops->subdev_init(dev, vfe);

	/* Power domain */

	if (res->vfe.pd_name) {
		vfe->genpd = dev_pm_domain_attach_by_name(camss->dev,
							  res->vfe.pd_name);
		if (IS_ERR(vfe->genpd)) {
			ret = PTR_ERR(vfe->genpd);
			return ret;
		}
	}

	if (!vfe->genpd && res->vfe.has_pd) {
		/*
		 * Legacy magic index.
		 * Requires
		 * power-domain = <VFE_X>,
		 *                <VFE_Y>,
		 *                <TITAN_TOP>
		 * id must correspondng to the index of the VFE which must
		 * come before the TOP GDSC. VFE Lite has no individually
		 * collapasible domain which is why id < vfe_num is a valid
		 * check.
		 */
		vfe->genpd = dev_pm_domain_attach_by_id(camss->dev, id);
		if (IS_ERR(vfe->genpd))
			return PTR_ERR(vfe->genpd);
	}

	/* Memory */

	vfe->base = devm_platform_ioremap_resource_byname(pdev, res->reg[0]);
	if (IS_ERR(vfe->base)) {
		dev_err(dev, "could not map memory\n");
		return PTR_ERR(vfe->base);
	}

	/* Interrupt */

	ret = platform_get_irq_byname(pdev, res->interrupt[0]);
	if (ret < 0)
		return ret;

	vfe->irq = ret;
	snprintf(vfe->irq_name, sizeof(vfe->irq_name), "%s_%s%d",
		 dev_name(dev), MSM_VFE_NAME, id);
	ret = devm_request_irq(dev, vfe->irq, vfe->res->hw_ops->isr,
			       IRQF_TRIGGER_RISING, vfe->irq_name, vfe);
	if (ret < 0) {
		dev_err(dev, "request_irq failed: %d\n", ret);
		return ret;
	}

	/* Clocks */

	vfe->nclocks = 0;
	while (res->clock[vfe->nclocks])
		vfe->nclocks++;

	vfe->clock = devm_kcalloc(dev, vfe->nclocks, sizeof(*vfe->clock),
				  GFP_KERNEL);
	if (!vfe->clock)
		return -ENOMEM;

	for (i = 0; i < vfe->nclocks; i++) {
		struct camss_clock *clock = &vfe->clock[i];

		clock->clk = devm_clk_get(dev, res->clock[i]);
		if (IS_ERR(clock->clk))
			return PTR_ERR(clock->clk);

		clock->name = res->clock[i];

		clock->nfreqs = 0;
		while (res->clock_rate[i][clock->nfreqs])
			clock->nfreqs++;

		if (!clock->nfreqs) {
			clock->freq = NULL;
			continue;
		}

		clock->freq = devm_kcalloc(dev,
					   clock->nfreqs,
					   sizeof(*clock->freq),
					   GFP_KERNEL);
		if (!clock->freq)
			return -ENOMEM;

		for (j = 0; j < clock->nfreqs; j++)
			clock->freq[j] = res->clock_rate[i][j];
	}

	mutex_init(&vfe->power_lock);
	vfe->power_count = 0;

	mutex_init(&vfe->stream_lock);
	vfe->stream_count = 0;

	spin_lock_init(&vfe->output_lock);

	vfe->camss = camss;
	vfe->id = id;
	vfe->reg_update = 0;

	for (i = VFE_LINE_RDI0; i < vfe->res->line_num; i++) {
		struct vfe_line *l = &vfe->line[i];

		l->video_out.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		l->video_out.camss = camss;
		l->id = i;
		init_completion(&l->output.sof);
		init_completion(&l->output.reg_update);

		if (i == VFE_LINE_PIX) {
			l->nformats = res->vfe.formats_pix->nformats;
			l->formats = res->vfe.formats_pix->formats;
		} else {
			l->nformats = res->vfe.formats_rdi->nformats;
			l->formats = res->vfe.formats_rdi->formats;
		}
	}

	init_completion(&vfe->reset_complete);
	init_completion(&vfe->halt_complete);

	return 0;
}

/*
 * msm_vfe_genpd_cleanup - Cleanup VFE genpd linkages
 * @vfe: VFE device
 */
void msm_vfe_genpd_cleanup(struct vfe_device *vfe)
{
	if (vfe->genpd_link)
		device_link_del(vfe->genpd_link);

	if (vfe->genpd)
		dev_pm_domain_detach(vfe->genpd, true);
}

/*
 * vfe_link_setup - Setup VFE connections
 * @entity: Pointer to media entity structure
 * @local: Pointer to local pad
 * @remote: Pointer to remote pad
 * @flags: Link flags
 *
 * Return 0 on success
 */
static int vfe_link_setup(struct media_entity *entity,
			  const struct media_pad *local,
			  const struct media_pad *remote, u32 flags)
{
	if (flags & MEDIA_LNK_FL_ENABLED)
		if (media_pad_remote_pad_first(local))
			return -EBUSY;

	return 0;
}

static const struct v4l2_subdev_core_ops vfe_core_ops = {
	.s_power = vfe_set_power,
};

static const struct v4l2_subdev_video_ops vfe_video_ops = {
	.s_stream = vfe_set_stream,
};

static const struct v4l2_subdev_pad_ops vfe_pad_ops = {
	.enum_mbus_code = vfe_enum_mbus_code,
	.enum_frame_size = vfe_enum_frame_size,
	.get_fmt = vfe_get_format,
	.set_fmt = vfe_set_format,
	.get_selection = vfe_get_selection,
	.set_selection = vfe_set_selection,
};

static const struct v4l2_subdev_ops vfe_v4l2_ops = {
	.core = &vfe_core_ops,
	.video = &vfe_video_ops,
	.pad = &vfe_pad_ops,
};

static const struct v4l2_subdev_internal_ops vfe_v4l2_internal_ops = {
	.open = vfe_init_formats,
};

static const struct media_entity_operations vfe_media_ops = {
	.link_setup = vfe_link_setup,
	.link_validate = v4l2_subdev_link_validate,
};

static int vfe_bpl_align(struct vfe_device *vfe)
{
	int ret = 8;

	switch (vfe->camss->res->version) {
	case CAMSS_7280:
	case CAMSS_8250:
	case CAMSS_8280XP:
	case CAMSS_845:
	case CAMSS_8550:
		ret = 16;
		break;
	default:
		break;
	}

	return ret;
}

/*
 * msm_vfe_register_entities - Register subdev node for VFE module
 * @vfe: VFE device
 * @v4l2_dev: V4L2 device
 *
 * Initialize and register a subdev node for the VFE module. Then
 * call msm_video_register() to register the video device node which
 * will be connected to this subdev node. Then actually create the
 * media link between them.
 *
 * Return 0 on success or a negative error code otherwise
 */
int msm_vfe_register_entities(struct vfe_device *vfe,
			      struct v4l2_device *v4l2_dev)
{
	struct device *dev = vfe->camss->dev;
	struct v4l2_subdev *sd;
	struct media_pad *pads;
	struct camss_video *video_out;
	int ret;
	int i;

	for (i = 0; i < vfe->res->line_num; i++) {
		char name[32];

		sd = &vfe->line[i].subdev;
		pads = vfe->line[i].pads;
		video_out = &vfe->line[i].video_out;

		v4l2_subdev_init(sd, &vfe_v4l2_ops);
		sd->internal_ops = &vfe_v4l2_internal_ops;
		sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
		if (i == VFE_LINE_PIX)
			snprintf(sd->name, ARRAY_SIZE(sd->name), "%s%d_%s",
				 MSM_VFE_NAME, vfe->id, "pix");
		else
			snprintf(sd->name, ARRAY_SIZE(sd->name), "%s%d_%s%d",
				 MSM_VFE_NAME, vfe->id, "rdi", i);

		v4l2_set_subdevdata(sd, &vfe->line[i]);

		ret = vfe_init_formats(sd, NULL);
		if (ret < 0) {
			dev_err(dev, "Failed to init format: %d\n", ret);
			goto error_init;
		}

		pads[MSM_VFE_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
		pads[MSM_VFE_PAD_SRC].flags = MEDIA_PAD_FL_SOURCE;

		sd->entity.function = MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER;
		sd->entity.ops = &vfe_media_ops;
		ret = media_entity_pads_init(&sd->entity, MSM_VFE_PADS_NUM,
					     pads);
		if (ret < 0) {
			dev_err(dev, "Failed to init media entity: %d\n", ret);
			goto error_init;
		}

		ret = v4l2_device_register_subdev(v4l2_dev, sd);
		if (ret < 0) {
			dev_err(dev, "Failed to register subdev: %d\n", ret);
			goto error_reg_subdev;
		}

		video_out->ops = &vfe->video_ops;
		video_out->bpl_alignment = vfe_bpl_align(vfe);
		video_out->line_based = 0;
		if (i == VFE_LINE_PIX) {
			video_out->bpl_alignment = 16;
			video_out->line_based = 1;
		}

		video_out->nformats = vfe->line[i].nformats;
		video_out->formats = vfe->line[i].formats;

		snprintf(name, ARRAY_SIZE(name), "%s%d_%s%d",
			 MSM_VFE_NAME, vfe->id, "video", i);
		ret = msm_video_register(video_out, v4l2_dev, name);
		if (ret < 0) {
			dev_err(dev, "Failed to register video node: %d\n",
				ret);
			goto error_reg_video;
		}

		ret = media_create_pad_link(
				&sd->entity, MSM_VFE_PAD_SRC,
				&video_out->vdev.entity, 0,
				MEDIA_LNK_FL_IMMUTABLE | MEDIA_LNK_FL_ENABLED);
		if (ret < 0) {
			dev_err(dev, "Failed to link %s->%s entities: %d\n",
				sd->entity.name, video_out->vdev.entity.name,
				ret);
			goto error_link;
		}
	}

	return 0;

error_link:
	msm_video_unregister(video_out);

error_reg_video:
	v4l2_device_unregister_subdev(sd);

error_reg_subdev:
	media_entity_cleanup(&sd->entity);

error_init:
	for (i--; i >= 0; i--) {
		sd = &vfe->line[i].subdev;
		video_out = &vfe->line[i].video_out;

		msm_video_unregister(video_out);
		v4l2_device_unregister_subdev(sd);
		media_entity_cleanup(&sd->entity);
	}

	return ret;
}

/*
 * msm_vfe_unregister_entities - Unregister VFE module subdev node
 * @vfe: VFE device
 */
void msm_vfe_unregister_entities(struct vfe_device *vfe)
{
	int i;

	mutex_destroy(&vfe->power_lock);
	mutex_destroy(&vfe->stream_lock);

	for (i = 0; i < vfe->res->line_num; i++) {
		struct v4l2_subdev *sd = &vfe->line[i].subdev;
		struct camss_video *video_out = &vfe->line[i].video_out;

		msm_video_unregister(video_out);
		v4l2_device_unregister_subdev(sd);
		media_entity_cleanup(&sd->entity);
	}
}

bool vfe_is_lite(struct vfe_device *vfe)
{
	return vfe->camss->res->vfe_res[vfe->id].vfe.is_lite;
}
