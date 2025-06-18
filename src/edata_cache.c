#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

/*
nanya

## What is `pa_shard->edata_cache` and when is it used?

### **`edata_cache_t` Purpose**
The `edata_cache` is a **cache of `edata_t` structures** (extent metadata objects) that are used to track memory extents.

### **What it contains:**
```c
typedef struct edata_cache_s edata_cache_t;
struct edata_cache_s {
    edata_avail_t avail;        // Available edata_t objects
    atomic_zu_t count;          // Count of cached edata_t objects
    malloc_mutex_t mtx;         // Mutex for thread safety
    base_t *base;              // Base allocator for new edata_t objects
};
```

### **When it's used:**

1. **During extent allocation:**
   - When the page allocator needs to track a new extent, it gets an `edata_t` from the cache.
   - If the cache is empty, it allocates a new `edata_t` from the base allocator.

2. **During extent deallocation:**
   - When an extent is freed, its `edata_t` is returned to the cache for reuse.
   - This avoids frequent allocation/deallocation of metadata structures.

3. **Performance optimization:**
   - Caching `edata_t` objects reduces the overhead of metadata allocation.
   - Provides a fast path for getting extent metadata without going through the base allocator.

### **Usage pattern:**
```c
// Get an edata_t for tracking a new extent
edata_t *edata = edata_cache_get(tsdn, &shard->edata_cache);

// Use edata_t to track the extent...

// Return edata_t to cache when extent is freed
edata_cache_put(tsdn, &shard->edata_cache, edata);


Based on our discussion, here's the updated diagram showing the key differences between base allocator tracked blocks and pa_shard tracked extents:

```
Base Allocator (base_t) - METADATA ALLOCATIONS
├── blocks list → [block_t1] → [block_t2] → [block_t3] → ...
│   └── Each block contains:
│       ├── base_block_t header
│       ├── edata_t (describing available memory in this block)
│       └── Available memory region containing:
│           ├── arena_t structures
│           ├── bin_t structures  
│           ├── edata_t objects (for tracking user extents) ← REUSABLE METADATA
│           └── Other jemalloc metadata
│
pa_shard->edata_cache - USER EXTENT TRACKING
├── avail list → [edata_t1] → [edata_t2] → [edata_t3] → ...
│   └── These edata_t objects:
│       ├── Originally allocated from base allocator (metadata)
│       ├── Cached for reuse to track different user extents
│       ├── When in use: track user memory extents (from mmap)
│       └── When cached: just metadata containers waiting for reuse
│
User Memory Extents (from mmap) - ACTUAL USER DATA
├── [User Memory Region 1] ← tracked by edata_t from cache
├── [User Memory Region 2] ← tracked by edata_t from cache  
├── [User Memory Region 3] ← tracked by edata_t from cache
└── ... (allocated via extent_alloc_wrapper → ehooks_alloc → mmap)
```

## **Key Differences:**

### **Base Allocator Blocks:**
- **Purpose**: Store jemalloc's internal metadata
- **Memory source**: Allocated via base allocator's own mmap calls
- **Content**: Arena structures, bin structures, reusable edata_t objects
- **Lifecycle**: Long-lived, managed by base allocator

### **pa_shard edata_cache:**
- **Purpose**: Cache reusable edata_t objects for tracking user extents
- **Source**: edata_t objects originally allocated from base allocator
- **Function**: Reusable metadata containers that get associated with user memory
- **Lifecycle**: Reused repeatedly to track different user extents

### **User Memory Extents:**
- **Purpose**: Store actual user data (malloc/free requests)
- **Memory source**: Allocated via mmap in extent_alloc_wrapper
- **Tracking**: Each user extent is tracked by an edata_t from the cache
- **Lifecycle**: Created/destroyed based on user allocation patterns

## **The Connection:**
The `edata_t` objects in `pa_shard->edata_cache` are the **bridge** between metadata management (base allocator) and user memory tracking (page allocator). They start as metadata allocations from the base allocator but are reused to track user memory extents.

The complete flow for when pa_shard tracked edata_cache are associated with extents holding user memory:

1. User requests allocation
   ↓
2. pa_alloc() → pac_alloc_impl() → extent_alloc_wrapper()
   ↓
3. edata_cache_get() → gets edata_t from cache (base allocator)
   ↓
4. ehooks_alloc() → mmap() → gets user memory
   ↓
5. edata_init() → ASSOCIATES edata_t with user memory
   ↓
6. extent_register() → registers the extent
   ↓
7. Return edata_t (now tracking user memory)


*/
bool
edata_cache_init(edata_cache_t *edata_cache, base_t *base) {
	edata_avail_new(&edata_cache->avail);
	/*
	 * This is not strictly necessary, since the edata_cache_t is only
	 * created inside an arena, which is zeroed on creation.  But this is
	 * handy as a safety measure.
	 */
	atomic_store_zu(&edata_cache->count, 0, ATOMIC_RELAXED);
	if (malloc_mutex_init(&edata_cache->mtx, "edata_cache",
	    WITNESS_RANK_EDATA_CACHE, malloc_mutex_rank_exclusive)) {
		return true;
	}
	edata_cache->base = base;
	return false;
}

