/*
 * gcma.c - Guaranteed Contiguous Memory Allocator
 *
 * GCMA aims for contiguous memory allocation with success and fast
 * latency guarantee.
 * It reserves large amount of memory and let it be allocated to
 * contiguous memory requests. Because system memory space efficiency could be
 * degraded if reserved area being idle, GCMA let the reserved area could be
 * used by other clients with lower priority.
 * We call those lower priority clients as second-class clients. In this
 * context, contiguous memory requests are first-class clients, of course.
 *
 * With this idea, gcma withdraw pages being used for second-class clients and
 * gives them to first-class clients if they required. Because latency
 * and success of first-class clients depend on speed and availability of
 * withdrawing, GCMA restricts only easily discardable memory could be used for
 * second-class clients.
 *
 * To support various second-class clients, GCMA provides interface and
 * backend of discardable memory. Any candiates satisfying with discardable
 * memory could be second-class client of GCMA using the interface.
 *
 * Copyright (C) 2014  LG Electronics Inc.,
 * Copyright (C) 2014  Minchan Kim <minchan@kernel.org>
 * Copyright (C) 2014-2015  SeongJae Park <sj38.park@gmail.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/gcma.h>
#include <linux/highmem.h>
#include <linux/module.h>
#include <linux/slab.h>

/* XXX: What's the ideal? */
#define NR_EVICT_BATCH	32

struct gcma {
	spinlock_t lock;
	unsigned long *bitmap;
	unsigned long base_pfn, size;
	struct list_head list;
};

struct gcma_info {
	spinlock_t lock;	/* protect list */
	struct list_head head;
};

static struct gcma_info ginfo = {
	.head = LIST_HEAD_INIT(ginfo.head),
	.lock = __SPIN_LOCK_UNLOCKED(ginfo.lock),
};

/*
 * Discardable memory(dmem) store and load easily discardable pages inside
 * gcma area. Because it's discardable memory, loading stored page could fail
 * anytime.
 */

/* entry for a discardable page */
struct dmem_entry {
	struct rb_node rbnode;
	struct gcma *gcma;
	void *key;
	struct page *page;
	atomic_t refcount;
};

/* dmem hash bucket */
struct dmem_hashbucket {
	struct dmem *dmem;
	struct rb_root rbroot;
	spinlock_t lock;
};

/* dmem pool */
struct dmem_pool {
	struct dmem_hashbucket *hashbuckets;
};

struct dmem {
	struct dmem_pool **pools;
	unsigned nr_pools;
	unsigned nr_hash;
	struct kmem_cache *key_cache;
	size_t bytes_key;
	struct list_head lru_list;
	spinlock_t lru_lock;

	unsigned (*hash_key)(void *key);
	int (*compare)(void *lkey, void *rkey);
};

static struct kmem_cache *dmem_entry_cache;

static unsigned long dmem_evict_lru(struct dmem *dmem, unsigned long nr_pages);

static struct dmem_hashbucket *dmem_hashbuck(struct page *page)
{
	return (struct dmem_hashbucket *)page->mapping;
}

static void set_dmem_hashbuck(struct page *page, struct dmem_hashbucket *buck)
{
	page->mapping = (struct address_space *)buck;
}

static struct dmem_entry *dmem_entry(struct page *page)
{
	return (struct dmem_entry *)page->index;
}

static void set_dmem_entry(struct page *page, struct dmem_entry *entry)
{
	page->index = (pgoff_t)entry;
}

/*
 * Flags for status of a page in gcma
 *
 * GF_LRU
 * The page is being used for a dmem and hang on LRU list of the dmem.
 * It could be discarded for contiguous memory allocation easily.
 * Protected by dmem->lru_lock.
 *
 * GF_RECLAIMING
 * The page is being discarded for contiguous memory allocation.
 * It should not be used for dmem anymore.
 * Protected by dmem->lru_lock.
 *
 * GF_ISOLATED
 * The page is isolated from dmem.
 * GCMA clients can use the page safely while dmem should not.
 * Protected by gcma->lock.
 */
