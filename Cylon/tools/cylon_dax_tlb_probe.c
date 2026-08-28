#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
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

#define PAGE_BYTES 4096ULL
#define MIB_BYTES (1024ULL * 1024ULL)
#define TARGET_CPU_FIRST 1U
#define TARGET_CPU_LAST 6U
#define EVICTOR_CPU 7U
#define WORKER_COUNT 7U
#define COLLIDERS_PER_SET 16U
#define DEFAULT_MAP_MIB 98304ULL
#define DEFAULT_ALIGNMENT_MIB 2ULL
#define DEFAULT_SET_COUNT 78640ULL
#define DEFAULT_BASE_LPN 1000000ULL
#define DEFAULT_ROUND_STRIDE 4096ULL
#define DEFAULT_ROUNDS 8U

enum action {
    ACTION_NONE = 0,
    ACTION_PRIME,
    ACTION_EVICT,
    ACTION_PROBE,
    ACTION_RACE_TARGET,
    ACTION_RACE_EVICT,
    ACTION_STOP,
};

struct race_state {
    atomic_bool done;
    atomic_uint_fast64_t target_cycles;
    unsigned int target_cpu;
};

struct options {
    const char *device;
    const char *run_id;
    uint64_t map_mib;
    uint64_t alignment_mib;
    uint64_t set_count;
    uint64_t base_lpn;
    uint64_t round_stride;
    unsigned int rounds;
};

struct worker {
    unsigned int cpu;
    const struct options *options;
    uint8_t *mapping;
    uint64_t mapping_offset;
    atomic_uint_fast64_t sequence;
    atomic_uint_fast64_t done_sequence;
    atomic_bool ready;
    enum action action;
    unsigned int round;

    uint64_t elapsed_ns;
    uint64_t checksum;
    uint64_t accesses;
    int observed_cpu;
    int affinity_error;
    int clock_error;
    struct race_state *race;
};

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "usage: %s [options]\n"
            "\n"
            "Interactive commands on stdin:\n"
            "  prime ROUND\n"
            "  evict ROUND\n"
            "  probe ROUND CPU       CPU must be 1..6\n"
            "  race ROUND CPU        keep CPU's translation hot while CPU 7 "
            "evicts it\n"
            "  quit\n"
            "\n"
            "Options:\n"
            "  --device PATH         mmap target (default /dev/dax0.0)\n"
            "  --map-mib N           device bounds (default 98304)\n"
            "  --alignment-mib N     mmap offset/length alignment (default 2)\n"
            "  --set-count N         FIFO set count (default 78640)\n"
            "  --base-lpn N          first target LPN (default 1000000)\n"
            "  --round-stride N      target-LPN stride (default 4096)\n"
            "  --rounds N            number of unique rounds (default 8)\n"
            "  --run-id LABEL        provenance label\n",
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

static void require_safe_label(const char *label)
{
    if (label[0] == '\0') {
        fprintf(stderr, "run-id cannot be empty\n");
        exit(EXIT_FAILURE);
    }
    for (const unsigned char *cursor = (const unsigned char *)label;
         *cursor != '\0'; ++cursor) {
        const bool safe = (*cursor >= 'a' && *cursor <= 'z') ||
                          (*cursor >= 'A' && *cursor <= 'Z') ||
                          (*cursor >= '0' && *cursor <= '9') ||
                          strchr("._:-", *cursor) != NULL;
        if (!safe) {
            fprintf(stderr, "run-id contains an unsafe character: %s\n",
                    label);
            exit(EXIT_FAILURE);
        }
    }
}

static bool now_ns(uint64_t *value)
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

static uint64_t target_lpn(const struct options *options,
                           unsigned int round, unsigned int cpu)
{
    return options->base_lpn +
           (uint64_t)round * options->round_stride +
           (uint64_t)cpu * 17ULL;
}

static uint64_t collider_lpn(const struct options *options,
                             unsigned int round, unsigned int cpu,
                             unsigned int collider)
{
    return target_lpn(options, round, cpu) +
           (uint64_t)collider * options->set_count;
}