edata_t *
edata_cache_get(tsdn_t *tsdn, edata_cache_t *edata_cache) {
	malloc_mutex_lock(tsdn, &edata_cache->mtx);
	edata_t *edata = edata_avail_first(&edata_cache->avail);
	if (edata == NULL) {
		malloc_mutex_unlock(tsdn, &edata_cache->mtx);
		return base_alloc_edata(tsdn, edata_cache->base);
	}
	edata_avail_remove(&edata_cache->avail, edata);
	atomic_load_sub_store_zu(&edata_cache->count, 1);
	malloc_mutex_unlock(tsdn, &edata_cache->mtx);
	return edata;
}

void
edata_cache_put(tsdn_t *tsdn, edata_cache_t *edata_cache, edata_t *edata) {
	malloc_mutex_lock(tsdn, &edata_cache->mtx);
	edata_avail_insert(&edata_cache->avail, edata);
	atomic_load_add_store_zu(&edata_cache->count, 1);
	malloc_mutex_unlock(tsdn, &edata_cache->mtx);
}

void
edata_cache_prefork(tsdn_t *tsdn, edata_cache_t *edata_cache) {
	malloc_mutex_prefork(tsdn, &edata_cache->mtx);
}

void
edata_cache_postfork_parent(tsdn_t *tsdn, edata_cache_t *edata_cache) {
	malloc_mutex_postfork_parent(tsdn, &edata_cache->mtx);
}

void
edata_cache_postfork_child(tsdn_t *tsdn, edata_cache_t *edata_cache) {
	malloc_mutex_postfork_child(tsdn, &edata_cache->mtx);
}

void
edata_cache_fast_init(edata_cache_fast_t *ecs, edata_cache_t *fallback) {
	edata_list_inactive_init(&ecs->list);
	ecs->fallback = fallback;
	ecs->disabled = false;
}

static void
edata_cache_fast_try_fill_from_fallback(tsdn_t *tsdn,
    edata_cache_fast_t *ecs) {
	edata_t *edata;
	malloc_mutex_lock(tsdn, &ecs->fallback->mtx);
	for (int i = 0; i < EDATA_CACHE_FAST_FILL; i++) {
		edata = edata_avail_remove_first(&ecs->fallback->avail);
		if (edata == NULL) {
			break;
		}
		edata_list_inactive_append(&ecs->list, edata);
		atomic_load_sub_store_zu(&ecs->fallback->count, 1);
	}
	malloc_mutex_unlock(tsdn, &ecs->fallback->mtx);
}

edata_t *
edata_cache_fast_get(tsdn_t *tsdn, edata_cache_fast_t *ecs) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_EDATA_CACHE, 0);

	if (ecs->disabled) {
		assert(edata_list_inactive_first(&ecs->list) == NULL);
		return edata_cache_get(tsdn, ecs->fallback);
	}

	edata_t *edata = edata_list_inactive_first(&ecs->list);
	if (edata != NULL) {
		edata_list_inactive_remove(&ecs->list, edata);
		return edata;
	}
	/* Slow path; requires synchronization. */
	edata_cache_fast_try_fill_from_fallback(tsdn, ecs);
	edata = edata_list_inactive_first(&ecs->list);
	if (edata != NULL) {
		edata_list_inactive_remove(&ecs->list, edata);
	} else {
		/*
		 * Slowest path (fallback was also empty); allocate something
		 * new.
		 */
		edata = base_alloc_edata(tsdn, ecs->fallback->base);
	}
	return edata;
}

static void
edata_cache_fast_flush_all(tsdn_t *tsdn, edata_cache_fast_t *ecs) {
	/*
	 * You could imagine smarter cache management policies (like
	 * only flushing down to some threshold in anticipation of
	 * future get requests).  But just flushing everything provides
	 * a good opportunity to defrag too, and lets us share code between the
	 * flush and disable pathways.
	 */
	edata_t *edata;
	size_t nflushed = 0;
	malloc_mutex_lock(tsdn, &ecs->fallback->mtx);
	while ((edata = edata_list_inactive_first(&ecs->list)) != NULL) {
		edata_list_inactive_remove(&ecs->list, edata);
		edata_avail_insert(&ecs->fallback->avail, edata);
		nflushed++;
	}
	atomic_load_add_store_zu(&ecs->fallback->count, nflushed);
	malloc_mutex_unlock(tsdn, &ecs->fallback->mtx);
}

void
edata_cache_fast_put(tsdn_t *tsdn, edata_cache_fast_t *ecs, edata_t *edata) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_EDATA_CACHE, 0);

	if (ecs->disabled) {
		assert(edata_list_inactive_first(&ecs->list) == NULL);
		edata_cache_put(tsdn, ecs->fallback, edata);
		return;
	}

	/*
	 * Prepend rather than append, to do LIFO ordering in the hopes of some
	 * cache locality.
	 */
	edata_list_inactive_prepend(&ecs->list, edata);
}

void
edata_cache_fast_disable(tsdn_t *tsdn, edata_cache_fast_t *ecs) {
	edata_cache_fast_flush_all(tsdn, ecs);
	ecs->disabled = true;
}