enum gpage_flags {
	GF_LRU = 0x1,
	GF_RECLAIMING = 0x2,
	GF_ISOLATED = 0x4,
};

static int gpage_flag(struct page *page, int flag)
{
	return page->private & flag;
}

static void set_gpage_flag(struct page *page, int flag)
{
	page->private |= flag;
}

static void clear_gpage_flag(struct page *page, int flag)
{
	page->private &= ~flag;
}

static void clear_gpage_flagall(struct page *page)
{
	page->private = 0;
}

/*
 * gcma_init - initializes a contiguous memory area
 *
 * @start_pfn	start pfn of contiguous memory area
 * @size	number of pages in the contiguous memory area
 * @res_gcma	pointer to store the created gcma region
 *
 * Returns 0 on success, error code on failure.
 */
int gcma_init(unsigned long start_pfn, unsigned long size,
		struct gcma **res_gcma)
{
	int bitmap_size = BITS_TO_LONGS(size) * sizeof(long);
	struct gcma *gcma;

	gcma = kmalloc(sizeof(*gcma), GFP_KERNEL);
	if (!gcma)
		goto out;

	gcma->bitmap = kzalloc(bitmap_size, GFP_KERNEL);
	if (!gcma->bitmap)
		goto free_cma;

	gcma->size = size;
	gcma->base_pfn = start_pfn;
	spin_lock_init(&gcma->lock);

	spin_lock(&ginfo.lock);
	list_add(&gcma->list, &ginfo.head);
	spin_unlock(&ginfo.lock);

	*res_gcma = gcma;
	pr_info("initialized gcma area [%lu, %lu]\n",
			start_pfn, start_pfn + size);
	return 0;

free_cma:
	kfree(gcma);
out:
	return -ENOMEM;
}

static struct page *gcma_alloc_page(struct gcma *gcma)
{
	unsigned long bit;
	unsigned long *bitmap = gcma->bitmap;
	struct page *page = NULL;

	spin_lock(&gcma->lock);
	bit = bitmap_find_next_zero_area(bitmap, gcma->size, 0, 1, 0);
	if (bit >= gcma->size) {
		spin_unlock(&gcma->lock);
		goto out;
	}

	bitmap_set(bitmap, bit, 1);
	page = pfn_to_page(gcma->base_pfn + bit);
	spin_unlock(&gcma->lock);
	clear_gpage_flagall(page);

out:
	return page;
}

/* Caller should hold lru_lock */
static void gcma_free_page(struct gcma *gcma, struct page *page)
{
	unsigned long pfn, offset;

	pfn = page_to_pfn(page);

	spin_lock(&gcma->lock);
	offset = pfn - gcma->base_pfn;

	if (likely(!gpage_flag(page, GF_RECLAIMING))) {
		bitmap_clear(gcma->bitmap, offset, 1);
	} else {
		/*
		 * The page should be safe to be used for a thread which
		 * reclaimed the page.
		 * To prevent further allocation from other thread,
		 * set bitmap and mark the page as isolated.
		 */
		bitmap_set(gcma->bitmap, offset, 1);
		set_gpage_flag(page, GF_ISOLATED);
	}
	spin_unlock(&gcma->lock);
}

/*
 * In the case that a entry with the same offset is found, a pointer to
 * the existing entry is stored in dupentry and the function returns -EEXIST.
 */
static int dmem_insert_entry(struct dmem_hashbucket *bucket,
		struct dmem_entry *entry,
		struct dmem_entry **dupentry)
{
	struct rb_node **link = &bucket->rbroot.rb_node, *parent = NULL;
	struct dmem_entry *iter;
	int cmp;

