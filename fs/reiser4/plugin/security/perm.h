/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

/* Perm (short for "permissions") plugins common stuff. */

#if !defined( __REISER4_PERM_H__ )
#define __REISER4_PERM_H__

#include "../../forward.h"
#include "../plugin_header.h"

#include <linux/types.h>

/* Definition of permission plugin */
/* NIKITA-FIXME-HANS: define what this is targeted for.
   It does not seem to be intended for use with sys_reiser4.  Explain. */

/* NOTE-EDWARD: This seems to be intended for deprecated sys_reiser4.
   Consider it like a temporary "seam" and reserved pset member.
   If you have something usefull to add, then rename this plugin and add here */
typedef struct perm_plugin {
	/* generic plugin fields */
	plugin_header h;
} perm_plugin;

typedef enum { NULL_PERM_ID, LAST_PERM_ID } reiser4_perm_id;

/* __REISER4_PERM_H__ */
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
