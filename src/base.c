#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/extent_mmap.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/sz.h"

/*
 * In auto mode, arenas switch to huge pages for the base allocator on the
 * second base block.  a0 switches to thp on the 5th block (after 20 megabytes
 * of metadata), since more metadata (e.g. rtree nodes) come from a0's base.
 */

#define BASE_AUTO_THP_THRESHOLD    2
#define BASE_AUTO_THP_THRESHOLD_A0 5

/******************************************************************************/
/* Data. */

// nanya: b0 is the global base allocator instance in jemalloc. Let me explain its significance:
// It's the primary base allocator used for metadata allocations
// Used to allocate memory for internal jemalloc structures
// Created during jemalloc's boot process
// Uses default extent hooks
// Has index 0 (first arena)
// Used for metadata allocations
/*
Used to allocate memory for:
Internal metadata structures
Arena structures
Cache structures
Other jemalloc internal components
*/
static base_t *b0;

metadata_thp_mode_t opt_metadata_thp = METADATA_THP_DEFAULT;

const char *const metadata_thp_mode_names[] = {
	"disabled",
	"auto",
	"always"
};

/******************************************************************************/

static inline bool
metadata_thp_madvise(void) {
	return (metadata_thp_enabled() &&
	    (init_system_thp_mode == thp_mode_default));
}

static void *
base_map(tsdn_t *tsdn, ehooks_t *ehooks, unsigned ind, size_t size) {
	void *addr;
	bool zero = true;
	bool commit = true;

	/*
	 * Use huge page sizes and alignment when opt_metadata_thp is enabled
	 * or auto.
	 */
	size_t alignment;
	if (opt_metadata_thp == metadata_thp_disabled) {
		alignment = BASE_BLOCK_MIN_ALIGN;
	} else {
		assert(size == HUGEPAGE_CEILING(size));
		alignment = HUGEPAGE;
	}
	if (ehooks_are_default(ehooks)) {
		addr = extent_alloc_mmap(NULL, size, alignment, &zero, &commit);
		if (have_madvise_huge && addr) {
			pages_set_thp_state(addr, size);
		}
	} else {
		addr = ehooks_alloc(tsdn, ehooks, NULL, size, alignment, &zero,
		    &commit);
	}

	return addr;
}

static void
base_unmap(tsdn_t *tsdn, ehooks_t *ehooks, unsigned ind, void *addr,
    size_t size) {
	/*
	 * Cascade through dalloc, decommit, purge_forced, and purge_lazy,
	 * stopping at first success.  This cascade is performed for consistency
	 * with the cascade in extent_dalloc_wrapper() because an application's
	 * custom hooks may not support e.g. dalloc.  This function is only ever
	 * called as a side effect of arena destruction, so although it might
	 * seem pointless to do anything besides dalloc here, the application
	 * may in fact want the end state of all associated virtual memory to be
	 * in some consistent-but-allocated state.
	 */
	if (ehooks_are_default(ehooks)) {
		if (!extent_dalloc_mmap(addr, size)) {
			goto label_done;
		}
		if (!pages_decommit(addr, size)) {
			goto label_done;
		}
		if (!pages_purge_forced(addr, size)) {
			goto label_done;
		}
		if (!pages_purge_lazy(addr, size)) {
			goto label_done;
		}
		/* Nothing worked.  This should never happen. */
		not_reached();
	} else {
		if (!ehooks_dalloc(tsdn, ehooks, addr, size, true)) {
			goto label_done;
		}
		if (!ehooks_decommit(tsdn, ehooks, addr, size, 0, size)) {
			goto label_done;
		}
		if (!ehooks_purge_forced(tsdn, ehooks, addr, size, 0, size)) {
			goto label_done;
		}
		if (!ehooks_purge_lazy(tsdn, ehooks, addr, size, 0, size)) {
			goto label_done;
		}
		/* Nothing worked.  That's the application's problem. */
	}
label_done:
	if (metadata_thp_madvise()) {
		/* Set NOHUGEPAGE after unmap to avoid kernel defrag. */
		assert(((uintptr_t)addr & HUGEPAGE_MASK) == 0 &&
		    (size & HUGEPAGE_MASK) == 0);
		pages_nohuge(addr, size);
	}
}

static inline bool
base_edata_is_reused(edata_t *edata) {
	/*
	 * Borrow the guarded bit to indicate if the extent is a recycled one,
	 * i.e. the ones returned to base for reuse; currently only tcache bin
	 * stacks.  Skips stats updating if so (needed for this purpose only).
	 */
	return edata_guarded_get(edata);
}

