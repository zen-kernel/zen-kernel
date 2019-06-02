/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2015-2019 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#ifndef _WG_COMPATASM_H
#define _WG_COMPATASM_H

#include <linux/linkage.h>
#include <linux/kconfig.h>
#include <linux/version.h>

/* PaX compatibility */
#if defined(RAP_PLUGIN)
#undef ENTRY
#define ENTRY RAP_ENTRY
#endif

#if defined(__LINUX_ARM_ARCH__) && LINUX_VERSION_CODE < KERNEL_VERSION(3, 17, 0)
	.irp	c,,eq,ne,cs,cc,mi,pl,vs,vc,hi,ls,ge,lt,gt,le,hs,lo
	.macro	ret\c, reg
#if __LINUX_ARM_ARCH__ < 6
	mov\c	pc, \reg
#else
	.ifeqs	"\reg", "lr"
	bx\c	\reg
	.else
	mov\c	pc, \reg
	.endif
#endif
	.endm
	.endr
#endif

#if defined(__LINUX_ARM_ARCH__) && LINUX_VERSION_CODE < KERNEL_VERSION(3, 15, 0)
#include <asm/assembler.h>
#define lspush push
#define lspull pull
#undef push
#undef pull
#endif

#endif /* _WG_COMPATASM_H */