	while (*link) {
		parent = *link;
		iter = rb_entry(parent, struct dmem_entry, rbnode);
		cmp = bucket->dmem->compare(entry->key, iter->key);
		if (cmp < 0)
			link = &(*link)->rb_left;
		else if (cmp > 0)
			link = &(*link)->rb_right;
		else {
			*dupentry = iter;
			return -EEXIST;
		}
	}
	rb_link_node(&entry->rbnode, parent, link);
	rb_insert_color(&entry->rbnode, &bucket->rbroot);
	return 0;
}

static void dmem_erase_entry(struct dmem_hashbucket *bucket,
		struct dmem_entry *entry)
{
	if (!RB_EMPTY_NODE(&entry->rbnode)) {
		rb_erase(&entry->rbnode, &bucket->rbroot);
		RB_CLEAR_NODE(&entry->rbnode);
	}
}

static struct dmem_entry *dmem_search_entry(struct dmem_hashbucket *bucket,
		void *key)
{
	struct rb_node *node = bucket->rbroot.rb_node;
	struct dmem_entry *iter;
	int cmp;

	while (node) {
		iter = rb_entry(node, struct dmem_entry, rbnode);
		cmp = bucket->dmem->compare(key, iter->key);
		if (cmp < 0)
			node = node->rb_left;
		else if (cmp > 0)
			node = node->rb_right;
		else
			return iter;
	}
	return NULL;
}

/* Allocates a page from gcma areas using round-robin way */
static struct page *dmem_alloc_page(struct dmem *dmem, struct gcma **res_gcma)
{
	struct page *page;
	struct gcma *gcma;

retry:
	spin_lock(&ginfo.lock);
	gcma = list_first_entry(&ginfo.head, struct gcma, list);
	list_move_tail(&gcma->list, &ginfo.head);

	list_for_each_entry(gcma, &ginfo.head, list) {
		page = gcma_alloc_page(gcma);
		if (page) {
			spin_unlock(&ginfo.lock);
			goto got;
		}
	}
	spin_unlock(&ginfo.lock);

	/*
	 * Failed to alloc a page from entire gcma. Evict adequate LRU
	 * discardable pages and try allocation again.
	 */
	if (dmem_evict_lru(dmem, NR_EVICT_BATCH))
		goto retry;

got:
	*res_gcma = gcma;
	return page;
}

/* Should be called from dmem_put only */
static void dmem_free_entry(struct dmem *dmem, struct dmem_entry *entry)
{
	gcma_free_page(entry->gcma, entry->page);
	kmem_cache_free(dmem->key_cache, entry->key);
	kmem_cache_free(dmem_entry_cache, entry);
}

/* Caller should hold hashbucket spinlock */
static void dmem_get(struct dmem_entry *entry)
{
	atomic_inc(&entry->refcount);
}

/*
 * Caller should hold hashbucket spinlock and dmem lru_lock.
 * Remove from the bucket and free it, if nobody reference the entry.
 */
static void dmem_put(struct dmem_hashbucket *buck,
				struct dmem_entry *entry)
{
	int refcount = atomic_dec_return(&entry->refcount);

	BUG_ON(refcount < 0);

	if (refcount == 0) {
		struct page *page = entry->page;

		dmem_erase_entry(buck, entry);
		list_del(&page->lru);
		dmem_free_entry(buck->dmem, entry);
	}
}

/*
 * dmem_evict_lru - evict @nr_pages LRU dmem pages
 *
 * @dmem	dmem to evict LRU pages from
 * @nr_pages	number of LRU pages to be evicted
 *
 * Returns number of successfully evicted pages
 */
