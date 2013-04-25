/*
 * vvar.h: Shared vDSO/kernel variable declarations
 * Copyright (c) 2011 Andy Lutomirski
 * Subject to the GNU General Public License, version 2
 *
 * A handful of variables are accessible (read-only) from userspace
 * code in the vsyscall page and the vdso.  They are declared here.
 * Some other file must define them with DEFINE_VVAR.
 *
 * In normal kernel code, they are used like any other variable.
 * In user code, they are accessed through the VVAR macro.
 *
 * These variables live in a page of kernel data that has an extra RO
 * mapping for userspace.  Each variable needs a unique offset within
 * that page; specify that offset with the DECLARE_VVAR macro.  (If
 * you mess up, the linker will catch it.)
 */

/* Base address of vvars.  This is not ABI. */
#define VVAR_ADDRESS (-10*1024*1024 - 4096)

#if defined(__VVAR_KERNEL_LDS)

/* The kernel linker script defines its own magic to put vvars in the
 * right place.
 */
#define DECLARE_VVAR(type, name) \
	EMIT_VVAR(name, VVAR_OFFSET_ ## name)

#elif defined(__VVAR_ADDR)

#define DECLARE_VVAR(type, name)					\
	type const * const vvaraddr_ ## name =				\
	(void *)(VVAR_ADDRESS + (VVAR_OFFSET_ ## name));

#else

#define DECLARE_VVAR(type, name)					\
	extern type const * const vvaraddr_ ## name;

#define DEFINE_VVAR(type, name)						\
	type name							\
	__attribute__((section(".vvar_" #name), aligned(16))) __visible
#endif

#define VVAR(name) (*vvaraddr_ ## name)

/* DECLARE_VVAR(offset, type, name) */

#define VVAR_OFFSET_jiffies 0
DECLARE_VVAR(volatile unsigned long, jiffies)
#define VVAR_OFFSET_vgetcpu_mode 16
DECLARE_VVAR(int, vgetcpu_mode)
#define VVAR_OFFSET_vsyscall_gtod_data 128
DECLARE_VVAR(struct vsyscall_gtod_data, vsyscall_gtod_data)

#undef DECLARE_VVAR