// nanya: initialize the given extent
static void
base_edata_init(size_t *extent_sn_next, edata_t *edata, void *addr,
    size_t size) {
	size_t sn;

	sn = *extent_sn_next;
	(*extent_sn_next)++;

	edata_binit(edata, addr, size, sn, false /* is_reused */);
}

static size_t
base_get_num_blocks(base_t *base, bool with_new_block) {
	base_block_t *b = base->blocks;
	assert(b != NULL);

	size_t n_blocks = with_new_block ? 2 : 1;
	while (b->next != NULL) {
		n_blocks++;
		b = b->next;
	}

	return n_blocks;
}

static void
base_auto_thp_switch(tsdn_t *tsdn, base_t *base) {
	assert(opt_metadata_thp == metadata_thp_auto);
	malloc_mutex_assert_owner(tsdn, &base->mtx);
	if (base->auto_thp_switched) {
		return;
	}
	/* Called when adding a new block. */
	bool should_switch;
	if (base_ind_get(base) != 0) {
		should_switch = (base_get_num_blocks(base, true) ==
		    BASE_AUTO_THP_THRESHOLD);
	} else {
		should_switch = (base_get_num_blocks(base, true) ==
		    BASE_AUTO_THP_THRESHOLD_A0);
	}
	if (!should_switch) {
		return;
	}

	base->auto_thp_switched = true;
	assert(!config_stats || base->n_thp == 0);
	/* Make the initial blocks THP lazily. */
	base_block_t *block = base->blocks;
	while (block != NULL) {
		assert((block->size & HUGEPAGE_MASK) == 0);
		pages_huge(block, block->size);
		if (config_stats) {
			base->n_thp += HUGEPAGE_CEILING(block->size -
			    edata_bsize_get(&block->edata)) >> LG_HUGEPAGE;
		}
		block = block->next;
		assert(block == NULL || (base_ind_get(base) == 0));
	}
}

// nanya:  This function is a helper function used in jemalloc's base allocator to handle memory allocation within an extent.
// It performs bump allocation within an extent (a contiguous block of memory)
/*
This function does "bump allocation" within an extent(edata_t).
This implies that the given extent is managed by a base allocator, e.g. base->blocks 
Note that the global base allocator b0 itself is allocated within an (first ever) extent

1. Does edata->e_addr always point to the next available/free address within the extent?
Not always.
edata->e_addr (accessed via edata_addr_get(edata)) is a pointer to the base address of the memory region managed by this edata_t structure.
In the context of the base allocator (for metadata allocations), e_addr is often updated to point to the next available address within the block, so it can act as a "bump pointer."
In the context of general extents (user allocations, slabs, etc.), e_addr is the start of the extent, not necessarily the next free address.
Summary:
For the base allocator: e_addr can be a moving pointer, tracking the next free address.
For general extents: e_addr is the base of the extent, not the next free address.

2. Does edata->e_bsize always store the remaining available space size within the extent?
Not always.
edata->e_bsize (accessed via edata_bsize_get(edata)) is the size of the memory region that this edata_t currently manages.
For the base allocator, it can represent the remaining available space in the block (after allocations).
For general extents, it represents the total size of the extent, not just the free space.
Summary:
For the base allocator: e_bsize can be the remaining space.

*/
static void *
base_extent_bump_alloc_helper(edata_t *edata, size_t *gap_size, size_t size,
    size_t alignment) {
	void *ret;

	assert(alignment == ALIGNMENT_CEILING(alignment, QUANTUM));
	assert(size == ALIGNMENT_CEILING(size, alignment));

	*gap_size = ALIGNMENT_CEILING((uintptr_t)edata_addr_get(edata),
	    alignment) - (uintptr_t)edata_addr_get(edata);
	ret = (void *)((byte_t *)edata_addr_get(edata) + *gap_size);
	assert(edata_bsize_get(edata) >= *gap_size + size);
	// Updates the extent's metadata to reflect the new allocation
	// Adjusts the extent's base address and size
	// so edata_addr_get always point to the next avaialble address in the extent
	edata_binit(edata, (void *)((byte_t *)edata_addr_get(edata) +
	    *gap_size + size), edata_bsize_get(edata) - *gap_size - size,
	    edata_sn_get(edata), base_edata_is_reused(edata));
	return ret;
}