static unsigned long dmem_evict_lru(struct dmem *dmem, unsigned long nr_pages)
{
	struct dmem_hashbucket *buck;
	struct dmem_entry *entry;
	struct page *page, *n;
	unsigned long evicted = 0;
	u8 key[dmem->bytes_key];
	LIST_HEAD(free_pages);

	spin_lock(&dmem->lru_lock);
	list_for_each_entry_safe_reverse(page, n, &dmem->lru_list, lru) {
		entry = dmem_entry(page);

		/*
		 * the entry could be free by other thread in the while.
		 * check whether the situation occurred and avoid others to
		 * free it by compare reference count and increase it
		 * atomically.
		 */
		if (!atomic_inc_not_zero(&entry->refcount))
			continue;

		clear_gpage_flag(page, GF_LRU);
		list_move(&page->lru, &free_pages);
		if (++evicted >= nr_pages)
			break;
	}
	spin_unlock(&dmem->lru_lock);

	list_for_each_entry_safe(page, n, &free_pages, lru) {
		buck = dmem_hashbuck(page);
		entry = dmem_entry(page);

		spin_lock(&buck->lock);
		spin_lock(&dmem->lru_lock);
		/* drop refcount increased by above loop */
		memcpy(&key, entry->key, dmem->bytes_key);
		dmem_put(buck, entry);
		/* free entry if the entry is still in tree */
		if (dmem_search_entry(buck, &key))
			dmem_put(buck, entry);
		spin_unlock(&dmem->lru_lock);
		spin_unlock(&buck->lock);
	}

	return evicted;
}

/* Caller should hold bucket spinlock */
static struct dmem_entry *dmem_find_get_entry(struct dmem_hashbucket *buck,
						void *key)
{
	struct dmem_entry *entry;

	assert_spin_locked(&buck->lock);
	entry = dmem_search_entry(buck, key);
	if (entry)
		dmem_get(entry);

	return entry;
}

static struct dmem_hashbucket *dmem_find_hashbucket(struct dmem *dmem,
							struct dmem_pool *pool,
							void *key)
{
	return &pool->hashbuckets[dmem->hash_key(key)];
}

/*
 * dmem_init_pool - initialize a pool in dmem
 *
 * @dmem	dmem of a pool to be initialized
 * @pool_id	id of a pool to be initialized
 *
 * Returns 0 if success,
 * Returns non-zero if failed.
 */
int dmem_init_pool(struct dmem *dmem, unsigned pool_id)
{
	struct dmem_pool *pool;
	struct dmem_hashbucket *buck;
	int i;

	pool = kzalloc(sizeof(struct dmem_pool), GFP_KERNEL);
	if (!pool) {
		pr_warn("%s: failed to alloc dmem pool %d\n",
				__func__, pool_id);
		return -ENOMEM;
	}

	pool->hashbuckets = kzalloc(
				sizeof(struct dmem_hashbucket) * dmem->nr_hash,
				GFP_KERNEL);
	if (!pool) {
		pr_warn("%s: failed to alloc hashbuckets\n", __func__);
		kfree(pool);
		return -ENOMEM;
	}

	for (i = 0; i < dmem->nr_hash; i++) {
		buck = &pool->hashbuckets[i];
		buck->dmem = dmem;
		buck->rbroot = RB_ROOT;
		spin_lock_init(&buck->lock);
	}

	dmem->pools[pool_id] = pool;
	return 0;
}

/*
 * dmem_store_page - store a page in dmem
 *
 * Saves content of @page in gcma area and manages it using dmem. The content
 * could be loaded again from dmem using @key if it has not been discarded for
 * first-class clients.
 *
 * @dmem	dmem to store the page in
 * @pool_id	id of a dmem pool to store the page in
 * @key		key of the page to be stored in
 * @page	page to be stored in
 *
 * Returns 0 if success,
 * Returns non-zero if failed.
 */
int dmem_store_page(struct dmem *dmem, unsigned pool_id, void *key,
			struct page *page)
{
	struct dmem_pool *pool;
	struct dmem_hashbucket *buck;
	struct dmem_entry *entry, *dupentry;
	struct gcma *gcma;
	struct page *gcma_page = NULL;

	u8 *src, *dst;
	int ret;

	pool = dmem->pools[pool_id];
	if (!pool) {
		pr_warn("%s: dmem pool for id %d is not exist\n",
				__func__, pool_id);
		return -ENODEV;
	}

	gcma_page = dmem_alloc_page(dmem, &gcma);
	if (!gcma_page)
		return -ENOMEM;

