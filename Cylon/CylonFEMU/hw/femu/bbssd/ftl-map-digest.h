#ifndef FEMU_FTL_MAP_DIGEST_H
#define FEMU_FTL_MAP_DIGEST_H

#include <glib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CYLON_FTL_MAP_DIGEST_SCHEMA "cylon-ftl-lpn-ppa-v1"
#define CYLON_FTL_MAP_DIGEST_VERSION 1U
#define CYLON_FTL_MAP_DIGEST_PPA_BYTES 8U
#define CYLON_FTL_MAP_DIGEST_SHA256_HEX_BYTES 65U

/*
 * Canonical cylon-ftl-lpn-ppa-v1 SHA-256 byte stream
 * --------------------------------------------------
 *
 * Multi-byte integers are unsigned and little endian.  There is no native
 * structure padding in the stream.
 *
 *   ASCII "cylon-ftl-lpn-ppa-v1" (without a trailing NUL)
 *   u32 schema version
 *   u32 raw PPA bytes
 *   u32 BLK_BITS, PG_BITS, SEC_BITS, PL_BITS, LUN_BITS, CH_BITS, RSV_BITS
 *   u64 sector bytes, sectors/page, pages/block, blocks/plane,
 *       planes/LUN, LUNs/channel, channels, LPN count
 *   u64 raw PPA for LPN 0
 *   ...
 *   u64 raw PPA for LPN (LPN count - 1)
 *
 * The geometry makes the raw mapping-table representation unambiguous.  The
 * schema version must be changed if this serialization changes.
 */
typedef struct CylonFtlMapGeometry {
    uint32_t blk_bits;
    uint32_t pg_bits;
    uint32_t sec_bits;
    uint32_t pl_bits;
    uint32_t lun_bits;
    uint32_t ch_bits;
    uint32_t rsv_bits;

    uint64_t sector_bytes;
    uint64_t sectors_per_page;
    uint64_t pages_per_block;
    uint64_t blocks_per_plane;
    uint64_t planes_per_lun;
    uint64_t luns_per_channel;
    uint64_t channels;
    uint64_t lpn_count;
} CylonFtlMapGeometry;

typedef struct CylonFtlMapDigest {
    char sha256[CYLON_FTL_MAP_DIGEST_SHA256_HEX_BYTES];
    uint64_t lpn_count;
    uint64_t mapped_count;
    uint64_t unmapped_count;
} CylonFtlMapDigest;

/*
 * Hash raw PPA fields embedded in an arbitrary fixed-stride entry array.
 * raw_ppa_offset identifies the uint64_t raw PPA field within each entry.
 * An entry is unmapped exactly when its raw PPA is UINT64_MAX.
 */
bool cylon_ftl_map_digest_compute(const CylonFtlMapGeometry *geometry,
                                  const void *entries, size_t entry_stride,
                                  size_t raw_ppa_offset,
                                  CylonFtlMapDigest *result);

#endif