static void
base_edata_heap_insert(tsdn_t *tsdn, base_t *base, edata_t *edata) {
	malloc_mutex_assert_owner(tsdn, &base->mtx);

	size_t bsize = edata_bsize_get(edata);
	assert(bsize > 0);
	/*
	 * Compute the index for the largest size class that does not exceed
	 * extent's size.
	 */
	szind_t index_floor = sz_size2index(bsize + 1) - 1;
	edata_heap_insert(&base->avail[index_floor], edata);
}

/*
 * Only can be called by top-level functions, since it may call base_alloc
 * internally when cache is empty.
 */
static edata_t *
base_alloc_base_edata(tsdn_t *tsdn, base_t *base) {
	edata_t *edata;

	malloc_mutex_lock(tsdn, &base->mtx);
	edata = edata_avail_first(&base->edata_avail);
	if (edata != NULL) {
		edata_avail_remove(&base->edata_avail, edata);
	}
	malloc_mutex_unlock(tsdn, &base->mtx);

	if (edata == NULL) {
		edata = base_alloc_edata(tsdn, base);
	}

	return edata;
}

/*
This function handles post-allocation tasks after a bump allocation in the base allocator
It manages the remaining space in the extent and updates statistics

This function is claled, when the given size has just been allocated by the given base allocator
from within the given extent. (edata_t)
This function inserts the given extent into either available per-size-class list, or full but reusuable list
*/
static void
base_extent_bump_alloc_post(tsdn_t *tsdn, base_t *base, edata_t *edata,
    size_t gap_size, void *addr, size_t size) {
	if (edata_bsize_get(edata) > 0) {
		// if the extent still has space after the bump allocation, insert it into base allocator's avail array,
		// specifically in the slot indexed corresponding to the biggest size class that is smaller than remainning space inside extent
		base_edata_heap_insert(tsdn, base, edata);
	} else {
		/* Freed base edata_t stored in edata_avail. */
		/*
		nanya:
		edata_t is specifically for reusing the metadata structures themselves, not the memory they represent
		When an extent is fully used (no available space), we can reuse its edata_t structure for future allocations
		exmaple:
		1. Allocate 4KB extent
		2. Use all 4KB
		3. Instead of discarding the edata_t:
			- Store it in edata_avail
		4. Next time we need an edata_t:
			- Reuse from edata_avail instead of allocating new one
		*/
		edata_avail_insert(&base->edata_avail, edata);
	}

	if (config_stats && !base_edata_is_reused(edata)) {
		base->allocated += size;
		/*
		 * Add one PAGE to base_resident for every page boundary that is
		 * crossed by the new allocation. Adjust n_thp similarly when
		 * metadata_thp is enabled.
		 */
		base->resident += PAGE_CEILING((uintptr_t)addr + size) -
		    PAGE_CEILING((uintptr_t)addr - gap_size);
		assert(base->allocated <= base->resident);
		assert(base->resident <= base->mapped);
		if (metadata_thp_madvise() && (opt_metadata_thp ==
		    metadata_thp_always || base->auto_thp_switched)) {
			base->n_thp += (HUGEPAGE_CEILING((uintptr_t)addr + size)
			    - HUGEPAGE_CEILING((uintptr_t)addr - gap_size)) >>
			    LG_HUGEPAGE;
			assert(base->mapped >= base->n_thp << LG_HUGEPAGE);
		}
	}
}

static void *
base_extent_bump_alloc(tsdn_t *tsdn, base_t *base, edata_t *edata, size_t size,
    size_t alignment) {
	void *ret;
	size_t gap_size;

	ret = base_extent_bump_alloc_helper(edata, &gap_size, size, alignment);
	base_extent_bump_alloc_post(tsdn, base, edata, gap_size, ret, size);
	return ret;
}

static size_t
base_block_size_ceil(size_t block_size) {
	return opt_metadata_thp == metadata_thp_disabled ?
	    ALIGNMENT_CEILING(block_size, BASE_BLOCK_MIN_ALIGN) :
	    HUGEPAGE_CEILING(block_size);
}

