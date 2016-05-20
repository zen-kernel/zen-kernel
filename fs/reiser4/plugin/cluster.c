/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Contains reiser4 cluster plugins (see
   http://www.namesys.com/cryptcompress_design.html
   "Concepts of clustering" for details). */

#include "plugin_header.h"
#include "plugin.h"
#include "../inode.h"

static int change_cluster(struct inode *inode,
			  reiser4_plugin * plugin,
			  pset_member memb)
{
	assert("edward-1324", inode != NULL);
	assert("edward-1325", plugin != NULL);
	assert("edward-1326", is_reiser4_inode(inode));
	assert("edward-1327", plugin->h.type_id == REISER4_CLUSTER_PLUGIN_TYPE);

	/* Can't change the cluster plugin for already existent regular files */
	if (!plugin_of_group(inode_file_plugin(inode), REISER4_DIRECTORY_FILE))
		return RETERR(-EINVAL);

	/* If matches, nothing to change. */
	if (inode_hash_plugin(inode) != NULL &&
	    inode_hash_plugin(inode)->h.id == plugin->h.id)
		return 0;

	return aset_set_unsafe(&reiser4_inode_data(inode)->pset,
			       PSET_CLUSTER, plugin);
}

static reiser4_plugin_ops cluster_plugin_ops = {
	.init = NULL,
	.load = NULL,
	.save_len = NULL,
	.save = NULL,
	.change = &change_cluster
};

#define SUPPORT_CLUSTER(SHIFT, ID, LABEL, DESC)			\
	[CLUSTER_ ## ID ## _ID] = {				\
		.h = {						\
			.type_id = REISER4_CLUSTER_PLUGIN_TYPE,	\
			.id = CLUSTER_ ## ID ## _ID,		\
			.pops = &cluster_plugin_ops,		\
			.label = LABEL,				\
			.desc = DESC,				\
			.linkage = {NULL, NULL}			\
		},						\
		.shift = SHIFT					\
	}

cluster_plugin cluster_plugins[LAST_CLUSTER_ID] = {
	SUPPORT_CLUSTER(16, 64K, "64K", "Large"),
	SUPPORT_CLUSTER(15, 32K, "32K", "Big"),
	SUPPORT_CLUSTER(14, 16K, "16K", "Average"),
	SUPPORT_CLUSTER(13, 8K, "8K", "Small"),
	SUPPORT_CLUSTER(12, 4K, "4K", "Minimal")
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