static volatile uint64_t *slot_for_lpn(const struct worker *worker,
                                       uint64_t lpn)
{
    const uint64_t byte_offset = lpn * PAGE_BYTES;
    return (volatile uint64_t *)(worker->mapping +
                                 (byte_offset - worker->mapping_offset));
}

static uint64_t timed_load(volatile uint64_t *slot, uint64_t *elapsed,
                           int *clock_error)
{
    uint64_t start = 0;
    uint64_t end = 0;

    _mm_lfence();
    if (!now_ns(&start)) {
        *clock_error = errno == 0 ? EIO : errno;
    }
    _mm_lfence();
    const uint64_t value = *slot;
    _mm_lfence();
    if (!now_ns(&end)) {
        *clock_error = errno == 0 ? EIO : errno;
    }
    _mm_lfence();
    if (*clock_error != 0 || end < start) {
        *elapsed = 0;
        if (*clock_error == 0) {
            *clock_error = ERANGE;
        }
    } else {
        *elapsed = end - start;
    }
    return value;
}

static void execute_action(struct worker *worker)
{
    worker->elapsed_ns = 0;
    worker->checksum = 0;
    worker->accesses = 0;
    worker->observed_cpu = sched_getcpu();
    worker->clock_error = 0;

    if (worker->action == ACTION_PRIME) {
        volatile uint64_t *slot = slot_for_lpn(
            worker, target_lpn(worker->options, worker->round, worker->cpu));
        worker->checksum =
            timed_load(slot, &worker->elapsed_ns, &worker->clock_error);
        worker->accesses = 1;
        /*
         * Remove the data line while deliberately retaining the translation.
         * The later target load must therefore perform address translation,
         * but cannot be satisfied by the CPU data cache.
         */
        _mm_clflush((const void *)slot);
        _mm_mfence();
    } else if (worker->action == ACTION_EVICT) {
        uint64_t start = 0;
        uint64_t end = 0;

        _mm_lfence();
        if (!now_ns(&start)) {
            worker->clock_error = errno == 0 ? EIO : errno;
        }
        _mm_lfence();
        for (unsigned int cpu = TARGET_CPU_FIRST;
             cpu <= TARGET_CPU_LAST; ++cpu) {
            for (unsigned int collider = 1;
                 collider <= COLLIDERS_PER_SET; ++collider) {
                volatile uint64_t *slot = slot_for_lpn(
                    worker,
                    collider_lpn(worker->options, worker->round, cpu,
                                 collider));
                worker->checksum ^=
                    *slot + ((uint64_t)cpu << 32) + collider;
                ++worker->accesses;
            }
        }
        _mm_lfence();
        if (!now_ns(&end)) {
            worker->clock_error = errno == 0 ? EIO : errno;
        }
        _mm_lfence();
        if (worker->clock_error != 0 || end < start) {
            worker->elapsed_ns = 0;
            if (worker->clock_error == 0) {
                worker->clock_error = ERANGE;
            }
        } else {
            worker->elapsed_ns = end - start;
        }
    } else if (worker->action == ACTION_PROBE) {
        volatile uint64_t *slot = slot_for_lpn(
            worker, target_lpn(worker->options, worker->round, worker->cpu));
        worker->checksum =
            timed_load(slot, &worker->elapsed_ns, &worker->clock_error);
        worker->accesses = 1;
    } else if (worker->action == ACTION_RACE_TARGET) {
        volatile uint64_t *slot = slot_for_lpn(
            worker, target_lpn(worker->options, worker->round, worker->cpu));

        /*
         * Keep the combined guest/EPT translation hot until the evictor's
         * final collider has completed.  Every iteration flushes the data
         * cache line first, so a post-eviction load cannot be mistaken for a
         * cache hit that avoided address translation altogether.
         */
        while (!atomic_load_explicit(&worker->race->done,
                                     memory_order_acquire)) {
            _mm_clflush((const void *)slot);
            _mm_mfence();
            worker->checksum ^= *slot + worker->accesses;
            ++worker->accesses;
            atomic_store_explicit(&worker->race->target_cycles,
                                  worker->accesses, memory_order_release);
        }

        _mm_clflush((const void *)slot);
        _mm_mfence();
        uint64_t post_value =
            timed_load(slot, &worker->elapsed_ns, &worker->clock_error);
        worker->checksum ^= post_value + worker->accesses;
        ++worker->accesses;
    } else if (worker->action == ACTION_RACE_EVICT) {
        uint64_t start = 0;
        uint64_t end = 0;

        while (atomic_load_explicit(&worker->race->target_cycles,
                                    memory_order_acquire) < 1000) {
            _mm_pause();
        }
        _mm_lfence();
        if (!now_ns(&start)) {
            worker->clock_error = errno == 0 ? EIO : errno;
        }
        _mm_lfence();
        for (unsigned int collider = 1;
             collider <= COLLIDERS_PER_SET; ++collider) {
            volatile uint64_t *slot = slot_for_lpn(
                worker,
                collider_lpn(worker->options, worker->round,
                             worker->race->target_cpu, collider));
            worker->checksum ^= *slot + collider;
            ++worker->accesses;
        }
        _mm_lfence();
        if (!now_ns(&end)) {
            worker->clock_error = errno == 0 ? EIO : errno;
        }
        _mm_lfence();
        if (worker->clock_error != 0 || end < start) {
            worker->elapsed_ns = 0;
            if (worker->clock_error == 0) {
                worker->clock_error = ERANGE;
            }
        } else {
            worker->elapsed_ns = end - start;
        }
        /*
         * The last collider load returns only after the FTL has removed the
         * FIFO victim and published the MMIO SPTE.  The target worker then
         * performs one guaranteed post-completion load.
         */
        atomic_store_explicit(&worker->race->done, true,
                              memory_order_release);
    }
    worker->observed_cpu = sched_getcpu();
}

