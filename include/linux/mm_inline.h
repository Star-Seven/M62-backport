/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_MM_INLINE_H
#define LINUX_MM_INLINE_H

#include <linux/huge_mm.h>
#include <linux/swap.h>

/**
 * page_is_file_cache - should the page be on a file LRU or anon LRU?
 * @page: the page to test
 *
 * Returns 1 if @page is page cache page backed by a regular filesystem,
 * or 0 if @page is anonymous, tmpfs or otherwise ram or swap backed.
 * Used by functions that manipulate the LRU lists, to sort a page
 * onto the right LRU list.
 *
 * We would like to get this info without a page flag, but the state
 * needs to survive until the page is last deleted from the LRU, which
 * could be as far down as __page_cache_release.
 */
static inline int page_is_file_cache(struct page *page)
{
	return !PageSwapBacked(page);
}

static __always_inline void __update_lru_size(struct lruvec *lruvec,
				enum lru_list lru, enum zone_type zid,
				int nr_pages)
{
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);

	lockdep_assert_held(&pgdat->lru_lock);

	__mod_node_page_state(pgdat, NR_LRU_BASE + lru, nr_pages);
	__mod_zone_page_state(&pgdat->node_zones[zid],
				NR_ZONE_LRU_BASE + lru, nr_pages);
}

static __always_inline void update_lru_size(struct lruvec *lruvec,
				enum lru_list lru, enum zone_type zid,
				int nr_pages)
{
	__update_lru_size(lruvec, lru, zid, nr_pages);
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

	lru = page_is_file_cache(page) ? LRU_INACTIVE_FILE : LRU_INACTIVE_ANON;
	if (PageActive(page))
		lru += LRU_ACTIVE;

	return lru;
}

#define lru_to_page(head) (list_entry((head)->prev, struct page, lru))

#ifdef CONFIG_LRU_GEN

static inline bool lru_gen_enabled(void)
{
#ifdef CONFIG_LRU_GEN_ENABLED
	DECLARE_STATIC_KEY_TRUE(lru_gen_caps[NR_LRU_GEN_CAPS]);

	return static_branch_likely(&lru_gen_caps[LRU_GEN_CORE]);
#else
	DECLARE_STATIC_KEY_FALSE(lru_gen_caps[NR_LRU_GEN_CAPS]);

	return static_branch_unlikely(&lru_gen_caps[LRU_GEN_CORE]);
#endif
}

static inline bool lru_gen_in_fault(void)
{
	return current->in_lru_fault;
}

static inline int lru_gen_from_seq(unsigned long seq)
{
	return seq % MAX_NR_GENS;
}

static inline int lru_get_gen_flags(unsigned long flags)
{
	return ((flags & LRU_GEN_MASK) >> LRU_GEN_PGOFF) - 1;
}

static inline void lru_set_gen_flags(unsigned long *flags, int gen)
{
	*flags &= ~LRU_GEN_MASK;
	*flags |= (gen + 1UL) << LRU_GEN_PGOFF;
}

static inline int lru_hist_from_seq(unsigned long seq)
{
	return seq % NR_HIST_GENS;
}

static inline int lru_get_refs_flags(unsigned long flags)
{
	int refs = 0;

	if (flags & BIT(PG_referenced))
		refs |= BIT(0);
	if (flags & BIT(PG_workingset))
		refs |= BIT(1);
	refs |= ((flags & LRU_REFS_MASK) >> LRU_REFS_PGOFF) << 2;

	return refs;
}

static inline void lru_set_refs_flags(unsigned long *flags, unsigned int refs)
{
	VM_BUG_ON(refs > LRU_REFS_MAX);

	*flags &= ~LRU_REFS_FLAGS;
	if (refs & BIT(0))
		*flags |= BIT(PG_referenced);
	if (refs & BIT(1))
		*flags |= BIT(PG_workingset);
	*flags |= ((unsigned long)refs >> 2) << LRU_REFS_PGOFF;
}

static inline int page_lru_refs(struct page *page)
{
	return lru_get_refs_flags(READ_ONCE(page->flags));
}

