#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <x86intrin.h>

#define CXL_PAGE_BYTES 4096ULL
#define CPU_CACHELINE_BYTES 64ULL
#define LINES_PER_PAGE (CXL_PAGE_BYTES / CPU_CACHELINE_BYTES)
#define MIB_BYTES (1024ULL * 1024ULL)
#define GIB_BYTES (1024.0 * 1024.0 * 1024.0)
#define DEFAULT_MAP_MIB 98304ULL
#define DEFAULT_SEED 20260728ULL
#define CHECKSUM_MIX 0x9e3779b97f4a7c15ULL

struct options {
    const char *device;
    const char *trace_path;
    const char *run_id;
    const char *phase;
    uint64_t map_mib;
    uint64_t mmap_align_mib;
    uint64_t seed;
    uint64_t line_offset;
    unsigned int threads;
    bool phase_offset;
    bool populate;
    bool flush_before;
};

struct trace {
    uint64_t *lpns;
    size_t count;
    char sha256[EVP_MAX_MD_SIZE * 2 + 1];
};

struct start_gate {
    atomic_uint ready;
    atomic_bool start;
    atomic_bool cancel;
};

struct worker {
    unsigned int id;
    int assigned_cpu;
    const struct options *options;
    const struct trace *trace;
    uint8_t *mapping;
    uint64_t mapping_offset;
    size_t shard_start;
    size_t shard_count;
    size_t initial_local_index;
    uint64_t *encoded_next;
    uint64_t *latencies;
    struct start_gate *gate;

    uint64_t start_ns;
    uint64_t end_ns;
    uint64_t checksum;
    uint64_t verify_errors;
    uint64_t migration_errors;
    size_t produced;
    size_t final_local_index;
    int cpu_start;
    int cpu_end;
    int affinity_error;
    int clock_error;
};

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "usage: %s [options] replay\n"
            "\n"
            "  --device PATH       mmap target (default /dev/dax0.0)\n"
            "  --trace PATH        one LPN per line, # comments allowed\n"
            "  --map-mib N         device size for bounds checks (default 98304)\n"
            "  --mmap-align-mib N  mmap window alignment (default 2)\n"
            "  --seed N            deterministic marker seed\n"
            "  --line-offset N     add N to global ordinal modulo 64\n"
            "  --threads N         worker count: 1, 2, 4, or 8\n"
            "  --phase-offset      rotate each shard by thread ID\n"
            "  --run-id LABEL      machine-readable run identifier\n"
            "  --phase LABEL       machine-readable phase label\n"
            "  --populate          MAP_POPULATE the minimal mmap window\n"
            "  --flush-before      clflush every target line, then mfence\n",
            program);
}

static uint64_t parse_u64(const char *name, const char *text)
{
    char *end = NULL;

    if (text[0] == '\0' || text[0] == '-' || text[0] == '+' ||
        text[0] == ' ' || text[0] == '\t' || text[0] == '\n') {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(EXIT_FAILURE);
    }

    errno = 0;
    const unsigned long long value = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(EXIT_FAILURE);
    }
    return (uint64_t)value;
}

static void require_safe_label(const char *name, const char *label)
{
    if (label[0] == '\0') {
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

static bool clock_now_ns(uint64_t *value)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC_RAW, &timestamp) != 0) {
        return false;
    }
    if ((uint64_t)timestamp.tv_sec >
        (UINT64_MAX - (uint64_t)timestamp.tv_nsec) / 1000000000ULL) {
        errno = EOVERFLOW;
        return false;
    }
    *value = (uint64_t)timestamp.tv_sec * 1000000000ULL +
             (uint64_t)timestamp.tv_nsec;
    return true;
}

static uint64_t clock_overhead_min(void)
{
    uint64_t minimum = UINT64_MAX;

    for (unsigned int sample = 0; sample < 1000; ++sample) {
        uint64_t start;
        uint64_t end;

        _mm_lfence();
        if (!clock_now_ns(&start)) {
            perror("clock_gettime");
            exit(EXIT_FAILURE);
        }
        _mm_lfence();
        if (!clock_now_ns(&end)) {
            perror("clock_gettime");
            exit(EXIT_FAILURE);
        }
        _mm_lfence();
        if (end < start) {
            fprintf(stderr, "CLOCK_MONOTONIC_RAW moved backwards\n");
            exit(EXIT_FAILURE);
        }
        if (end - start < minimum) {
            minimum = end - start;
        }
    }
    return minimum;
}

