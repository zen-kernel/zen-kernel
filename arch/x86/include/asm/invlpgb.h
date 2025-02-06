/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_INVLPGB
#define _ASM_X86_INVLPGB

#include <linux/kernel.h>
#include <vdso/bits.h>
#include <vdso/page.h>

/*
 * INVLPGB does broadcast TLB invalidation across all the CPUs in the system.
 *
 * The INVLPGB instruction is weakly ordered, and a batch of invalidations can
 * be done in a parallel fashion.
 *
 * TLBSYNC is used to ensure that pending INVLPGB invalidations initiated from
 * this CPU have completed.
 */
static inline void __invlpgb(unsigned long asid, unsigned long pcid,
			     unsigned long addr, u16 extra_count,
			     bool pmd_stride, u8 flags)
{
	u32 edx = (pcid << 16) | asid;
	u32 ecx = (pmd_stride << 31) | extra_count;
	u64 rax = addr | flags;

	/* The low bits in rax are for flags. Verify addr is clean. */
	VM_WARN_ON_ONCE(addr & ~PAGE_MASK);

	/* INVLPGB; supported in binutils >= 2.36. */
	asm volatile(".byte 0x0f, 0x01, 0xfe" : : "a" (rax), "c" (ecx), "d" (edx));
}

/* Wait for INVLPGB originated by this CPU to complete. */
static inline void __tlbsync(void)
{
	/* TLBSYNC: supported in binutils >= 0.36. */
	asm volatile(".byte 0x0f, 0x01, 0xff" ::: "memory");
}

/*
 * INVLPGB can be targeted by virtual address, PCID, ASID, or any combination
 * of the three. For example:
 * - INVLPGB_VA | INVLPGB_INCLUDE_GLOBAL: invalidate all TLB entries at the address
 * - INVLPGB_PCID:			  invalidate all TLB entries matching the PCID
 *
 * The first can be used to invalidate (kernel) mappings at a particular
 * address across all processes.
 *
 * The latter invalidates all TLB entries matching a PCID.
 */
#define INVLPGB_VA			BIT(0)
#define INVLPGB_PCID			BIT(1)
#define INVLPGB_ASID			BIT(2)
#define INVLPGB_INCLUDE_GLOBAL		BIT(3)
#define INVLPGB_FINAL_ONLY		BIT(4)
#define INVLPGB_INCLUDE_NESTED		BIT(5)

/* Flush all mappings for a given pcid and addr, not including globals. */
static inline void invlpgb_flush_user(unsigned long pcid,
				      unsigned long addr)
{
	__invlpgb(0, pcid, addr, 0, 0, INVLPGB_PCID | INVLPGB_VA);
	__tlbsync();
}

static inline void __invlpgb_flush_user_nr_nosync(unsigned long pcid,
						  unsigned long addr,
						  u16 nr,
						  bool pmd_stride,
						  bool freed_tables)
{
	u8 flags = INVLPGB_PCID | INVLPGB_VA;

	if (!freed_tables)
		flags |= INVLPGB_FINAL_ONLY;

	__invlpgb(0, pcid, addr, nr - 1, pmd_stride, flags);
}

/* Flush all mappings for a given PCID, not including globals. */
static inline void __invlpgb_flush_single_pcid_nosync(unsigned long pcid)
{
	__invlpgb(0, pcid, 0, 0, 0, INVLPGB_PCID);
}

/* Flush all mappings, including globals, for all PCIDs. */
static inline void invlpgb_flush_all(void)
{
	__invlpgb(0, 0, 0, 0, 0, INVLPGB_INCLUDE_GLOBAL);
	__tlbsync();
}

/* Flush addr, including globals, for all PCIDs. */
static inline void __invlpgb_flush_addr_nosync(unsigned long addr, u16 nr)
{
	__invlpgb(0, 0, addr, nr - 1, 0, INVLPGB_INCLUDE_GLOBAL);
}

/* Flush all mappings for all PCIDs except globals. */
static inline void invlpgb_flush_all_nonglobals(void)
{
	__invlpgb(0, 0, 0, 0, 0, 0);
	__tlbsync();
}

#endif /* _ASM_X86_INVLPGB */
