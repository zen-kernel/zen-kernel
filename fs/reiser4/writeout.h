/* Copyright 2002, 2003, 2004 by Hans Reiser, licensing governed by reiser4/README  */

#if !defined (__FS_REISER4_WRITEOUT_H__)

#define WRITEOUT_SINGLE_STREAM (0x1)
#define WRITEOUT_FOR_PAGE_RECLAIM  (0x2)
#define WRITEOUT_BARRIER (0x4)

extern int reiser4_get_writeout_flags(void);

#endif				/* __FS_REISER4_WRITEOUT_H__ */

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 80
   End:
*/
