/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_MM_INLINE_H
#define LINUX_MM_INLINE_H

#include <linux/huge_mm.h>
#include <linux/swap.h>

/**
 * page_is_file_lru - should the page be on a file LRU or anon LRU?
 * @page: the page to test
 *
 * Returns 1 if @page is a regular filesystem backed page cache page or a lazily
 * freed anonymous page (e.g. via MADV_FREE).  Returns 0 if @page is a normal
 * anonymous page, a tmpfs page or otherwise ram or swap backed page.  Used by
 * functions that manipulate the LRU lists, to sort a page onto the right LRU
 * list.
 *
 * We would like to get this info without a page flag, but the state
 * needs to survive until the page is last deleted from the LRU, which
 * could be as far down as __page_cache_release.
 */
static inline int page_is_file_lru(struct page *page)
{
	return !PageSwapBacked(page);
}

static __always_inline void update_lru_size(struct lruvec *lruvec,
				enum lru_list lru, enum zone_type zid,
				int nr_pages)
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
 * __clear_page_lru_flags - clear page lru flags before releasing a page
 * @page: the page that was on lru and now has a zero reference
 */
static __always_inline void __clear_page_lru_flags(struct page *page)
{
	VM_BUG_ON_PAGE(!PageLRU(page), page);

	__ClearPageLRU(page);

	/* this shouldn't happen, so leave the flags to bad_page() */
	if (PageActive(page) && PageUnevictable(page))
		return;

	__ClearPageActive(page);
	__ClearPageUnevictable(page);
}

/**
 * page_lru - which LRU list should a page be on?
 * @page: the page to test
 *
 * Returns the LRU list a page should be on, as an index
 * into the array of LRU lists.
 */
