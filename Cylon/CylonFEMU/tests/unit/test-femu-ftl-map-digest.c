#include "qemu/osdep.h"

#include "hw/femu/bbssd/ftl-map-digest.h"

typedef struct TestMapEntry {
    uint32_t unrelated_prefix;
    uint64_t raw_ppa;
    uint8_t unrelated_suffix;
} TestMapEntry;

static const CylonFtlMapGeometry test_geometry = {
    .blk_bits = 16,
    .pg_bits = 16,
    .sec_bits = 8,
    .pl_bits = 8,
    .lun_bits = 8,
    .ch_bits = 7,
    .rsv_bits = 1,
    .sector_bytes = 512,
    .sectors_per_page = 8,
    .pages_per_block = 256,
    .blocks_per_plane = 128,
    .planes_per_lun = 1,
    .luns_per_channel = 4,
    .channels = 8,
    .lpn_count = 4,
};

static void compute(const CylonFtlMapGeometry *geometry,
                    const TestMapEntry *entries, CylonFtlMapDigest *result)
{
    g_assert(cylon_ftl_map_digest_compute(
        geometry, entries, sizeof(entries[0]),
        offsetof(TestMapEntry, raw_ppa), result));
}

static void test_known_vector_and_counts(void)
{
    const TestMapEntry entries[] = {
        { 0xaabbccdd, UINT64_MAX, 0x11 },
        { 0x12345678, UINT64_C(0), 0x22 },
        { 0xabcdef01, UINT64_C(0x0102030405060708), 0x33 },
        { 0x87654321, UINT64_MAX, 0x44 },
    };
    CylonFtlMapDigest result;

    compute(&test_geometry, entries, &result);

    /* Independently generated from the documented canonical byte stream. */
    g_assert(strcmp(result.sha256,
                    "b9fdc63ae571d652a613892e5610f02b"
                    "d6ce7cdb44090e413868eb86a197877e") == 0);
    g_assert(result.lpn_count == 4);
    g_assert(result.mapped_count == 2);
    g_assert(result.unmapped_count == 2);
}

static void test_ignores_non_mapping_bytes(void)
{
    TestMapEntry first[] = {
        { 1, UINT64_MAX, 2 },
        { 3, 0, 4 },
        { 5, UINT64_C(0x0102030405060708), 6 },
        { 7, UINT64_MAX, 8 },
    };
    TestMapEntry second[G_N_ELEMENTS(first)];
    CylonFtlMapDigest first_result;
    CylonFtlMapDigest second_result;

    memcpy(second, first, sizeof(first));
    for (size_t i = 0; i < G_N_ELEMENTS(second); i++) {
        second[i].unrelated_prefix ^= UINT32_MAX;
        second[i].unrelated_suffix ^= UINT8_MAX;
    }

    compute(&test_geometry, first, &first_result);
    compute(&test_geometry, second, &second_result);
    g_assert(strcmp(first_result.sha256, second_result.sha256) == 0);
}

static void test_mapping_and_geometry_are_committed(void)
{
    TestMapEntry entries[] = {
        { 0, UINT64_MAX, 0 },
        { 0, 0, 0 },
        { 0, UINT64_C(0x0102030405060708), 0 },
        { 0, UINT64_MAX, 0 },
    };
    CylonFtlMapGeometry changed_geometry = test_geometry;
    CylonFtlMapDigest baseline;
    CylonFtlMapDigest changed_map;
    CylonFtlMapDigest changed_geo;

    compute(&test_geometry, entries, &baseline);
    entries[2].raw_ppa++;
    compute(&test_geometry, entries, &changed_map);
    g_assert(strcmp(baseline.sha256, changed_map.sha256) != 0);

    entries[2].raw_ppa--;
    changed_geometry.channels++;
    compute(&changed_geometry, entries, &changed_geo);
    g_assert(strcmp(baseline.sha256, changed_geo.sha256) != 0);
}

static void test_rejects_invalid_layout(void)
{
    const uint64_t entries[] = { UINT64_MAX, 0, 1, UINT64_MAX };
    CylonFtlMapDigest result;

    g_assert(!cylon_ftl_map_digest_compute(
        &test_geometry, entries, sizeof(uint32_t), 0, &result));
    g_assert(!cylon_ftl_map_digest_compute(
        &test_geometry, entries, sizeof(uint64_t), 1, &result));
    g_assert(!cylon_ftl_map_digest_compute(
        NULL, entries, sizeof(uint64_t), 0, &result));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/femu/ftl-map-digest/known-vector",
                    test_known_vector_and_counts);
    g_test_add_func("/femu/ftl-map-digest/non-mapping-bytes",
                    test_ignores_non_mapping_bytes);
    g_test_add_func("/femu/ftl-map-digest/commits-map-and-geometry",
                    test_mapping_and_geometry_are_committed);
    g_test_add_func("/femu/ftl-map-digest/rejects-invalid-layout",
                    test_rejects_invalid_layout);
    return g_test_run();
}
