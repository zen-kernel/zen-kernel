/* Copyright 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* "Black box" entry to fixed-width contain user supplied data */

#if !defined( __FS_REISER4_BLACK_BOX_H__ )
#define __FS_REISER4_BLACK_BOX_H__

#include "../../forward.h"
#include "../../dformat.h"
#include "../../kassign.h"
#include "../../key.h"

extern int store_black_box(reiser4_tree * tree,
			   const reiser4_key * key, void *data, int length);
extern int load_black_box(reiser4_tree * tree,
			  reiser4_key * key, void *data, int length, int exact);
extern int kill_black_box(reiser4_tree * tree, const reiser4_key * key);
extern int update_black_box(reiser4_tree * tree,
			    const reiser4_key * key, void *data, int length);

/* __FS_REISER4_BLACK_BOX_H__ */
#endif

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