	entry = kmem_cache_alloc(dmem_entry_cache, GFP_ATOMIC);
	if (!entry) {
		spin_lock(&dmem->lru_lock);
		gcma_free_page(gcma, gcma_page);
		spin_unlock(&dmem->lru_lock);
		return -ENOMEM;
	}

	entry->gcma = gcma;
	entry->page = gcma_page;
	entry->key = kmem_cache_alloc(dmem->key_cache, GFP_ATOMIC);
	if (!entry->key) {
		spin_lock(&dmem->lru_lock);
		gcma_free_page(gcma, gcma_page);
		spin_unlock(&dmem->lru_lock);
		kmem_cache_free(dmem_entry_cache, entry);
		return -ENOMEM;
	}
	memcpy(entry->key, key, dmem->bytes_key);
	atomic_set(&entry->refcount, 1);
	RB_CLEAR_NODE(&entry->rbnode);

	buck = dmem_find_hashbucket(dmem, pool, entry->key);
	set_dmem_hashbuck(gcma_page, buck);
	set_dmem_entry(gcma_page, entry);

	/* copy from orig data to gcma_page */
	src = kmap_atomic(page);
	dst = kmap_atomic(gcma_page);
	memcpy(dst, src, PAGE_SIZE);
	kunmap_atomic(src);
	kunmap_atomic(dst);

	spin_lock(&buck->lock);
	do {
		/*
		 * Though this duplication scenario may happen rarely by
		 * race of dmem client layer, we handle this case here rather
		 * than fix the client layer because handling the possibility
		 * of duplicates is part of the tmem ABI.
		 */
		ret = dmem_insert_entry(buck, entry, &dupentry);
		if (ret == -EEXIST) {
			dmem_erase_entry(buck, dupentry);
			spin_lock(&dmem->lru_lock);
			dmem_put(buck, dupentry);
			spin_unlock(&dmem->lru_lock);
		}
	} while (ret == -EEXIST);

	spin_lock(&dmem->lru_lock);
	set_gpage_flag(gcma_page, GF_LRU);
	list_add(&gcma_page->lru, &dmem->lru_list);
	spin_unlock(&dmem->lru_lock);
	spin_unlock(&buck->lock);

	return ret;
}

/*
 * dmem_load_page - load a page stored in dmem using @key
 *
 * @dmem	dmem which the page stored in
 * @pool_id	id of a dmem pool the page stored in
 * @key		key of the page looking for
 * @page	page to store the loaded content
 *
 * Returns 0 if success,
 * Returns non-zero if failed.
 */
int dmem_load_page(struct dmem *dmem, unsigned pool_id, void *key,
			struct page *page)
{
	struct dmem_pool *pool;
	struct dmem_hashbucket *buck;
	struct dmem_entry *entry;
	struct page *gcma_page;
	u8 *src, *dst;

	pool = dmem->pools[pool_id];
	if (!pool) {
		pr_warn("dmem pool for id %d not exist\n", pool_id);
		return -1;
	}

	buck = dmem_find_hashbucket(dmem, pool, key);

	spin_lock(&buck->lock);
	entry = dmem_find_get_entry(buck, key);
	spin_unlock(&buck->lock);
	if (!entry)
		return -1;

	gcma_page = entry->page;
	src = kmap_atomic(gcma_page);
	dst = kmap_atomic(page);
	memcpy(dst, src, PAGE_SIZE);
	kunmap_atomic(src);
	kunmap_atomic(dst);

	spin_lock(&buck->lock);
	spin_lock(&dmem->lru_lock);
	if (likely(gpage_flag(gcma_page, GF_LRU)))
		list_move(&gcma_page->lru, &dmem->lru_list);
	dmem_put(buck, entry);
	spin_unlock(&dmem->lru_lock);
	spin_unlock(&buck->lock);

	return 0;
}

