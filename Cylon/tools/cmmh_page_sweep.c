#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <omp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static uint64_t parse_positive(const char *name, const char *text)
{
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);

    if (errno != 0 || *text == '\0' || *end != '\0' || value == 0) {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(EXIT_FAILURE);
    }
    return (uint64_t)value;
}

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec) +
           (double)(end->tv_nsec - start->tv_nsec) / 1.0e9;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 5) {
        fprintf(stderr,
                "usage: %s <size_MiB> [sweeps=3] [threads=8] [seed=20260728]\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    const uint64_t size_mib = parse_positive("size_MiB", argv[1]);
    const uint64_t sweeps =
        argc >= 3 ? parse_positive("sweeps", argv[2]) : 3;
    const uint64_t threads_raw =
        argc >= 4 ? parse_positive("threads", argv[3]) : 8;
    const uint64_t seed =
        argc >= 5 ? parse_positive("seed", argv[4]) : 20260728;
    const long page_size_raw = sysconf(_SC_PAGESIZE);

    if (page_size_raw <= 0 || threads_raw > 1024) {
        fprintf(stderr, "unsupported page size or thread count\n");
        return EXIT_FAILURE;
    }

    const uint64_t page_size = (uint64_t)page_size_raw;
    const uint64_t size = size_mib * 1024ULL * 1024ULL;
    if (size % page_size != 0) {
        fprintf(stderr, "working-set size must be page aligned\n");
        return EXIT_FAILURE;
    }

    const uint64_t pages = size / page_size;
    const uint64_t words_per_page = page_size / sizeof(uint64_t);
    const int threads = (int)threads_raw;
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

    omp_set_dynamic(0);
    printf("config,size_mib=%" PRIu64 ",size_bytes=%" PRIu64
           ",page_size=%" PRIu64 ",pages=%" PRIu64
           ",sweeps=%" PRIu64 ",threads=%d,seed=%" PRIu64 "\n",
           size_mib, size, page_size, pages, sweeps, threads, seed);
    puts("sweep,seconds,covered_gib_per_sec,ns_per_page,checksum");

    for (uint64_t sweep = 1; sweep <= sweeps; ++sweep) {
        struct timespec start;
        struct timespec end;
        const uint64_t value = seed ^ (sweep << 48);

        clock_gettime(CLOCK_MONOTONIC_RAW, &start);
#pragma omp parallel for num_threads(threads) schedule(static)
        for (uint64_t page = 0; page < pages; ++page) {
            memory[page * words_per_page] = value ^ page;
        }
        clock_gettime(CLOCK_MONOTONIC_RAW, &end);

        const double seconds = elapsed_seconds(&start, &end);
        const double covered_gib_per_second =
            ((double)size / (1024.0 * 1024.0 * 1024.0)) / seconds;
        const double ns_per_page = seconds * 1.0e9 / (double)pages;
        const uint64_t checksum =
            memory[0] ^ memory[(pages - 1) * words_per_page];

        printf("%" PRIu64 ",%.9f,%.6f,%.3f,%" PRIu64 "\n",
               sweep, seconds, covered_gib_per_second,
               ns_per_page, checksum);
        fflush(stdout);
    }

    munmap((void *)memory, size);
    return EXIT_SUCCESS;
}
