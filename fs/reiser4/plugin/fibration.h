/* Copyright 2004 by Hans Reiser, licensing governed by reiser4/README */

/* Fibration plugin used by hashed directory plugin to segment content
 * of directory. See fs/reiser4/plugin/fibration.c for more on this. */

#if !defined(__FS_REISER4_PLUGIN_FIBRATION_H__)
#define __FS_REISER4_PLUGIN_FIBRATION_H__

#include "plugin_header.h"

typedef struct fibration_plugin {
	/* generic fields */
	plugin_header h;

	 __u64(*fibre) (const struct inode *dir, const char *name, int len);
} fibration_plugin;

typedef enum {
	FIBRATION_LEXICOGRAPHIC,
	FIBRATION_DOT_O,
	FIBRATION_EXT_1,
	FIBRATION_EXT_3,
	LAST_FIBRATION_ID
} reiser4_fibration_id;

/* __FS_REISER4_PLUGIN_FIBRATION_H__ */
#endif

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
