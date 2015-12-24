/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* TRIM/discard interoperation subsystem for reiser4. */

#if !defined(__FS_REISER4_DISCARD_H__)
#define __FS_REISER4_DISCARD_H__

#include "forward.h"
#include "dformat.h"

/**
 * Issue discard requests for all block extents recorded in @atom's delete sets,
 * if discard is enabled. The extents processed are removed from the @atom's
 * delete sets and stored in @processed_set.
 *
 * @atom must be locked on entry and is unlocked on exit.
 * @processed_set must be initialized with blocknr_list_init().
 */
extern int discard_atom(txn_atom *atom, struct list_head *processed_set);

/**
 * Splices @processed_set back to @atom's delete set.
 * Must be called after discard_atom() loop, using the same @processed_set.
 *
 * @atom must be locked on entry and is unlocked on exit.
 * @processed_set must be the same as passed to discard_atom().
 */
extern void discard_atom_post(txn_atom *atom, struct list_head *processed_set);

/* __FS_REISER4_DISCARD_H__ */
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
