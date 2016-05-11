/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

#if !defined( __REISER4_NODE41_H__ )
#define __REISER4_NODE41_H__

#include "../../forward.h"
#include "../../dformat.h"
#include "node40.h"
#include <linux/types.h>

/*
 * node41 layout: the same as node40, but with 32-bit checksum
 */

typedef struct node41_header {
	node40_header head;
	d32 csum;
} PACKED node41_header;

/*
 * functions to get/set fields of node41_header
 */
#define nh41_get_csum(nh) le32_to_cpu(get_unaligned(&(nh)->csum))
#define nh41_set_csum(nh, value) put_unaligned(cpu_to_le32(value), &(nh)->csum)

int init_node41(znode * node);
int parse_node41(znode *node);
int max_item_size_node41(void);
int shift_node41(coord_t *from, znode *to, shift_direction pend,
		 int delete_child, int including_stop_coord,
		 carry_plugin_info *info);
int csum_node41(znode *node, int check);

#ifdef GUESS_EXISTS
int guess_node41(const znode * node);
#endif
extern void reiser4_handle_error(void);

/* __REISER4_NODE41_H__ */
#endif
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
