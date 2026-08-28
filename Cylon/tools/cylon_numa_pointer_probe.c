#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <linux/mempolicy.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <x86intrin.h>

#define DEFAULT_PAGES 64ULL
#define DEFAULT_NODE 1
#define DEFAULT_CPU 0
#define DEFAULT_SEED 20260824ULL
#define DEFAULT_STATS_BASE 1000ULL

struct chain_node {
    struct chain_node *next;
    uint64_t marker;
};

struct sample {
    uint64_t ordinal;
    uint64_t page_index;
    uint64_t next_page_index;
    uint64_t latency_ns;
};

struct options {
    uint64_t pages;
    int node;
    int cpu;
    uint64_t seed;
    uint64_t stats_base;
    const char *run_id;
};

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s --run-id ID [--pages N] [--node N] [--cpu N] "
            "[--seed N] [--stats-base N]\n",
            program);
}

static uint64_t parse_u64(const char *text, const char *name)
{
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0') {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(EXIT_FAILURE);
    }
    return (uint64_t)value;
}

static int parse_int(const char *text, const char *name)
{
    uint64_t value = parse_u64(text, name);
    if (value > INT32_MAX) {
        fprintf(stderr, "%s is too large: %s\n", name, text);
        exit(EXIT_FAILURE);
    }
    return (int)value;
}

static void validate_label(const char *label)
{
    if (label == NULL || *label == '\0') {
        fputs("--run-id is required\n", stderr);
        exit(EXIT_FAILURE);
    }
    for (const char *cursor = label; *cursor != '\0'; ++cursor) {
        const bool safe = (*cursor >= 'a' && *cursor <= 'z') ||
                          (*cursor >= 'A' && *cursor <= 'Z') ||
                          (*cursor >= '0' && *cursor <= '9') ||
                          strchr("._:-", *cursor) != NULL;
        if (!safe) {
            fprintf(stderr, "unsafe run-id: %s\n", label);
            exit(EXIT_FAILURE);
        }
    }
}