static void *worker_main(void *opaque)
{
    struct worker *worker = opaque;
    cpu_set_t affinity;
    uint64_t seen_sequence = 0;

    CPU_ZERO(&affinity);
    CPU_SET(worker->cpu, &affinity);
    worker->affinity_error =
        pthread_setaffinity_np(pthread_self(), sizeof(affinity), &affinity);
    atomic_store_explicit(&worker->ready, true, memory_order_release);

    for (;;) {
        const uint64_t sequence =
            atomic_load_explicit(&worker->sequence, memory_order_acquire);
        if (sequence == seen_sequence) {
            _mm_pause();
            continue;
        }
        seen_sequence = sequence;
        if (worker->action == ACTION_STOP) {
            atomic_store_explicit(&worker->done_sequence, sequence,
                                  memory_order_release);
            return NULL;
        }
        execute_action(worker);
        atomic_store_explicit(&worker->done_sequence, sequence,
                              memory_order_release);
    }
}

static void dispatch(struct worker *worker, enum action action,
                     unsigned int round)
{
    const uint64_t next =
        atomic_load_explicit(&worker->sequence, memory_order_relaxed) + 1;
    worker->action = action;
    worker->round = round;
    atomic_store_explicit(&worker->sequence, next, memory_order_release);
}

static void wait_worker(const struct worker *worker)
{
    const uint64_t expected =
        atomic_load_explicit(&worker->sequence, memory_order_acquire);
    while (atomic_load_explicit(&worker->done_sequence,
                                memory_order_acquire) != expected) {
        _mm_pause();
    }
}

static struct worker *worker_for_cpu(struct worker *workers,
                                     unsigned int cpu)
{
    for (unsigned int index = 0; index < WORKER_COUNT; ++index) {
        if (workers[index].cpu == cpu) {
            return &workers[index];
        }
    }
    return NULL;
}

static bool worker_ok(const struct worker *worker)
{
    return worker->affinity_error == 0 && worker->clock_error == 0 &&
           worker->observed_cpu == (int)worker->cpu &&
           worker->accesses != 0 && worker->elapsed_ns != 0;
}

