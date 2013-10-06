/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Formatting policy plugins */

/*
 * Formatting policy plugin is used by object plugin (of regular file) to
 * convert file between two representations.
 *
 * Currently following policies are implemented:
 *  never store file in formatted nodes
 *  always store file in formatted nodes
 *  store file in formatted nodes if file is smaller than 4 blocks (default)
 */

#include "../tree.h"
#include "../inode.h"
#include "../super.h"
#include "object.h"
#include "plugin.h"
#include "node/node.h"
#include "plugin_header.h"

#include <linux/pagemap.h>
#include <linux/fs.h>		/* For struct inode */

/**
 * have_formatting_never -
 * @inode:
 * @size:
 *
 *
 */
/* Never store file's tail as direct item */
/* Audited by: green(2002.06.12) */
static int have_formatting_never(const struct inode *inode UNUSED_ARG
		      /* inode to operate on */ ,
		      loff_t size UNUSED_ARG/* new object size */)
{
	return 0;
}

/* Always store file's tail as direct item */
/* Audited by: green(2002.06.12) */
static int
have_formatting_always(const struct inode *inode UNUSED_ARG
		       /* inode to operate on */ ,
		       loff_t size UNUSED_ARG/* new object size */)
{
	return 1;
}

/* This function makes test if we should store file denoted @inode as tails only
   or as extents only. */
static int
have_formatting_default(const struct inode *inode UNUSED_ARG
			/* inode to operate on */ ,
			loff_t size/* new object size */)
{
	assert("umka-1253", inode != NULL);

	if (size > inode->i_sb->s_blocksize * 4)
		return 0;

	return 1;
}

/* tail plugins */
formatting_plugin formatting_plugins[LAST_TAIL_FORMATTING_ID] = {
	[NEVER_TAILS_FORMATTING_ID] = {
		.h = {
			.type_id = REISER4_FORMATTING_PLUGIN_TYPE,
			.id = NEVER_TAILS_FORMATTING_ID,
			.pops = NULL,
			.label = "never",
			.desc = "Never store file's tail",
			.linkage = {NULL, NULL}
		},
		.have_tail = have_formatting_never
	},
	[ALWAYS_TAILS_FORMATTING_ID] = {
		.h = {
			.type_id = REISER4_FORMATTING_PLUGIN_TYPE,
			.id = ALWAYS_TAILS_FORMATTING_ID,
			.pops = NULL,
			.label = "always",
			.desc =	"Always store file's tail",
			.linkage = {NULL, NULL}
		},
		.have_tail = have_formatting_always
	},
	[SMALL_FILE_FORMATTING_ID] = {
		.h = {
			.type_id = REISER4_FORMATTING_PLUGIN_TYPE,
			.id = SMALL_FILE_FORMATTING_ID,
			.pops = NULL,
			.label = "4blocks",
			.desc = "store files shorter than 4 blocks in tail items",
			.linkage = {NULL, NULL}
		},
		.have_tail = have_formatting_default
	}
};

/*
 * Local variables:
 * c-indentation-style: "K&R"
 * mode-name: "LC"
 * c-basic-offset: 8
 * tab-width: 8
 * fill-column: 79
 * End:
 */
