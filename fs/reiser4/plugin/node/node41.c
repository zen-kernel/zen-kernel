/*
 * Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README
 */

#include "../../debug.h"
#include "../../key.h"
#include "../../coord.h"
#include "../plugin_header.h"
#include "../item/item.h"
#include "node.h"
#include "node41.h"
#include "../plugin.h"
#include "../../jnode.h"
#include "../../znode.h"
#include "../../pool.h"
#include "../../carry.h"
#include "../../tap.h"
#include "../../tree.h"
#include "../../super.h"
#include "../../checksum.h"
#include "../../reiser4.h"

#include <asm/uaccess.h>
#include <linux/types.h>
#include <linux/prefetch.h>

/*
 * node41 layout it almost the same as node40:
 * node41_header is at the beginning and a table of item headers
 * is at the end. Ther difference is that node41_header contains
 * a 32-bit checksum (see node41.h)
 */

static const __u32 REISER4_NODE41_MAGIC = 0x19051966;

static inline node41_header *node41_node_header(const znode *node)
{
	assert("edward-1634", node != NULL);
	assert("edward-1635", znode_page(node) != NULL);
	assert("edward-1636", zdata(node) != NULL);

	return (node41_header *)zdata(node);
}

int csum_node41(znode *node, int check)
{
	__u32 cpu_csum;

	cpu_csum = reiser4_crc32c(get_current_super_private()->csum_tfm,
				  ~0,
				  zdata(node),
				  sizeof(struct node40_header));
	cpu_csum = reiser4_crc32c(get_current_super_private()->csum_tfm,
				  cpu_csum,
				  zdata(node) + sizeof(struct node41_header),
				  reiser4_get_current_sb()->s_blocksize -
				  sizeof(node41_header));
	if (check)
		return cpu_csum == nh41_get_csum(node41_node_header(node));
	else {
		nh41_set_csum(node41_node_header(node), cpu_csum);
		return 1;
	}
}

/*
 * plugin->u.node.parse
 * look for description of this method in plugin/node/node.h
 */
int parse_node41(znode *node /* node to parse */)
{
	int ret;

	ret = csum_node41(node, 1/* check */);
	if (!ret) {
		warning("edward-1645",
			"block %llu: bad checksum. FSCK?",
			*jnode_get_block(ZJNODE(node)));
		reiser4_handle_error();
		return RETERR(-EIO);
	}
	return parse_node40_common(node, REISER4_NODE41_MAGIC);
}

/*
 * plugin->u.node.init
 * look for description of this method in plugin/node/node.h
 */
int init_node41(znode *node /* node to initialise */)
{
	return init_node40_common(node, node_plugin_by_id(NODE41_ID),
				  sizeof(node41_header), REISER4_NODE41_MAGIC);
}

/*
 * plugin->u.node.shift
 * look for description of this method in plugin/node/node.h
 */
int shift_node41(coord_t *from, znode *to,
		 shift_direction pend,
		 int delete_child, /* if @from->node becomes empty,
				    * it will be deleted from the
				    * tree if this is set to 1 */
		 int including_stop_coord,
		 carry_plugin_info *info)
{
	return shift_node40_common(from, to, pend, delete_child,
				   including_stop_coord, info,
				   sizeof(node41_header));
}

#ifdef GUESS_EXISTS
int guess_node41(const znode *node /* node to guess plugin of */)
{
	return guess_node40_common(node, NODE41_ID, REISER4_NODE41_MAGIC);
}
#endif

/*
 * plugin->u.node.max_item_size
 */
int max_item_size_node41(void)
{
	return reiser4_get_current_sb()->s_blocksize - sizeof(node41_header) -
		sizeof(item_header40);
}

/*
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 80
   scroll-step: 1
   End:
*/
