#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec) +
           (double)(end->tv_nsec - start->tv_nsec) / 1.0e9;
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

static uint64_t parse_size_mib(const char *text)
{
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);

    if (errno != 0 || *text == '\0' || *end != '\0' || value == 0) {
        fprintf(stderr, "invalid size in MiB: %s\n", text);
        exit(EXIT_FAILURE);
    }
    return (uint64_t)value;
}

static void print_result(const char *metric, double seconds,
                         uint64_t covered_bytes, uint64_t operations,
                         uint64_t checksum)
{
    const double gib_per_second =
        ((double)covered_bytes / (1024.0 * 1024.0 * 1024.0)) / seconds;
    const double ns_per_operation = seconds * 1.0e9 / (double)operations;

    printf("%s,%.9f,%.6f,%.3f,%" PRIu64 "\n",
           metric, seconds, gib_per_second, ns_per_operation, checksum);
}

int main(int argc, char **argv)
{
    const uint64_t size_mib = argc >= 2 ? parse_size_mib(argv[1]) : 1024;
    const uint64_t seed = argc >= 3 ? parse_size_mib(argv[2]) : 20260728;
    const uint64_t size = size_mib * 1024ULL * 1024ULL;
    const long page_size_raw = sysconf(_SC_PAGESIZE);

    if (argc > 3 || page_size_raw <= 0 ||
        size % (uint64_t)page_size_raw != 0) {
        fprintf(stderr, "usage: %s [size_MiB] [seed]\n", argv[0]);
        return EXIT_FAILURE;
    }

    const uint64_t page_size = (uint64_t)page_size_raw;
    const uint64_t pages = size / page_size;
    const uint64_t words = size / sizeof(uint64_t);

    volatile uint64_t *memory =
        mmap(NULL, size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        perror("mmap");
        return EXIT_FAILURE;
    }
    if (madvise((void *)memory, size, MADV_NOHUGEPAGE) != 0) {
        perror("madvise MADV_NOHUGEPAGE");
        munmap((void *)memory, size);
        return EXIT_FAILURE;
    }

    uint32_t *order = malloc(pages * sizeof(*order));
    if (order == NULL) {
        perror("malloc order");
        munmap((void *)memory, size);
        return EXIT_FAILURE;
    }
    for (uint64_t i = 0; i < pages; ++i) {
        order[i] = (uint32_t)i;
    }

    struct timespec start;
    struct timespec end;
    uint64_t checksum = 0;

    printf("config,size_mib=%" PRIu64 ",size_bytes=%" PRIu64
           ",page_size=%" PRIu64 ",pages=%" PRIu64 ",seed=%" PRIu64 "\n",
           size_mib, size, page_size, pages, seed);
    puts("metric,seconds,covered_gib_per_sec,ns_per_operation,checksum");

    clock_gettime(CLOCK_MONOTONIC_RAW, &start);
    for (uint64_t page = 0; page < pages; ++page) {
        memory[(page * page_size) / sizeof(uint64_t)] = page ^ seed;
    }
    clock_gettime(CLOCK_MONOTONIC_RAW, &end);
    print_result("first_touch_4k", elapsed_seconds(&start, &end),
                 size, pages, 0);

    clock_gettime(CLOCK_MONOTONIC_RAW, &start);
    for (uint64_t word = 0; word < words; ++word) {
        memory[word] = word + seed;
    }
    clock_gettime(CLOCK_MONOTONIC_RAW, &end);
    print_result("sequential_write_8b", elapsed_seconds(&start, &end),
                 size, words, 0);

    clock_gettime(CLOCK_MONOTONIC_RAW, &start);
    for (uint64_t word = 0; word < words; ++word) {
        checksum += memory[word];
    }
    clock_gettime(CLOCK_MONOTONIC_RAW, &end);
    print_result("sequential_read_8b", elapsed_seconds(&start, &end),
                 size, words, checksum);

    uint64_t random_state = seed;
    for (uint64_t i = pages - 1; i > 0; --i) {
        uint64_t selected = xorshift64(&random_state) % (i + 1);
        uint32_t temporary = order[i];
        order[i] = order[selected];
        order[selected] = temporary;
    }

    checksum = 0;
    clock_gettime(CLOCK_MONOTONIC_RAW, &start);
    for (uint64_t i = 0; i < pages; ++i) {
        checksum += memory[((uint64_t)order[i] * page_size) /
                           sizeof(uint64_t)];
    }
    clock_gettime(CLOCK_MONOTONIC_RAW, &end);
    print_result("random_read_one_per_4k", elapsed_seconds(&start, &end),
                 size, pages, checksum);

    free(order);
    munmap((void *)memory, size);
    return EXIT_SUCCESS;
}
