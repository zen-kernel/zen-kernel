/* SPDX-License-Identifier: GPL-2.0
 *
 * Definitions for kernel modules using asus-armoury driver
 *
 *  Copyright (c) 2024 Luke Jones <luke@ljones.dev>
 */

#ifndef _ASUS_ARMOURY_H_
#define _ASUS_ARMOURY_H_

#include <linux/dmi.h>
#include <linux/types.h>
#include <linux/platform_device.h>

#define DRIVER_NAME "asus-armoury"

#define __ASUS_ATTR_RO(_func, _name)					\
	{								\
		.attr = { .name = __stringify(_name), .mode = 0444 },	\
		.show = _func##_##_name##_show,				\
	}

#define __ASUS_ATTR_RO_AS(_name, _show)					\
	{								\
		.attr = { .name = __stringify(_name), .mode = 0444 },	\
		.show = _show,						\
	}

#define __ASUS_ATTR_RW(_func, _name) \
	__ATTR(_name, 0644, _func##_##_name##_show, _func##_##_name##_store)

#define __WMI_STORE_INT(_attr, _min, _max, _wmi)			\
	static ssize_t _attr##_store(struct kobject *kobj,		\
				     struct kobj_attribute *attr,	\
				     const char *buf, size_t count)	\
	{								\
		return attr_uint_store(kobj, attr, buf, count, _min,	\
					_max, NULL, _wmi);		\
	}

#define WMI_SHOW_INT(_attr, _fmt, _wmi)						\
	static ssize_t _attr##_show(struct kobject *kobj,			\
				    struct kobj_attribute *attr, char *buf)	\
	{									\
		u32 result;							\
		int err;							\
										\
		err = asus_wmi_get_devstate_dsts(_wmi, &result);		\
		if (err)							\
			return err;						\
		return sysfs_emit(buf, _fmt,					\
				  result & ~ASUS_WMI_DSTS_PRESENCE_BIT);	\
	}

/* Create functions and attributes for use in other macros or on their own */

/* Shows a formatted static variable */
#define __ATTR_SHOW_FMT(_prop, _attrname, _fmt, _val)				\
	static ssize_t _attrname##_##_prop##_show(				\
		struct kobject *kobj, struct kobj_attribute *attr, char *buf)	\
	{									\
		return sysfs_emit(buf, _fmt, _val);				\
	}									\
	static struct kobj_attribute attr_##_attrname##_##_prop =		\
		__ASUS_ATTR_RO(_attrname, _prop)

#define __ATTR_RO_INT_GROUP_ENUM(_attrname, _wmi, _fsname, _possible, _dispname)\
	WMI_SHOW_INT(_attrname##_current_value, "%d\n", _wmi);			\
	static struct kobj_attribute attr_##_attrname##_current_value =		\
		__ASUS_ATTR_RO(_attrname, current_value);			\
	__ATTR_SHOW_FMT(display_name, _attrname, "%s\n", _dispname);		\
	__ATTR_SHOW_FMT(possible_values, _attrname, "%s\n", _possible);		\
	static struct kobj_attribute attr_##_attrname##_type =			\
		__ASUS_ATTR_RO_AS(type, enum_type_show);			\
	static struct attribute *_attrname##_attrs[] = {			\
		&attr_##_attrname##_current_value.attr,				\
		&attr_##_attrname##_display_name.attr,				\
		&attr_##_attrname##_possible_values.attr,			\
		&attr_##_attrname##_type.attr,					\
		NULL								\
	};									\
	static const struct attribute_group _attrname##_attr_group = {		\
		.name = _fsname, .attrs = _attrname##_attrs			\
	}

#define __ATTR_RW_INT_GROUP_ENUM(_attrname, _minv, _maxv, _wmi, _fsname,\
				 _possible, _dispname)			\
	__WMI_STORE_INT(_attrname##_current_value, _minv, _maxv, _wmi);	\
	WMI_SHOW_INT(_attrname##_current_value, "%d\n", _wmi);		\
	static struct kobj_attribute attr_##_attrname##_current_value =	\
		__ASUS_ATTR_RW(_attrname, current_value);		\
	__ATTR_SHOW_FMT(display_name, _attrname, "%s\n", _dispname);	\
	__ATTR_SHOW_FMT(possible_values, _attrname, "%s\n", _possible);	\
	static struct kobj_attribute attr_##_attrname##_type =		\
		__ASUS_ATTR_RO_AS(type, enum_type_show);		\
	static struct attribute *_attrname##_attrs[] = {		\
		&attr_##_attrname##_current_value.attr,			\
		&attr_##_attrname##_display_name.attr,			\
		&attr_##_attrname##_possible_values.attr,		\
		&attr_##_attrname##_type.attr,				\
		NULL							\
	};								\
	static const struct attribute_group _attrname##_attr_group = {	\
		.name = _fsname, .attrs = _attrname##_attrs		\
	}