static __always_inline enum lru_list page_lru(struct page *page)
{
	enum lru_list lru;

	VM_BUG_ON_PAGE(PageActive(page) && PageUnevictable(page), page);

	if (PageUnevictable(page))
		return LRU_UNEVICTABLE;

	lru = page_is_file_lru(page) ? LRU_INACTIVE_FILE : LRU_INACTIVE_ANON;
	if (PageActive(page))
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

/* Return an index within the sliding window that tracks MAX_NR_GENS generations. */
static inline int lru_gen_from_seq(unsigned long seq)
{
	return seq % MAX_NR_GENS;
}

/* Return a proper index regardless whether we keep stats for historical generations. */
static inline int lru_hist_from_seq(unsigned long seq)
{
	return seq % NR_HIST_GENS;
}

/* Convert the number of accesses to a tier. See the comment on MAX_NR_TIERS. */
static inline int lru_tier_from_refs(int refs)
{
	VM_BUG_ON(refs > BIT(LRU_REFS_WIDTH));

	return order_base_2(refs + 1);
}

/* The youngest and the second youngest generations are counted as active. */
static inline bool lru_gen_is_active(struct lruvec *lruvec, int gen)
{
	unsigned long max_seq = lruvec->evictable.max_seq;

	VM_BUG_ON(gen >= MAX_NR_GENS);

	return gen == lru_gen_from_seq(max_seq) || gen == lru_gen_from_seq(max_seq - 1);
}

/* Update the sizes of the multigenerational lru lists. */
static inline void lru_gen_update_size(struct page *page, struct lruvec *lruvec,
				       int old_gen, int new_gen)
{
	int type = page_is_file_lru(page);
	int zone = page_zonenum(page);
	int delta = thp_nr_pages(page);
	enum lru_list lru = type * LRU_FILE;
	struct lrugen *lrugen = &lruvec->evictable;

	lockdep_assert_held(&lruvec->lru_lock);
	VM_BUG_ON(old_gen != -1 && old_gen >= MAX_NR_GENS);
	VM_BUG_ON(new_gen != -1 && new_gen >= MAX_NR_GENS);
	VM_BUG_ON(old_gen == -1 && new_gen == -1);

	if (old_gen >= 0)
		WRITE_ONCE(lrugen->sizes[old_gen][type][zone],
			   lrugen->sizes[old_gen][type][zone] - delta);
	if (new_gen >= 0)
		WRITE_ONCE(lrugen->sizes[new_gen][type][zone],
			   lrugen->sizes[new_gen][type][zone] + delta);

	if (old_gen < 0) {
		if (lru_gen_is_active(lruvec, new_gen))
			lru += LRU_ACTIVE;
		update_lru_size(lruvec, lru, zone, delta);
		return;
	}

	if (new_gen < 0) {
		if (lru_gen_is_active(lruvec, old_gen))
			lru += LRU_ACTIVE;
		update_lru_size(lruvec, lru, zone, -delta);
		return;
	}

	if (!lru_gen_is_active(lruvec, old_gen) && lru_gen_is_active(lruvec, new_gen)) {
		update_lru_size(lruvec, lru, zone, -delta);
		update_lru_size(lruvec, lru + LRU_ACTIVE, zone, delta);
	}

	VM_BUG_ON(lru_gen_is_active(lruvec, old_gen) && !lru_gen_is_active(lruvec, new_gen));
}

/* Add a page to one of the multigenerational lru lists. Return true on success. */
static inline bool lru_gen_add_page(struct page *page, struct lruvec *lruvec, bool reclaiming)
{
	int gen;
	unsigned long old_flags, new_flags;
	int type = page_is_file_lru(page);
	int zone = page_zonenum(page);
	struct lrugen *lrugen = &lruvec->evictable;

	if (PageUnevictable(page) || !lrugen->enabled[type])
		return false;
	/*
	 * If a page shouldn't be considered for eviction, i.e., a page mapped
	 * upon fault during which the accessed bit is set, add it to the
	 * youngest generation.
	 *
	 * If a page can't be evicted immediately, i.e., an anon page not in
	 * swap cache or a dirty page pending writeback, add it to the second
	 * oldest generation.
	 *
	 * If a page could be evicted immediately, e.g., a clean page, add it to
	 * the oldest generation.
	 */
	if (PageActive(page))
		gen = lru_gen_from_seq(lrugen->max_seq);
	else if ((!type && !PageSwapCache(page)) ||
		 (PageReclaim(page) && (PageDirty(page) || PageWriteback(page))))
		gen = lru_gen_from_seq(lrugen->min_seq[type] + 1);
	else
		gen = lru_gen_from_seq(lrugen->min_seq[type]);

	do {
		new_flags = old_flags = READ_ONCE(page->flags);
		VM_BUG_ON_PAGE(new_flags & LRU_GEN_MASK, page);

		new_flags &= ~(LRU_GEN_MASK | BIT(PG_active));
		new_flags |= (gen + 1UL) << LRU_GEN_PGOFF;
	} while (cmpxchg(&page->flags, old_flags, new_flags) != old_flags);

	lru_gen_update_size(page, lruvec, -1, gen);
	/* for rotate_reclaimable_page() */
	if (reclaiming)
		list_add_tail(&page->lru, &lrugen->lists[gen][type][zone]);
	else
		list_add(&page->lru, &lrugen->lists[gen][type][zone]);

	return true;
}

/* Delete a page from one of the multigenerational lru lists. Return true on success. */
static inline bool lru_gen_del_page(struct page *page, struct lruvec *lruvec, bool reclaiming)
{
	int gen;
	unsigned long old_flags, new_flags;

	do {
		new_flags = old_flags = READ_ONCE(page->flags);
		if (!(new_flags & LRU_GEN_MASK))
			return false;

		VM_BUG_ON_PAGE(PageActive(page), page);
		VM_BUG_ON_PAGE(PageUnevictable(page), page);

		gen = ((new_flags & LRU_GEN_MASK) >> LRU_GEN_PGOFF) - 1;

		new_flags &= ~LRU_GEN_MASK;
		if ((new_flags & LRU_REFS_FLAGS) != LRU_REFS_FLAGS)
			new_flags &= ~(LRU_REFS_MASK | LRU_REFS_FLAGS);
		/* for shrink_page_list() */
		if (reclaiming)
			new_flags &= ~(BIT(PG_referenced) | BIT(PG_reclaim));
		else if (lru_gen_is_active(lruvec, gen))
			new_flags |= BIT(PG_active);
	} while (cmpxchg(&page->flags, old_flags, new_flags) != old_flags);

	lru_gen_update_size(page, lruvec, gen, -1);
	list_del(&page->lru);

	return true;
}

#else

static inline bool lru_gen_enabled(void)
{
	return false;
}

static inline bool lru_gen_add_page(struct page *page, struct lruvec *lruvec, bool reclaiming)
{
	return false;
}

static inline bool lru_gen_del_page(struct page *page, struct lruvec *lruvec, bool reclaiming)
{
	return false;
}

#endif /* CONFIG_LRU_GEN */

static __always_inline void add_page_to_lru_list(struct page *page,
				struct lruvec *lruvec)
{
	enum lru_list lru = page_lru(page);

	if (lru_gen_add_page(page, lruvec, false))
		return;

	update_lru_size(lruvec, lru, page_zonenum(page), thp_nr_pages(page));
	list_add(&page->lru, &lruvec->lists[lru]);
}

static __always_inline void add_page_to_lru_list_tail(struct page *page,
				struct lruvec *lruvec)
{
	enum lru_list lru = page_lru(page);

	if (lru_gen_add_page(page, lruvec, true))
		return;

	update_lru_size(lruvec, lru, page_zonenum(page), thp_nr_pages(page));
	list_add_tail(&page->lru, &lruvec->lists[lru]);
}

static __always_inline void del_page_from_lru_list(struct page *page,
				struct lruvec *lruvec)
{
	if (lru_gen_del_page(page, lruvec, false))
		return;

	list_del(&page->lru);
	update_lru_size(lruvec, page_lru(page), page_zonenum(page),
			-thp_nr_pages(page));
}
#endif
