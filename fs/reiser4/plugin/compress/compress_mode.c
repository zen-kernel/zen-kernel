/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */
/* This file contains Reiser4 compression mode plugins.

   Compression mode plugin is a set of handlers called by compressor
   at flush time and represent some heuristics including the ones
   which are to avoid compression of incompressible data, see
   http://www.namesys.com/cryptcompress_design.html for more details.
*/
#include "../../inode.h"
#include "../plugin.h"

static int should_deflate_none(struct inode * inode, cloff_t index)
{
	return 0;
}

static int should_deflate_common(struct inode * inode, cloff_t index)
{
	return compression_is_on(cryptcompress_inode_data(inode));
}

static int discard_hook_ultim(struct inode *inode, cloff_t index)
{
	turn_off_compression(cryptcompress_inode_data(inode));
	return 0;
}

static int discard_hook_lattd(struct inode *inode, cloff_t index)
{
	struct cryptcompress_info * info = cryptcompress_inode_data(inode);

	assert("edward-1462",
	       get_lattice_factor(info) >= MIN_LATTICE_FACTOR &&
	       get_lattice_factor(info) <= MAX_LATTICE_FACTOR);

	turn_off_compression(info);
	if (get_lattice_factor(info) < MAX_LATTICE_FACTOR)
		set_lattice_factor(info, get_lattice_factor(info) << 1);
	return 0;
}

static int accept_hook_lattd(struct inode *inode, cloff_t index)
{
	turn_on_compression(cryptcompress_inode_data(inode));
	set_lattice_factor(cryptcompress_inode_data(inode), MIN_LATTICE_FACTOR);
	return 0;
}

/* Check on dynamic lattice, the adaptive compression modes which
   defines the following behavior:

   Compression is on: try to compress everything and turn
   it off, whenever cluster is incompressible.

   Compression is off: try to compress clusters of indexes
   k * FACTOR (k = 0, 1, 2, ...) and turn it on, if some of
   them is compressible. If incompressible, then increase FACTOR */

/* check if @index belongs to one-dimensional lattice
   of sparce factor @factor */
static int is_on_lattice(cloff_t index, int factor)
{
	return (factor ? index % factor == 0: index == 0);
}

static int should_deflate_lattd(struct inode * inode, cloff_t index)
{
	return should_deflate_common(inode, index) ||
		is_on_lattice(index,
			      get_lattice_factor
			      (cryptcompress_inode_data(inode)));
}

/* compression mode_plugins */
compression_mode_plugin compression_mode_plugins[LAST_COMPRESSION_MODE_ID] = {
	[NONE_COMPRESSION_MODE_ID] = {
		.h = {
			.type_id = REISER4_COMPRESSION_MODE_PLUGIN_TYPE,
			.id = NONE_COMPRESSION_MODE_ID,
			.pops = NULL,
			.label = "none",
			.desc = "Compress nothing",
			.linkage = {NULL, NULL}
		},
		.should_deflate = should_deflate_none,
		.accept_hook = NULL,
		.discard_hook = NULL
	},
	/* Check-on-dynamic-lattice adaptive compression mode */
	[LATTD_COMPRESSION_MODE_ID] = {
		.h = {
			.type_id = REISER4_COMPRESSION_MODE_PLUGIN_TYPE,
			.id = LATTD_COMPRESSION_MODE_ID,
			.pops = NULL,
			.label = "lattd",
			.desc = "Check on dynamic lattice",
			.linkage = {NULL, NULL}
		},
		.should_deflate = should_deflate_lattd,
		.accept_hook = accept_hook_lattd,
		.discard_hook = discard_hook_lattd
	},
	/* Check-ultimately compression mode:
	   Turn off compression forever as soon as we meet
	   incompressible data */
	[ULTIM_COMPRESSION_MODE_ID] = {
		.h = {
			.type_id = REISER4_COMPRESSION_MODE_PLUGIN_TYPE,
			.id = ULTIM_COMPRESSION_MODE_ID,
			.pops = NULL,
			.label = "ultim",
			.desc = "Check ultimately",
			.linkage = {NULL, NULL}
		},
		.should_deflate = should_deflate_common,
		.accept_hook = NULL,
		.discard_hook = discard_hook_ultim
	},
	/* Force-to-compress-everything compression mode */
	[FORCE_COMPRESSION_MODE_ID] = {
		.h = {
			.type_id = REISER4_COMPRESSION_MODE_PLUGIN_TYPE,
			.id = FORCE_COMPRESSION_MODE_ID,
			.pops = NULL,
			.label = "force",
			.desc = "Force to compress everything",
			.linkage = {NULL, NULL}
		},
		.should_deflate = NULL,
		.accept_hook = NULL,
		.discard_hook = NULL
	},
	/* Convert-to-extent compression mode.
	   In this mode items will be converted to extents and management
	   will be passed to (classic) unix file plugin as soon as ->write()
	   detects that the first complete logical cluster (of index #0) is
	   incompressible. */
	[CONVX_COMPRESSION_MODE_ID] = {
		.h = {
			.type_id = REISER4_COMPRESSION_MODE_PLUGIN_TYPE,
			.id = CONVX_COMPRESSION_MODE_ID,
			.pops = NULL,
			.label = "conv",
			.desc = "Convert to extent",
			.linkage = {NULL, NULL}
		},
		.should_deflate = should_deflate_common,
		.accept_hook = NULL,
		.discard_hook = NULL
	}
};

/*
  Local variables:
  c-indentation-style: "K&R"
  mode-name: "LC"
  c-basic-offset: 8
  tab-width: 8
  fill-column: 120
  scroll-step: 1
  End:
*/