/**
 * __page_set_lru_refs - Set a page's LRU refs.
 * @page: the page
 * @refs: the new referenced count (0 .. LRU_REFS_MAX)
 *
 * Set the page's LRU refs.  The page must be off the LRU list (e.g.,
 * isolated), or use lru_gen_inc_refs or page_reset_lru_refs instead.
 */
static inline void __page_set_lru_refs(struct page *page, unsigned int refs)
{
	unsigned long old_flags, new_flags;

	do {
		new_flags = old_flags = READ_ONCE(page->flags);
		VM_WARN_ON_ONCE(lru_get_gen_flags(old_flags) != -1);
		lru_set_refs_flags(&new_flags, refs);
	} while (cmpxchg(&page->flags, old_flags, new_flags) != old_flags);
}

static inline int lru_tier_from_refs(unsigned int refs)
{
	VM_BUG_ON(refs > LRU_REFS_MAX);

	/* see the comment on MAX_NR_TIERS */
	if (refs < LRU_REFS_WORKINGSET)
		return 0;
	return fls(refs - 1);
}

static inline bool lru_gen_at_tail(struct lruvec *lruvec, int type, int gen)
{
	unsigned long min_seq = READ_ONCE(lruvec->lrugen.min_seq[type]);

	/*
	 * Consider a page at the tail of its LRU when it is within
	 * MIN_NR_GENS of the oldest generation of its own type.
	 */
	BUILD_BUG_ON(MIN_NR_GENS != 2);
	return gen == lru_gen_from_seq(min_seq) ||
	       gen == lru_gen_from_seq(min_seq + 1);
}

static inline bool lru_refs_is_active(unsigned int refs)
{
	VM_BUG_ON(refs > LRU_REFS_MAX);

	return refs >= LRU_REFS_ACTIVATED;
}

static inline void lru_gen_update_size(struct lruvec *lruvec, int type,
					struct page *page, int old_gen, int new_gen)
{
	int zone = page_zonenum(page);
	int delta = hpage_nr_pages(page);
	struct lru_gen_struct *lrugen = &lruvec->lrugen;

	VM_BUG_ON(old_gen != -1 && old_gen >= MAX_NR_GENS);
	VM_BUG_ON(new_gen != -1 && new_gen >= MAX_NR_GENS);
	VM_BUG_ON(old_gen == -1 && new_gen == -1);

	if (old_gen >= 0)
		atomic_long_sub(delta, &lrugen->nr_pages[old_gen][type][zone]);
	if (new_gen >= 0)
		atomic_long_add(delta, &lrugen->nr_pages[new_gen][type][zone]);

	/* active/inactive counters are maintained by the callers, via refs */
}

