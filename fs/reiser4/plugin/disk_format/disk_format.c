/* Copyright 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

#include "../../debug.h"
#include "../plugin_header.h"
#include "disk_format40.h"
#include "disk_format.h"
#include "../plugin.h"

/* initialization of disk layout plugins */
disk_format_plugin format_plugins[LAST_FORMAT_ID] = {
	[FORMAT40_ID] = {
		.h = {
			.type_id = REISER4_FORMAT_PLUGIN_TYPE,
			.id = FORMAT40_ID,
			.pops = NULL,
			.label = "reiser40",
			.desc = "standard disk layout for reiser40",
			.linkage = {NULL, NULL}
		},
		.init_format = init_format_format40,
		.root_dir_key = root_dir_key_format40,
		.release = release_format40,
		.log_super = log_super_format40,
		.check_open = check_open_format40,
		.version_update = version_update_format40
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
