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

#ifndef __KDBUS_MATCH_H
#define __KDBUS_MATCH_H

struct kdbus_conn;
struct kdbus_kmsg;
struct kdbus_match_db;

int kdbus_match_db_new(struct kdbus_match_db **db);
void kdbus_match_db_free(struct kdbus_match_db *db);
int kdbus_match_db_add(struct kdbus_conn *conn, void __user *buf);
int kdbus_match_db_remove(struct kdbus_conn *conn, void __user *buf);
bool kdbus_match_db_match_kmsg(struct kdbus_match_db *db,
			       struct kdbus_conn *conn_src,
			       struct kdbus_kmsg *kmsg);
#endif
