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

#ifndef __KDBUS_POLICY_H
#define __KDBUS_POLICY_H

struct kdbus_conn;
struct kdbus_policy_db;

int kdbus_policy_db_new(struct kdbus_policy_db **db);
void kdbus_policy_db_free(struct kdbus_policy_db *db);
int kdbus_cmd_policy_set_from_user(struct kdbus_policy_db *db,
				   void __user *buf);
int kdbus_policy_db_check_send_access(struct kdbus_policy_db *db,
				      struct kdbus_conn *conn_src,
				      struct kdbus_conn *conn_dst);
bool kdbus_policy_db_check_own_access(struct kdbus_policy_db *db,
				      struct kdbus_conn *conn,
				      const char *name);
void kdbus_policy_db_remove_conn(struct kdbus_policy_db *db,
				 struct kdbus_conn *conn);
#endif
