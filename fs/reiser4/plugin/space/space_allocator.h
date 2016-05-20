/* Copyright 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

#ifndef __SPACE_ALLOCATOR_H__
#define __SPACE_ALLOCATOR_H__

#include "../../forward.h"
#include "bitmap.h"
/* NIKITA-FIXME-HANS: surely this could use a comment. Something about how bitmap is the only space allocator for now,
 * but... */
#define DEF_SPACE_ALLOCATOR(allocator)											\
															\
static inline int sa_init_allocator (reiser4_space_allocator * al, struct super_block *s, void * opaque)		\
{															\
	return reiser4_init_allocator_##allocator (al, s, opaque);	                                                \
}															\
															\
static inline void sa_destroy_allocator (reiser4_space_allocator *al, struct super_block *s)				\
{															\
	reiser4_destroy_allocator_##allocator (al, s);									\
}															\
															\
static inline int sa_alloc_blocks (reiser4_space_allocator *al, reiser4_blocknr_hint * hint, 				\
				   int needed, reiser4_block_nr * start, reiser4_block_nr * len)			\
{															\
	return reiser4_alloc_blocks_##allocator (al, hint, needed, start, len);						\
}															\
static inline void sa_dealloc_blocks (reiser4_space_allocator * al, reiser4_block_nr start, reiser4_block_nr len)	\
{															\
	reiser4_dealloc_blocks_##allocator (al, start, len);								\
}															\
															\
static inline int sa_check_blocks (const reiser4_block_nr * start, const reiser4_block_nr * end, int desired) 		\
{															\
	return reiser4_check_blocks_##allocator (start, end, desired);							        \
}															\
															\
static inline void sa_pre_commit_hook (void)										\
{ 															\
	reiser4_pre_commit_hook_##allocator ();										\
}															\
															\
static inline void sa_post_commit_hook (void) 										\
{ 															\
	reiser4_post_commit_hook_##allocator ();									\
}															\
															\
static inline void sa_post_write_back_hook (void) 									\
{ 															\
	reiser4_post_write_back_hook_##allocator();									\
}															\
															\
static inline void sa_print_info(const char * prefix, reiser4_space_allocator * al)					\
{															\
	reiser4_print_info_##allocator (prefix, al);                                                                    \
}

DEF_SPACE_ALLOCATOR(bitmap)

/* this object is part of reiser4 private in-core super block */
struct reiser4_space_allocator {
	union {
		/* space allocators might use this pointer to reference their
		 * data. */
		void *generic;
	} u;
};

/* __SPACE_ALLOCATOR_H__ */
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