/* Boolean style enumeration, base macro. Requires adding show/store */
#define __ATTR_GROUP_ENUM(_attrname, _fsname, _possible, _dispname)	\
	__ATTR_SHOW_FMT(display_name, _attrname, "%s\n", _dispname);	\
	__ATTR_SHOW_FMT(possible_values, _attrname, "%s\n", _possible);	\
	static struct kobj_attribute attr_##_attrname##_type =		\
		__ASUS_ATTR_RO_AS(type, enum_type_show);		\
	static struct attribute *_attrname##_attrs[] = {		\
		&attr_##_attrname##_current_value.attr,			\
		&attr_##_attrname##_display_name.attr,			\
		&attr_##_attrname##_possible_values.attr,		\
		&attr_##_attrname##_type.attr,				\
		NULL							\
	};								\
	static const struct attribute_group _attrname##_attr_group = {	\
		.name = _fsname, .attrs = _attrname##_attrs		\
	}

#define ATTR_GROUP_BOOL_RO(_attrname, _fsname, _wmi, _dispname)	\
	__ATTR_RO_INT_GROUP_ENUM(_attrname, _wmi, _fsname, "0;1", _dispname)


#define ATTR_GROUP_BOOL_RW(_attrname, _fsname, _wmi, _dispname)	\
	__ATTR_RW_INT_GROUP_ENUM(_attrname, 0, 1, _wmi, _fsname, "0;1", _dispname)

#define ATTR_GROUP_ENUM_INT_RO(_attrname, _fsname, _wmi, _possible, _dispname)	\
	__ATTR_RO_INT_GROUP_ENUM(_attrname, _wmi, _fsname, _possible, _dispname)

/*
 * Requires <name>_current_value_show(), <name>_current_value_show()
 */
#define ATTR_GROUP_BOOL_CUSTOM(_attrname, _fsname, _dispname)		\
	static struct kobj_attribute attr_##_attrname##_current_value =	\
		__ASUS_ATTR_RW(_attrname, current_value);		\
	__ATTR_GROUP_ENUM(_attrname, _fsname, "0;1", _dispname)

/*
 * Requires <name>_current_value_show(), <name>_current_value_show()
 * and <name>_possible_values_show()
 */
#define ATTR_GROUP_ENUM_CUSTOM(_attrname, _fsname, _dispname)			\
	__ATTR_SHOW_FMT(display_name, _attrname, "%s\n", _dispname);		\
	static struct kobj_attribute attr_##_attrname##_current_value =		\
		__ASUS_ATTR_RW(_attrname, current_value);			\
	static struct kobj_attribute attr_##_attrname##_possible_values =	\
		__ASUS_ATTR_RO(_attrname, possible_values);			\
	static struct kobj_attribute attr_##_attrname##_type =			\
		__ASUS_ATTR_RO_AS(type, enum_type_show);			\
	static struct attribute *_attrname##_attrs[] = {			\
		&attr_##_attrname##_current_value.attr,				\
		&attr_##_attrname##_display_name.attr,				\
		&attr_##_attrname##_possible_values.attr,			\
		&attr_##_attrname##_type.attr,					\
		NULL								\
	};									\
	static const struct attribute_group _attrname##_attr_group = {		\
		.name = _fsname, .attrs = _attrname##_attrs			\
	}

/* CPU core attributes need a little different in setup */
#define ATTR_GROUP_CORES_RW(_attrname, _fsname, _dispname)		\
	__ATTR_SHOW_FMT(scalar_increment, _attrname, "%d\n", 1);	\
	__ATTR_SHOW_FMT(display_name, _attrname, "%s\n", _dispname);	\
	static struct kobj_attribute attr_##_attrname##_current_value =	\
		__ASUS_ATTR_RW(_attrname, current_value);		\
	static struct kobj_attribute attr_##_attrname##_default_value = \
		__ASUS_ATTR_RO(_attrname, default_value);		\
	static struct kobj_attribute attr_##_attrname##_min_value =	\
		__ASUS_ATTR_RO(_attrname, min_value);			\
	static struct kobj_attribute attr_##_attrname##_max_value =	\
		__ASUS_ATTR_RO(_attrname, max_value);			\
	static struct kobj_attribute attr_##_attrname##_type =		\
		__ASUS_ATTR_RO_AS(type, int_type_show);			\
	static struct attribute *_attrname##_attrs[] = {		\
		&attr_##_attrname##_current_value.attr,			\
		&attr_##_attrname##_default_value.attr,			\
		&attr_##_attrname##_min_value.attr,			\
		&attr_##_attrname##_max_value.attr,			\
		&attr_##_attrname##_scalar_increment.attr,		\
		&attr_##_attrname##_display_name.attr,			\
		&attr_##_attrname##_type.attr,				\
		NULL							\
	};								\
	static const struct attribute_group _attrname##_attr_group = {	\
		.name = _fsname, .attrs = _attrname##_attrs		\
	}