static void print_worker_event(const char *kind, unsigned int round,
                               const struct worker *worker)
{
    printf("{\"schema\":\"cylon-dax-tlb-probe/v1\",\"kind\":\"%s\","
           "\"round\":%u,\"cpu\":%u,\"observed_cpu\":%d,"
           "\"accesses\":%" PRIu64 ",\"elapsed_ns\":%" PRIu64
           ",\"checksum\":\"0x%016" PRIx64
           "\",\"affinity_error\":%d,\"clock_error\":%d,"
           "\"status\":\"%s\"}\n",
           kind, round, worker->cpu, worker->observed_cpu,
           worker->accesses, worker->elapsed_ns, worker->checksum,
           worker->affinity_error, worker->clock_error,
           worker_ok(worker) ? "ok" : "error");
    fflush(stdout);
}

static int pin_main_to_cpu_zero(void)
{
    cpu_set_t affinity;

    CPU_ZERO(&affinity);
    CPU_SET(0, &affinity);
    return sched_setaffinity(0, sizeof(affinity), &affinity);
}

static void stop_workers(struct worker *workers, pthread_t *thread_ids,
                         unsigned int created)
{
    for (unsigned int index = 0; index < created; ++index) {
        dispatch(&workers[index], ACTION_STOP, 0);
    }
    for (unsigned int index = 0; index < created; ++index) {
        wait_worker(&workers[index]);
        const int error = pthread_join(thread_ids[index], NULL);
        if (error != 0) {
            fprintf(stderr, "pthread_join CPU %u: %s\n",
                    workers[index].cpu, strerror(error));
        }
    }
}

