#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <openssl/evp.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <x86intrin.h>

#define CXL_PAGE_BYTES 4096ULL
#define CPU_CACHELINE_BYTES 64ULL
#define LINES_PER_PAGE (CXL_PAGE_BYTES / CPU_CACHELINE_BYTES)
#define DEFAULT_MAP_MIB 98304ULL
#define DEFAULT_SEED 20260728ULL
#define MIB_BYTES (1024ULL * 1024ULL)
#define MAX_POPULATE_BYTES (64ULL * 1024ULL * 1024ULL)
#define MAX_EVICT_BEFORE_MIB 4096ULL

struct options {
    const char *device;
    const char *trace_path;
    const char *run_id;
    const char *phase;
    uint64_t map_mib;
    uint64_t mmap_align_mib;
    uint64_t seed;
    uint64_t line_offset;
    uint64_t seed_line_count;
    uint64_t evict_before_mib;
    int cpu;
    bool evict_before_specified;
    bool flush_before;
    bool flush_after;
    bool populate;
    bool summary_only;
};

struct trace {
    uint64_t *lpns;
    size_t count;
    char sha256[EVP_MAX_MD_SIZE * 2 + 1];
};

struct sample {
    uint64_t ordinal;
    uint64_t lpn;
    uint64_t line;
    uint64_t latency_ns;
    uint64_t value;
    uint64_t expected;
    bool ok;
};

struct eviction_result {
    uint64_t bytes;
    uint64_t elapsed_ns;
    uint64_t checksum;
};

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "usage: %s [options] <seed|replay|map>\n"
            "\n"
            "  --device PATH       mmap target (default /dev/dax0.0)\n"
            "  --trace PATH        one LPN per line, # comments allowed\n"
            "  --map-mib N         device size for bounds checks (default 98304)\n"
            "  --mmap-align-mib N  mmap window alignment (default 2)\n"
            "  --seed N            deterministic data seed\n"
            "  --line-offset N     add N modulo 64 to the accessed line\n"
            "  --seed-line-count N initialize N consecutive lines (1-64)\n"
            "  --run-id LABEL      machine-readable run identifier\n"
            "  --phase LABEL       seed/cold/warm/custom phase label\n"
            "  --cpu N             pin to guest CPU N; -1 disables pinning\n"
            "  --evict-before-mib N\n"
            "                      replay-only anonymous CPU-cache eviction\n"
            "                      (0 disables, maximum 4096 MiB)\n"
            "  --flush-before      clflush trace lines before timing\n"
            "  --flush-after       clflush every accessed 64-byte line\n"
            "  --populate          MAP_POPULATE the minimal mmap window\n"
            "  --summary-only      omit per-access JSON samples\n",
            program);
}

static uint64_t parse_u64(const char *name, const char *text)
{
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 0);

    if (errno != 0 || *text == '\0' || *end != '\0') {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(EXIT_FAILURE);
    }
    return (uint64_t)value;
}

static int parse_cpu(const char *text)
{
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);

    if (errno != 0 || *text == '\0' || *end != '\0' ||
        value < -1 || value >= CPU_SETSIZE) {
        fprintf(stderr, "invalid cpu: %s\n", text);
        exit(EXIT_FAILURE);
    }
    return (int)value;
}

static void require_safe_label(const char *name, const char *label)
{
    if (*label == '\0') {
        fprintf(stderr, "%s cannot be empty\n", name);
        exit(EXIT_FAILURE);
    }

    for (const unsigned char *cursor = (const unsigned char *)label;
         *cursor != '\0'; ++cursor) {
        const bool safe = (*cursor >= 'a' && *cursor <= 'z') ||
                          (*cursor >= 'A' && *cursor <= 'Z') ||
                          (*cursor >= '0' && *cursor <= '9') ||
                          strchr("._:-/", *cursor) != NULL;
        if (!safe) {
            fprintf(stderr, "%s contains an unsafe character: %s\n",
                    name, label);
            exit(EXIT_FAILURE);
        }
    }
}

static uint64_t monotonic_raw_ns(void)
{
    struct timespec timestamp;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &timestamp) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)timestamp.tv_sec * 1000000000ULL +
           (uint64_t)timestamp.tv_nsec;
}