/*
 * Allocate a block of virtual memory that is large enough to start with a
 * base_block_t header, followed by an object of specified size and alignment.
 * On success a pointer to the initialized base_block_t header is returned.
 * 
 * example layout of an block_t allocated by this function
 * 
 * Base Block (block_t)
+------------------------+
| base_block_t header    |
|   +----------------+   |
|   | size          |   | <- Total size of the block
|   | next          |   |    (including header)
|   | edata_t       |   |
|   |   - e_addr    |   | <- Points to available memory
|   |   - e_bits    |   |    e_addr = (void *)((byte_t *)block + 
|   |   - e_size_esn|   |              sizeof(base_block_t))
|   |   - e_bsize   |   | <- Size of available memory
|   |   - etc.      |   |    e_bsize = block->size - 
|   +----------------+   |              sizeof(base_block_t)
+------------------------+
           |
           v
+------------------------+
| Available Memory      |  <- Memory region for
|   +----------------+  |    metadata allocations
|   | Metadata       |  |    (size = e_bsize)
|   | allocations    |  |
|   | ...            |  |
|   +----------------+  |
+------------------------+

tsdn - only used to fetch arean to allocate from DSS, jemalloc doesn't use DSS by default(use mmap), so not used.
base - not used, unless transparent hugh page used as "primary" (THP). THP by default is set to "secondary" (DSS_PREC_DEFAULT dss_prec_secondary), 
    meaning it will be used as a fallback after mmap
ind - not used
pind_last - used to remember the page-multiple size used to allocate the last block, thus allocate the next large size

 */
static base_block_t *
base_block_alloc(tsdn_t *tsdn, base_t *base, ehooks_t *ehooks, unsigned ind,
    pszind_t *pind_last, size_t *extent_sn_next, size_t size,
    size_t alignment) {
	alignment = ALIGNMENT_CEILING(alignment, QUANTUM);
	size_t usize = ALIGNMENT_CEILING(size, alignment);
	size_t header_size = sizeof(base_block_t);
	size_t gap_size = ALIGNMENT_CEILING(header_size, alignment) -
	    header_size;
	/*
	 * Create increasingly larger blocks in order to limit the total number
	 * of disjoint virtual memory ranges.  Choose the next size in the page
	 * size class series (skipping size classes that are not a multiple of
	 * HUGEPAGE when using metadata_thp), or a size large enough to satisfy
	 * the requested size and alignment, whichever is larger.
	 */
	// header_size + gap_size is aligned space needef for base_block_t header
	// usize is the actual space requested by called
	size_t min_block_size = base_block_size_ceil(sz_psz2u(header_size +
	    gap_size + usize));
	pszind_t pind_next = (*pind_last + 1 < sz_psz2ind(SC_LARGE_MAXCLASS)) ?
	    *pind_last + 1 : *pind_last;
	size_t next_block_size = base_block_size_ceil(sz_pind2sz(pind_next));
	size_t block_size = (min_block_size > next_block_size) ? min_block_size
	    : next_block_size;
	
	// actual allocation
	// tsdn is TSD_NULL
	// ehooks is fake ehooks
	// ind is 0
	// block_size is 'size' input, requested space by caller
	base_block_t *block = (base_block_t *)base_map(tsdn, ehooks, ind,
	    block_size);
	if (block == NULL) {
		return NULL;
	}

	if (metadata_thp_madvise()) {
		void *addr = (void *)block;
		assert(((uintptr_t)addr & HUGEPAGE_MASK) == 0 &&
		    (block_size & HUGEPAGE_MASK) == 0);
		if (opt_metadata_thp == metadata_thp_always) {
			pages_huge(addr, block_size);
		} else if (opt_metadata_thp == metadata_thp_auto &&
		    base != NULL) {
			/* base != NULL indicates this is not a new base. */
			malloc_mutex_lock(tsdn, &base->mtx);
			base_auto_thp_switch(tsdn, base);
			if (base->auto_thp_switched) {
				pages_huge(addr, block_size);
			}
			malloc_mutex_unlock(tsdn, &base->mtx);
		}
	}

	*pind_last = sz_psz2ind(block_size);
	block->size = block_size;
	block->next = NULL;
	assert(block_size >= header_size);
	// initialize extent within this just-allocated base_block
	// base_block layour: base_block_t header + actual extent
	base_edata_init(extent_sn_next, &block->edata,
	    (void *)((byte_t *)block + header_size), block_size - header_size);
	return block;
}

/*
 * Allocate an extent that is at least as large as specified size, with
 * specified alignment.
 */