static inline bool lru_gen_add_page(struct lruvec *lruvec, struct page *page, bool reclaiming)
{
	int gen;
	int refs = page_lru_refs(page);
	unsigned long seq;
	unsigned long old_flags, new_flags;
	int type = page_is_file_cache(page);
	enum lru_list lru = type * LRU_INACTIVE_FILE;
	int zone = page_zonenum(page);
	struct lru_gen_struct *lrugen = &lruvec->lrugen;

	if (PageUnevictable(page) || !lrugen->enabled)
		return false;
	seq = lrugen->max_seq;
	/*
	 * Frequency-guided placement:
	 * 1. Active pages with more than one access start in the youngest
	 *    generation; active pages accessed only once start one generation
	 *    older, so that a single fault does not protect them forever.
	 * 2. Reclaimable pages rotated back from writeback start in the oldest
	 *    generation; anon pages not in swapcache and dirty pages pending
	 *    writeback start in the second oldest generation.
	 * 3. Other pages are placed according to whether they are part of the
	 *    workingset (refs >= 2), i.e., second oldest or oldest.
	 */
	if (PageActive(page)) {
		if (refs < LRU_REFS_WORKINGSET && seq > lrugen->min_seq[type])
			seq--;
		gen = lru_gen_from_seq(seq);
	} else if (reclaiming) {
		gen = lru_gen_from_seq(lrugen->min_seq[type]);
	} else if ((type == LRU_GEN_ANON && !PageSwapCache(page)) ||
		 (PageReclaim(page) && (PageDirty(page) || PageWriteback(page)))) {
		gen = lru_gen_from_seq(lrugen->min_seq[type] + 1);
	} else {
		gen = lru_gen_from_seq(lrugen->min_seq[type] +
				       (refs >= LRU_REFS_WORKINGSET));
	}

	do {
		new_flags = old_flags = READ_ONCE(page->flags);
		VM_BUG_ON_PAGE(new_flags & LRU_GEN_MASK, page);

		/* see the comment on MIN_NR_GENS */
		new_flags &= ~(LRU_GEN_MASK | BIT(PG_active));
		new_flags |= (gen + 1UL) << LRU_GEN_PGOFF;
	} while (cmpxchg(&page->flags, old_flags, new_flags) != old_flags);

	lru_gen_update_size(lruvec, type, page, -1, gen);
	if (lru_refs_is_active(refs))
		lru += LRU_ACTIVE;
	update_lru_size(lruvec, lru, zone, hpage_nr_pages(page));
	/* for rotate_reclaimable_page() */
	if (reclaiming)
		list_add_tail(&page->lru, &lrugen->lists[gen][type][zone]);
	else
		list_add(&page->lru, &lrugen->lists[gen][type][zone]);

	return true;
}

static inline bool lru_gen_del_page(struct lruvec *lruvec, struct page *page, bool reclaiming)
{
	int gen;
	int refs = page_lru_refs(page);
	unsigned long old_flags, new_flags;
	int type = page_is_file_cache(page);
	int zone = page_zonenum(page);
	enum lru_list lru = type * LRU_INACTIVE_FILE;
	unsigned long max_seq = READ_ONCE(lruvec->lrugen.max_seq);

	do {
		new_flags = old_flags = READ_ONCE(page->flags);
		if (!(new_flags & LRU_GEN_MASK))
			return false;

		VM_BUG_ON_PAGE(PageActive(page), page);
		VM_BUG_ON_PAGE(PageUnevictable(page), page);

		gen = lru_get_gen_flags(new_flags);

		new_flags &= ~LRU_GEN_MASK;
		/* refs are access frequency history and survive isolation */
		if (reclaiming)
			new_flags &= ~(BIT(PG_reclaim) | BIT(PG_active));
		else if ((max_seq - gen) % MAX_NR_GENS < MIN_NR_GENS)
			new_flags |= BIT(PG_active);
	} while (cmpxchg(&page->flags, old_flags, new_flags) != old_flags);

	lru_gen_update_size(lruvec, type, page, gen, -1);
	if (lru_refs_is_active(refs))
		lru += LRU_ACTIVE;
	update_lru_size(lruvec, lru, zone, -hpage_nr_pages(page));
	list_del(&page->lru);

	return true;
}

void lru_gen_inc_refs(struct page *page, unsigned int flags);
bool page_reset_lru_refs(struct page *page);

#else

static inline bool lru_gen_enabled(void)
{
	return false;
}

static inline bool lru_gen_in_fault(void)
{
	return false;
}

static inline bool lru_gen_add_page(struct lruvec *lruvec, struct page *page, bool reclaiming)
{
	return false;
}

static inline bool lru_gen_del_page(struct lruvec *lruvec, struct page *page, bool reclaiming)
{
	return false;
}

static inline void lru_gen_inc_refs(struct page *page, unsigned int flags)
{
}

static inline bool page_reset_lru_refs(struct page *page)
{
	return false;
}

#endif /* CONFIG_LRU_GEN */

/**
 * page_test_workingset - Test if a page is in the workingset.
 * @page: the page
 *
 * A page is workingset when its LRU refs count reaches
 * LRU_REFS_WORKINGSET.  Under the classical LRU the extra refs counter
 * are always zero so this is just testing the PG_workingset bit.
 * NOTE: page_set_workingset() must not be used under MGLRU, as the
 * page refs are tracked by lru_gen_inc_refs() with rules.
 *
 * Return: true if the page is workingset.
 */