static uint64_t marker(uint64_t lpn, uint64_t line, uint64_t seed)
{
    uint64_t value = seed ^ 0x43594c4f4e444158ULL;
    value ^= lpn * 0x9e3779b97f4a7c15ULL;
    value ^= line * 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value;
}

static uint64_t eviction_marker(uint64_t cacheline)
{
    uint64_t value = cacheline ^ 0x4556494354434143ULL;
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value;
}

static bool evict_cpu_cache(size_t bytes, struct eviction_result *result)
{
    *result = (struct eviction_result){0};
    if (bytes == 0) {
        return true;
    }
    if (bytes % CPU_CACHELINE_BYTES != 0) {
        fprintf(stderr, "CPU-cache eviction size is not cacheline aligned\n");
        return false;
    }

    /*
     * This mapping is deliberately private anonymous DRAM.  This helper has
     * no DAX mapping argument, so neither pass can service a CXL access.
     */
    uint8_t *buffer = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buffer == MAP_FAILED) {
        perror("mmap anonymous CPU-cache eviction buffer");
        return false;
    }

    const size_t cachelines = bytes / CPU_CACHELINE_BYTES;
    const uint64_t start = monotonic_raw_ns();

    /* Pass one: fault in every page and write one word per cacheline. */
    for (size_t cacheline = 0; cacheline < cachelines; ++cacheline) {
        volatile uint64_t *slot = (volatile uint64_t *)(
            buffer + cacheline * CPU_CACHELINE_BYTES);
        *slot = eviction_marker((uint64_t)cacheline);
    }
    _mm_mfence();

    /*
     * Pass two: read every cacheline back.  Volatile accesses plus the
     * observable checksum prevent the compiler from deleting either pass.
     */
    uint64_t checksum = 0xcbf29ce484222325ULL;
    for (size_t cacheline = 0; cacheline < cachelines; ++cacheline) {
        volatile const uint64_t *slot = (volatile const uint64_t *)(
            buffer + cacheline * CPU_CACHELINE_BYTES);
        checksum ^= *slot;
        checksum *= 0x100000001b3ULL;
    }
    _mm_lfence();

    result->bytes = (uint64_t)bytes;
    result->elapsed_ns = monotonic_raw_ns() - start;
    result->checksum = checksum;

    if (munmap(buffer, bytes) != 0) {
        perror("munmap anonymous CPU-cache eviction buffer");
        return false;
    }
    return true;
}

static void hash_file_sha256(const char *path, char *hex)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;
    unsigned char buffer[65536];
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    FILE *stream = fopen(path, "rb");

    if (context == NULL || stream == NULL) {
        perror("open trace for SHA-256");
        EVP_MD_CTX_free(context);
        exit(EXIT_FAILURE);
    }
    if (EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) {
        fprintf(stderr, "EVP_DigestInit_ex failed\n");
        fclose(stream);
        EVP_MD_CTX_free(context);
        exit(EXIT_FAILURE);
    }

    while (!feof(stream)) {
        size_t count = fread(buffer, 1, sizeof(buffer), stream);
        if (count > 0 && EVP_DigestUpdate(context, buffer, count) != 1) {
            fprintf(stderr, "EVP_DigestUpdate failed\n");
            fclose(stream);
            EVP_MD_CTX_free(context);
            exit(EXIT_FAILURE);
        }
        if (ferror(stream)) {
            perror("read trace for SHA-256");
            fclose(stream);
            EVP_MD_CTX_free(context);
            exit(EXIT_FAILURE);
        }
    }
    if (EVP_DigestFinal_ex(context, digest, &digest_length) != 1) {
        fprintf(stderr, "EVP_DigestFinal_ex failed\n");
        fclose(stream);
        EVP_MD_CTX_free(context);
        exit(EXIT_FAILURE);
    }

    fclose(stream);
    EVP_MD_CTX_free(context);
    for (unsigned int index = 0; index < digest_length; ++index) {
        snprintf(hex + index * 2, 3, "%02x", digest[index]);
    }
    hex[digest_length * 2] = '\0';
}

static struct trace load_trace(const char *path)
{
    struct trace trace = {0};
    size_t capacity = 0;
    char *line = NULL;
    size_t line_capacity = 0;
    unsigned long line_number = 0;
    FILE *stream;

