/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

#if !defined(__REISER4_IOCTL_H__)
#define __REISER4_IOCTL_H__

#include <linux/fs.h>

/*
 * ioctl(2) command used to "unpack" reiser4 file, that is, convert it into
 * extents and fix in this state. This is used by applications that rely on
 *
 *     . files being block aligned, and
 *
 *     . files never migrating on disk
 *
 * for example, boot loaders (LILO) need this.
 *
 * This ioctl should be used as
 *
 *     result = ioctl(fd, REISER4_IOC_UNPACK);
 *
 * File behind fd descriptor will be converted to the extents (if necessary),
 * and its stat-data will be updated so that it will never be converted back
 * into tails again.
 */
#define REISER4_IOC_UNPACK _IOW(0xCD, 1, long)

/* __REISER4_IOCTL_H__ */
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