/* Keep this byte-for-byte equivalent to cylon_dax_probe.c. */
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

static void hash_file_sha256(const char *path, char *hex)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;
    unsigned char buffer[65536];
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    FILE *stream = fopen(path, "rb");

    if (context == NULL) {
        fprintf(stderr, "EVP_MD_CTX_new failed\n");
        exit(EXIT_FAILURE);
    }
    if (stream == NULL) {
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
        const size_t count = fread(buffer, 1, sizeof(buffer), stream);
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

static void free_trace_and_fail(struct trace *trace, char *line, FILE *stream,
                                const char *message, const char *path,
                                unsigned long line_number)
{
    if (line_number == 0) {
        fprintf(stderr, "%s: %s\n", message, path);
    } else {
        fprintf(stderr, "%s at %s:%lu\n", message, path, line_number);
    }
    free(line);
    fclose(stream);
    free(trace->lpns);
    exit(EXIT_FAILURE);
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
        if (*cursor == '-' || *cursor == '+') {
            free_trace_and_fail(&trace, line, stream, "invalid LPN", path,
                                line_number);
        }

        errno = 0;
        const unsigned long long value = strtoull(cursor, &end, 0);
        if (errno != 0 || cursor == end) {
            free_trace_and_fail(&trace, line, stream, "invalid LPN", path,
                                line_number);
        }
        while (*end == ' ' || *end == '\t' || *end == '\r') {
            ++end;
        }
        if (*end != '\0' && *end != '\n' && *end != '#') {
            free_trace_and_fail(&trace, line, stream, "trailing data", path,
                                line_number);
        }

        if (trace.count == capacity) {
            size_t new_capacity;

            if (capacity == 0) {
                new_capacity = 128;
            } else {
                if (capacity > SIZE_MAX / 2) {
                    free_trace_and_fail(&trace, line, stream,
                                        "trace capacity overflow", path, 0);
                }
                new_capacity = capacity * 2;
            }
            if (new_capacity > SIZE_MAX / sizeof(*trace.lpns)) {
                free_trace_and_fail(&trace, line, stream,
                                    "trace allocation overflow", path, 0);
            }
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
    uint64_t *copy;
    size_t unique = 0;

    if (trace->count > SIZE_MAX / sizeof(*copy)) {
        fprintf(stderr, "unique-LPN allocation overflow\n");
        exit(EXIT_FAILURE);
    }
    copy = malloc(trace->count * sizeof(*copy));
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
                                 uint64_t device_bytes, uint64_t alignment,
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

    if (minimum_lpn > UINT64_MAX / CXL_PAGE_BYTES ||
        maximum_lpn == UINT64_MAX ||
        maximum_lpn + 1 > UINT64_MAX / CXL_PAGE_BYTES) {
        fprintf(stderr, "trace byte-offset overflow\n");
        exit(EXIT_FAILURE);
    }
    const uint64_t first_byte = minimum_lpn * CXL_PAGE_BYTES;
    const uint64_t last_byte = (maximum_lpn + 1) * CXL_PAGE_BYTES;
    const uint64_t end_remainder = last_byte % alignment;
    uint64_t aligned_end = last_byte;

    *mapping_offset = first_byte - first_byte % alignment;
    if (end_remainder != 0) {
        const uint64_t increment = alignment - end_remainder;
        if (last_byte > UINT64_MAX - increment) {
            fprintf(stderr, "mmap end alignment overflow\n");
            exit(EXIT_FAILURE);
        }
        aligned_end += increment;
    }
    if (aligned_end < *mapping_offset) {
        fprintf(stderr, "mmap window arithmetic overflow\n");
        exit(EXIT_FAILURE);
    }
    *mapping_bytes = aligned_end - *mapping_offset;
    if (*mapping_offset > device_bytes ||
        *mapping_bytes > device_bytes - *mapping_offset) {
        fprintf(stderr, "trace mmap window exceeds device bounds\n");
        exit(EXIT_FAILURE);
    }
    if (*mapping_bytes == 0 || *mapping_bytes > SIZE_MAX ||
        *mapping_bytes > (uint64_t)PTRDIFF_MAX ||
        *mapping_offset > (uint64_t)INT64_MAX) {
        fprintf(stderr, "mmap window is not representable on this host\n");
        exit(EXIT_FAILURE);
    }
}

static volatile uint64_t *trace_slot(uint8_t *mapping,
                                     uint64_t mapping_offset, uint64_t lpn,
                                     size_t global_ordinal,
                                     uint64_t line_offset)
{
    const uint64_t line =
        ((uint64_t)(global_ordinal % LINES_PER_PAGE) + line_offset) %
        LINES_PER_PAGE;
    const uint64_t page_byte = lpn * CXL_PAGE_BYTES;
    return (volatile uint64_t *)(mapping + (page_byte - mapping_offset) +
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

static uint64_t percentile(const uint64_t *sorted, size_t count,
                           uint64_t numerator, uint64_t denominator)
{
    const size_t quotient = count / (size_t)denominator;
    const size_t remainder = count % (size_t)denominator;
    size_t rank =
        quotient * (size_t)numerator +
        (remainder * (size_t)numerator + (size_t)denominator - 1) /
            (size_t)denominator;
    if (rank == 0) {
        rank = 1;
    }
    return sorted[rank - 1];
}

static void print_json_string(const char *text)
{
    putchar('"');
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor != '\0'; ++cursor) {
        switch (*cursor) {
        case '"':
            fputs("\\\"", stdout);
            break;
        case '\\':
            fputs("\\\\", stdout);
            break;
        case '\b':
            fputs("\\b", stdout);
            break;
        case '\f':
            fputs("\\f", stdout);
            break;
        case '\n':
            fputs("\\n", stdout);
            break;
        case '\r':
            fputs("\\r", stdout);
            break;
        case '\t':
            fputs("\\t", stdout);
            break;
        default:
            if (*cursor < 0x20) {
                printf("\\u%04x", (unsigned int)*cursor);
            } else {
                putchar(*cursor);
            }
            break;
        }
    }
    putchar('"');
}

static void print_config(const struct options *options,
                         const struct trace *trace, uint64_t device_bytes,
                         uint64_t mapping_offset, uint64_t mapping_bytes,
                         uint64_t clock_overhead,
                         const struct worker *workers)
{
    printf("{\"schema\":\"cylon-dax-mt-probe/v1\",\"kind\":\"config\","
           "\"run_id\":");
    print_json_string(options->run_id);
    printf(",\"op\":\"replay\",\"phase\":");
    print_json_string(options->phase);
    printf(",\"device\":");
    print_json_string(options->device);
    printf(",\"trace\":");
    print_json_string(options->trace_path);
    printf(",\"device_bytes\":%" PRIu64 ",\"mapping_offset\":%" PRIu64
           ",\"mapping_bytes\":%" PRIu64 ",\"mmap_alignment_bytes\":%" PRIu64
           ",\"page_bytes\":%" PRIu64 ",\"cacheline_bytes\":%" PRIu64
           ",\"line_policy\":\"(global_ordinal+line_offset)-mod-64\","
           "\"line_offset\":%" PRIu64 ",\"trace_sha256\":\"%s\","
           "\"accesses\":%zu,\"unique_lpns\":%zu,\"seed\":%" PRIu64
           ",\"thread_count\":%u,\"assigned_cpus\":[",
           device_bytes, mapping_offset, mapping_bytes,
           (uint64_t)(options->mmap_align_mib * MIB_BYTES),
           (uint64_t)CXL_PAGE_BYTES,
           (uint64_t)CPU_CACHELINE_BYTES, options->line_offset,
           trace->sha256, trace->count, unique_lpn_count(trace),
           options->seed, options->threads);
    for (unsigned int index = 0; index < options->threads; ++index) {
        printf("%s%d", index == 0 ? "" : ",", workers[index].assigned_cpu);
    }
    printf("],\"phase_offset\":%s,\"rotation_policy\":\"%s\","
           "\"shards\":[",
           options->phase_offset ? "true" : "false",
           options->phase_offset ? "thread-id-mod-shard-accesses"
                                 : "aligned-local-zero");
    for (unsigned int index = 0; index < options->threads; ++index) {
        printf("%s{\"thread\":%u,\"start\":%zu,\"accesses\":%zu,"
               "\"initial_local_index\":%zu}",
               index == 0 ? "" : ",", workers[index].id,
               workers[index].shard_start, workers[index].shard_count,
               workers[index].initial_local_index);
    }
    printf("],\"clock_overhead_min_ns\":%" PRIu64
           ",\"latency_is_raw\":true,"
           "\"effective_page_bytes_per_access\":%" PRIu64
           ",\"issued_load_bytes_per_access\":%zu,"
           "\"populate\":%s,\"flush_before\":%s,"
           "\"summary_only\":true}\n",
           clock_overhead, (uint64_t)CXL_PAGE_BYTES, sizeof(uint64_t),
           options->populate ? "true" : "false",
           options->flush_before ? "true" : "false");
}

static void *worker_main(void *opaque)
{
    struct worker *worker = opaque;
    cpu_set_t affinity;

    CPU_ZERO(&affinity);
    CPU_SET(worker->assigned_cpu, &affinity);
    worker->affinity_error =
        pthread_setaffinity_np(pthread_self(), sizeof(affinity), &affinity);

    atomic_fetch_add_explicit(&worker->gate->ready, 1,
                              memory_order_acq_rel);
    while (!atomic_load_explicit(&worker->gate->start,
                                 memory_order_acquire)) {
        _mm_pause();
    }
    if (atomic_load_explicit(&worker->gate->cancel,
                             memory_order_acquire)) {
        return NULL;
    }

    worker->cpu_start = sched_getcpu();
    if (worker->cpu_start != worker->assigned_cpu) {
        ++worker->migration_errors;
    }
    if (!clock_now_ns(&worker->start_ns)) {
        worker->clock_error = errno == 0 ? EIO : errno;
        worker->cpu_end = sched_getcpu();
        return NULL;
    }

    size_t local_index = worker->initial_local_index;
    while (local_index < worker->shard_count &&
           worker->produced < worker->shard_count) {
        const size_t global_ordinal = worker->shard_start + local_index;
        const uint64_t lpn = worker->trace->lpns[global_ordinal];
        const uint64_t line =
            ((uint64_t)(global_ordinal % LINES_PER_PAGE) +
             worker->options->line_offset) %
            LINES_PER_PAGE;
        const uint64_t expected =
            marker(lpn, line, worker->options->seed);
        volatile uint64_t *slot =
            trace_slot(worker->mapping, worker->mapping_offset, lpn,
                       global_ordinal,
                       worker->options->line_offset);
        uint64_t start;
        uint64_t end;

        _mm_lfence();
        if (!clock_now_ns(&start)) {
            worker->clock_error = errno == 0 ? EIO : errno;
            break;
        }
        _mm_lfence();
        const uint64_t value = *slot;
        _mm_lfence();
        if (!clock_now_ns(&end)) {
            worker->clock_error = errno == 0 ? EIO : errno;
            break;
        }
        _mm_lfence();
        if (end < start) {
            worker->clock_error = EDOM;
            break;
        }

        worker->latencies[worker->produced] = end - start;
        worker->checksum ^=
            value + (uint64_t)global_ordinal * CHECKSUM_MIX;
        if (value != expected) {
            ++worker->verify_errors;
        }
        ++worker->produced;
        if (sched_getcpu() != worker->assigned_cpu) {
            ++worker->migration_errors;
        }

        /*
         * The next local position cannot be computed until the prior DAX
         * value arrives.  A correct marker decodes to local_index + 1.
         */
        const uint64_t decoded =
            worker->encoded_next[local_index] ^ value;
        if (decoded > SIZE_MAX || decoded > worker->shard_count) {
            ++worker->verify_errors;
            local_index = worker->shard_count + 1;
            break;
        }
        local_index = (size_t)decoded;
    }

    worker->final_local_index = local_index;
    if (!clock_now_ns(&worker->end_ns)) {
        worker->clock_error = errno == 0 ? EIO : errno;
    }
    worker->cpu_end = sched_getcpu();
    if (worker->cpu_end != worker->assigned_cpu) {
        ++worker->migration_errors;
    }
    return NULL;
}

static bool worker_ok(const struct worker *worker)
{
    return worker->affinity_error == 0 && worker->clock_error == 0 &&
           worker->verify_errors == 0 && worker->migration_errors == 0 &&
           worker->produced == worker->shard_count &&
           worker->final_local_index == worker->shard_count &&
           worker->cpu_start == worker->assigned_cpu &&
           worker->cpu_end == worker->assigned_cpu &&
           worker->end_ns > worker->start_ns;
}

static int run_replay(const struct options *options, const struct trace *trace,
                      uint8_t *mapping, uint64_t device_bytes,
                      uint64_t mapping_offset, uint64_t mapping_bytes)
{
    struct worker workers[8] = {0};
    pthread_t thread_ids[8];
    struct start_gate gate;
    size_t created = 0;
    uint64_t *all_latencies = NULL;
    size_t all_produced = 0;
    uint64_t clock_overhead;
    int join_errors = 0;

    atomic_init(&gate.ready, 0);
    atomic_init(&gate.start, false);
    atomic_init(&gate.cancel, false);

    const size_t base_count = trace->count / options->threads;
    const size_t remainder = trace->count % options->threads;
    size_t next_start = 0;
    for (unsigned int index = 0; index < options->threads; ++index) {
        const size_t shard_count =
            base_count + (index < remainder ? 1U : 0U);
        struct worker *worker = &workers[index];

        worker->id = index;
        worker->assigned_cpu = (int)index;
        worker->options = options;
        worker->trace = trace;
        worker->mapping = mapping;
        worker->mapping_offset = mapping_offset;
        worker->shard_start = next_start;
        worker->shard_count = shard_count;
        worker->initial_local_index =
            options->phase_offset ? index % shard_count : 0;
        worker->gate = &gate;
        worker->cpu_start = -1;
        worker->cpu_end = -1;
        next_start += shard_count;

        if (shard_count > SIZE_MAX / sizeof(*worker->encoded_next) ||
            shard_count > SIZE_MAX / sizeof(*worker->latencies)) {
            fprintf(stderr, "worker buffer allocation overflow\n");
            goto allocation_failure;
        }
        worker->encoded_next =
            malloc(shard_count * sizeof(*worker->encoded_next));
        worker->latencies =
            malloc(shard_count * sizeof(*worker->latencies));
        if (worker->encoded_next == NULL || worker->latencies == NULL) {
            perror("allocate worker buffers");
            goto allocation_failure;
        }

        for (size_t local = 0; local < shard_count; ++local) {
            const size_t global = worker->shard_start + local;
            const uint64_t lpn = trace->lpns[global];
            const uint64_t line =
                ((uint64_t)(global % LINES_PER_PAGE) +
                 options->line_offset) %
                LINES_PER_PAGE;
            const uint64_t expected = marker(lpn, line, options->seed);
            size_t next_local;
            const size_t last_local =
                worker->initial_local_index == 0
                    ? shard_count - 1
                    : worker->initial_local_index - 1;
            if (local == last_local) {
                next_local = shard_count;
            } else if (local + 1 == shard_count) {
                next_local = 0;
            } else {
                next_local = local + 1;
            }
            worker->encoded_next[local] =
                (uint64_t)next_local ^ expected;
        }
    }

    if (options->flush_before) {
        flush_trace_lines(mapping, mapping_offset, trace,
                          options->line_offset);
    }
    clock_overhead = clock_overhead_min();
    for (created = 0; created < options->threads; ++created) {
        const int error =
            pthread_create(&thread_ids[created], NULL, worker_main,
                           &workers[created]);
        if (error != 0) {
            fprintf(stderr, "pthread_create worker %zu: %s\n", created,
                    strerror(error));
            atomic_store_explicit(&gate.cancel, true, memory_order_release);
            atomic_store_explicit(&gate.start, true, memory_order_release);
            for (size_t index = 0; index < created; ++index) {
                const int join_error =
                    pthread_join(thread_ids[index], NULL);
                if (join_error != 0) {
                    fprintf(stderr,
                            "pthread_join cancelled worker %zu: %s\n",
                            index, strerror(join_error));
                    exit(EXIT_FAILURE);
                }
            }
            goto allocation_failure;
        }
    }

    while (atomic_load_explicit(&gate.ready, memory_order_acquire) <
           options->threads) {
        _mm_pause();
    }
    bool affinity_failed = false;
    for (unsigned int index = 0; index < options->threads; ++index) {
        if (workers[index].affinity_error != 0) {
            fprintf(stderr, "worker %u affinity setup: %s\n", index,
                    strerror(workers[index].affinity_error));
            affinity_failed = true;
        }
    }
    if (affinity_failed) {
        atomic_store_explicit(&gate.cancel, true, memory_order_release);
    } else {
        print_config(options, trace, device_bytes, mapping_offset,
                     mapping_bytes, clock_overhead, workers);
        fflush(stdout);
    }
    atomic_store_explicit(&gate.start, true, memory_order_release);

    for (unsigned int index = 0; index < options->threads; ++index) {
        const int error = pthread_join(thread_ids[index], NULL);
        if (error != 0) {
            fprintf(stderr, "pthread_join worker %u: %s\n", index,
                    strerror(error));
            /*
             * A failed join gives us no safe point at which to inspect or
             * free that worker's buffers.  Process exit is the only
             * race-free recovery from this internal pthread failure.
             */
            exit(EXIT_FAILURE);
        }
        if (affinity_failed) {
            continue;
        }
        if (workers[index].produced > SIZE_MAX - all_produced) {
            fprintf(stderr, "produced-access count overflow\n");
            goto allocation_failure;
        }
        all_produced += workers[index].produced;
    }
    if (affinity_failed) {
        goto allocation_failure;
    }

    if (all_produced == 0 ||
        all_produced > SIZE_MAX / sizeof(*all_latencies)) {
        fprintf(stderr, "invalid aggregate latency count\n");
        goto allocation_failure;
    }
    all_latencies = malloc(all_produced * sizeof(*all_latencies));
    if (all_latencies == NULL) {
        perror("allocate aggregate latency buffer");
        goto allocation_failure;
    }

    size_t latency_cursor = 0;
    uint64_t earliest_start = UINT64_MAX;
    uint64_t latest_end = 0;
    uint64_t checksum = 0;
    uint64_t verify_errors = 0;
    uint64_t migration_errors = 0;
    unsigned int affinity_errors = 0;
    unsigned int clock_errors = 0;
    bool ok = join_errors == 0 && all_produced == trace->count;

    for (unsigned int index = 0; index < options->threads; ++index) {
        struct worker *worker = &workers[index];
        memcpy(all_latencies + latency_cursor, worker->latencies,
               worker->produced * sizeof(*all_latencies));
        latency_cursor += worker->produced;
        qsort(worker->latencies, worker->produced,
              sizeof(*worker->latencies), compare_u64);
        if (worker->start_ns < earliest_start) {
            earliest_start = worker->start_ns;
        }
        if (worker->end_ns > latest_end) {
            latest_end = worker->end_ns;
        }
        checksum ^= worker->checksum;
        verify_errors += worker->verify_errors;
        migration_errors += worker->migration_errors;
        affinity_errors += worker->affinity_error != 0 ? 1U : 0U;
        clock_errors += worker->clock_error != 0 ? 1U : 0U;
        if (!worker_ok(worker)) {
            ok = false;
        }
    }
    if (earliest_start == UINT64_MAX || latest_end < earliest_start) {
        fprintf(stderr, "invalid aggregate timing interval\n");
        goto allocation_failure;
    }
    const uint64_t elapsed = latest_end - earliest_start;
    if (elapsed == 0) {
        fprintf(stderr, "aggregate elapsed time is zero\n");
        goto allocation_failure;
    }

    qsort(all_latencies, all_produced, sizeof(*all_latencies), compare_u64);
    const double pages_per_second =
        (double)all_produced * 1000000000.0 / (double)elapsed;
    const double gib_per_second =
        (double)all_produced * (double)CXL_PAGE_BYTES * 1000000000.0 /
        ((double)elapsed * GIB_BYTES);
    const double issued_load_gib_per_second =
        (double)all_produced * (double)sizeof(uint64_t) * 1000000000.0 /
        ((double)elapsed * GIB_BYTES);
    const double inverse_ns_per_page =
        (double)elapsed / (double)all_produced;

    printf("{\"kind\":\"summary\",\"elapsed_ns\":%" PRIu64
           ",\"accesses\":%zu,\"produced\":%zu,"
           "\"latency_ns\":{\"min\":%" PRIu64 ",\"p50\":%" PRIu64
           ",\"p95\":%" PRIu64 ",\"p99\":%" PRIu64
           ",\"max\":%" PRIu64 "},"
           "\"aggregate\":{\"pages_per_second\":%.9f,"
           "\"gib_per_second\":%.9f,"
           "\"effective_page_gib_per_second\":%.9f,"
           "\"issued_load_gib_per_second\":%.9f,"
           "\"inverse_ns_per_page\":%.9f},"
           "\"checksum\":\"0x%016" PRIx64
           "\",\"verify_errors\":%" PRIu64
           ",\"migration_errors\":%" PRIu64
           ",\"affinity_errors\":%u,\"clock_errors\":%u,"
           "\"join_errors\":%d,\"threads\":[",
           elapsed, trace->count, all_produced, all_latencies[0],
           percentile(all_latencies, all_produced, 50, 100),
           percentile(all_latencies, all_produced, 95, 100),
           percentile(all_latencies, all_produced, 99, 100),
           all_latencies[all_produced - 1], pages_per_second,
           gib_per_second, gib_per_second, issued_load_gib_per_second,
           inverse_ns_per_page, checksum, verify_errors, migration_errors,
           affinity_errors, clock_errors, join_errors);
    for (unsigned int index = 0; index < options->threads; ++index) {
        const struct worker *worker = &workers[index];
        const uint64_t worker_elapsed =
            worker->end_ns >= worker->start_ns
                ? worker->end_ns - worker->start_ns
                : 0;
        const double worker_pages_per_second =
            worker_elapsed == 0
                ? 0.0
                : (double)worker->produced * 1000000000.0 /
                      (double)worker_elapsed;
        const double worker_inverse_ns_per_page =
            worker->produced == 0
                ? 0.0
                : (double)worker_elapsed / (double)worker->produced;
        const uint64_t worker_min =
            worker->produced == 0 ? 0 : worker->latencies[0];
        const uint64_t worker_p50 =
            worker->produced == 0
                ? 0
                : percentile(worker->latencies, worker->produced, 50, 100);
        const uint64_t worker_p95 =
            worker->produced == 0
                ? 0
                : percentile(worker->latencies, worker->produced, 95, 100);
        const uint64_t worker_p99 =
            worker->produced == 0
                ? 0
                : percentile(worker->latencies, worker->produced, 99, 100);
        const uint64_t worker_max =
            worker->produced == 0
                ? 0
                : worker->latencies[worker->produced - 1];
        printf("%s{\"thread\":%u,\"assigned_cpu\":%d,"
               "\"shard_start\":%zu,"
               "\"initial_local_index\":%zu,\"wrapped\":%s,"
               "\"accesses\":%zu,\"produced\":%zu,"
               "\"elapsed_ns\":%" PRIu64 ","
               "\"latency_ns\":{\"min\":%" PRIu64
               ",\"p50\":%" PRIu64 ",\"p95\":%" PRIu64
               ",\"p99\":%" PRIu64 ",\"max\":%" PRIu64 "},"
               "\"pages_per_second\":%.9f,"
               "\"inverse_ns_per_page\":%.9f,\"cpu_start\":%d,"
               "\"cpu_end\":%d,\"checksum\":\"0x%016" PRIx64
               "\",\"verify_errors\":%" PRIu64
               ",\"migration_errors\":%" PRIu64
               ",\"affinity_error\":%d,\"clock_error\":%d,"
               "\"status\":\"%s\"}",
               index == 0 ? "" : ",", worker->id, worker->assigned_cpu,
               worker->shard_start, worker->initial_local_index,
               worker->initial_local_index == 0 ? "false" : "true",
               worker->shard_count, worker->produced,
               worker_elapsed, worker_min, worker_p50, worker_p95,
               worker_p99, worker_max, worker_pages_per_second,
               worker_inverse_ns_per_page, worker->cpu_start,
               worker->cpu_end, worker->checksum, worker->verify_errors,
               worker->migration_errors, worker->affinity_error,
               worker->clock_error, worker_ok(worker) ? "ok" : "error");
    }
    printf("],\"status\":\"%s\"}\n", ok ? "ok" : "error");

    free(all_latencies);
    for (unsigned int index = 0; index < options->threads; ++index) {
        free(workers[index].encoded_next);
        free(workers[index].latencies);
    }
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;

allocation_failure:
    free(all_latencies);
    for (unsigned int index = 0; index < options->threads; ++index) {
        free(workers[index].encoded_next);
        free(workers[index].latencies);
    }
    return EXIT_FAILURE;
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
        .threads = 1,
        .phase_offset = false,
        .populate = false,
        .flush_before = false,
    };
    static const struct option long_options[] = {
        {"device", required_argument, NULL, 'd'},
        {"trace", required_argument, NULL, 't'},
        {"map-mib", required_argument, NULL, 'm'},
        {"mmap-align-mib", required_argument, NULL, 'a'},
        {"seed", required_argument, NULL, 's'},
        {"line-offset", required_argument, NULL, 'o'},
        {"threads", required_argument, NULL, 'n'},
        {"phase-offset", no_argument, NULL, 'R'},
        {"run-id", required_argument, NULL, 'r'},
        {"phase", required_argument, NULL, 'p'},
        {"populate", no_argument, NULL, 'P'},
        {"flush-before", no_argument, NULL, 'F'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    int option;
    while ((option = getopt_long(argc, argv, "d:t:m:a:s:o:n:r:p:RPFh",
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
        case 'n': {
            const uint64_t threads = parse_u64("threads", optarg);
            if (threads > UINT_MAX) {
                fprintf(stderr, "invalid threads: %s\n", optarg);
                return EXIT_FAILURE;
            }
            options.threads = (unsigned int)threads;
            break;
        }
        case 'R':
            options.phase_offset = true;
            break;
        case 'r':
            options.run_id = optarg;
            break;
        case 'p':
            options.phase = optarg;
            break;
        case 'P':
            options.populate = true;
            break;
        case 'F':
            options.flush_before = true;
            break;
        case 'h':
            usage(stdout, argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(stderr, argv[0]);
            return EXIT_FAILURE;
        }
    }

    const bool valid_threads =
        options.threads == 1 || options.threads == 2 ||
        options.threads == 4 || options.threads == 8;
    if (options.trace_path == NULL || optind + 1 != argc ||
        strcmp(argv[optind], "replay") != 0 || options.map_mib == 0 ||
        options.map_mib > UINT64_MAX / MIB_BYTES ||
        options.mmap_align_mib == 0 ||
        options.mmap_align_mib > UINT64_MAX / MIB_BYTES ||
        options.line_offset >= LINES_PER_PAGE || !valid_threads) {
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }
    require_safe_label("run-id", options.run_id);
    require_safe_label("phase", options.phase);
    if (options.threads > CPU_SETSIZE) {
        fprintf(stderr, "thread CPU index exceeds CPU_SETSIZE\n");
        return EXIT_FAILURE;
    }
    const long configured_cpus = sysconf(_SC_NPROCESSORS_CONF);
    if (configured_cpus < 0 ||
        (uint64_t)configured_cpus < (uint64_t)options.threads) {
        fprintf(stderr, "CPUs 0..%u are not configured\n",
                options.threads - 1);
        return EXIT_FAILURE;
    }
    cpu_set_t allowed_cpus;
    CPU_ZERO(&allowed_cpus);
    if (sched_getaffinity(0, sizeof(allowed_cpus), &allowed_cpus) != 0) {
        perror("sched_getaffinity");
        return EXIT_FAILURE;
    }
    for (unsigned int cpu = 0; cpu < options.threads; ++cpu) {
        if (!CPU_ISSET((int)cpu, &allowed_cpus)) {
            fprintf(stderr,
                    "guest CPU %u is offline or excluded by this process's "
                    "cpuset\n",
                    cpu);
            return EXIT_FAILURE;
        }
    }

    const uint64_t device_bytes = options.map_mib * MIB_BYTES;
    const uint64_t mmap_alignment = options.mmap_align_mib * MIB_BYTES;
    struct trace trace = load_trace(options.trace_path);
    validate_trace_bounds(&trace, device_bytes);
    if (trace.count < options.threads) {
        fprintf(stderr,
                "trace has %zu accesses, fewer than %u worker threads\n",
                trace.count, options.threads);
        free(trace.lpns);
        return EXIT_FAILURE;
    }

    uint64_t mapping_offset;
    uint64_t mapping_bytes;
    trace_mapping_window(&trace, device_bytes, mmap_alignment,
                         &mapping_offset, &mapping_bytes);

    int descriptor = open(options.device, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        perror("open mmap target");
        free(trace.lpns);
        return EXIT_FAILURE;
    }
    struct stat device_stat;
    if (fstat(descriptor, &device_stat) != 0) {
        perror("fstat mmap target");
        close(descriptor);
        free(trace.lpns);
        return EXIT_FAILURE;
    }
    if (S_ISREG(device_stat.st_mode) &&
        (device_stat.st_size < 0 ||
         (uint64_t)device_stat.st_size < device_bytes)) {
        fprintf(stderr,
                "regular mmap target is smaller than --map-mib bounds\n");
        close(descriptor);
        free(trace.lpns);
        return EXIT_FAILURE;
    }

    int mmap_flags = MAP_SHARED;
    if (options.populate) {
        mmap_flags |= MAP_POPULATE;
    }
    uint8_t *mapping = mmap(NULL, (size_t)mapping_bytes, PROT_READ,
                            mmap_flags, descriptor, (off_t)mapping_offset);
    if (mapping == MAP_FAILED) {
        perror("mmap target");
        close(descriptor);
        free(trace.lpns);
        return EXIT_FAILURE;
    }

    const int result =
        run_replay(&options, &trace, mapping, device_bytes, mapping_offset,
                   mapping_bytes);

    int final_result = result;
    if (munmap(mapping, (size_t)mapping_bytes) != 0) {
        perror("munmap target");
        final_result = EXIT_FAILURE;
    }
    if (close(descriptor) != 0) {
        perror("close mmap target");
        final_result = EXIT_FAILURE;
    }
    free(trace.lpns);
    return final_result;
}
