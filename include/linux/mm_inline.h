/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_MM_INLINE_H
#define LINUX_MM_INLINE_H

#include <linux/huge_mm.h>
#include <linux/swap.h>

/**
 * folio_is_file_lru - Should the folio be on a file LRU or anon LRU?
 * @folio: The folio to test.
 *
 * We would like to get this info without a page flag, but the state
 * needs to survive until the folio is last deleted from the LRU, which
 * could be as far down as __page_cache_release.
 *
 * Return: An integer (not a boolean!) used to sort a folio onto the
 * right LRU list and to account folios correctly.
 * 1 if @folio is a regular filesystem backed page cache folio
 * or a lazily freed anonymous folio (e.g. via MADV_FREE).
 * 0 if @folio is a normal anonymous folio, a tmpfs folio or otherwise
 * ram or swap backed folio.
 */
static inline int folio_is_file_lru(struct folio *folio)
{
	return !folio_test_swapbacked(folio);
}

static inline int page_is_file_lru(struct page *page)
{
	return folio_is_file_lru(page_folio(page));
}

static __always_inline void update_lru_size(struct lruvec *lruvec,
				enum lru_list lru, enum zone_type zid,
				long nr_pages)
{
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);

	__mod_lruvec_state(lruvec, NR_LRU_BASE + lru, nr_pages);
	__mod_zone_page_state(&pgdat->node_zones[zid],
				NR_ZONE_LRU_BASE + lru, nr_pages);
#ifdef CONFIG_MEMCG
	mem_cgroup_update_lru_size(lruvec, lru, zid, nr_pages);
#endif
}

/**
 * __folio_clear_lru_flags - Clear page lru flags before releasing a page.
 * @folio: The folio that was on lru and now has a zero reference.
 */
static __always_inline void __folio_clear_lru_flags(struct folio *folio)
{
	VM_BUG_ON_FOLIO(!folio_test_lru(folio), folio);

	__folio_clear_lru(folio);

	/* this shouldn't happen, so leave the flags to bad_page() */
	if (folio_test_active(folio) && folio_test_unevictable(folio))
		return;

	__folio_clear_active(folio);
	__folio_clear_unevictable(folio);
}

static __always_inline void __clear_page_lru_flags(struct page *page)
{
	__folio_clear_lru_flags(page_folio(page));
}

/**
 * folio_lru_list - Which LRU list should a folio be on?
 * @folio: The folio to test.
 *
 * Return: The LRU list a folio should be on, as an index
 * into the array of LRU lists.
 */
static __always_inline enum lru_list folio_lru_list(struct folio *folio)
{
	enum lru_list lru;

	VM_BUG_ON_FOLIO(folio_test_active(folio) && folio_test_unevictable(folio), folio);

	if (folio_test_unevictable(folio))
		return LRU_UNEVICTABLE;

	lru = folio_is_file_lru(folio) ? LRU_INACTIVE_FILE : LRU_INACTIVE_ANON;
	if (folio_test_active(folio))
		lru += LRU_ACTIVE;

	return lru;
}

#ifdef CONFIG_LRU_GEN

static inline bool lru_gen_enabled(void)
{
#ifdef CONFIG_LRU_GEN_ENABLED
	DECLARE_STATIC_KEY_TRUE(lru_gen_static_key);

	return static_branch_likely(&lru_gen_static_key);
#else
	DECLARE_STATIC_KEY_FALSE(lru_gen_static_key);

	return static_branch_unlikely(&lru_gen_static_key);
#endif
}

static inline int lru_gen_from_seq(unsigned long seq)
{
	return seq % MAX_NR_GENS;
}

static inline int lru_hist_from_seq(unsigned long seq)
{
	return seq % NR_HIST_GENS;
}

static inline int lru_tier_from_refs(int refs)
{
	VM_BUG_ON(refs > BIT(LRU_REFS_WIDTH));

	/* see the comment on MAX_NR_TIERS */
	return order_base_2(refs + 1);
}