static __always_inline bool page_test_workingset(struct page *page)
{
#ifdef CONFIG_LRU_GEN
	return page_lru_refs(page) >= LRU_REFS_WORKINGSET;
#else
	return test_bit(PG_workingset, &page->flags);
#endif
}

/*
 * For the classical LRU only: with MGLRU the PG_workingset bit is part of
 * the page refs count maintained by lru_gen_inc_refs(), and a raw set
 * would corrupt it.
 */
static __always_inline void page_set_workingset(struct page *page)
{
#ifdef CONFIG_LRU_GEN
	VM_WARN_ON_ONCE(lru_gen_enabled());
#endif
	set_bit(PG_workingset, &page->flags);
}

/**
 * page_inc_lru_refs_fast - Bump page refs count without promotion.
 * @page: the page
 *
 * Raise refs from 0 to LRU_REFS_REFERENCED and leave hotter pages
 * untouched. For the classical LRU, a plain PG_referenced set.
 */
static __always_inline void page_inc_lru_refs_fast(struct page *page)
{
#ifdef CONFIG_LRU_GEN
	unsigned long new_flags, old_flags;

	if (!lru_gen_enabled()) {
		set_bit(PG_referenced, &page->flags);
		return;
	}

	old_flags = READ_ONCE(page->flags);
	do {
		if (lru_get_refs_flags(old_flags))
			break;
		new_flags = old_flags;
		lru_set_refs_flags(&new_flags, LRU_REFS_REFERENCED);
	} while (cmpxchg(&page->flags, old_flags, new_flags) != old_flags);
#else
	set_bit(PG_referenced, &page->flags);
#endif
}

/**
 * page_check_lru_refs - Test if an LRU managed page has been referenced.
 * @page: The page to test.
 *
 * Returns true if the MGLRU refs count is nonzero, or PG_referenced is set,
 * which is also correct for the non-MGLRU case.  Kept as a separate helper,
 * to avoid splattering the lru_gen_enabled() static key.
 */
static inline bool page_check_lru_refs(struct page *page)
{
#ifdef CONFIG_LRU_GEN
	if (lru_gen_enabled())
		return page_lru_refs(page) > 0;
#endif
	return test_bit(PG_referenced, &page->flags);
}

/**
 * page_clear_lru_refs - Clear the referenced state of an LRU managed page.
 * @page: The page to clear.
 *
 * Reset the MGLRU refs count to zero, or clear PG_referenced, leaving
 * PG_workingset alone for the non-MGLRU case.  Kept as a separate helper
 * just like page_check_lru_refs().
 */
static inline void page_clear_lru_refs(struct page *page)
{
#ifdef CONFIG_LRU_GEN
	if (lru_gen_enabled()) {
		__page_set_lru_refs(page, 0);
		return;
	}
#endif
	clear_bit(PG_referenced, &page->flags);
}

static __always_inline void add_page_to_lru_list(struct page *page,
				struct lruvec *lruvec)
{
	enum lru_list lru = page_lru(page);

	if (lru_gen_add_page(lruvec, page, false))
		return;

	update_lru_size(lruvec, lru, page_zonenum(page), hpage_nr_pages(page));
	list_add(&page->lru, &lruvec->lists[lru]);
}

static __always_inline void add_page_to_lru_list_tail(struct page *page,
				struct lruvec *lruvec)
{
	enum lru_list lru = page_lru(page);

	if (lru_gen_add_page(lruvec, page, true))
		return;

	update_lru_size(lruvec, lru, page_zonenum(page), hpage_nr_pages(page));
	list_add_tail(&page->lru, &lruvec->lists[lru]);
}

static __always_inline void del_page_from_lru_list(struct page *page,
				struct lruvec *lruvec)
{
	if (lru_gen_del_page(lruvec, page, false))
		return;

	list_del(&page->lru);
	update_lru_size(lruvec, page_lru(page), page_zonenum(page),
			-hpage_nr_pages(page));
}
#endif
