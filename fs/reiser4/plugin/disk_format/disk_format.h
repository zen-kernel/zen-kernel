/* Copyright 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

/* identifiers for disk layouts, they are also used as indexes in array of disk
   plugins */

#if !defined( __REISER4_DISK_FORMAT_H__ )
#define __REISER4_DISK_FORMAT_H__

typedef enum {
	/* standard reiser4 disk layout plugin id */
	FORMAT40_ID,
	LAST_FORMAT_ID
} disk_format_id;

/* __REISER4_DISK_FORMAT_H__ */
#endif

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   scroll-step: 1
   End:
*/
