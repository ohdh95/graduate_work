#include "qemu/osdep.h"

#include "ftl-map-digest.h"

#define RAW_PPA_BATCH_ENTRIES 4096U

static void checksum_u32_le(GChecksum *checksum, uint32_t value)
{
    const uint32_t little_endian = GUINT32_TO_LE(value);

    g_checksum_update(checksum, (const guchar *)&little_endian,
                      sizeof(little_endian));
}

static void checksum_u64_le(GChecksum *checksum, uint64_t value)
{
    const uint64_t little_endian = GUINT64_TO_LE(value);

    g_checksum_update(checksum, (const guchar *)&little_endian,
                      sizeof(little_endian));
}

static void checksum_geometry(GChecksum *checksum,
                              const CylonFtlMapGeometry *geometry)
{
    static const char schema[] = CYLON_FTL_MAP_DIGEST_SCHEMA;

    g_checksum_update(checksum, (const guchar *)schema, sizeof(schema) - 1);
    checksum_u32_le(checksum, CYLON_FTL_MAP_DIGEST_VERSION);
    checksum_u32_le(checksum, CYLON_FTL_MAP_DIGEST_PPA_BYTES);

    checksum_u32_le(checksum, geometry->blk_bits);
    checksum_u32_le(checksum, geometry->pg_bits);
    checksum_u32_le(checksum, geometry->sec_bits);
    checksum_u32_le(checksum, geometry->pl_bits);
    checksum_u32_le(checksum, geometry->lun_bits);
    checksum_u32_le(checksum, geometry->ch_bits);
    checksum_u32_le(checksum, geometry->rsv_bits);

    checksum_u64_le(checksum, geometry->sector_bytes);
    checksum_u64_le(checksum, geometry->sectors_per_page);
    checksum_u64_le(checksum, geometry->pages_per_block);
    checksum_u64_le(checksum, geometry->blocks_per_plane);
    checksum_u64_le(checksum, geometry->planes_per_lun);
    checksum_u64_le(checksum, geometry->luns_per_channel);
    checksum_u64_le(checksum, geometry->channels);
    checksum_u64_le(checksum, geometry->lpn_count);
}

bool cylon_ftl_map_digest_compute(const CylonFtlMapGeometry *geometry,
                                  const void *entries, size_t entry_stride,
                                  size_t raw_ppa_offset,
                                  CylonFtlMapDigest *result)
{
    uint64_t little_endian_batch[RAW_PPA_BATCH_ENTRIES];
    const uint8_t *entry_bytes = entries;
    uint64_t mapped_count = 0;
    uint64_t batch_count = 0;
    GChecksum *checksum;

    if (!geometry || !result ||
        (geometry->lpn_count && !entries) ||
        entry_stride < CYLON_FTL_MAP_DIGEST_PPA_BYTES ||
        raw_ppa_offset >
            entry_stride - CYLON_FTL_MAP_DIGEST_PPA_BYTES ||
        (geometry->lpn_count &&
         geometry->lpn_count >
             (G_MAXSIZE - raw_ppa_offset) / entry_stride)) {
        return false;
    }

    checksum = g_checksum_new(G_CHECKSUM_SHA256);
    if (!checksum) {
        return false;
    }
    checksum_geometry(checksum, geometry);

    for (uint64_t lpn = 0; lpn < geometry->lpn_count; lpn++) {
        uint64_t raw_ppa;

        memcpy(&raw_ppa,
               entry_bytes + (size_t)lpn * entry_stride + raw_ppa_offset,
               sizeof(raw_ppa));
        if (raw_ppa != UINT64_MAX) {
            mapped_count++;
        }
        little_endian_batch[batch_count++] = GUINT64_TO_LE(raw_ppa);
        if (batch_count == RAW_PPA_BATCH_ENTRIES) {
            g_checksum_update(checksum, (const guchar *)little_endian_batch,
                              sizeof(little_endian_batch));
            batch_count = 0;
        }
    }
    if (batch_count) {
        g_checksum_update(checksum, (const guchar *)little_endian_batch,
                          batch_count * sizeof(little_endian_batch[0]));
    }

    g_strlcpy(result->sha256, g_checksum_get_string(checksum),
              sizeof(result->sha256));
    result->lpn_count = geometry->lpn_count;
    result->mapped_count = mapped_count;
    result->unmapped_count = geometry->lpn_count - mapped_count;
    g_checksum_free(checksum);
    return true;
}
