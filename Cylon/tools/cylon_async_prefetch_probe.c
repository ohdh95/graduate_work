#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <x86intrin.h>

#define PAGE_BYTES 4096ULL
#define DEFAULT_DEVICE "/dev/dax0.0"

enum probe_mode {
    MODE_MAP,
    MODE_SEED,
    MODE_DEMAND,
    MODE_PREFETCH,
    MODE_PREFETCH_DEMAND,
};

struct options {
    enum probe_mode mode;
    const char *mode_name;
    const char *device;
    uint64_t offset_pages;
    uint64_t pages;
    uint64_t repeat;
    uint64_t lead_us;
    uint64_t settle_us;
    int cpu;
    bool populate;
    bool expect_markers;
};

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s MODE [options]\n"
            "MODE: map | seed | demand | prefetch | prefetch-demand\n"
            "  --device PATH       mmap target (default %s)\n"
            "  --offset-pages N    first device LPN (default 0)\n"
            "  --pages N           number of 4 KiB pages (default 1)\n"
            "  --repeat N          hints per page (default 1)\n"
            "  --lead-us N         hint-to-demand delay (default 0)\n"
            "  --settle-us N       delay before exit (default 0)\n"
            "  --cpu N             pin to guest CPU N (default 0)\n"
            "  --expect-markers    fail unless demand values match seed markers\n"
            "  --no-populate       omit MAP_POPULATE\n",
            program, DEFAULT_DEVICE);
}

static uint64_t parse_u64(const char *name, const char *text)
{
    char *end = NULL;
    unsigned long long value;

    errno = 0;
    value = strtoull(text, &end, 0);
    if (errno || !text[0] || !end || *end) {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(EXIT_FAILURE);
    }
    return value;
}

static int parse_cpu(const char *text)
{
    uint64_t value = parse_u64("cpu", text);

    if (value >= CPU_SETSIZE) {
        fprintf(stderr, "cpu exceeds CPU_SETSIZE: %" PRIu64 "\n", value);
        exit(EXIT_FAILURE);
    }
    return (int)value;
}

static enum probe_mode parse_mode(const char *text)
{
    if (!strcmp(text, "map")) {
        return MODE_MAP;
    }
    if (!strcmp(text, "seed")) {
        return MODE_SEED;
    }
    if (!strcmp(text, "demand")) {
        return MODE_DEMAND;
    }
    if (!strcmp(text, "prefetch")) {
        return MODE_PREFETCH;
    }
    if (!strcmp(text, "prefetch-demand")) {
        return MODE_PREFETCH_DEMAND;
    }
    fprintf(stderr, "unknown mode: %s\n", text);
    exit(EXIT_FAILURE);
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
}