#define ATTR_GROUP_INT_VALUE_ONLY_RO(_attrname, _fsname, _wmi, _dispname)	\
	WMI_SHOW_INT(_attrname##_current_value, "%d\n", _wmi);			\
	static struct kobj_attribute attr_##_attrname##_current_value =		\
		__ASUS_ATTR_RO(_attrname, current_value);			\
	__ATTR_SHOW_FMT(display_name, _attrname, "%s\n", _dispname);		\
	static struct kobj_attribute attr_##_attrname##_type =			\
		__ASUS_ATTR_RO_AS(type, int_type_show);				\
	static struct attribute *_attrname##_attrs[] = {			\
		&attr_##_attrname##_current_value.attr,				\
		&attr_##_attrname##_display_name.attr,				\
		&attr_##_attrname##_type.attr, NULL				\
	};									\
	static const struct attribute_group _attrname##_attr_group = {		\
		.name = _fsname, .attrs = _attrname##_attrs			\
	}

/*
 * ROG PPT attributes need a little different in setup as they
 * require rog_tunables members.
 */

#define __ROG_TUNABLE_SHOW(_prop, _attrname, _val)				\
	static ssize_t _attrname##_##_prop##_show(				\
		struct kobject *kobj, struct kobj_attribute *attr, char *buf)	\
	{									\
		struct rog_tunables *tunables = get_current_tunables();		\
										\
		if (!tunables || !tunables->power_limits)			\
			return -ENODEV;						\
										\
		return sysfs_emit(buf, "%d\n", tunables->power_limits->_val);	\
	}									\
	static struct kobj_attribute attr_##_attrname##_##_prop =		\
		__ASUS_ATTR_RO(_attrname, _prop)

#define __ROG_TUNABLE_SHOW_DEFAULT(_attrname)					\
	static ssize_t _attrname##_default_value_show(				\
		struct kobject *kobj, struct kobj_attribute *attr, char *buf)	\
	{									\
		struct rog_tunables *tunables = get_current_tunables();		\
										\
		if (!tunables || !tunables->power_limits)			\
			return -ENODEV;						\
										\
		return sysfs_emit(						\
			buf, "%d\n",						\
			tunables->power_limits->_attrname##_def ?		\
				tunables->power_limits->_attrname##_def :	\
				tunables->power_limits->_attrname##_max);	\
	}									\
	static struct kobj_attribute attr_##_attrname##_default_value =		\
		__ASUS_ATTR_RO(_attrname, default_value)

#define __ROG_TUNABLE_RW(_attr, _wmi)						\
	static ssize_t _attr##_current_value_store(				\
		struct kobject *kobj, struct kobj_attribute *attr,		\
		const char *buf, size_t count)					\
	{									\
		struct rog_tunables *tunables = get_current_tunables();		\
										\
		if (!tunables || !tunables->power_limits)			\
			return -ENODEV;						\
										\
		return attr_uint_store(kobj, attr, buf, count,			\
				       tunables->power_limits->_attr##_min,	\
				       tunables->power_limits->_attr##_max,	\
				       &tunables->_attr, _wmi);			\
	}									\
	static ssize_t _attr##_current_value_show(				\
		struct kobject *kobj, struct kobj_attribute *attr, char *buf)	\
	{									\
		struct rog_tunables *tunables = get_current_tunables();		\
										\
		if (!tunables)							\
			return -ENODEV;						\
										\
		return sysfs_emit(buf, "%u\n", tunables->_attr);		\
	}									\
	static struct kobj_attribute attr_##_attr##_current_value =		\
		__ASUS_ATTR_RW(_attr, current_value)

#define ATTR_GROUP_ROG_TUNABLE(_attrname, _fsname, _wmi, _dispname)	\
	__ROG_TUNABLE_RW(_attrname, _wmi);				\
	__ROG_TUNABLE_SHOW_DEFAULT(_attrname);				\
	__ROG_TUNABLE_SHOW(min_value, _attrname, _attrname##_min);	\
	__ROG_TUNABLE_SHOW(max_value, _attrname, _attrname##_max);	\
	__ATTR_SHOW_FMT(scalar_increment, _attrname, "%d\n", 1);	\
	__ATTR_SHOW_FMT(display_name, _attrname, "%s\n", _dispname);	\
	static struct kobj_attribute attr_##_attrname##_type =		\
		__ASUS_ATTR_RO_AS(type, int_type_show);			\
	static struct attribute *_attrname##_attrs[] = {		\
		&attr_##_attrname##_current_value.attr,			\
		&attr_##_attrname##_default_value.attr,			\
		&attr_##_attrname##_min_value.attr,			\
		&attr_##_attrname##_max_value.attr,			\
		&attr_##_attrname##_scalar_increment.attr,		\
		&attr_##_attrname##_display_name.attr,			\
		&attr_##_attrname##_type.attr,				\
		NULL							\
	};								\
	static const struct attribute_group _attrname##_attr_group = {	\
		.name = _fsname, .attrs = _attrname##_attrs		\
	}

