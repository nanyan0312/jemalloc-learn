#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"
#include "jemalloc/internal/sz.h"

JEMALLOC_ALIGNED(CACHELINE)
size_t sz_pind2sz_tab[SC_NPSIZES+1]; // size class lookup table for page-multiple size classes, The sz_pind2sz_tab only contains entries for page-aligned size classes
size_t sz_large_pad;

size_t
sz_psz_quantize_floor(size_t size) {
	size_t ret;
	pszind_t pind;

	assert(size > 0);
	assert((size & PAGE_MASK) == 0);

	pind = sz_psz2ind(size - sz_large_pad + 1);
	if (pind == 0) {
		/*
		 * Avoid underflow.  This short-circuit would also do the right
		 * thing for all sizes in the range for which there are
		 * PAGE-spaced size classes, but it's simplest to just handle
		 * the one case that would cause erroneous results.
		 */
		return size;
	}
	ret = sz_pind2sz(pind - 1) + sz_large_pad;
	assert(ret <= size);
	return ret;
}

size_t
sz_psz_quantize_ceil(size_t size) {
	size_t ret;

	assert(size > 0);
	assert(size - sz_large_pad <= SC_LARGE_MAXCLASS);
	assert((size & PAGE_MASK) == 0);

	ret = sz_psz_quantize_floor(size);
	if (ret < size) {
		/*
		 * Skip a quantization that may have an adequately large extent,
		 * because under-sized extents may be mixed in.  This only
		 * happens when an unusual size is requested, i.e. for aligned
		 * allocation, and is just one of several places where linear
		 * search would potentially find sufficiently aligned available
		 * memory somewhere lower.
		 */
		ret = sz_pind2sz(sz_psz2ind(ret - sz_large_pad + 1)) +
		    sz_large_pad;
	}
	return ret;
}

static void
sz_boot_pind2sz_tab(const sc_data_t *sc_data) {
	/*
	The first few page size classes (for x86_64 Linux) are:
	Index 0: 4096 bytes (1 page)
	Index 1: 8192 bytes (2 pages)
	Index 2: 12288 bytes (3 pages)
	Index 3: 16384 bytes (4 pages)
	*/
	int pind = 0;
	for (unsigned i = 0; i < SC_NSIZES; i++) {
		const sc_t *sc = &sc_data->sc[i];
		if (sc->psz) { // if the size class is a page-multiple size class
			sz_pind2sz_tab[pind] = (ZU(1) << sc->lg_base)
			    + (ZU(sc->ndelta) << sc->lg_delta);
			pind++;
		}
	}

	/* nanya:
	* Array Size Guarantee: The sz_pind2sz_tab array is declared with size SC_NPSIZES+1, so it needs to be fully initialized to avoid any uninitialized elements.
	* Fallback Values: The second loop sets a fallback value for any remaining indices in the array. 
	* This ensures that if someone tries to access a page index that doesn't correspond to a valid page-multiple size class, 
	* they'll get a reasonable value (the maximum large class size plus one page) rather than an uninitialized value.
	*/
	for (int i = pind; i <= (int)SC_NPSIZES; i++) {
		sz_pind2sz_tab[pind] = sc_data->large_maxclass + PAGE;
	}
}

JEMALLOC_ALIGNED(CACHELINE)
size_t sz_index2size_tab[SC_NSIZES];

static void
sz_boot_index2size_tab(const sc_data_t *sc_data) {
	for (unsigned i = 0; i < SC_NSIZES; i++) {
		const sc_t *sc = &sc_data->sc[i];
		sz_index2size_tab[i] = (ZU(1) << sc->lg_base)
		    + (ZU(sc->ndelta) << (sc->lg_delta)); // mapping from some index to the size of a size class
	}
}

/*
 * To keep this table small, we divide sizes by the tiny min size, which gives
 * the smallest interval for which the result can change.
 * 
 * nanya: SC_LOOKUP_MAXCLASS >> SC_LG_TINY_MIN is SC_LOOKUP_MAXCLASS / 2^SC_LG_TINY_MIN, so 4096 / 8 = 512
 */
JEMALLOC_ALIGNED(CACHELINE)
uint8_t sz_size2index_tab[(SC_LOOKUP_MAXCLASS >> SC_LG_TINY_MIN) + 1];

static void
sz_boot_size2index_tab(const sc_data_t *sc_data) {
	size_t dst_max = (SC_LOOKUP_MAXCLASS >> SC_LG_TINY_MIN) + 1; // 513
	size_t dst_ind = 0;
	for (unsigned sc_ind = 0; sc_ind < SC_NSIZES && dst_ind < dst_max;
	    sc_ind++) {
		const sc_t *sc = &sc_data->sc[sc_ind];
		size_t sz = (ZU(1) << sc->lg_base)
		    + (ZU(sc->ndelta) << sc->lg_delta); // suppose 8, 16, 32, 64, 80, 96, 112, 128 .. 
		size_t max_ind = ((sz + (ZU(1) << SC_LG_TINY_MIN) - 1) 
				   >> SC_LG_TINY_MIN); // (8/16/32/... + 7)/8 = 1,2,4,8,10,12,14,16....
		for (; dst_ind <= max_ind && dst_ind < dst_max; dst_ind++) {
			assert(sc_ind < 1 << (sizeof(uint8_t) * 8));
			sz_size2index_tab[dst_ind] = (uint8_t)sc_ind;
			/* nanya: this table first few entries look like:
			 * 0 -> 0
			 * 1 -> 0
			 * 2 -> 1
			 * 3 -> 2
			 * 4 -> 2
			 * 5 -> 3
			 * 6 -> 3
			 * 7 -> 3
			 * 8 -> 3
			 * ....
			 * so to use this table, we do: 
			 * 1. requested size divided by SC_LG_TINY_MIN to get the index into the table, say requests is for 24 bytes, so index is 3 into this table
			 * 2. The table entry gives the size class index to use, which is size class number 2
			 * 3. Then use the size class index to find the size class in sc_data->sc[] array. index 2 is entry # 3 which is 32 bytes
			 * 
			*/
		}
	}
}

void
sz_boot(const sc_data_t *sc_data, bool cache_oblivious) {
	sz_large_pad = cache_oblivious ? PAGE : 0;
	sz_boot_pind2sz_tab(sc_data);
	sz_boot_index2size_tab(sc_data);
	sz_boot_size2index_tab(sc_data);
}
