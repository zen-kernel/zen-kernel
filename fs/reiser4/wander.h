/* Copyright 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

#if !defined (__FS_REISER4_WANDER_H__)
#define __FS_REISER4_WANDER_H__

#include "dformat.h"

#include <linux/fs.h>		/* for struct super_block  */

/* REISER4 JOURNAL ON-DISK DATA STRUCTURES   */

#define TX_HEADER_MAGIC  "TxMagic4"
#define WANDER_RECORD_MAGIC "LogMagc4"

#define TX_HEADER_MAGIC_SIZE  (8)
#define WANDER_RECORD_MAGIC_SIZE (8)

/* journal header block format */
struct journal_header {
	/* last written transaction head location */
	d64 last_committed_tx;
};

typedef struct journal_location {
	reiser4_block_nr footer;
	reiser4_block_nr header;
} journal_location;

/* The wander.c head comment describes usage and semantic of all these structures */
/* journal footer block format */
struct journal_footer {
	/* last flushed transaction location. */
	/* This block number is no more valid after the transaction it points
	   to gets flushed, this number is used only at journal replaying time
	   for detection of the end of on-disk list of committed transactions
	   which were not flushed completely */
	d64 last_flushed_tx;

	/* free block counter is written in journal footer at transaction
	   flushing , not in super block because free blocks counter is logged
	   by another way than super block fields (root pointer, for
	   example). */
	d64 free_blocks;

	/* number of used OIDs and maximal used OID are logged separately from
	   super block */
	d64 nr_files;
	d64 next_oid;
};

/* Each wander record (except the first one) has unified format with wander
   record header followed by an array of log entries */
struct wander_record_header {
	/* when there is no predefined location for wander records, this magic
	   string should help reiser4fsck. */
	char magic[WANDER_RECORD_MAGIC_SIZE];

	/* transaction id */
	d64 id;

	/* total number of wander records in current transaction  */
	d32 total;

	/* this block number in transaction */
	d32 serial;

	/* number of previous block in commit */
	d64 next_block;
};

/* The first wander record (transaction head) of written transaction has the
   special format */
struct tx_header {
	/* magic string makes first block in transaction different from other
	   logged blocks, it should help fsck. */
	char magic[TX_HEADER_MAGIC_SIZE];

	/* transaction id */
	d64 id;

	/* total number of records (including this first tx head) in the
	   transaction */
	d32 total;

	/* align next field to 8-byte boundary; this field always is zero */
	d32 padding;

	/* block number of previous transaction head */
	d64 prev_tx;

	/* next wander record location */
	d64 next_block;

	/* committed versions of free blocks counter */
	d64 free_blocks;

	/* number of used OIDs (nr_files) and maximal used OID are logged
	   separately from super block */
	d64 nr_files;
	d64 next_oid;
};

/* A transaction gets written to disk as a set of wander records (each wander
   record size is fs block) */

/* As it was told above a wander The rest of wander record is filled by these log entries, unused space filled
   by zeroes */
struct wander_entry {
	d64 original;		/* block original location */
	d64 wandered;		/* block wandered location */
};

/* REISER4 JOURNAL WRITER FUNCTIONS   */

extern int reiser4_write_logs(long *);
extern int reiser4_journal_replay(struct super_block *);
extern int reiser4_journal_recover_sb_data(struct super_block *);

extern int reiser4_init_journal_info(struct super_block *);
extern void reiser4_done_journal_info(struct super_block *);

extern int write_jnode_list(struct list_head *, flush_queue_t *, long *, int);

#endif				/* __FS_REISER4_WANDER_H__ */

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 80
   scroll-step: 1
   End:
*/