static edata_t *
base_extent_alloc(tsdn_t *tsdn, base_t *base, size_t size, size_t alignment) {
	malloc_mutex_assert_owner(tsdn, &base->mtx);

	ehooks_t *ehooks = base_ehooks_get_for_metadata(base);
	/*
	 * Drop mutex during base_block_alloc(), because an extent hook will be
	 * called.
	 */
	malloc_mutex_unlock(tsdn, &base->mtx);
	base_block_t *block = base_block_alloc(tsdn, base, ehooks,
	    base_ind_get(base), &base->pind_last, &base->extent_sn_next, size,
	    alignment);
	malloc_mutex_lock(tsdn, &base->mtx);
	if (block == NULL) {
		return NULL;
	}
	block->next = base->blocks;
	base->blocks = block;
	if (config_stats) {
		base->allocated += sizeof(base_block_t);
		base->resident += PAGE_CEILING(sizeof(base_block_t));
		base->mapped += block->size;
		if (metadata_thp_madvise() &&
		    !(opt_metadata_thp == metadata_thp_auto
		      && !base->auto_thp_switched)) {
			assert(base->n_thp > 0);
			base->n_thp += HUGEPAGE_CEILING(sizeof(base_block_t)) >>
			    LG_HUGEPAGE;
		}
		assert(base->allocated <= base->resident);
		assert(base->resident <= base->mapped);
		assert(base->n_thp << LG_HUGEPAGE <= base->mapped);
	}
	return &block->edata;
}

base_t *
b0get(void) {
	return b0;
}

/*
This function 
1. allocates a new block_t
2. allocate a new base allocator as the first thing in the new block_t's extent
3. store that block_t as the first item in the base allocator's block list
4. store the block_t's extent's remainnig space into the base allocator's avail per-size-class list

Visualizing the memory layout of the new block_t and the base_t and how they cross-reference each other

Base Block (block_t)       <- a block is allocated in one page-aligned contiguous piece of memory
+------------------------+    it contains a header followed by remaining memory space
| base_block_t header    |  
|   +----------------+   |  
|   | size          |    | <- Total size of the block  
|   | next          |    |    (including header) , can be used to infer the starting addr of Available Memory
|   | edata_t       |    |  
|   |   - e_addr    |    | <- Points to next available memory  
|   |   - e_bits    |    |    (shown with arrow below)  
|   |   - e_size_esn|    |  
|   |   - e_bsize   |    | <- Size of remainig available memory  
|   |   - etc.      |    |    (block size - header size - base_t size)  
|   +----------------+   |  
|                        |
|                        |  
| Available Memory       |  <- Memory region for metadata allocations. 
|   +----------------+   |     (size = base_block_t->size - header size)  
|                        |  
|   +----------------+   |  <- a base allocator is allocated as the first thing in the available memory area
|   | base_t struct  |   |     within this newly allocated block. 
|   |   - ...        |   |  
|   |   - ...        |   |  
|   |   - blocks ----+---+-----> Points back to base_block_t  
|   |   - ...        |   |       (circular reference)  
|   |   - ...        |   |  
|   |   - avail[i]---+---+-----> One size class entry points to next available address after this base_t
|   |   - mtx        |   |  
|   |   - etc.       |   |  
|   +----------------+   |  
|   | Next available |   | <- 1. edata_t->e_addr points here 
|   | memory starts  |   |    2. base_t->avail[size_class] points here (after base_t allocation)  
|   | here           |   |  
|   |                |   |  
|   | Future metadata|   |  
|   | allocations... |   |  
|   +----------------+   |  
+------------------------+ 

*/
base_t *
base_new(tsdn_t *tsdn, unsigned ind, const extent_hooks_t *extent_hooks,
    bool metadata_use_hooks) {
	pszind_t pind_last = 0;
	size_t extent_sn_next = 0;

	/*
	 * The base will contain the ehooks eventually, but it itself is
	 * allocated using them.  So we use some stack ehooks to bootstrap its
	 * memory, and then initialize the ehooks within the base_t.
	 */
	ehooks_t fake_ehooks;
	ehooks_init(&fake_ehooks, metadata_use_hooks ?
	    (extent_hooks_t *)extent_hooks :
	    (extent_hooks_t *)&ehooks_default_extent_hooks, ind);

	// allocated one base_block_t at a random new location, tsdn is not used.
	// base_block_t memory block layout: base_block_t header + extent
	// This block is large enough to hold both the base_block_t header and the base_t structure
	base_block_t *block = base_block_alloc(tsdn, NULL, &fake_ehooks, ind,
	    &pind_last, &extent_sn_next, sizeof(base_t), QUANTUM);
	if (block == NULL) {
		return NULL;
	}

	size_t gap_size;
	size_t base_alignment = CACHELINE;
	size_t base_size = ALIGNMENT_CEILING(sizeof(base_t), base_alignment);

	// so the base allocator is allocated inside the first base_block_t? yes
	// It's placed after the base_block_t header, with proper alignment
	// memory layout of this block: 
	// [base_block_t header][base_t structure][remaining space]
	base_t *base = (base_t *)base_extent_bump_alloc_helper(&block->edata,
	    &gap_size, base_size, base_alignment);
	ehooks_init(&base->ehooks, (extent_hooks_t *)extent_hooks, ind); // ehooks_default_extent_hooks, ind is 0
	ehooks_init(&base->ehooks_base, metadata_use_hooks ?
	    (extent_hooks_t *)extent_hooks :
	    (extent_hooks_t *)&ehooks_default_extent_hooks, ind); // ehooks_default_extent_hooks
	if (malloc_mutex_init(&base->mtx, "base", WITNESS_RANK_BASE,
	    malloc_mutex_rank_exclusive)) {
		base_unmap(tsdn, &fake_ehooks, ind, block, block->size);
		return NULL;
	}
	base->pind_last = pind_last; // what's this?
	base->extent_sn_next = extent_sn_next; // what's this?
	base->blocks = block; // put the first allocated base_block_t at the head of the block list in this base allocator
	base->auto_thp_switched = false;
	for (szind_t i = 0; i < SC_NSIZES; i++) {
		edata_heap_new(&base->avail[i]); // what are these?
	}
	edata_avail_new(&base->edata_avail);

	if (config_stats) {
		base->edata_allocated = 0;
		base->rtree_allocated = 0;
		base->allocated = sizeof(base_block_t);
		base->resident = PAGE_CEILING(sizeof(base_block_t));
		base->mapped = block->size;
		base->n_thp = (opt_metadata_thp == metadata_thp_always) &&
		    metadata_thp_madvise() ? HUGEPAGE_CEILING(sizeof(base_block_t))
		    >> LG_HUGEPAGE : 0;
		assert(base->allocated <= base->resident);
		assert(base->resident <= base->mapped);
		assert(base->n_thp << LG_HUGEPAGE <= base->mapped);
	}

	/* Locking here is only necessary because of assertions. */
	malloc_mutex_lock(tsdn, &base->mtx);
	base_extent_bump_alloc_post(tsdn, base, &block->edata, gap_size, base,
	    base_size);
	malloc_mutex_unlock(tsdn, &base->mtx);

	return base;
}

