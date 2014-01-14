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

#ifndef __KDBUS_DEFAULTS_H
#define __KDBUS_DEFAULTS_H

/* maximum size of message header and items */
#define KDBUS_MSG_MAX_SIZE		SZ_8K

/* maximum number of message items */
#define KDBUS_MSG_MAX_ITEMS		128

/* maximum number of passed file descriptors */
#define KDBUS_MSG_MAX_FDS		256

/* maximum message payload size */
#define KDBUS_MSG_MAX_PAYLOAD_VEC_SIZE	SZ_2M

/* maximum length of well-known bus name */
#define KDBUS_NAME_MAX_LEN		255

/* maximum length of bus, ns, ep name */
#define KDBUS_MAKE_MAX_LEN		63

/* maximum size of make data */
#define KDBUS_MAKE_MAX_SIZE		SZ_32K

/* maximum size of hello data */
#define KDBUS_HELLO_MAX_SIZE		SZ_32K

/* maximum size of match data */
#define KDBUS_MATCH_MAX_SIZE		SZ_32K

/* maximum size of policy data */
#define KDBUS_POLICY_MAX_SIZE		SZ_32K

/* maximum number of queued messages per connection */
#define KDBUS_CONN_MAX_MSGS		64

/* maximum number of well-known names */
#define KDBUS_CONN_MAX_NAMES		64

/* maximum number of queud requests waiting ot a reply */
#define KDBUS_CONN_MAX_REQUESTS_PENDING	64

#endif