static struct options parse_options(int argc, char **argv)
{
    struct options options = {
        .pages = DEFAULT_PAGES,
        .node = DEFAULT_NODE,
        .cpu = DEFAULT_CPU,
        .seed = DEFAULT_SEED,
        .stats_base = DEFAULT_STATS_BASE,
        .run_id = NULL,
    };
    static const struct option long_options[] = {
        {"pages", required_argument, NULL, 'p'},
        {"node", required_argument, NULL, 'n'},
        {"cpu", required_argument, NULL, 'c'},
        {"seed", required_argument, NULL, 's'},
        {"stats-base", required_argument, NULL, 'b'},
        {"run-id", required_argument, NULL, 'r'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    for (;;) {
        int option = getopt_long(argc, argv, "p:n:c:s:b:r:h", long_options,
                                 NULL);
        if (option == -1) {
            break;
        }
        switch (option) {
        case 'p':
            options.pages = parse_u64(optarg, "pages");
            break;
        case 'n':
            options.node = parse_int(optarg, "node");
            break;
        case 'c':
            options.cpu = parse_int(optarg, "cpu");
            break;
        case 's':
            options.seed = parse_u64(optarg, "seed");
            break;
        case 'b':
            options.stats_base = parse_u64(optarg, "stats-base");
            break;
        case 'r':
            options.run_id = optarg;
            break;
        case 'h':
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        default:
            usage(argv[0]);
            exit(EXIT_FAILURE);
        }
    }
    if (optind != argc || options.pages < 2 || options.pages > UINT32_MAX ||
        options.node < 0 || options.node >= (int)(sizeof(unsigned long) * 8) ||
        options.cpu < 0 || options.stats_base > UINT64_MAX - 4) {
        usage(argv[0]);
        exit(EXIT_FAILURE);
    }
    validate_label(options.run_id);
    return options;
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

static uint64_t xorshift64(uint64_t *state)
{
    uint64_t value = *state;
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    *state = value;
    return value;
}

static void pin_cpu(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        perror("sched_setaffinity");
        exit(EXIT_FAILURE);
    }
    if (sched_getcpu() != cpu) {
        fprintf(stderr, "failed to remain on CPU %d\n", cpu);
        exit(EXIT_FAILURE);
    }
}

static void bind_node(void *memory, size_t bytes, int node)
{
    unsigned long nodemask = 1UL << node;
    long result = syscall(SYS_mbind, memory, bytes, MPOL_BIND, &nodemask,
                          sizeof(nodemask) * 8, MPOL_MF_STRICT);
    if (result != 0) {
        perror("mbind MPOL_BIND");
        exit(EXIT_FAILURE);
    }
}

static void verify_node_placement(void *memory, uint64_t pages,
                                  size_t page_size, int expected_node)
{
    void **addresses = calloc((size_t)pages, sizeof(*addresses));
    int *status = calloc((size_t)pages, sizeof(*status));
    if (addresses == NULL || status == NULL) {
        perror("allocate move_pages query");
        exit(EXIT_FAILURE);
    }
    for (uint64_t index = 0; index < pages; ++index) {
        addresses[index] = (uint8_t *)memory + index * page_size;
    }
    long result = syscall(SYS_move_pages, 0, pages, addresses, NULL, status, 0);
    if (result != 0) {
        perror("move_pages query");
        exit(EXIT_FAILURE);
    }
    uint64_t matched = 0;
    for (uint64_t index = 0; index < pages; ++index) {
        if (status[index] == expected_node) {
            ++matched;
        } else {
            fprintf(stderr, "page %" PRIu64 " is on node %d, expected %d\n",
                    index, status[index], expected_node);
        }
    }
    printf("{\"kind\":\"numa-placement\",\"expected_node\":%d,"
           "\"pages\":%" PRIu64 ",\"matched\":%" PRIu64
           ",\"status\":\"%s\"}\n",
           expected_node, pages, matched, matched == pages ? "ok" : "error");
    fflush(stdout);
    free(status);
    free(addresses);
    if (matched != pages) {
        exit(EXIT_FAILURE);
    }
}

static struct chain_node *node_at(void *memory, size_t page_size,
                                  uint64_t index)
{
    return (struct chain_node *)((uint8_t *)memory + index * page_size);
}

static struct chain_node *build_chain(void *memory, uint64_t pages,
                                      size_t page_size, uint64_t seed)
{
    uint32_t *order = malloc((size_t)pages * sizeof(*order));
    if (order == NULL) {
        perror("allocate permutation");
        exit(EXIT_FAILURE);
    }
    for (uint64_t index = 0; index < pages; ++index) {
        order[index] = (uint32_t)index;
    }
    uint64_t state = seed == 0 ? DEFAULT_SEED : seed;
    for (uint64_t index = pages - 1; index > 0; --index) {
        uint64_t selected = xorshift64(&state) % (index + 1);
        uint32_t temporary = order[index];
        order[index] = order[selected];
        order[selected] = temporary;
    }
    for (uint64_t ordinal = 0; ordinal < pages; ++ordinal) {
        uint64_t current_index = order[ordinal];
        uint64_t next_index = order[(ordinal + 1) % pages];
        struct chain_node *current = node_at(memory, page_size, current_index);
        current->next = node_at(memory, page_size, next_index);
        current->marker = current_index ^ seed ^ 0x43594c3031424e55ULL;
    }
    struct chain_node *start = node_at(memory, page_size, order[0]);
    free(order);
    return start;
}

static uint64_t pointer_to_index(const struct chain_node *node, void *memory,
                                 uint64_t pages, size_t page_size)
{
    uintptr_t base = (uintptr_t)memory;
    uintptr_t address = (uintptr_t)node;
    uintptr_t bytes = (uintptr_t)pages * page_size;
    if (address < base || address >= base + bytes ||
        (address - base) % page_size != 0) {
        fprintf(stderr, "invalid chain pointer: 0x%" PRIxPTR "\n", address);
        exit(EXIT_FAILURE);
    }
    return (uint64_t)((address - base) / page_size);
}

static int compare_u64(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static uint64_t nearest_rank(const uint64_t *sorted, size_t count,
                             uint64_t numerator)
{
    size_t rank = (count * numerator + 99) / 100;
    if (rank == 0) {
        rank = 1;
    }
    return sorted[rank - 1];
}

static void run_chain(const char *phase, struct chain_node *start,
                      void *memory, uint64_t pages, size_t page_size,
                      int expected_cpu)
{
    struct sample *samples = calloc((size_t)pages, sizeof(*samples));
    uint64_t *latencies = calloc((size_t)pages, sizeof(*latencies));
    if (samples == NULL || latencies == NULL) {
        perror("allocate samples");
        exit(EXIT_FAILURE);
    }

    struct chain_node *current = start;
    uint64_t sum = 0;
    int cpu_start = sched_getcpu();
    for (uint64_t ordinal = 0; ordinal < pages; ++ordinal) {
        uint64_t page_index = pointer_to_index(current, memory, pages, page_size);
        _mm_lfence();
        uint64_t begin = monotonic_raw_ns();
        _mm_lfence();
        struct chain_node *next = *(struct chain_node *volatile *)&current->next;
        _mm_lfence();
        uint64_t end = monotonic_raw_ns();
        _mm_lfence();
        uint64_t next_index = pointer_to_index(next, memory, pages, page_size);
        samples[ordinal] = (struct sample){
            .ordinal = ordinal,
            .page_index = page_index,
            .next_page_index = next_index,
            .latency_ns = end - begin,
        };
        latencies[ordinal] = end - begin;
        sum += end - begin;
        current = next;
    }
    int cpu_end = sched_getcpu();
    bool ok = current == start && cpu_start == expected_cpu &&
              cpu_end == expected_cpu;

    for (uint64_t index = 0; index < pages; ++index) {
        printf("{\"kind\":\"sample\",\"phase\":\"%s\","
               "\"ordinal\":%" PRIu64 ",\"page_index\":%" PRIu64
               ",\"next_page_index\":%" PRIu64
               ",\"latency_ns\":%" PRIu64 "}\n",
               phase, samples[index].ordinal, samples[index].page_index,
               samples[index].next_page_index, samples[index].latency_ns);
    }
    qsort(latencies, (size_t)pages, sizeof(*latencies), compare_u64);
    printf("{\"kind\":\"summary\",\"phase\":\"%s\","
           "\"samples\":%" PRIu64 ",\"mean_ns\":%.3f,"
           "\"min_ns\":%" PRIu64 ",\"p50_ns\":%" PRIu64
           ",\"p95_ns\":%" PRIu64 ",\"p99_ns\":%" PRIu64
           ",\"max_ns\":%" PRIu64 ",\"cpu_start\":%d,"
           "\"cpu_end\":%d,\"cycle_closed\":%s,\"status\":\"%s\"}\n",
           phase, pages, (double)sum / (double)pages, latencies[0],
           nearest_rank(latencies, (size_t)pages, 50),
           nearest_rank(latencies, (size_t)pages, 95),
           nearest_rank(latencies, (size_t)pages, 99),
           latencies[pages - 1], cpu_start, cpu_end,
           current == start ? "true" : "false", ok ? "ok" : "error");
    fflush(stdout);
    free(latencies);
    free(samples);
    if (!ok) {
        exit(EXIT_FAILURE);
    }
}

static void flush_chain(void *memory, uint64_t pages, size_t page_size)
{
    for (uint64_t index = 0; index < pages; ++index) {
        _mm_clflush(node_at(memory, page_size, index));
    }
    _mm_mfence();
}

static void run_cxl_control(uint64_t size, uint64_t offset, const char *label)
{
    char size_text[32];
    char offset_text[32];
    snprintf(size_text, sizeof(size_text), "%" PRIu64, size);
    snprintf(offset_text, sizeof(offset_text), "%" PRIu64, offset);
    fflush(NULL);
    pid_t child = fork();
    if (child < 0) {
        perror("fork cxl control");
        exit(EXIT_FAILURE);
    }
    if (child == 0) {
        int output = open("/tmp/cyl01b-control.bin",
                          O_WRONLY | O_CREAT | O_TRUNC, 0600);
        int errors = open("/tmp/cyl01b-control.err",
                          O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (output < 0 || errors < 0 || dup2(output, STDOUT_FILENO) < 0 ||
            dup2(errors, STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(output);
        close(errors);
        execlp("cxl", "cxl", "read-labels", "mem0", "-s", size_text,
               "-O", offset_text, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        fprintf(stderr, "CXL control failed: %s size=%" PRIu64
                        " offset=%" PRIu64 " status=%d\n",
                label, size, offset, status);
        exit(EXIT_FAILURE);
    }
    printf("{\"kind\":\"control\",\"label\":\"%s\","
           "\"size\":%" PRIu64 ",\"offset\":%" PRIu64
           ",\"status\":\"ok\"}\n",
           label, size, offset);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    struct options options = parse_options(argc, argv);
    long page_size_raw = sysconf(_SC_PAGESIZE);
    if (page_size_raw <= 0 || (uint64_t)page_size_raw != 4096) {
        fprintf(stderr, "expected a 4096-byte base page\n");
        return EXIT_FAILURE;
    }
    size_t page_size = (size_t)page_size_raw;
    if (options.pages > SIZE_MAX / page_size) {
        fputs("working set is too large\n", stderr);
        return EXIT_FAILURE;
    }
    size_t bytes = (size_t)options.pages * page_size;

    pin_cpu(options.cpu);
    void *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        perror("mmap anonymous");
        return EXIT_FAILURE;
    }
    if (madvise(memory, bytes, MADV_NOHUGEPAGE) != 0) {
        perror("madvise MADV_NOHUGEPAGE");
        return EXIT_FAILURE;
    }
    bind_node(memory, bytes, options.node);
    struct chain_node *start = build_chain(memory, options.pages, page_size,
                                           options.seed);
    verify_node_placement(memory, options.pages, page_size, options.node);

    printf("{\"schema\":\"cylon-numa-pointer-probe/v1\","
           "\"kind\":\"config\",\"run_id\":\"%s\","
           "\"pages\":%" PRIu64 ",\"bytes\":%zu,\"page_size\":%zu,"
           "\"node\":%d,\"cpu\":%d,\"seed\":%" PRIu64
           ",\"stats_base\":%" PRIu64 ","
           "\"access_pattern\":\"random-linked-list-pointer-chase\"}\n",
           options.run_id, options.pages, bytes, page_size, options.node,
           options.cpu, options.seed, options.stats_base);
    fflush(stdout);

    run_cxl_control(1, options.stats_base, "reset_after_initialization");
    run_cxl_control(2, 0, "clear_before_cold");
    run_cxl_control(1, options.stats_base + 1, "empty_before_cold");
    run_chain("cold", start, memory, options.pages, page_size, options.cpu);
    run_cxl_control(1, options.stats_base + 2, "cold_stats");

    flush_chain(memory, options.pages, page_size);
    run_chain("warm", start, memory, options.pages, page_size, options.cpu);
    run_cxl_control(1, options.stats_base + 3, "warm_stats");

    run_cxl_control(2, 0, "final_clear");
    run_cxl_control(1, options.stats_base + 4, "final_empty");
    printf("{\"kind\":\"done\",\"run_id\":\"%s\","
           "\"status\":\"ok\"}\n",
           options.run_id);
    fflush(stdout);

    if (munmap(memory, bytes) != 0) {
        perror("munmap");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