    hash_file_sha256(path, trace.sha256);
    stream = fopen(path, "r");
    if (stream == NULL) {
        perror("open trace");
        exit(EXIT_FAILURE);
    }

    while (getline(&line, &line_capacity, stream) >= 0) {
        char *cursor = line;
        char *end = NULL;
        ++line_number;

        while (*cursor == ' ' || *cursor == '\t') {
            ++cursor;
        }
        if (*cursor == '\0' || *cursor == '\n' || *cursor == '#') {
            continue;
        }

        errno = 0;
        unsigned long long value = strtoull(cursor, &end, 0);
        if (errno != 0 || cursor == end) {
            fprintf(stderr, "invalid LPN at %s:%lu\n", path, line_number);
            free(line);
            fclose(stream);
            free(trace.lpns);
            exit(EXIT_FAILURE);
        }
        while (*end == ' ' || *end == '\t' || *end == '\r') {
            ++end;
        }
        if (*end != '\0' && *end != '\n' && *end != '#') {
            fprintf(stderr, "trailing data at %s:%lu\n", path, line_number);
            free(line);
            fclose(stream);
            free(trace.lpns);
            exit(EXIT_FAILURE);
        }

        if (trace.count == capacity) {
            size_t new_capacity = capacity == 0 ? 128 : capacity * 2;
            uint64_t *new_lpns =
                realloc(trace.lpns, new_capacity * sizeof(*trace.lpns));
            if (new_lpns == NULL) {
                perror("realloc trace");
                free(line);
                fclose(stream);
                free(trace.lpns);
                exit(EXIT_FAILURE);
            }
            trace.lpns = new_lpns;
            capacity = new_capacity;
        }
        trace.lpns[trace.count++] = (uint64_t)value;
    }

    free(line);
    if (ferror(stream)) {
        perror("read trace");
        fclose(stream);
        free(trace.lpns);
        exit(EXIT_FAILURE);
    }
    fclose(stream);

    if (trace.count == 0) {
        fprintf(stderr, "trace has no LPNs: %s\n", path);
        free(trace.lpns);
        exit(EXIT_FAILURE);
    }
    return trace;
}