void
base_delete(tsdn_t *tsdn, base_t *base) {
	ehooks_t *ehooks = base_ehooks_get_for_metadata(base);
	base_block_t *next = base->blocks;
	do {
		base_block_t *block = next;
		next = block->next;
		base_unmap(tsdn, ehooks, base_ind_get(base), block,
		    block->size);
	} while (next != NULL);
}

ehooks_t *
base_ehooks_get(base_t *base) {
	return &base->ehooks;
}

ehooks_t *
base_ehooks_get_for_metadata(base_t *base) {
	return &base->ehooks_base;
}

extent_hooks_t *
base_extent_hooks_set(base_t *base, extent_hooks_t *extent_hooks) {
	extent_hooks_t *old_extent_hooks =
	    ehooks_get_extent_hooks_ptr(&base->ehooks);
	ehooks_init(&base->ehooks, extent_hooks, ehooks_ind_get(&base->ehooks));
	return old_extent_hooks;
}

static void *
base_alloc_impl(tsdn_t *tsdn, base_t *base, size_t size, size_t alignment,
    size_t *esn, size_t *ret_usize) {
	alignment = QUANTUM_CEILING(alignment);
	size_t usize = ALIGNMENT_CEILING(size, alignment);
	size_t asize = usize + alignment - QUANTUM;

	edata_t *edata = NULL;
	malloc_mutex_lock(tsdn, &base->mtx);
	// edata is an extent, like a slab
	// if there is existing slab in base, use it, otherwise, allocate a new slab/extent
	for (szind_t i = sz_size2index(asize); i < SC_NSIZES; i++) {
		edata = edata_heap_remove_first(&base->avail[i]);
		if (edata != NULL) {
			/* Use existing space. */
			break;
		}
	}
	if (edata == NULL) {
		/* Try to allocate more space. */
		edata = base_extent_alloc(tsdn, base, usize, alignment);
	}
	void *ret;
	if (edata == NULL) {
		ret = NULL;
		goto label_return;
	}

	ret = base_extent_bump_alloc(tsdn, base, edata, usize, alignment);
	if (esn != NULL) {
		*esn = (size_t)edata_sn_get(edata);
	}
	if (ret_usize != NULL) {
		*ret_usize = usize;
	}
label_return:
	malloc_mutex_unlock(tsdn, &base->mtx);
	return ret;
}

/*
 * base_alloc() returns zeroed memory, which is always demand-zeroed for the
 * auto arenas, in order to make multi-page sparse data structures such as radix
 * tree nodes efficient with respect to physical memory usage.  Upon success a
 * pointer to at least size bytes with specified alignment is returned.  Note
 * that size is rounded up to the nearest multiple of alignment to avoid false
 * sharing.
 */