static int run_protocol(const struct options *options, uint8_t *mapping,
                        uint64_t mapping_offset, uint64_t mapping_bytes)
{
    struct worker workers[WORKER_COUNT] = {0};
    pthread_t thread_ids[WORKER_COUNT];
    struct race_state race;
    unsigned int created = 0;
    char line[256];
    int result = EXIT_SUCCESS;

    if (pin_main_to_cpu_zero() != 0 || sched_getcpu() != 0) {
        perror("pin main thread to CPU 0");
        return EXIT_FAILURE;
    }
    atomic_init(&race.done, false);
    atomic_init(&race.target_cycles, 0);
    race.target_cpu = TARGET_CPU_FIRST;

    for (unsigned int index = 0; index < WORKER_COUNT; ++index) {
        struct worker *worker = &workers[index];
        worker->cpu = index + TARGET_CPU_FIRST;
        worker->options = options;
        worker->mapping = mapping;
        worker->mapping_offset = mapping_offset;
        worker->race = &race;
        atomic_init(&worker->sequence, 0);
        atomic_init(&worker->done_sequence, 0);
        atomic_init(&worker->ready, false);
        worker->observed_cpu = -1;
        const int error =
            pthread_create(&thread_ids[index], NULL, worker_main, worker);
        if (error != 0) {
            fprintf(stderr, "pthread_create CPU %u: %s\n",
                    worker->cpu, strerror(error));
            result = EXIT_FAILURE;
            break;
        }
        ++created;
    }
    if (created != WORKER_COUNT) {
        stop_workers(workers, thread_ids, created);
        return EXIT_FAILURE;
    }
    for (unsigned int index = 0; index < WORKER_COUNT; ++index) {
        while (!atomic_load_explicit(&workers[index].ready,
                                     memory_order_acquire)) {
            _mm_pause();
        }
        if (workers[index].affinity_error != 0) {
            fprintf(stderr, "worker CPU %u affinity: %s\n",
                    workers[index].cpu,
                    strerror(workers[index].affinity_error));
            result = EXIT_FAILURE;
        }
    }
    if (result != EXIT_SUCCESS) {
        stop_workers(workers, thread_ids, created);
        return result;
    }

    printf("{\"schema\":\"cylon-dax-tlb-probe/v1\",\"kind\":\"config\","
           "\"run_id\":\"%s\",\"device\":\"%s\",\"rounds\":%u,"
           "\"page_bytes\":%" PRIu64 ",\"set_count\":%" PRIu64
           ",\"ways\":%u,\"base_lpn\":%" PRIu64
           ",\"round_stride\":%" PRIu64
           ",\"target_cpus\":[1,2,3,4,5,6],\"evictor_cpu\":7,"
           "\"mapping_offset\":%" PRIu64
           ",\"mapping_bytes\":%" PRIu64 "}\n",
           options->run_id, options->device, options->rounds,
           (uint64_t)PAGE_BYTES, options->set_count,
           COLLIDERS_PER_SET, options->base_lpn,
           options->round_stride, mapping_offset, mapping_bytes);
    printf("{\"schema\":\"cylon-dax-tlb-probe/v1\",\"kind\":\"ready\","
           "\"status\":\"ok\"}\n");
    fflush(stdout);

    while (fgets(line, sizeof(line), stdin) != NULL) {
        unsigned int round;
        unsigned int cpu;
        char extra;

        if (sscanf(line, "prime %u %c", &round, &extra) == 1 &&
            round < options->rounds) {
            for (cpu = TARGET_CPU_FIRST; cpu <= TARGET_CPU_LAST; ++cpu) {
                dispatch(worker_for_cpu(workers, cpu), ACTION_PRIME, round);
            }
            uint64_t elapsed = 0;
            uint64_t checksum = 0;
            bool ok = true;
            for (cpu = TARGET_CPU_FIRST; cpu <= TARGET_CPU_LAST; ++cpu) {
                struct worker *worker = worker_for_cpu(workers, cpu);
                wait_worker(worker);
                if (worker->elapsed_ns > elapsed) {
                    elapsed = worker->elapsed_ns;
                }
                checksum ^= worker->checksum;
                ok = ok && worker_ok(worker);
            }
            printf("{\"schema\":\"cylon-dax-tlb-probe/v1\","
                   "\"kind\":\"prime\",\"round\":%u,\"workers\":6,"
                   "\"accesses\":6,\"max_elapsed_ns\":%" PRIu64
                   ",\"checksum\":\"0x%016" PRIx64
                   "\",\"status\":\"%s\"}\n",
                   round, elapsed, checksum, ok ? "ok" : "error");
            fflush(stdout);
            if (!ok) {
                result = EXIT_FAILURE;
                break;
            }
            continue;
        }
        if (sscanf(line, "evict %u %c", &round, &extra) == 1 &&
            round < options->rounds) {
            struct worker *worker = worker_for_cpu(workers, EVICTOR_CPU);
            dispatch(worker, ACTION_EVICT, round);
            wait_worker(worker);
            print_worker_event("evict", round, worker);
            if (!worker_ok(worker) ||
                worker->accesses !=
                    (uint64_t)(TARGET_CPU_LAST - TARGET_CPU_FIRST + 1) *
                        COLLIDERS_PER_SET) {
                result = EXIT_FAILURE;
                break;
            }
            continue;
        }
        if (sscanf(line, "probe %u %u %c", &round, &cpu, &extra) == 2 &&
            round < options->rounds && cpu >= TARGET_CPU_FIRST &&
            cpu <= TARGET_CPU_LAST) {
            struct worker *worker = worker_for_cpu(workers, cpu);
            dispatch(worker, ACTION_PROBE, round);
            wait_worker(worker);
            print_worker_event("probe", round, worker);
            if (!worker_ok(worker)) {
                result = EXIT_FAILURE;
                break;
            }
            continue;
        }
        if (sscanf(line, "race %u %u %c", &round, &cpu, &extra) == 2 &&
            round < options->rounds && cpu >= TARGET_CPU_FIRST &&
            cpu <= TARGET_CPU_LAST) {
            struct worker *target = worker_for_cpu(workers, cpu);
            struct worker *evictor =
                worker_for_cpu(workers, EVICTOR_CPU);

            atomic_store_explicit(&race.done, false, memory_order_relaxed);
            atomic_store_explicit(&race.target_cycles, 0,
                                  memory_order_relaxed);
            race.target_cpu = cpu;
            dispatch(target, ACTION_RACE_TARGET, round);
            dispatch(evictor, ACTION_RACE_EVICT, round);
            wait_worker(evictor);
            wait_worker(target);
            const bool ok =
                worker_ok(target) && worker_ok(evictor) &&
                evictor->accesses == COLLIDERS_PER_SET &&
                target->accesses >= 1001;
            printf("{\"schema\":\"cylon-dax-tlb-probe/v1\","
                   "\"kind\":\"race\",\"round\":%u,\"cpu\":%u,"
                   "\"observed_cpu\":%d,\"target_accesses\":%" PRIu64
                   ",\"post_elapsed_ns\":%" PRIu64
                   ",\"target_checksum\":\"0x%016" PRIx64
                   "\",\"evictor_cpu\":%u,\"evictor_observed_cpu\":%d,"
                   "\"evictor_accesses\":%" PRIu64
                   ",\"evictor_elapsed_ns\":%" PRIu64
                   ",\"affinity_error\":%d,\"clock_error\":%d,"
                   "\"status\":\"%s\"}\n",
                   round, cpu, target->observed_cpu, target->accesses,
                   target->elapsed_ns, target->checksum, EVICTOR_CPU,
                   evictor->observed_cpu, evictor->accesses,
                   evictor->elapsed_ns,
                   target->affinity_error + evictor->affinity_error,
                   target->clock_error + evictor->clock_error,
                   ok ? "ok" : "error");
            fflush(stdout);
            if (!ok) {
                result = EXIT_FAILURE;
                break;
            }
            continue;
        }
        if (strcmp(line, "quit\n") == 0 || strcmp(line, "quit\r\n") == 0) {
            printf("{\"schema\":\"cylon-dax-tlb-probe/v1\","
                   "\"kind\":\"done\",\"status\":\"ok\"}\n");
            fflush(stdout);
            break;
        }

        fprintf(stderr, "invalid command: %s", line);
        result = EXIT_FAILURE;
        break;
    }

    if (ferror(stdin)) {
        perror("read command");
        result = EXIT_FAILURE;
    }
    stop_workers(workers, thread_ids, created);
    return result;
}

