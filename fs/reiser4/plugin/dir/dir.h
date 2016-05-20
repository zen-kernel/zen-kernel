/* Copyright 2001, 2002, 2003, 2004 by Hans Reiser, licensing governed by
 * reiser4/README */

/* this file contains declarations of methods implementing directory plugins */

#if !defined( __REISER4_DIR_H__ )
#define __REISER4_DIR_H__

/*#include "../../key.h"

#include <linux/fs.h>*/

/* declarations of functions implementing HASHED_DIR_PLUGIN_ID dir plugin */

/* "hashed" directory methods of dir plugin */
void build_entry_key_hashed(const struct inode *, const struct qstr *,
			    reiser4_key *);

/* declarations of functions implementing SEEKABLE_HASHED_DIR_PLUGIN_ID dir plugin */

/* "seekable" directory methods of dir plugin */
void build_entry_key_seekable(const struct inode *, const struct qstr *,
			      reiser4_key *);

/* __REISER4_DIR_H__ */
#endif

/*
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
