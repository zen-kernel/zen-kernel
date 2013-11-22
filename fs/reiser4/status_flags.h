/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Here we declare structures and flags that store reiser4 status on disk.
   The status that helps us to find out if the filesystem is valid or if it
   contains some critical, or not so critical errors */

#if !defined(__REISER4_STATUS_FLAGS_H__)
#define __REISER4_STATUS_FLAGS_H__

#include "dformat.h"
/* These are major status flags */
#define REISER4_STATUS_OK 0
#define REISER4_STATUS_CORRUPTED 0x1
#define REISER4_STATUS_DAMAGED 0x2
#define REISER4_STATUS_DESTROYED 0x4
#define REISER4_STATUS_IOERROR 0x8

/* Return values for reiser4_status_query() */
#define REISER4_STATUS_MOUNT_OK 0
#define REISER4_STATUS_MOUNT_WARN 1
#define REISER4_STATUS_MOUNT_RO 2
#define REISER4_STATUS_MOUNT_UNKNOWN -1

#define REISER4_TEXTERROR_LEN 256

#define REISER4_STATUS_MAGIC "ReiSeR4StATusBl"
/* We probably need to keep its size under sector size which is 512 bytes */
struct reiser4_status {
	char magic[16];
	d64 status;		/* Current FS state */
	d64 extended_status;	/* Any additional info that might have sense in
				 * addition to "status". E.g. last sector where
				 * io error happened if status is
				 * "io error encountered" */
	d64 stacktrace[10];	/* Last ten functional calls made (addresses) */
	char texterror[REISER4_TEXTERROR_LEN];	/* Any error message if
						 * appropriate, otherwise filled
						 * with zeroes */
};

int reiser4_status_init(reiser4_block_nr block);
int reiser4_status_query(u64 *status, u64 *extended);
int reiser4_status_write(u64 status, u64 extended_status, char *message);
int reiser4_status_finish(void);

#endif
