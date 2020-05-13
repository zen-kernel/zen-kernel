/* SPDX-License-Identifier: GPL-2.0 */
//
// Code shared between 32 and 64 bit

#include <asm/spec-ctrl.h>

void __switch_to_xtra(struct task_struct *prev_p, struct task_struct *next_p);

/*
 * This needs to be inline to optimize for the common case where no extra
 * work needs to be done.
 */
static inline void switch_to_extra(struct task_struct *prev,
				   struct task_struct *next)
{
	unsigned long next_tif = task_thread_info(next)->flags;
	unsigned long prev_tif = task_thread_info(prev)->flags;

	if (IS_ENABLED(CONFIG_SMP)) {
		/*
		 * Avoid __switch_to_xtra() invocation when conditional
		 * STIBP is disabled and the only different bit is
		 * TIF_SPEC_IB. For CONFIG_SMP=n TIF_SPEC_IB is not
		 * in the TIF_WORK_CTXSW masks.
		 */
		if (!static_branch_likely(&switch_to_cond_stibp)) {
			prev_tif &= ~_TIF_SPEC_IB;
			next_tif &= ~_TIF_SPEC_IB;
		}
	}

	/*
	 * __switch_to_xtra() handles debug registers, i/o bitmaps,
	 * speculation mitigations etc.
	 */
	if (unlikely(next_tif & _TIF_WORK_CTXSW_NEXT ||
		     prev_tif & _TIF_WORK_CTXSW_PREV))
		__switch_to_xtra(prev, next);
}

#ifdef CONFIG_X86_64

enum which_selector {
	FS,
	GS
};

/*
 * Saves the FS or GS base for an outgoing thread if FSGSBASE extensions are
 * not available.  The goal is to be reasonably fast on non-FSGSBASE systems.
 * It's forcibly inlined because it'll generate better code and this function
 * is hot.
 */
static __always_inline void save_base_legacy(struct task_struct *prev_p,
                                             unsigned short selector,
                                             enum which_selector which)
{
	if (likely(selector == 0)) {
		/*
		 * On Intel (without X86_BUG_NULL_SEG), the segment base could
		 * be the pre-existing saved base or it could be zero.  On AMD
		 * (with X86_BUG_NULL_SEG), the segment base could be almost
		 * anything.
		 *
		 * This branch is very hot (it's hit twice on almost every
		 * context switch between 64-bit programs), and avoiding
		 * the RDMSR helps a lot, so we just assume that whatever
		 * value is already saved is correct.  This matches historical
		 * Linux behavior, so it won't break existing applications.
		 *
		 * To avoid leaking state, on non-X86_BUG_NULL_SEG CPUs, if we
		 * report that the base is zero, it needs to actually be zero:
		 * see the corresponding logic in load_seg_legacy.
		 */
	} else {
		/*
		 * If the selector is 1, 2, or 3, then the base is zero on
		 * !X86_BUG_NULL_SEG CPUs and could be anything on
		 * X86_BUG_NULL_SEG CPUs.  In the latter case, Linux
		 * has never attempted to preserve the base across context
		 * switches.
		 *
		 * If selector > 3, then it refers to a real segment, and
		 * saving the base isn't necessary.
		 */
		if (which == FS)
			prev_p->thread.fsbase = 0;
		else
			prev_p->thread.gsbase = 0;
	}
}

static __always_inline void save_fsgs(struct task_struct *task)
{
	savesegment(fs, task->thread.fsindex);
	savesegment(gs, task->thread.gsindex);
	if (static_cpu_has(X86_FEATURE_FSGSBASE)) {
		/*
		 * If FSGSBASE is enabled, we can't make any useful guesses
		 * about the base, and user code expects us to save the current
		 * value.  Fortunately, reading the base directly is efficient.
		 */
		task->thread.fsbase = rdfsbase();
		task->thread.gsbase = x86_gsbase_read_cpu_inactive();
	} else {
		save_base_legacy(task, task->thread.fsindex, FS);
		save_base_legacy(task, task->thread.gsindex, GS);
	}
}

#endif
