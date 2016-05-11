/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */
/* Memory pressure hooks. Fake inodes handling. See page_cache.c. */

#if !defined(__REISER4_PAGE_CACHE_H__)
#define __REISER4_PAGE_CACHE_H__

#include "forward.h"
#include "context.h"            /* for reiser4_ctx_gfp_mask_get() */

#include <linux/fs.h>		/* for struct super_block, address_space  */
#include <linux/mm.h>		/* for struct page  */
#include <linux/pagemap.h>	/* for lock_page()  */
#include <linux/vmalloc.h>	/* for __vmalloc()  */

extern int reiser4_init_formatted_fake(struct super_block *);
extern void reiser4_done_formatted_fake(struct super_block *);

extern reiser4_tree *reiser4_tree_by_page(const struct page *);

#define reiser4_submit_bio(rw, bio) submit_bio((rw), (bio))

extern void reiser4_wait_page_writeback(struct page *);
static inline void lock_and_wait_page_writeback(struct page *page)
{
	lock_page(page);
	if (unlikely(PageWriteback(page)))
		reiser4_wait_page_writeback(page);
}

#define jprivate(page) ((jnode *)page_private(page))

extern int reiser4_page_io(struct page *, jnode *, int rw, gfp_t);
extern void reiser4_drop_page(struct page *);
extern void reiser4_invalidate_pages(struct address_space *, pgoff_t from,
				     unsigned long count, int even_cows);
extern void capture_reiser4_inodes(struct super_block *,
				   struct writeback_control *);
static inline void *reiser4_vmalloc(unsigned long size)
{
	return __vmalloc(size,
			 reiser4_ctx_gfp_mask_get() | __GFP_HIGHMEM,
			 PAGE_KERNEL);
}

#define PAGECACHE_TAG_REISER4_MOVED PAGECACHE_TAG_DIRTY

#if REISER4_DEBUG
extern void print_page(const char *prefix, struct page *page);
#else
#define print_page(prf, p) noop
#endif

/* __REISER4_PAGE_CACHE_H__ */
#endif

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