/*
nanya:

## Summary of allocation functions related to base allocator

To summarize, here are the relationships:  
base_t → block_t → edata_t.  
The base allocator (`base_t`) has a linked list of blocks (`block_t`), each block contains an extent described by an `edata_t`. 
The base allocator also maintains per-size-class **heaps** of available extents (`edata_t`), not just a direct array of blocks.

---

### 1. Extent Level Allocation

- `base_extent_bump_alloc_helper`, `base_extent_bump_alloc_post`, and `base_extent_bump_alloc` operate at the extent level. They are base-specific, meaning they are used when we want to allocate space (for internal metadata) within an extent managed by a base allocator.
- `base_extent_bump_alloc_helper` updates the `edata_t` to reflect the new allocation (moves the bump pointer and reduces the available size). No actual system memory allocation occurs here.
- `base_extent_bump_alloc_post` may re-insert the extent into the base allocator’s per-size-class heap if there is still space left after the allocation.
- `base_extent_bump_alloc` is a thin wrapper that calls both `base_extent_bump_alloc_helper` and `base_extent_bump_alloc_post`.

---

### 2. Block Level Allocation

- `base_block_alloc` is the raw function that allocates a new `block_t` (a new memory mapping from the system) and initializes its extent (`edata_t`) with the right address and size. This is a real, new system allocation.
- `base_extent_alloc` is a thin wrapper around `base_block_alloc`. It calls `base_block_alloc` to allocate a new block for a given base allocator, adds the new block to the base allocator’s block list, and returns the new extent (`edata_t`) for further allocation.

---

### 3. Base Level Allocation

- `base_new` allocates a brand new base allocator. It internally allocates a new block and bump-allocates a new base inside that new block, and makes the new base manage the new block. This is a rare operation.
- `base_alloc` is the general-purpose, top-level function to allocate some space within a base allocator. It orchestrates between all the above levels:
  1. It first checks the base allocator’s per-size-class heaps for an available extent (`edata_t`) that is large enough for the requested size. If found, it pops that extent and uses it.
  2. If not, it calls `base_extent_alloc` to allocate a new block/extent within the base allocator for the requested size.
  3. Either way, it now has an available extent suitable for the requested size, and calls `base_extent_bump_alloc` to bump-allocate the requested size within the chosen extent.

---

## Detailed and Graphical Call Diagram

```
[base_alloc]
   |
   |-- checks per-size-class heaps (base->avail[]) for available extent (edata_t)
   |      |
   |      |-- if found: use it
   |      |-- if not found:
   |             |
   |             v
   |        [base_extent_alloc]
   |             |
   |             v
   |        [base_block_alloc]
   |             |
   |             v
   |        (allocates new block_t, initializes edata_t)
   |             |
   |             v
   |        (adds block to base_t's block list)
   |             |
   |             v
   |        (returns new edata_t)
   |
   v
[base_extent_bump_alloc]
   |
   |-- calls [base_extent_bump_alloc_helper] (does bump allocation: advances bump pointer, reduces available size)
   |-- calls [base_extent_bump_alloc_post] (re-inserts extent into heap if space remains)
   |
   v
(returns pointer to allocated memory)
```

### Base Allocator Creation

```
[base_new]
   |
   v
[base_block_alloc]  (allocates block for base_t)
   |
   v
[base_extent_bump_alloc_helper] (allocates base_t struct inside block)
   |
   v
[base_extent_bump_alloc_post] (inserts extent into heap if space remains)
   |
   v
(setup: base_t manages the new block)
```

### Data Structure Relationships

```
base_t
  |
  |-- blocks (linked list of block_t)
  |      |
  |      +-- block_t
  |             |
  |             +-- edata_t (describes extent in block)
  |
  |-- avail[] (per-size-class heaps of available edata_t)
```

*/
void *
base_alloc(tsdn_t *tsdn, base_t *base, size_t size, size_t alignment) {
	return base_alloc_impl(tsdn, base, size, alignment, NULL, NULL);
}

edata_t *
base_alloc_edata(tsdn_t *tsdn, base_t *base) {
	size_t esn, usize;
	edata_t *edata = base_alloc_impl(tsdn, base, sizeof(edata_t),
	    EDATA_ALIGNMENT, &esn, &usize);
	if (edata == NULL) {
		return NULL;
	}
	if (config_stats) {
		base->edata_allocated += usize;
	}
	edata_esn_set(edata, esn);
	return edata;
}