static inline bool lru_gen_is_active(struct lruvec *lruvec, int gen)
{
	unsigned long max_seq = lruvec->lrugen.max_seq;

	VM_BUG_ON(gen >= MAX_NR_GENS);

	/* see the comment on MIN_NR_GENS */
	return gen == lru_gen_from_seq(max_seq) || gen == lru_gen_from_seq(max_seq - 1);
}

static inline void lru_gen_update_size(struct lruvec *lruvec, enum lru_list lru,
				       int zone, long delta)
{
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);

	lockdep_assert_held(&lruvec->lru_lock);
	WARN_ON_ONCE(delta != (int)delta);

	__mod_lruvec_state(lruvec, NR_LRU_BASE + lru, delta);
	__mod_zone_page_state(&pgdat->node_zones[zone], NR_ZONE_LRU_BASE + lru, delta);
}

static inline void lru_gen_balance_size(struct lruvec *lruvec, struct folio *folio,
					int old_gen, int new_gen)
{
	int type = folio_is_file_lru(folio);
	int zone = folio_zonenum(folio);
	int delta = folio_nr_pages(folio);
	enum lru_list lru = type * LRU_FILE;
	struct lru_gen_struct *lrugen = &lruvec->lrugen;

	VM_BUG_ON(old_gen != -1 && old_gen >= MAX_NR_GENS);
	VM_BUG_ON(new_gen != -1 && new_gen >= MAX_NR_GENS);
	VM_BUG_ON(old_gen == -1 && new_gen == -1);

	if (old_gen >= 0)
		WRITE_ONCE(lrugen->nr_pages[old_gen][type][zone],
			   lrugen->nr_pages[old_gen][type][zone] - delta);
	if (new_gen >= 0)
		WRITE_ONCE(lrugen->nr_pages[new_gen][type][zone],
			   lrugen->nr_pages[new_gen][type][zone] + delta);

	if (old_gen < 0) {
		if (lru_gen_is_active(lruvec, new_gen))
			lru += LRU_ACTIVE;
		lru_gen_update_size(lruvec, lru, zone, delta);
		return;
	}

	if (new_gen < 0) {
		if (lru_gen_is_active(lruvec, old_gen))
			lru += LRU_ACTIVE;
		lru_gen_update_size(lruvec, lru, zone, -delta);
		return;
	}

	if (!lru_gen_is_active(lruvec, old_gen) && lru_gen_is_active(lruvec, new_gen)) {
		lru_gen_update_size(lruvec, lru, zone, -delta);
		lru_gen_update_size(lruvec, lru + LRU_ACTIVE, zone, delta);
	}

	/* Promotion is legit while a page is on an lru list, but demotion isn't. */
	VM_BUG_ON(lru_gen_is_active(lruvec, old_gen) && !lru_gen_is_active(lruvec, new_gen));
}

static inline bool lru_gen_add_folio(struct lruvec *lruvec, struct folio *folio, bool reclaiming)
{
	int gen;
	unsigned long old_flags, new_flags;
	int type = folio_is_file_lru(folio);
	int zone = folio_zonenum(folio);
	struct lru_gen_struct *lrugen = &lruvec->lrugen;

	if (folio_test_unevictable(folio) || !lrugen->enabled)
		return false;
	/*
	 * There are three cases for this page:
	 * 1) If it shouldn't be evicted, e.g., it was just faulted in, add it
	 *    to the youngest generation.
	 * 2) If it can't be evicted immediately, i.e., it's an anon page and
	 *    not in swapcache, or a dirty page pending writeback, add it to the
	 *    second oldest generation.
	 * 3) If it may be evicted immediately, e.g., it's a clean page, add it
	 *    to the oldest generation.
	 */
	if (folio_test_active(folio))
		gen = lru_gen_from_seq(lrugen->max_seq);
	else if ((!type && !folio_test_swapcache(folio)) ||
		 (folio_test_reclaim(folio) &&
		  (folio_test_dirty(folio) || folio_test_writeback(folio))))
		gen = lru_gen_from_seq(lrugen->min_seq[type] + 1);
	else
		gen = lru_gen_from_seq(lrugen->min_seq[type]);

	do {
		new_flags = old_flags = READ_ONCE(folio->flags);
		VM_BUG_ON_FOLIO(new_flags & LRU_GEN_MASK, folio);

		new_flags &= ~(LRU_GEN_MASK | BIT(PG_active));
		new_flags |= (gen + 1UL) << LRU_GEN_PGOFF;
	} while (cmpxchg(&folio->flags, old_flags, new_flags) != old_flags);

	lru_gen_balance_size(lruvec, folio, -1, gen);
	/* for folio_rotate_reclaimable() */
	if (reclaiming)
		list_add_tail(&folio->lru, &lrugen->lists[gen][type][zone]);
	else
		list_add(&folio->lru, &lrugen->lists[gen][type][zone]);

	return true;
}

