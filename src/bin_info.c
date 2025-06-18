#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/bin_info.h"

/*
 * We leave bin-batching disabled by default, with other settings chosen mostly
 * empirically; across the test programs I looked at they provided the most bang
 * for the buck.  With other default settings, these choices for bin batching
 * result in them consuming far less memory (even in the worst case) than the
 * tcaches themselves, the arena, etc.
 * Note that we always try to pop all bins on every arena cache bin lock
 * operation, so the typical memory waste is far less than this (and only on
 * hot bins, which tend to be large anyways).
 */
size_t opt_bin_info_max_batched_size = 0; /* 192 is a good default. */
size_t opt_bin_info_remote_free_max_batch = 4;
size_t opt_bin_info_remote_free_max = BIN_REMOTE_FREE_ELEMS_MAX;

// nanya: global array of info about each bin
// index corresponds to index into the global array of all size classes sc_data->sc[].
bin_info_t bin_infos[SC_NBINS];

szind_t bin_info_nbatched_sizes;
unsigned bin_info_nbatched_bins;
unsigned bin_info_nunbatched_bins;

static void
bin_infos_init(sc_data_t *sc_data, unsigned bin_shard_sizes[SC_NBINS],
    bin_info_t infos[SC_NBINS]) {
	for (unsigned i = 0; i < SC_NBINS; i++) {
		bin_info_t *bin_info = &infos[i];
		sc_t *sc = &sc_data->sc[i]; // get the size class corresponds to the bin
		bin_info->reg_size = ((size_t)1U << sc->lg_base)
		    + ((size_t)sc->ndelta << sc->lg_delta); // in each slab, each region is the size of the size class 
		// this is the size of each slab.
		// recall sc->pgs is the least common multiple of pages and size class, so say for size class 64, pgs is 1 meaning 1 page
		// only size classes < 4*PAGE are binnable, so slab sizes are mostly 1 page, 2 page, ...
		bin_info->slab_size = (sc->pgs << LG_PAGE);
		bin_info->nregs =
		    (uint32_t)(bin_info->slab_size / bin_info->reg_size);
		// where are the shards actually allocated/materialized?
		bin_info->n_shards = bin_shard_sizes[i];
		// what is this for?
		bitmap_info_t bitmap_info = BITMAP_INFO_INITIALIZER(
		    bin_info->nregs);
		bin_info->bitmap_info = bitmap_info;
		// size classes smaller than 192 bytes by default, their bins are "batched"
		// size classes larger, their bins are not batched.
		// at the moment it is not clear what's the difference.
		if (bin_info->reg_size <= opt_bin_info_max_batched_size) {
			bin_info_nbatched_sizes++; // Number of size classes that use batching, within each arean
			bin_info_nbatched_bins += bin_info->n_shards; // Total number of batched bins within each arenas
		} else {
			bin_info_nunbatched_bins += bin_info->n_shards; // // Total number of non-batched bins within each arenas
		}
	}
}

void
bin_info_boot(sc_data_t *sc_data, unsigned bin_shard_sizes[SC_NBINS]) {
	assert(sc_data->initialized);
	bin_infos_init(sc_data, bin_shard_sizes, bin_infos);
}
