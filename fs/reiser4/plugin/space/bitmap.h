/* Copyright 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

#if !defined (__REISER4_PLUGIN_SPACE_BITMAP_H__)
#define __REISER4_PLUGIN_SPACE_BITMAP_H__

#include "../../dformat.h"
#include "../../block_alloc.h"

#include <linux/types.h>	/* for __u??  */
#include <linux/fs.h>		/* for struct super_block  */
/* EDWARD-FIXME-HANS: write something as informative as the below for every .h file lacking it. */
/* declarations of functions implementing methods of space allocator plugin for
   bitmap based allocator. The functions themselves are in bitmap.c */
extern int reiser4_init_allocator_bitmap(reiser4_space_allocator *,
					 struct super_block *, void *);
extern int reiser4_destroy_allocator_bitmap(reiser4_space_allocator *,
					    struct super_block *);
extern int reiser4_alloc_blocks_bitmap(reiser4_space_allocator *,
				       reiser4_blocknr_hint *, int needed,
				       reiser4_block_nr * start,
				       reiser4_block_nr * len);
extern int reiser4_check_blocks_bitmap(const reiser4_block_nr *,
					const reiser4_block_nr *, int);
extern void reiser4_dealloc_blocks_bitmap(reiser4_space_allocator *,
					  reiser4_block_nr,
					  reiser4_block_nr);
extern int reiser4_pre_commit_hook_bitmap(void);

#define reiser4_post_commit_hook_bitmap() do{}while(0)
#define reiser4_post_write_back_hook_bitmap() do{}while(0)
#define reiser4_print_info_bitmap(pref, al) do{}while(0)

typedef __u64 bmap_nr_t;
typedef __u32 bmap_off_t;

#endif				/* __REISER4_PLUGIN_SPACE_BITMAP_H__ */

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