static int compare_u64(const void *left, const void *right)
{
    const uint64_t a = *(const uint64_t *)left;
    const uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static size_t unique_lpn_count(const struct trace *trace)
{
    uint64_t *copy = malloc(trace->count * sizeof(*copy));
    size_t unique = 0;

    if (copy == NULL) {
        perror("malloc unique LPN copy");
        exit(EXIT_FAILURE);
    }
    memcpy(copy, trace->lpns, trace->count * sizeof(*copy));
    qsort(copy, trace->count, sizeof(*copy), compare_u64);
    for (size_t index = 0; index < trace->count; ++index) {
        if (index == 0 || copy[index] != copy[index - 1]) {
            ++unique;
        }
    }
    free(copy);
    return unique;
}

static void validate_trace_bounds(const struct trace *trace,
                                  uint64_t device_bytes)
{
    const uint64_t page_count = device_bytes / CXL_PAGE_BYTES;
    for (size_t index = 0; index < trace->count; ++index) {
        if (trace->lpns[index] >= page_count) {
            fprintf(stderr,
                    "LPN %" PRIu64 " at ordinal %zu exceeds map (%" PRIu64
                    " pages)\n",
                    trace->lpns[index], index, page_count);
            exit(EXIT_FAILURE);
        }
    }
}

static void trace_mapping_window(const struct trace *trace,
                                 uint64_t device_bytes,
                                 uint64_t alignment,
                                 uint64_t *mapping_offset,
                                 uint64_t *mapping_bytes)
{
    uint64_t minimum_lpn = trace->lpns[0];
    uint64_t maximum_lpn = trace->lpns[0];

    if (alignment == 0 || alignment % CXL_PAGE_BYTES != 0) {
        fprintf(stderr, "mmap alignment must be a positive 4 KiB multiple\n");
        exit(EXIT_FAILURE);
    }
    for (size_t index = 1; index < trace->count; ++index) {
        if (trace->lpns[index] < minimum_lpn) {
            minimum_lpn = trace->lpns[index];
        }
        if (trace->lpns[index] > maximum_lpn) {
            maximum_lpn = trace->lpns[index];
        }
    }

    const uint64_t first_byte = minimum_lpn * CXL_PAGE_BYTES;
    const uint64_t last_byte = (maximum_lpn + 1) * CXL_PAGE_BYTES;
    *mapping_offset = (first_byte / alignment) * alignment;
    *mapping_bytes =
        ((last_byte + alignment - 1) / alignment) * alignment -
        *mapping_offset;

    if (*mapping_offset > device_bytes ||
        *mapping_bytes > device_bytes - *mapping_offset) {
        fprintf(stderr, "trace mmap window exceeds device bounds\n");
        exit(EXIT_FAILURE);
    }
}

static void pin_cpu(int cpu)
{
    if (cpu < 0) {
        return;
    }
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    CPU_SET(cpu, &affinity);
    if (sched_setaffinity(0, sizeof(affinity), &affinity) != 0) {
        perror("sched_setaffinity");
        exit(EXIT_FAILURE);
    }
}

static uint64_t clock_overhead_min(void)
{
    uint64_t minimum = UINT64_MAX;
    for (unsigned int sample = 0; sample < 1000; ++sample) {
        _mm_lfence();
        uint64_t start = monotonic_raw_ns();
        _mm_lfence();
        uint64_t end = monotonic_raw_ns();
        _mm_lfence();
        if (end - start < minimum) {
            minimum = end - start;
        }
    }
    return minimum;
}

static volatile uint64_t *trace_slot(uint8_t *mapping,
                                     uint64_t mapping_offset,
                                     uint64_t lpn, uint64_t ordinal,
                                     uint64_t line_offset)
{
    const uint64_t line = (ordinal + line_offset) % LINES_PER_PAGE;
    return (volatile uint64_t *)(mapping + lpn * CXL_PAGE_BYTES -
                                 mapping_offset +
                                 line * CPU_CACHELINE_BYTES);
}

static void flush_trace_lines(uint8_t *mapping, uint64_t mapping_offset,
                              const struct trace *trace,
                              uint64_t line_offset)
{
    for (size_t ordinal = 0; ordinal < trace->count; ++ordinal) {
        _mm_clflush((const void *)trace_slot(
            mapping, mapping_offset, trace->lpns[ordinal], ordinal,
            line_offset));
    }
    _mm_mfence();
}

static void print_config(const struct options *options,
                         const struct trace *trace, const char *operation,
                         uint64_t device_bytes, uint64_t mapping_offset,
                         uint64_t mapping_bytes, uint64_t clock_overhead)
{
    printf("{\"schema\":\"cylon-dax-probe/v1\",\"kind\":\"config\","
           "\"run_id\":\"%s\",\"op\":\"%s\",\"phase\":\"%s\","
           "\"device\":\"%s\",\"device_bytes\":%" PRIu64 ","
           "\"mapping_offset\":%" PRIu64 ",\"mapping_bytes\":%" PRIu64 ","
           "\"page_bytes\":%" PRIu64 ",\"cacheline_bytes\":%" PRIu64 ","
           "\"line_policy\":\"(ordinal+line_offset)-mod-64\","
           "\"line_offset\":%" PRIu64 ",\"seed_line_count\":%" PRIu64 ","
           "\"trace_sha256\":\"%s\",\"accesses\":%zu,"
           "\"unique_lpns\":%zu,\"seed\":%" PRIu64 ",\"cpu\":%d,"
           "\"evict_before_mib\":%" PRIu64 ","
           "\"evict_before_bytes\":%" PRIu64 ","
           "\"clock_overhead_min_ns\":%" PRIu64 ",\"populate\":%s,"
           "\"flush_before\":%s,\"flush_after\":%s,"
           "\"summary_only\":%s}\n",
           options->run_id, operation, options->phase, options->device,
           device_bytes, mapping_offset, mapping_bytes,
           (uint64_t)CXL_PAGE_BYTES,
           (uint64_t)CPU_CACHELINE_BYTES, options->line_offset,
           options->seed_line_count, trace->sha256,
           trace->count, unique_lpn_count(trace), options->seed, options->cpu,
           options->evict_before_mib,
           (uint64_t)(options->evict_before_mib * MIB_BYTES),
           clock_overhead, options->populate ? "true" : "false",
           options->flush_before ? "true" : "false",
           options->flush_after ? "true" : "false",
           options->summary_only ? "true" : "false");
}

static int run_seed(const struct options *options, const struct trace *trace,
                    uint8_t *mapping, uint64_t device_bytes,
                    uint64_t mapping_offset, uint64_t mapping_bytes)
{
    uint64_t checksum = 0;
    uint64_t errors = 0;
    const uint64_t start = monotonic_raw_ns();

    print_config(options, trace, "seed", device_bytes, mapping_offset,
                 mapping_bytes, 0);
    for (size_t ordinal = 0; ordinal < trace->count; ++ordinal) {
        const uint64_t lpn = trace->lpns[ordinal];
        for (uint64_t delta = 0; delta < options->seed_line_count; ++delta) {
            const uint64_t offset = options->line_offset + delta;
            const uint64_t line = (ordinal + offset) % LINES_PER_PAGE;
            const uint64_t expected = marker(lpn, line, options->seed);
            *trace_slot(mapping, mapping_offset, lpn, ordinal, offset) =
                expected;
        }
    }
    _mm_mfence();

    for (size_t ordinal = 0; ordinal < trace->count; ++ordinal) {
        const uint64_t lpn = trace->lpns[ordinal];
        for (uint64_t delta = 0; delta < options->seed_line_count; ++delta) {
            const uint64_t offset = options->line_offset + delta;
            const uint64_t line = (ordinal + offset) % LINES_PER_PAGE;
            const uint64_t expected = marker(lpn, line, options->seed);
            const uint64_t value =
                *trace_slot(mapping, mapping_offset, lpn, ordinal, offset);
            if (delta == 0) {
                checksum ^= value + ordinal * 0x9e3779b97f4a7c15ULL;
            }
            if (value != expected) {
                ++errors;
            }
        }
    }
    const uint64_t elapsed = monotonic_raw_ns() - start;

    if (options->flush_after) {
        flush_trace_lines(mapping, mapping_offset, trace,
                          options->line_offset);
    }
    printf("{\"kind\":\"summary\",\"elapsed_ns\":%" PRIu64
           ",\"accesses\":%zu,\"checksum\":\"0x%016" PRIx64
           "\",\"verify_errors\":%" PRIu64 ",\"cpu_start\":%d,"
           "\"cpu_end\":%d,\"status\":\"%s\"}\n",
           elapsed, trace->count, checksum, errors, sched_getcpu(),
           sched_getcpu(), errors == 0 ? "ok" : "error");
    return errors == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int run_map(const struct options *options, const struct trace *trace,
                   uint64_t device_bytes, uint64_t mapping_offset,
                   uint64_t mapping_bytes)
{
    const int cpu = sched_getcpu();

    /*
     * mmap() has already completed in main.  Do not dereference the mapping:
     * this operation isolates MAP_POPULATE/mapping side effects from demand
     * accesses so the caller can inspect Cylon counters afterwards.
     */
    print_config(options, trace, "map", device_bytes, mapping_offset,
                 mapping_bytes, 0);
    printf("{\"kind\":\"summary\",\"elapsed_ns\":0,\"accesses\":0,"
           "\"produced\":0,\"verify_errors\":0,\"cpu_start\":%d,"
           "\"cpu_end\":%d,\"status\":\"ok\"}\n",
           cpu, cpu);
    return EXIT_SUCCESS;
}

static uint64_t percentile(const uint64_t *sorted, size_t count,
                           uint64_t numerator, uint64_t denominator)
{
    size_t rank = (size_t)((count * numerator + denominator - 1) /
                           denominator);
    if (rank == 0) {
        rank = 1;
    }
    return sorted[rank - 1];
}

static int run_replay(const struct options *options, const struct trace *trace,
                      uint8_t *mapping, uint64_t device_bytes,
                      uint64_t mapping_offset, uint64_t mapping_bytes)
{
    struct sample *samples = options->summary_only ?
        NULL : calloc(trace->count, sizeof(*samples));
    uint64_t *encoded_next = malloc(trace->count * sizeof(*encoded_next));
    uint64_t *latencies = malloc(trace->count * sizeof(*latencies));
    uint64_t checksum = 0;
    uint64_t errors = 0;
    size_t produced = 0;
    size_t trace_index = 0;
    struct eviction_result eviction = {0};
    const uint64_t overhead = clock_overhead_min();

    if ((!options->summary_only && samples == NULL) ||
        encoded_next == NULL || latencies == NULL) {
        perror("allocate replay buffers");
        free(samples);
        free(encoded_next);
        free(latencies);
        return EXIT_FAILURE;
    }

    for (size_t ordinal = 0; ordinal < trace->count; ++ordinal) {
        const uint64_t line =
            (ordinal + options->line_offset) % LINES_PER_PAGE;
        const uint64_t expected =
            marker(trace->lpns[ordinal], line, options->seed);
        encoded_next[ordinal] = (uint64_t)(ordinal + 1) ^ expected;
    }

    if (options->flush_before) {
        flush_trace_lines(mapping, mapping_offset, trace,
                          options->line_offset);
    }
    print_config(options, trace, "replay", device_bytes, mapping_offset,
                 mapping_bytes, overhead);

    /* pin_cpu() has already run in main; keep this outside replay timing. */
    const uint64_t eviction_bytes = options->evict_before_mib * MIB_BYTES;
    if (!evict_cpu_cache((size_t)eviction_bytes, &eviction)) {
        free(samples);
        free(encoded_next);
        free(latencies);
        return EXIT_FAILURE;
    }

    const int cpu_start = sched_getcpu();
    const uint64_t total_start = monotonic_raw_ns();
    while (trace_index < trace->count && produced < trace->count) {
        const uint64_t ordinal = trace_index;
        const uint64_t lpn = trace->lpns[ordinal];
        const uint64_t line =
            (ordinal + options->line_offset) % LINES_PER_PAGE;
        const uint64_t expected = marker(lpn, line, options->seed);
        volatile uint64_t *slot =
            trace_slot(mapping, mapping_offset, lpn, ordinal,
                       options->line_offset);

        _mm_lfence();
        const uint64_t start = monotonic_raw_ns();
        _mm_lfence();
        const uint64_t value = *slot;
        _mm_lfence();
        const uint64_t end = monotonic_raw_ns();
        _mm_lfence();

        if (!options->summary_only) {
            samples[produced] = (struct sample){
                .ordinal = ordinal,
                .lpn = lpn,
                .line = line,
                .latency_ns = end - start,
                .value = value,
                .expected = expected,
                .ok = value == expected,
            };
        }
        latencies[produced] = end - start;
        checksum ^= value + ordinal * 0x9e3779b97f4a7c15ULL;
        if (value != expected) {
            ++errors;
        }
        ++produced;

        /*
         * The next local trace index cannot be computed until the DAX value
         * has arrived.  Correct data yields ordinal + 1; corrupt data safely
         * terminates at the bounds check below.
         */
        trace_index = (size_t)(encoded_next[ordinal] ^ value);
        if (trace_index > trace->count) {
            ++errors;
            break;
        }
    }
    const uint64_t total_elapsed = monotonic_raw_ns() - total_start;
    const int cpu_end = sched_getcpu();

    if (!options->summary_only) {
        for (size_t index = 0; index < produced; ++index) {
            const struct sample *sample = &samples[index];
            printf("{\"kind\":\"sample\",\"ordinal\":%" PRIu64
                   ",\"lpn\":%" PRIu64 ",\"line\":%" PRIu64
                   ",\"latency_ns\":%" PRIu64
                   ",\"value\":\"0x%016" PRIx64
                   "\",\"expected\":\"0x%016" PRIx64
                   "\",\"ok\":%s}\n",
                   sample->ordinal, sample->lpn, sample->line,
                   sample->latency_ns, sample->value, sample->expected,
                   sample->ok ? "true" : "false");
        }
    }

    qsort(latencies, produced, sizeof(*latencies), compare_u64);
    if (options->flush_after) {
        flush_trace_lines(mapping, mapping_offset, trace,
                          options->line_offset);
    }

    const bool ok = errors == 0 && produced == trace->count &&
                    trace_index == trace->count && cpu_start == cpu_end;
    printf("{\"kind\":\"summary\",\"elapsed_ns\":%" PRIu64
           ",\"accesses\":%zu,\"produced\":%zu,"
           "\"latency_ns\":{\"min\":%" PRIu64 ",\"p50\":%" PRIu64
           ",\"p95\":%" PRIu64 ",\"p99\":%" PRIu64
           ",\"max\":%" PRIu64 "},"
           "\"checksum\":\"0x%016" PRIx64
           "\",\"verify_errors\":%" PRIu64 ",\"cpu_start\":%d,"
           "\"evict_before_bytes\":%" PRIu64 ","
           "\"evict_before_elapsed_ns\":%" PRIu64 ","
           "\"evict_before_checksum\":\"0x%016" PRIx64 "\","
           "\"cpu_end\":%d,\"status\":\"%s\"}\n",
           total_elapsed, trace->count, produced, latencies[0],
           percentile(latencies, produced, 50, 100),
           percentile(latencies, produced, 95, 100),
           percentile(latencies, produced, 99, 100),
           latencies[produced - 1], checksum, errors, cpu_start,
           eviction.bytes, eviction.elapsed_ns, eviction.checksum, cpu_end,
           ok ? "ok" : "error");

    free(samples);
    free(encoded_next);
    free(latencies);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char **argv)
{
    struct options options = {
        .device = "/dev/dax0.0",
        .trace_path = NULL,
        .run_id = "unspecified",
        .phase = "unspecified",
        .map_mib = DEFAULT_MAP_MIB,
        .mmap_align_mib = 2,
        .seed = DEFAULT_SEED,
        .line_offset = 0,
        .seed_line_count = 1,
        .evict_before_mib = 0,
        .cpu = -1,
        .evict_before_specified = false,
        .flush_before = false,
        .flush_after = false,
        .populate = false,
        .summary_only = false,
    };
    static const struct option long_options[] = {
        {"device", required_argument, NULL, 'd'},
        {"trace", required_argument, NULL, 't'},
        {"map-mib", required_argument, NULL, 'm'},
        {"mmap-align-mib", required_argument, NULL, 'a'},
        {"seed", required_argument, NULL, 's'},
        {"line-offset", required_argument, NULL, 'o'},
        {"seed-line-count", required_argument, NULL, 'n'},
        {"run-id", required_argument, NULL, 'r'},
        {"phase", required_argument, NULL, 'p'},
        {"cpu", required_argument, NULL, 'c'},
        {"evict-before-mib", required_argument, NULL, 'e'},
        {"flush-before", no_argument, NULL, 'F'},
        {"flush-after", no_argument, NULL, 'f'},
        {"populate", no_argument, NULL, 'P'},
        {"summary-only", no_argument, NULL, 'q'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    int option;
    while ((option = getopt_long(argc, argv, "d:t:m:a:s:o:n:r:p:c:e:FfPqh",
                                 long_options, NULL)) != -1) {
        switch (option) {
        case 'd':
            options.device = optarg;
            break;
        case 't':
            options.trace_path = optarg;
            break;
        case 'm':
            options.map_mib = parse_u64("map-mib", optarg);
            break;
        case 'a':
            options.mmap_align_mib =
                parse_u64("mmap-align-mib", optarg);
            break;
        case 's':
            options.seed = parse_u64("seed", optarg);
            break;
        case 'o':
            options.line_offset = parse_u64("line-offset", optarg);
            break;
        case 'n':
            options.seed_line_count =
                parse_u64("seed-line-count", optarg);
            break;
        case 'r':
            options.run_id = optarg;
            break;
        case 'p':
            options.phase = optarg;
            break;
        case 'c':
            options.cpu = parse_cpu(optarg);
            break;
        case 'e':
            options.evict_before_mib =
                parse_u64("evict-before-mib", optarg);
            options.evict_before_specified = true;
            break;
        case 'F':
            options.flush_before = true;
            break;
        case 'f':
            options.flush_after = true;
            break;
        case 'P':
            options.populate = true;
            break;
        case 'q':
            options.summary_only = true;
            break;
        case 'h':
            usage(stdout, argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(stderr, argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (options.trace_path == NULL || optind + 1 != argc ||
        options.map_mib == 0 ||
        options.map_mib > UINT64_MAX / (1024ULL * 1024ULL) ||
        options.mmap_align_mib == 0 ||
        options.mmap_align_mib > UINT64_MAX / (1024ULL * 1024ULL) ||
        options.line_offset >= LINES_PER_PAGE ||
        options.seed_line_count == 0 ||
        options.seed_line_count > LINES_PER_PAGE) {
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }
    require_safe_label("run-id", options.run_id);
    require_safe_label("phase", options.phase);

    const char *operation = argv[optind];
    if (strcmp(operation, "seed") != 0 &&
        strcmp(operation, "replay") != 0 &&
        strcmp(operation, "map") != 0) {
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }
    if (strcmp(operation, "replay") != 0 &&
        options.evict_before_specified) {
        fprintf(stderr, "--evict-before-mib is valid only for replay\n");
        return EXIT_FAILURE;
    }
    if (options.evict_before_mib > UINT64_MAX / MIB_BYTES) {
        fprintf(stderr, "--evict-before-mib byte count overflows uint64\n");
        return EXIT_FAILURE;
    }
    const uint64_t evict_before_bytes =
        options.evict_before_mib * MIB_BYTES;
    if (evict_before_bytes > SIZE_MAX) {
        fprintf(stderr, "--evict-before-mib byte count exceeds size_t\n");
        return EXIT_FAILURE;
    }
    if (options.evict_before_mib > MAX_EVICT_BEFORE_MIB) {
        fprintf(stderr,
                "--evict-before-mib exceeds the 4096 MiB safety limit\n");
        return EXIT_FAILURE;
    }
    if (strcmp(operation, "seed") != 0 &&
        options.seed_line_count != 1) {
        fprintf(stderr, "--seed-line-count is valid only for seed\n");
        return EXIT_FAILURE;
    }
    if (strcmp(operation, "map") == 0 &&
        (options.flush_before || options.flush_after)) {
        fprintf(stderr, "flush options are invalid for map\n");
        return EXIT_FAILURE;
    }

    const uint64_t device_bytes = options.map_mib * 1024ULL * 1024ULL;
    const uint64_t mmap_alignment =
        options.mmap_align_mib * 1024ULL * 1024ULL;
    struct trace trace = load_trace(options.trace_path);
    validate_trace_bounds(&trace, device_bytes);
    uint64_t mapping_offset;
    uint64_t mapping_bytes;
    trace_mapping_window(&trace, device_bytes, mmap_alignment,
                         &mapping_offset, &mapping_bytes);
    if (options.populate && mapping_bytes > MAX_POPULATE_BYTES) {
        fprintf(stderr,
                "--populate refuses mmap windows larger than 64 MiB; "
                "use it only for compact PTE-calibration traces\n");
        free(trace.lpns);
        return EXIT_FAILURE;
    }
    pin_cpu(options.cpu);

    int descriptor = open(options.device, O_RDWR | O_CLOEXEC);
    if (descriptor < 0) {
        perror("open mmap target");
        free(trace.lpns);
        return EXIT_FAILURE;
    }
    int mmap_flags = MAP_SHARED;
    if (options.populate) {
        mmap_flags |= MAP_POPULATE;
    }
    uint8_t *mapping = mmap(NULL, mapping_bytes, PROT_READ | PROT_WRITE,
                            mmap_flags, descriptor, (off_t)mapping_offset);
    if (mapping == MAP_FAILED) {
        perror("mmap target");
        close(descriptor);
        free(trace.lpns);
        return EXIT_FAILURE;
    }

    int result;
    if (strcmp(operation, "seed") == 0) {
        result = run_seed(&options, &trace, mapping, device_bytes,
                          mapping_offset, mapping_bytes);
    } else if (strcmp(operation, "map") == 0) {
        result = run_map(&options, &trace, device_bytes, mapping_offset,
                         mapping_bytes);
    } else {
        result = run_replay(&options, &trace, mapping, device_bytes,
                            mapping_offset, mapping_bytes);
    }

    if (munmap(mapping, mapping_bytes) != 0) {
        perror("munmap target");
        result = EXIT_FAILURE;
    }
    close(descriptor);
    free(trace.lpns);
    return result;
}