/*
 * dmem_invalidate_entry - invalidates an entry from dmem
 *
 * @dmem	dmem of entry to be invalidated
 * @pool_id	dmem pool id of entry to be invalidated
 * @key		key of entry to be invalidated
 *
 * Returns 0 if success,
 * Returns non-zero if failed.
 */
int dmem_invalidate_entry(struct dmem *dmem, unsigned pool_id, void *key)
{
	struct dmem_pool *pool;
	struct dmem_hashbucket *buck;
	struct dmem_entry *entry;

	pool = dmem->pools[pool_id];
	buck = dmem_find_hashbucket(dmem, pool, key);

	spin_lock(&buck->lock);
	entry = dmem_search_entry(buck, key);
	if (!entry) {
		spin_unlock(&buck->lock);
		return -ENOENT;
	}

	spin_lock(&dmem->lru_lock);
	dmem_put(buck, entry);
	spin_unlock(&dmem->lru_lock);
	spin_unlock(&buck->lock);

	return 0;
}

/*
 * dmem_invalidate_pool - invalidates whole entries in a dmem pool
 *
 * @dmem	dmem of pool to be invalidated
 * @pool_id	id of pool to be invalidated
 *
 * Returns 0 if success,
 * Returns non-zero if failed.
 */
int dmem_invalidate_pool(struct dmem *dmem, unsigned pool_id)
{
	struct dmem_pool *pool;
	struct dmem_hashbucket *buck;
	struct dmem_entry *entry, *n;
	int i;

	pool = dmem->pools[pool_id];
	if (!pool)
		return -1;

	for (i = 0; i < dmem->nr_hash; i++) {
		buck = &pool->hashbuckets[i];
		spin_lock(&buck->lock);
		rbtree_postorder_for_each_entry_safe(entry, n, &buck->rbroot,
							rbnode) {
			spin_lock(&dmem->lru_lock);
			dmem_put(buck, entry);
			spin_unlock(&dmem->lru_lock);
		}
		buck->rbroot = RB_ROOT;
		spin_unlock(&buck->lock);
	}

	kfree(pool->hashbuckets);
	kfree(pool);
	dmem->pools[pool_id] = NULL;

	return 0;
}

/*
 * Return 0 if [start_pfn, end_pfn] is isolated.
 * Otherwise, return first unisolated pfn from the start_pfn.
 */
static unsigned long isolate_interrupted(struct gcma *gcma,
		unsigned long start_pfn, unsigned long end_pfn)
{
	unsigned long offset;
	unsigned long *bitmap;
	unsigned long pfn, ret = 0;
	struct page *page;

	spin_lock(&gcma->lock);

	for (pfn = start_pfn; pfn < end_pfn; pfn++) {
		int set;

		offset = pfn - gcma->base_pfn;
		bitmap = gcma->bitmap + offset / BITS_PER_LONG;

		set = test_bit(pfn % BITS_PER_LONG, bitmap);
		if (!set) {
			ret = pfn;
			break;
		}

		page = pfn_to_page(pfn);
		if (!gpage_flag(page, GF_ISOLATED)) {
			ret = pfn;
			break;
		}

	}
	spin_unlock(&gcma->lock);
	return ret;
}

/*
 * gcma_alloc_contig - allocates contiguous pages
 *
 * @start_pfn	start pfn of requiring contiguous memory area
 * @size	size of the requiring contiguous memory area
 *
 * Returns 0 on success, error code on failure.
 */