int main(int argc, char **argv)
{
    struct options options = {
        .device = "/dev/dax0.0",
        .run_id = "unspecified",
        .map_mib = DEFAULT_MAP_MIB,
        .alignment_mib = DEFAULT_ALIGNMENT_MIB,
        .set_count = DEFAULT_SET_COUNT,
        .base_lpn = DEFAULT_BASE_LPN,
        .round_stride = DEFAULT_ROUND_STRIDE,
        .rounds = DEFAULT_ROUNDS,
    };
    static const struct option long_options[] = {
        {"device", required_argument, NULL, 'd'},
        {"map-mib", required_argument, NULL, 'm'},
        {"alignment-mib", required_argument, NULL, 'a'},
        {"set-count", required_argument, NULL, 's'},
        {"base-lpn", required_argument, NULL, 'b'},
        {"round-stride", required_argument, NULL, 't'},
        {"rounds", required_argument, NULL, 'n'},
        {"run-id", required_argument, NULL, 'r'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    int option;
    while ((option = getopt_long(argc, argv, "d:m:a:s:b:t:n:r:h",
                                 long_options, NULL)) != -1) {
        uint64_t value;
        switch (option) {
        case 'd':
            options.device = optarg;
            break;
        case 'm':
            options.map_mib = parse_u64("map-mib", optarg);
            break;
        case 'a':
            options.alignment_mib = parse_u64("alignment-mib", optarg);
            break;
        case 's':
            options.set_count = parse_u64("set-count", optarg);
            break;
        case 'b':
            options.base_lpn = parse_u64("base-lpn", optarg);
            break;
        case 't':
            options.round_stride = parse_u64("round-stride", optarg);
            break;
        case 'n':
            value = parse_u64("rounds", optarg);
            if (value == 0 || value > UINT_MAX) {
                fprintf(stderr, "invalid rounds: %s\n", optarg);
                return EXIT_FAILURE;
            }
            options.rounds = (unsigned int)value;
            break;
        case 'r':
            options.run_id = optarg;
            break;
        case 'h':
            usage(stdout, argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(stderr, argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (optind != argc || options.map_mib == 0 ||
        options.map_mib > UINT64_MAX / MIB_BYTES ||
        options.alignment_mib == 0 ||
        options.alignment_mib > UINT64_MAX / MIB_BYTES ||
        options.set_count == 0 || options.round_stride == 0) {
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }
    require_safe_label(options.run_id);

    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
        perror("sched_getaffinity");
        return EXIT_FAILURE;
    }
    for (unsigned int cpu = 0; cpu <= EVICTOR_CPU; ++cpu) {
        if (!CPU_ISSET((int)cpu, &allowed)) {
            fprintf(stderr, "CPU %u is not allowed for this process\n", cpu);
            return EXIT_FAILURE;
        }
    }

    const uint64_t device_bytes = options.map_mib * MIB_BYTES;
    const uint64_t alignment = options.alignment_mib * MIB_BYTES;
    const uint64_t maximum_lpn =
        collider_lpn(&options, options.rounds - 1, TARGET_CPU_LAST,
                     COLLIDERS_PER_SET);
    const uint64_t minimum_byte =
        target_lpn(&options, 0, TARGET_CPU_FIRST) * PAGE_BYTES;
    if (maximum_lpn == UINT64_MAX ||
        maximum_lpn + 1 > UINT64_MAX / PAGE_BYTES) {
        fprintf(stderr, "LPN byte range overflow\n");
        return EXIT_FAILURE;
    }
    const uint64_t maximum_byte = (maximum_lpn + 1) * PAGE_BYTES;
    const uint64_t mapping_offset = minimum_byte - minimum_byte % alignment;
    uint64_t mapping_end = maximum_byte;
    if (mapping_end % alignment != 0) {
        const uint64_t increment = alignment - mapping_end % alignment;
        if (mapping_end > UINT64_MAX - increment) {
            fprintf(stderr, "mapping alignment overflow\n");
            return EXIT_FAILURE;
        }
        mapping_end += increment;
    }
    if (mapping_end <= mapping_offset || mapping_end > device_bytes ||
        mapping_end - mapping_offset > SIZE_MAX ||
        mapping_offset > INT64_MAX) {
        fprintf(stderr, "probe mapping is outside device bounds\n");
        return EXIT_FAILURE;
    }
    const uint64_t mapping_bytes = mapping_end - mapping_offset;

    const int descriptor = open(options.device, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        perror("open mmap target");
        return EXIT_FAILURE;
    }
    struct stat device_stat;
    if (fstat(descriptor, &device_stat) != 0) {
        perror("fstat mmap target");
        close(descriptor);
        return EXIT_FAILURE;
    }
    if (S_ISREG(device_stat.st_mode) &&
        (device_stat.st_size < 0 ||
         (uint64_t)device_stat.st_size < device_bytes)) {
        fprintf(stderr, "regular mmap target is smaller than --map-mib\n");
        close(descriptor);
        return EXIT_FAILURE;
    }

    uint8_t *mapping =
        mmap(NULL, (size_t)mapping_bytes, PROT_READ, MAP_SHARED,
             descriptor, (off_t)mapping_offset);
    if (mapping == MAP_FAILED) {
        perror("mmap target");
        close(descriptor);
        return EXIT_FAILURE;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    const int result =
        run_protocol(&options, mapping, mapping_offset, mapping_bytes);
    int final_result = result;
    if (munmap(mapping, (size_t)mapping_bytes) != 0) {
        perror("munmap target");
        final_result = EXIT_FAILURE;
    }
    if (close(descriptor) != 0) {
        perror("close mmap target");
        final_result = EXIT_FAILURE;
    }
    return final_result;
}