static void sleep_us(uint64_t usec)
{
    struct timespec delay = {
        .tv_sec = (time_t)(usec / 1000000ULL),
        .tv_nsec = (long)((usec % 1000000ULL) * 1000ULL),
    };

    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

/*
 * Keep one auditable PREFETCHT0 opcode in a noinline wrapper.  The byte
 * sequence is 0f 18 /1 with ModRM 08, i.e. PREFETCHT0 [RAX].
 */
__attribute__((noinline, noclone))
static void raw_prefetcht0(const void *address)
{
    __asm__ __volatile__(".byte 0x0f, 0x18, 0x08"
                         :
                         : "a"(address)
                         : "memory");
}

static uint64_t marker(uint64_t lpn)
{
    return 0xc710000000000000ULL ^ (lpn * 0x9e3779b97f4a7c15ULL);
}

int main(int argc, char **argv)
{
    static const struct option long_options[] = {
        {"device", required_argument, NULL, 'd'},
        {"offset-pages", required_argument, NULL, 'o'},
        {"pages", required_argument, NULL, 'p'},
        {"repeat", required_argument, NULL, 'r'},
        {"lead-us", required_argument, NULL, 'l'},
        {"settle-us", required_argument, NULL, 's'},
        {"cpu", required_argument, NULL, 'c'},
        {"expect-markers", no_argument, NULL, 'v'},
        {"no-populate", no_argument, NULL, 'P'},
        {NULL, 0, NULL, 0},
    };
    struct options options = {
        .device = DEFAULT_DEVICE,
        .offset_pages = 0,
        .pages = 1,
        .repeat = 1,
        .lead_us = 0,
        .settle_us = 0,
        .cpu = 0,
        .populate = true,
        .expect_markers = false,
    };
    uint64_t mapping_bytes;
    uint64_t mapping_offset;
    volatile uint8_t *mapping;
    uint64_t checksum = 0;
    uint64_t verify_errors = 0;
    int descriptor;
    int option;

    if (argc < 2) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    options.mode_name = argv[1];
    options.mode = parse_mode(options.mode_name);
    optind = 2;
    while ((option = getopt_long(argc, argv, "d:o:p:r:l:s:c:vP",
                                 long_options, NULL)) != -1) {
        switch (option) {
        case 'd':
            options.device = optarg;
            break;
        case 'o':
            options.offset_pages = parse_u64("offset-pages", optarg);
            break;
        case 'p':
            options.pages = parse_u64("pages", optarg);
            break;
        case 'r':
            options.repeat = parse_u64("repeat", optarg);
            break;
        case 'l':
            options.lead_us = parse_u64("lead-us", optarg);
            break;
        case 's':
            options.settle_us = parse_u64("settle-us", optarg);
            break;
        case 'c':
            options.cpu = parse_cpu(optarg);
            break;
        case 'v':
            options.expect_markers = true;
            break;
        case 'P':
            options.populate = false;
            break;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (!options.pages || !options.repeat || optind != argc ||
        options.pages > UINT64_MAX / PAGE_BYTES ||
        options.offset_pages > (uint64_t)INT64_MAX / PAGE_BYTES) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    mapping_bytes = options.pages * PAGE_BYTES;
    mapping_offset = options.offset_pages * PAGE_BYTES;
    pin_cpu(options.cpu);
    descriptor = open(options.device, O_RDWR | O_CLOEXEC);
    if (descriptor < 0) {
        perror("open mmap target");
        return EXIT_FAILURE;
    }
    mapping = mmap(NULL, mapping_bytes, PROT_READ | PROT_WRITE,
                   MAP_SHARED | (options.populate ? MAP_POPULATE : 0),
                   descriptor, (off_t)mapping_offset);
    if (mapping == MAP_FAILED) {
        perror("mmap target");
        close(descriptor);
        return EXIT_FAILURE;
    }

    printf("{\"kind\":\"config\",\"mode\":\"%s\","
           "\"device\":\"%s\",\"offset_pages\":%" PRIu64
           ",\"pages\":%" PRIu64 ",\"repeat\":%" PRIu64
           ",\"lead_us\":%" PRIu64 ",\"settle_us\":%" PRIu64
           ",\"cpu\":%d,\"populate\":%s,\"expect_markers\":%s}\n",
           options.mode_name, options.device, options.offset_pages,
           options.pages, options.repeat, options.lead_us,
           options.settle_us, options.cpu,
           options.populate ? "true" : "false",
           options.expect_markers ? "true" : "false");

    if (options.mode == MODE_SEED) {
        for (uint64_t page = 0; page < options.pages; ++page) {
            volatile uint64_t *slot =
                (volatile uint64_t *)(mapping + page * PAGE_BYTES);
            *slot = marker(options.offset_pages + page);
        }
        _mm_mfence();
        for (uint64_t page = 0; page < options.pages; ++page) {
            volatile uint64_t *slot =
                (volatile uint64_t *)(mapping + page * PAGE_BYTES);
            uint64_t value = *slot;

            checksum ^= value;
            if (value != marker(options.offset_pages + page)) {
                verify_errors++;
            }
            _mm_clflush((const void *)slot);
        }
        _mm_mfence();
    } else if (options.mode == MODE_PREFETCH ||
               options.mode == MODE_PREFETCH_DEMAND) {
        for (uint64_t repetition = 0; repetition < options.repeat;
             ++repetition) {
            for (uint64_t page = 0; page < options.pages; ++page) {
                raw_prefetcht0((const void *)(mapping + page * PAGE_BYTES));
            }
        }
        _mm_mfence();
    }

    if (options.mode == MODE_PREFETCH_DEMAND) {
        sleep_us(options.lead_us);
    }
    if (options.mode == MODE_DEMAND ||
        options.mode == MODE_PREFETCH_DEMAND) {
        for (uint64_t page = 0; page < options.pages; ++page) {
            volatile uint64_t *slot =
                (volatile uint64_t *)(mapping + page * PAGE_BYTES);
            uint64_t value = *slot;

            checksum ^= value;
            if (options.expect_markers &&
                value != marker(options.offset_pages + page)) {
                verify_errors++;
            }
        }
    }
    _mm_mfence();
    sleep_us(options.settle_us);

    printf("{\"kind\":\"summary\",\"mode\":\"%s\","
           "\"first_lpn\":%" PRIu64 ",\"last_lpn\":%" PRIu64
           ",\"hints\":%" PRIu64 ",\"demands\":%" PRIu64
           ",\"checksum\":\"0x%016" PRIx64
           "\",\"verify_errors\":%" PRIu64
           ",\"cpu_end\":%d,\"status\":\"%s\"}\n",
           options.mode_name, options.offset_pages,
           options.offset_pages + options.pages - 1,
           (options.mode == MODE_PREFETCH ||
            options.mode == MODE_PREFETCH_DEMAND) ?
               options.pages * options.repeat : 0,
           (options.mode == MODE_DEMAND ||
            options.mode == MODE_PREFETCH_DEMAND) ? options.pages : 0,
           checksum, verify_errors, sched_getcpu(),
           verify_errors ? "error" : "ok");

    if (munmap((void *)mapping, mapping_bytes) != 0) {
        perror("munmap target");
        close(descriptor);
        return EXIT_FAILURE;
    }
    close(descriptor);
    return verify_errors ? EXIT_FAILURE : EXIT_SUCCESS;
}