/* Default is always the maximum value unless *_def is specified */
struct power_limits {
	u8 ppt_pl1_spl_min;
	u8 ppt_pl1_spl_def;
	u8 ppt_pl1_spl_max;
	u8 ppt_pl2_sppt_min;
	u8 ppt_pl2_sppt_def;
	u8 ppt_pl2_sppt_max;
	u8 ppt_pl3_fppt_min;
	u8 ppt_pl3_fppt_def;
	u8 ppt_pl3_fppt_max;
	u8 ppt_apu_sppt_min;
	u8 ppt_apu_sppt_def;
	u8 ppt_apu_sppt_max;
	u8 ppt_platform_sppt_min;
	u8 ppt_platform_sppt_def;
	u8 ppt_platform_sppt_max;
	/* Nvidia GPU specific, default is always max */
	u8 nv_dynamic_boost_def; // unused. exists for macro
	u8 nv_dynamic_boost_min;
	u8 nv_dynamic_boost_max;
	u8 nv_temp_target_def; // unused. exists for macro
	u8 nv_temp_target_min;
	u8 nv_temp_target_max;
	u8 nv_tgp_def; // unused. exists for macro
	u8 nv_tgp_min;
	u8 nv_tgp_max;
};

struct power_data {
		const struct power_limits *ac_data;
		const struct power_limits *dc_data;
		bool requires_fan_curve;
};

/*
 * For each avilable attribute there must be a min and a max.
 * _def is not required and will be assumed to be default == max if missing.
 */