static inline bool lru_gen_del_folio(struct lruvec *lruvec, struct folio *folio, bool reclaiming)
{
	int gen;
	unsigned long old_flags, new_flags;

	do {
		new_flags = old_flags = READ_ONCE(folio->flags);
		if (!(new_flags & LRU_GEN_MASK))
			return false;

		VM_BUG_ON_FOLIO(folio_test_active(folio), folio);
		VM_BUG_ON_FOLIO(folio_test_unevictable(folio), folio);

		gen = ((new_flags & LRU_GEN_MASK) >> LRU_GEN_PGOFF) - 1;

		new_flags &= ~LRU_GEN_MASK;
		if ((new_flags & LRU_REFS_FLAGS) != LRU_REFS_FLAGS)
			new_flags &= ~(LRU_REFS_MASK | LRU_REFS_FLAGS);
		/* for shrink_page_list() */
		if (reclaiming)
			new_flags &= ~(BIT(PG_referenced) | BIT(PG_reclaim));
		else if (lru_gen_is_active(lruvec, gen))
			new_flags |= BIT(PG_active);
	} while (cmpxchg(&folio->flags, old_flags, new_flags) != old_flags);

	lru_gen_balance_size(lruvec, folio, gen, -1);
	list_del(&folio->lru);

	return true;
}

#else

static inline bool lru_gen_enabled(void)
{
	return false;
}

static inline bool lru_gen_add_folio(struct lruvec *lruvec, struct folio *folio, bool reclaiming)
{
	return false;
}

static inline bool lru_gen_del_folio(struct lruvec *lruvec, struct folio *folio, bool reclaiming)
{
	return false;
}

#endif /* CONFIG_LRU_GEN */

static __always_inline
void lruvec_add_folio(struct lruvec *lruvec, struct folio *folio)
{
	enum lru_list lru = folio_lru_list(folio);

	if (lru_gen_add_folio(lruvec, folio, false))
		return;

	update_lru_size(lruvec, lru, folio_zonenum(folio),
			folio_nr_pages(folio));
	list_add(&folio->lru, &lruvec->lists[lru]);
}

static __always_inline void add_page_to_lru_list(struct page *page,
				struct lruvec *lruvec)
{
	lruvec_add_folio(lruvec, page_folio(page));
}

static __always_inline
void lruvec_add_folio_tail(struct lruvec *lruvec, struct folio *folio)
{
	enum lru_list lru = folio_lru_list(folio);

	if (lru_gen_add_folio(lruvec, folio, true))
		return;

	update_lru_size(lruvec, lru, folio_zonenum(folio),
			folio_nr_pages(folio));
	list_add_tail(&folio->lru, &lruvec->lists[lru]);
}

static __always_inline void add_page_to_lru_list_tail(struct page *page,
				struct lruvec *lruvec)
{
	lruvec_add_folio_tail(lruvec, page_folio(page));
}

static __always_inline
void lruvec_del_folio(struct lruvec *lruvec, struct folio *folio)
{
	if (lru_gen_del_folio(lruvec, folio, false))
		return;

	list_del(&folio->lru);
	update_lru_size(lruvec, folio_lru_list(folio), folio_zonenum(folio),
			-folio_nr_pages(folio));
}

static __always_inline void del_page_from_lru_list(struct page *page,
				struct lruvec *lruvec)
{
	lruvec_del_folio(lruvec, page_folio(page));
}
#endif
