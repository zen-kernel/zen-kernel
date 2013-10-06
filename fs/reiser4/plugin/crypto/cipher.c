/* Copyright 2001, 2002, 2003 by Hans Reiser,
   licensing governed by reiser4/README */
/* Reiser4 cipher transform plugins */

#include "../../debug.h"
#include "../plugin.h"

cipher_plugin cipher_plugins[LAST_CIPHER_ID] = {
	[NONE_CIPHER_ID] = {
		.h = {
			.type_id = REISER4_CIPHER_PLUGIN_TYPE,
			.id = NONE_CIPHER_ID,
			.pops = NULL,
			.label = "none",
			.desc = "no cipher transform",
			.linkage = {NULL, NULL}
		},
		.alloc = NULL,
		.free = NULL,
		.scale = NULL,
		.align_stream = NULL,
		.setkey = NULL,
		.encrypt = NULL,
		.decrypt = NULL
	}
};

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