static const struct dmi_system_id power_limits[] = {
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "FA401W"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 15,
				.ppt_pl1_spl_max = 80,
				.ppt_pl2_sppt_min = 35,
				.ppt_pl2_sppt_max = 80,
				.ppt_pl3_fppt_min = 35,
				.ppt_pl3_fppt_max = 80,
				.nv_dynamic_boost_min = 5,
				.nv_dynamic_boost_max = 25,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
				.nv_tgp_min = 55,
				.nv_tgp_max = 75,
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 25,
				.ppt_pl1_spl_max = 30,
				.ppt_pl2_sppt_min = 31,
				.ppt_pl2_sppt_max = 44,
				.ppt_pl3_fppt_min = 45,
				.ppt_pl3_fppt_max = 65,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			},
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "FA507N"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 15,
				.ppt_pl1_spl_max = 80,
				.ppt_pl2_sppt_min = 35,
				.ppt_pl2_sppt_max = 80,
				.ppt_pl3_fppt_min = 35,
				.ppt_pl3_fppt_max = 80,
				.nv_dynamic_boost_min = 5,
				.nv_dynamic_boost_max = 25,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 15,
				.ppt_pl1_spl_def = 45,
				.ppt_pl1_spl_max = 65,
				.ppt_pl2_sppt_min = 35,
				.ppt_pl2_sppt_def = 54,
				.ppt_pl2_sppt_max = 65,
				.ppt_pl3_fppt_min = 35,
				.ppt_pl3_fppt_max = 65,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			}
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "FA507R"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 15,
				.ppt_pl1_spl_max = 80,
				.ppt_pl2_sppt_min = 25,
				.ppt_pl2_sppt_max = 80,
				.ppt_pl3_fppt_min = 35,
				.ppt_pl3_fppt_max = 80
			},
			.dc_data = NULL
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "FA507X"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 15,
				.ppt_pl1_spl_max = 80,
				.ppt_pl2_sppt_min = 35,
				.ppt_pl2_sppt_max = 80,
				.ppt_pl3_fppt_min = 35,
				.ppt_pl3_fppt_max = 80,
				.nv_dynamic_boost_min = 5,
				.nv_dynamic_boost_max = 20,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
				.nv_tgp_min = 55,
				.nv_tgp_max = 85,
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 15,
				.ppt_pl1_spl_def = 45,
				.ppt_pl1_spl_max = 65,
				.ppt_pl2_sppt_min = 35,
				.ppt_pl2_sppt_def = 54,
				.ppt_pl2_sppt_max = 65,
				.ppt_pl3_fppt_min = 35,
				.ppt_pl3_fppt_max = 65,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			}
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "FA507Z"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 28,
				.ppt_pl1_spl_max = 65,
				.ppt_pl2_sppt_min = 28,
				.ppt_pl2_sppt_max = 105,
				.nv_dynamic_boost_min = 5,
				.nv_dynamic_boost_max = 15,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
				.nv_tgp_min = 55,
				.nv_tgp_max = 85,
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 25,
				.ppt_pl1_spl_max = 45,
				.ppt_pl2_sppt_min = 35,
				.ppt_pl2_sppt_max = 60,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			}
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "FA607P"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 30,
				.ppt_pl1_spl_def = 100,
				.ppt_pl1_spl_max = 135,
				.ppt_pl2_sppt_min = 30,
				.ppt_pl2_sppt_def = 115,
				.ppt_pl2_sppt_max = 135,
				.ppt_pl3_fppt_min = 30,
				.ppt_pl3_fppt_max = 135,
				.nv_dynamic_boost_min = 5,
				.nv_dynamic_boost_max = 25,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
				.nv_tgp_min = 55,
				.nv_tgp_max = 115,
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 25,
				.ppt_pl1_spl_def = 45,
				.ppt_pl1_spl_max = 80,
				.ppt_pl2_sppt_min = 25,
				.ppt_pl2_sppt_def = 60,
				.ppt_pl2_sppt_max = 80,
				.ppt_pl3_fppt_min = 25,
				.ppt_pl3_fppt_max = 80,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			}
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "FA617NS"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_apu_sppt_min = 15,
				.ppt_apu_sppt_max = 80,
				.ppt_platform_sppt_min = 30,
				.ppt_platform_sppt_max = 120
			},
			.dc_data = &(struct power_limits) {
				.ppt_apu_sppt_min = 25,
				.ppt_apu_sppt_max = 35,
				.ppt_platform_sppt_min = 45,
				.ppt_platform_sppt_max = 100
			}
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "FA617NT"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_apu_sppt_min = 15,
				.ppt_apu_sppt_max = 80,
				.ppt_platform_sppt_min = 30,
				.ppt_platform_sppt_max = 115
			},
			.dc_data = &(struct power_limits) {
				.ppt_apu_sppt_min = 15,
				.ppt_apu_sppt_max = 45,
				.ppt_platform_sppt_min = 30,
				.ppt_platform_sppt_max = 50
			}
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "FA617XS"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_apu_sppt_min = 15,
				.ppt_apu_sppt_max = 80,
				.ppt_platform_sppt_min = 30,
				.ppt_platform_sppt_max = 120,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			},
			.dc_data = &(struct power_limits) {
				.ppt_apu_sppt_min = 25,
				.ppt_apu_sppt_max = 35,
				.ppt_platform_sppt_min = 45,
				.ppt_platform_sppt_max = 100,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			}
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "FX507Z"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 28,
				.ppt_pl1_spl_max = 90,
				.ppt_pl2_sppt_min = 28,
				.ppt_pl2_sppt_max = 135,
				.nv_dynamic_boost_min = 5,
				.nv_dynamic_boost_max = 15,
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 25,
				.ppt_pl1_spl_max = 45,
				.ppt_pl2_sppt_min = 35,
				.ppt_pl2_sppt_max = 60,
			},
			.requires_fan_curve = true,
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "GA401Q"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 15,
				.ppt_pl1_spl_max = 80,
				.ppt_pl2_sppt_min = 15,
				.ppt_pl2_sppt_max = 80,
			},
			.dc_data = NULL
		},
	},
	{
		.matches = {
			// This model is full AMD. No Nvidia dGPU.
			DMI_MATCH(DMI_BOARD_NAME, "GA402R"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_apu_sppt_min = 15,
				.ppt_apu_sppt_max = 80,
				.ppt_platform_sppt_min = 30,
				.ppt_platform_sppt_max = 115,
			},
			.dc_data = &(struct power_limits) {
				.ppt_apu_sppt_min = 25,
				.ppt_apu_sppt_def = 30,
				.ppt_apu_sppt_max = 45,
				.ppt_platform_sppt_min = 40,
				.ppt_platform_sppt_max = 60,
			}
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "GA402X"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 15,
				.ppt_pl1_spl_def = 35,
				.ppt_pl1_spl_max = 80,
				.ppt_pl2_sppt_min = 25,
				.ppt_pl2_sppt_def = 65,
				.ppt_pl2_sppt_max = 80,
				.ppt_pl3_fppt_min = 35,
				.ppt_pl3_fppt_max = 80,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 15,
				.ppt_pl1_spl_max = 35,
				.ppt_pl2_sppt_min = 25,
				.ppt_pl2_sppt_max = 35,
				.ppt_pl3_fppt_min = 35,
				.ppt_pl3_fppt_max = 65,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			},
			.requires_fan_curve = true,
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "GA403U"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 15,
				.ppt_pl1_spl_max = 80,
				.ppt_pl2_sppt_min = 25,
				.ppt_pl2_sppt_max = 80,
				.ppt_pl3_fppt_min = 35,
				.ppt_pl3_fppt_max = 80,
				.nv_dynamic_boost_min = 5,
				.nv_dynamic_boost_max = 25,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
				.nv_tgp_min = 55,
				.nv_tgp_max = 65,
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 15,
				.ppt_pl1_spl_max = 35,
				.ppt_pl2_sppt_min = 25,
				.ppt_pl2_sppt_max = 35,
				.ppt_pl3_fppt_min = 35,
				.ppt_pl3_fppt_max = 65,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			},
			.requires_fan_curve = true,
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "GA503R"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 15,
				.ppt_pl1_spl_def = 35,
				.ppt_pl1_spl_max = 80,
				.ppt_pl2_sppt_min = 35,
				.ppt_pl2_sppt_def = 65,
				.ppt_pl2_sppt_max = 80,
				.ppt_pl3_fppt_min = 35,
				.ppt_pl3_fppt_max = 80,
				.nv_dynamic_boost_min = 5,
				.nv_dynamic_boost_max = 20,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 15,
				.ppt_pl1_spl_def = 25,
				.ppt_pl1_spl_max = 65,
				.ppt_pl2_sppt_min = 35,
				.ppt_pl2_sppt_def = 54,
				.ppt_pl2_sppt_max = 60,
				.ppt_pl3_fppt_min = 35,
				.ppt_pl3_fppt_max = 65
			}
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "GA605W"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 15,
				.ppt_pl1_spl_max = 80,
				.ppt_pl2_sppt_min = 35,
				.ppt_pl2_sppt_max = 80,
				.ppt_pl3_fppt_min = 35,
				.ppt_pl3_fppt_max = 80,
				.nv_dynamic_boost_min = 5,
				.nv_dynamic_boost_max = 20,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
				.nv_tgp_min = 55,
				.nv_tgp_max = 85,
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 25,
				.ppt_pl1_spl_max = 35,
				.ppt_pl2_sppt_min = 31,
				.ppt_pl2_sppt_max = 44,
				.ppt_pl3_fppt_min = 45,
				.ppt_pl3_fppt_max = 65,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			},
			.requires_fan_curve = true,
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "GU603Z"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 25,
				.ppt_pl1_spl_max = 60,
				.ppt_pl2_sppt_min = 25,
				.ppt_pl2_sppt_max = 135,
				/* Only allowed in AC mode */
				.nv_dynamic_boost_min = 5,
				.nv_dynamic_boost_max = 20,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 25,
				.ppt_pl1_spl_max = 40,
				.ppt_pl2_sppt_min = 25,
				.ppt_pl2_sppt_max = 40,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			}
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "GU604V"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 65,
				.ppt_pl1_spl_max = 120,
				.ppt_pl2_sppt_min = 65,
				.ppt_pl2_sppt_max = 150,
				/* Only allowed in AC mode */
				.nv_dynamic_boost_min = 5,
				.nv_dynamic_boost_max = 25,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 25,
				.ppt_pl1_spl_max = 40,
				.ppt_pl2_sppt_min = 35,
				.ppt_pl2_sppt_def = 40,
				.ppt_pl2_sppt_max = 60,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			}
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "GU605M"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 28,
				.ppt_pl1_spl_max = 90,
				.ppt_pl2_sppt_min = 28,
				.ppt_pl2_sppt_max = 135,
				.nv_dynamic_boost_min = 5,
				.nv_dynamic_boost_max = 20,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 25,
				.ppt_pl1_spl_max = 35,
				.ppt_pl2_sppt_min = 38,
				.ppt_pl2_sppt_max = 53,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			},
			.requires_fan_curve = true,
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "GV301Q"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 15,
				.ppt_pl1_spl_max = 45,
				.ppt_pl2_sppt_min = 65,
				.ppt_pl2_sppt_max = 80,
			},
			.dc_data = NULL
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "GV301R"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 15,
				.ppt_pl1_spl_max = 45,
				.ppt_pl2_sppt_min = 25,
				.ppt_pl2_sppt_max = 54,
				.ppt_pl3_fppt_min = 35,
				.ppt_pl3_fppt_max = 65,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 15,
				.ppt_pl1_spl_max = 35,
				.ppt_pl2_sppt_min = 25,
				.ppt_pl2_sppt_max = 35,
				.ppt_pl3_fppt_min = 35,
				.ppt_pl3_fppt_max = 65,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			}
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "GV601R"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 15,
				.ppt_pl1_spl_def = 35,
				.ppt_pl1_spl_max = 90,
				.ppt_pl2_sppt_min = 35,
				.ppt_pl2_sppt_def = 54,
				.ppt_pl2_sppt_max = 100,
				.ppt_pl3_fppt_min = 35,
				.ppt_pl3_fppt_def = 80,
				.ppt_pl3_fppt_max = 125,
				.nv_dynamic_boost_min = 5,
				.nv_dynamic_boost_max = 25,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 15,
				.ppt_pl1_spl_def = 28,
				.ppt_pl1_spl_max = 65,
				.ppt_pl2_sppt_min = 35,
				.ppt_pl2_sppt_def = 54,
				.ppt_pl2_sppt_def = 40,
				.ppt_pl2_sppt_max = 60,
				.ppt_pl3_fppt_min = 35,
				.ppt_pl3_fppt_def = 80,
				.ppt_pl3_fppt_max = 65,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			}
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "GV601V"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 28,
				.ppt_pl1_spl_def = 100,
				.ppt_pl1_spl_max = 110,
				.ppt_pl2_sppt_min = 28,
				.ppt_pl2_sppt_max = 135,
				.nv_dynamic_boost_min = 5,
				.nv_dynamic_boost_max = 20,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 25,
				.ppt_pl1_spl_max = 40,
				.ppt_pl2_sppt_min = 35,
				.ppt_pl2_sppt_def = 40,
				.ppt_pl2_sppt_max = 60,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			}
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "GX650P"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 15,
				.ppt_pl1_spl_def = 110,
				.ppt_pl1_spl_max = 130,
				.ppt_pl2_sppt_min = 35,
				.ppt_pl2_sppt_def = 125,
				.ppt_pl2_sppt_max = 130,
				.ppt_pl3_fppt_min = 35,
				.ppt_pl3_fppt_def = 125,
				.ppt_pl3_fppt_max = 135,
				.nv_dynamic_boost_min = 5,
				.nv_dynamic_boost_max = 25,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 15,
				.ppt_pl1_spl_def = 25,
				.ppt_pl1_spl_max = 65,
				.ppt_pl2_sppt_min = 35,
				.ppt_pl2_sppt_def = 35,
				.ppt_pl2_sppt_max = 65,
				.ppt_pl3_fppt_min = 35,
				.ppt_pl3_fppt_def = 42,
				.ppt_pl3_fppt_max = 65,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			}
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "G513I"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				/* Yes this laptop is very limited */
				.ppt_pl1_spl_min = 15,
				.ppt_pl1_spl_max = 80,
				.ppt_pl2_sppt_min = 15,
				.ppt_pl2_sppt_max = 80,
			},
			.dc_data = NULL,
			.requires_fan_curve = true,
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "G513QM"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				/* Yes this laptop is very limited */
				.ppt_pl1_spl_min = 15,
				.ppt_pl1_spl_max = 100,
				.ppt_pl2_sppt_min = 15,
				.ppt_pl2_sppt_max = 190,
			},
			.dc_data = NULL,
			.requires_fan_curve = true,
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "G513R"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 35,
				.ppt_pl1_spl_max = 90,
				.ppt_pl2_sppt_min = 54,
				.ppt_pl2_sppt_max = 100,
				.ppt_pl3_fppt_min = 54,
				.ppt_pl3_fppt_max = 125,
				.nv_dynamic_boost_min = 5,
				.nv_dynamic_boost_max = 25,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 28,
				.ppt_pl1_spl_max = 50,
				.ppt_pl2_sppt_min = 28,
				.ppt_pl2_sppt_max = 50,
				.ppt_pl3_fppt_min = 28,
				.ppt_pl3_fppt_max = 65,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			},
			.requires_fan_curve = true,
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "G614J"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 28,
				.ppt_pl1_spl_max = 140,
				.ppt_pl2_sppt_min = 28,
				.ppt_pl2_sppt_max = 175,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
				.nv_dynamic_boost_min = 5,
				.nv_dynamic_boost_max = 25,
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 25,
				.ppt_pl1_spl_max = 55,
				.ppt_pl2_sppt_min = 25,
				.ppt_pl2_sppt_max = 70,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			},
			.requires_fan_curve = true,
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "G634J"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 28,
				.ppt_pl1_spl_max = 140,
				.ppt_pl2_sppt_min = 28,
				.ppt_pl2_sppt_max = 175,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
				.nv_dynamic_boost_min = 5,
				.nv_dynamic_boost_max = 25,
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 25,
				.ppt_pl1_spl_max = 55,
				.ppt_pl2_sppt_min = 25,
				.ppt_pl2_sppt_max = 70,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			},
			.requires_fan_curve = true,
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "G733C"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 28,
				.ppt_pl1_spl_max = 170,
				.ppt_pl2_sppt_min = 28,
				.ppt_pl2_sppt_max = 175,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
				.nv_dynamic_boost_min = 5,
				.nv_dynamic_boost_max = 25,
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 28,
				.ppt_pl1_spl_max = 35,
				.ppt_pl2_sppt_min = 28,
				.ppt_pl2_sppt_max = 35,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			},
			.requires_fan_curve = true,
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "G733P"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 30,
				.ppt_pl1_spl_def = 100,
				.ppt_pl1_spl_max = 130,
				.ppt_pl2_sppt_min = 65,
				.ppt_pl2_sppt_def = 125,
				.ppt_pl2_sppt_max = 130,
				.ppt_pl3_fppt_min = 65,
				.ppt_pl3_fppt_def = 125,
				.ppt_pl3_fppt_max = 130,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
				.nv_dynamic_boost_min = 5,
				.nv_dynamic_boost_max = 25,
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 25,
				.ppt_pl1_spl_max = 65,
				.ppt_pl2_sppt_min = 25,
				.ppt_pl2_sppt_max = 65,
				.ppt_pl3_fppt_min = 35,
				.ppt_pl3_fppt_max = 75,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			},
			.requires_fan_curve = true,
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "G814J"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 28,
				.ppt_pl1_spl_max = 140,
				.ppt_pl2_sppt_min = 28,
				.ppt_pl2_sppt_max = 140,
				.nv_dynamic_boost_min = 5,
				.nv_dynamic_boost_max = 25,
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 25,
				.ppt_pl1_spl_max = 55,
				.ppt_pl2_sppt_min = 25,
				.ppt_pl2_sppt_max = 70,
			},
			.requires_fan_curve = true,
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "G834J"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 28,
				.ppt_pl1_spl_max = 140,
				.ppt_pl2_sppt_min = 28,
				.ppt_pl2_sppt_max = 175,
				.nv_dynamic_boost_min = 5,
				.nv_dynamic_boost_max = 25,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 25,
				.ppt_pl1_spl_max = 55,
				.ppt_pl2_sppt_min = 25,
				.ppt_pl2_sppt_max = 70,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			},
			.requires_fan_curve = true,
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "H7606W"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 15,
				.ppt_pl1_spl_max = 80,
				.ppt_pl2_sppt_min = 35,
				.ppt_pl2_sppt_max = 80,
				.ppt_pl3_fppt_min = 35,
				.ppt_pl3_fppt_max = 80,
				.nv_dynamic_boost_min = 5,
				.nv_dynamic_boost_max = 20,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
				.nv_tgp_min = 55,
				.nv_tgp_max = 85,
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 25,
				.ppt_pl1_spl_max = 35,
				.ppt_pl2_sppt_min = 31,
				.ppt_pl2_sppt_max = 44,
				.ppt_pl3_fppt_min = 45,
				.ppt_pl3_fppt_max = 65,
				.nv_temp_target_min = 75,
				.nv_temp_target_max = 87,
			}
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "RC71"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 7,
				.ppt_pl1_spl_max = 30,
				.ppt_pl2_sppt_min = 15,
				.ppt_pl2_sppt_max = 43,
				.ppt_pl3_fppt_min = 15,
				.ppt_pl3_fppt_max = 53
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 7,
				.ppt_pl1_spl_def = 15,
				.ppt_pl1_spl_max = 25,
				.ppt_pl2_sppt_min = 15,
				.ppt_pl2_sppt_def = 20,
				.ppt_pl2_sppt_max = 30,
				.ppt_pl3_fppt_min = 15,
				.ppt_pl3_fppt_def = 25,
				.ppt_pl3_fppt_max = 35
			}
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "RC72"),
		},
		.driver_data = &(struct power_data) {
			.ac_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 7,
				.ppt_pl1_spl_max = 30,
				.ppt_pl2_sppt_min = 15,
				.ppt_pl2_sppt_max = 43,
				.ppt_pl3_fppt_min = 15,
				.ppt_pl3_fppt_max = 53
			},
			.dc_data = &(struct power_limits) {
				.ppt_pl1_spl_min = 7,
				.ppt_pl1_spl_def = 17,
				.ppt_pl1_spl_max = 25,
				.ppt_pl2_sppt_min = 15,
				.ppt_pl2_sppt_def = 24,
				.ppt_pl2_sppt_max = 30,
				.ppt_pl3_fppt_min = 15,
				.ppt_pl3_fppt_def = 30,
				.ppt_pl3_fppt_max = 35
			}
		},
	},
	{}
};

#endif /* _ASUS_ARMOURY_H_ */