void *
base_alloc_rtree(tsdn_t *tsdn, base_t *base, size_t size) {
	size_t usize;
	void *rtree = base_alloc_impl(tsdn, base, size, CACHELINE, NULL,
	    &usize);
	if (rtree == NULL) {
		return NULL;
	}
	if (config_stats) {
		base->rtree_allocated += usize;
	}
	return rtree;
}

static inline void
b0_alloc_header_size(size_t *header_size, size_t *alignment) {
	*alignment = QUANTUM;
	*header_size = QUANTUM > sizeof(edata_t *) ? QUANTUM :
	    sizeof(edata_t *);
}

/*
 * Each piece allocated here is managed by a separate edata, because it was bump
 * allocated and cannot be merged back into the original base_block.  This means
 * it's not for general purpose: 1) they are not page aligned, nor page sized,
 * and 2) the requested size should not be too small (as each piece comes with
 * an edata_t).  Only used for tcache bin stack allocation now.
 */
void *
b0_alloc_tcache_stack(tsdn_t *tsdn, size_t stack_size) {
	base_t *base = b0get();
	edata_t *edata = base_alloc_base_edata(tsdn, base);
	if (edata == NULL) {
		return NULL;
	}

	/*
	 * Reserve room for the header, which stores a pointer to the managing
	 * edata_t.  The header itself is located right before the return
	 * address, so that edata can be retrieved on dalloc.  Bump up to usize
	 * to improve reusability -- otherwise the freed stacks will be put back
	 * into the previous size class.
	 */
	size_t esn, alignment, header_size;
	b0_alloc_header_size(&header_size, &alignment);

	size_t alloc_size = sz_s2u(stack_size + header_size);
	void *addr = base_alloc_impl(tsdn, base, alloc_size, alignment, &esn,
	    NULL);
	if (addr == NULL) {
		edata_avail_insert(&base->edata_avail, edata);
		return NULL;
	}

	/* Set is_reused: see comments in base_edata_is_reused. */
	edata_binit(edata, addr, alloc_size, esn, true /* is_reused */);
	*(edata_t **)addr = edata;

	return (byte_t *)addr + header_size;
}

void
b0_dalloc_tcache_stack(tsdn_t *tsdn, void *tcache_stack) {
	/* edata_t pointer stored in header. */
	size_t alignment, header_size;
	b0_alloc_header_size(&header_size, &alignment);

	edata_t *edata = *(edata_t **)((byte_t *)tcache_stack - header_size);
	void *addr = edata_addr_get(edata);
	size_t bsize = edata_bsize_get(edata);
	/* Marked as "reused" to avoid double counting stats. */
	assert(base_edata_is_reused(edata));
	assert(addr != NULL && bsize > 0);

	/* Zero out since base_alloc returns zeroed memory. */
	memset(addr, 0, bsize);

	base_t *base = b0get();
	malloc_mutex_lock(tsdn, &base->mtx);
	base_edata_heap_insert(tsdn, base, edata);
	malloc_mutex_unlock(tsdn, &base->mtx);
}

void
base_stats_get(tsdn_t *tsdn, base_t *base, size_t *allocated,
    size_t *edata_allocated, size_t *rtree_allocated, size_t *resident,
    size_t *mapped, size_t *n_thp) {
	cassert(config_stats);

	malloc_mutex_lock(tsdn, &base->mtx);
	assert(base->allocated <= base->resident);
	assert(base->resident <= base->mapped);
	assert(base->edata_allocated + base->rtree_allocated <= base->allocated);
	*allocated = base->allocated;
	*edata_allocated = base->edata_allocated;
	*rtree_allocated = base->rtree_allocated;
	*resident = base->resident;
	*mapped = base->mapped;
	*n_thp = base->n_thp;
	malloc_mutex_unlock(tsdn, &base->mtx);
}

void
base_prefork(tsdn_t *tsdn, base_t *base) {
	malloc_mutex_prefork(tsdn, &base->mtx);
}

void
base_postfork_parent(tsdn_t *tsdn, base_t *base) {
	malloc_mutex_postfork_parent(tsdn, &base->mtx);
}

void
base_postfork_child(tsdn_t *tsdn, base_t *base) {
	malloc_mutex_postfork_child(tsdn, &base->mtx);
}

bool
base_boot(tsdn_t *tsdn) {
	// allocate global singleton base allocator
	b0 = base_new(tsdn, 0, (extent_hooks_t *)&ehooks_default_extent_hooks,
	    /* metadata_use_hooks */ true);
	return (b0 == NULL);
}