int gcma_alloc_contig(struct gcma *gcma, unsigned long start_pfn,
			unsigned long size)
{
	LIST_HEAD(free_pages);
	struct dmem_hashbucket *buck;
	struct dmem_entry *entry;
	struct cleancache_dmem_key key;	/* cc key is larger than fs's */
	struct page *page, *n;
	unsigned long offset;
	unsigned long *bitmap;
	unsigned long pfn;
	unsigned long orig_start = start_pfn;
	spinlock_t *lru_lock;

retry:
	for (pfn = start_pfn; pfn < start_pfn + size; pfn++) {
		spin_lock(&gcma->lock);

		offset = pfn - gcma->base_pfn;
		bitmap = gcma->bitmap + offset / BITS_PER_LONG;
		page = pfn_to_page(pfn);

		if (!test_bit(offset % BITS_PER_LONG, bitmap)) {
			/* set a bit to prevent allocation for dmem */
			bitmap_set(gcma->bitmap, offset, 1);
			set_gpage_flag(page, GF_ISOLATED);
			spin_unlock(&gcma->lock);
			continue;
		}
		if (gpage_flag(page, GF_ISOLATED)) {
			spin_unlock(&gcma->lock);
			continue;
		}

		/* Someone is using the page so it's complicated :( */
		spin_unlock(&gcma->lock);

		/* During dmem_store, hashbuck could not be set in page, yet */
		if (dmem_hashbuck(page) == NULL)
			continue;

		lru_lock = &dmem_hashbuck(page)->dmem->lru_lock;
		spin_lock(lru_lock);
		spin_lock(&gcma->lock);

		/* Avoid allocation from other threads */
		set_gpage_flag(page, GF_RECLAIMING);

		/*
		 * The page is in LRU and being used by someone. Discard it
		 * after removing from lru_list.
		 */
		if (gpage_flag(page, GF_LRU)) {
			entry = dmem_entry(page);
			if (atomic_inc_not_zero(&entry->refcount)) {
				clear_gpage_flag(page, GF_LRU);
				list_move(&page->lru, &free_pages);
				goto next_page;
			}
		}

		/*
		 * The page is
		 * 1) allocated by others but not yet in LRU in case of
		 *    dmem_store or
		 * 2) deleted from LRU but not yet from gcma's bitmap in case
		 *    of dmem_invalidate or dmem_evict_lru.
		 * Anycase, the race is small so retry after a while will see
		 * success. Below isolate_interrupted handles it.
		 */
next_page:
		spin_unlock(&gcma->lock);
		spin_unlock(lru_lock);
	}

	/*
	 * Since we increased refcount of the page above, we can access
	 * dmem_entry with safe.
	 */
	list_for_each_entry_safe(page, n, &free_pages, lru) {
		buck = dmem_hashbuck(page);
		entry = dmem_entry(page);
		lru_lock = &dmem_hashbuck(page)->dmem->lru_lock;

		spin_lock(&buck->lock);
		spin_lock(lru_lock);
		/* drop refcount increased by above loop */
		memcpy(&key, entry->key, dmem_hashbuck(page)->dmem->bytes_key);
		dmem_put(buck, entry);
		/* free entry if the entry is still in tree */
		if (dmem_search_entry(buck, &key))
			dmem_put(buck, entry);
		spin_unlock(lru_lock);
		spin_unlock(&buck->lock);
	}

	start_pfn = isolate_interrupted(gcma, orig_start, orig_start + size);
	if (start_pfn)
		goto retry;

	return 0;
}

/*
 * gcma_free_contig - free allocated contiguous pages
 *
 * @start_pfn	start pfn of freeing contiguous memory area
 * @size	number of pages in freeing contiguous memory area
 */
void gcma_free_contig(struct gcma *gcma,
			unsigned long start_pfn, unsigned long size)
{
	unsigned long offset;

	spin_lock(&gcma->lock);
	offset = start_pfn - gcma->base_pfn;
	bitmap_clear(gcma->bitmap, offset, size);
	spin_unlock(&gcma->lock);
}

static int __init init_gcma(void)
{
	pr_info("loading gcma\n");

	dmem_entry_cache = KMEM_CACHE(dmem_entry, 0);
	if (dmem_entry_cache == NULL)
		return -ENOMEM;

	return 0;
}

module_init(init_gcma);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Minchan Kim <minchan@kernel.org>");
MODULE_AUTHOR("SeongJae Park <sj38.park@gmail.com>");
MODULE_DESCRIPTION("Guaranteed Contiguous Memory Allocator");
