/*
 * Copyright (C) 2013 Kay Sievers
 * Copyright (C) 2013 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
 * Copyright (C) 2013 Daniel Mack <daniel@zonque.org>
 * Copyright (C) 2013 Linux Foundation
 *
 * kdbus is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/ctype.h>
#include <linux/fs.h>

#include "util.h"

/**
 * kdbus_devname_valid - validate names showing up in /dev
 * @name:		Name of namepspace, bus, endpoint
 *
 * Returns: 0 if the given name is valid, otherwise negative errno
 */
int kdbus_devname_valid(const char *name)
{
	unsigned int i;
	size_t len;

	len = strlen(name);
	if (len == 0)
		return -EINVAL;

	for (i = 0; i < len; i++) {
		if (isalpha(name[i]))
			continue;
		if (isdigit(name[i]))
			continue;
		if (i > 0 && i + 1 < len && strchr("-.", name[i]))
			continue;

		return -EINVAL;
	}

	return 0;
}
