#ifndef JEMALLOC_INTERNAL_BIN_TYPES_H
#define JEMALLOC_INTERNAL_BIN_TYPES_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/sc.h"

#define BIN_SHARDS_MAX (1 << EDATA_BITS_BINSHARD_WIDTH) // 1 << 12 = 4096, the maximum number of bin shards
#define N_BIN_SHARDS_DEFAULT 1 // 1, the default number of bin shards

/* Used in TSD static initializer only. Real init in arena_bind(). */
#define TSD_BINSHARDS_ZERO_INITIALIZER {{UINT8_MAX}} // 0xFF, the value to initialize the bin shards to

typedef struct tsd_binshards_s tsd_binshards_t;
struct tsd_binshards_s {
	uint8_t binshard[SC_NBINS];
};

#endif /* JEMALLOC_INTERNAL_BIN_TYPES_H */
