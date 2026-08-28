#include "qemu/osdep.h"

#include "hw/femu/cxlssd/cxl-request-lifetime.h"
#include "hw/femu/inc/rte_ring.h"
#include "qemu/atomic.h"
#include "qemu/thread.h"

#define STRESS_PRODUCERS 8
#define STRESS_RING_SIZE 16
#define DEFAULT_REQUESTS_PER_PRODUCER 1500000
#define DEFAULT_STRESS_SEEDS 1
#define MAX_REQUESTS_PER_PRODUCER 5000000
#define MAX_STRESS_SEEDS 16

typedef struct TestRequest {
    CylonCxlRequestLifetime lifetime;
    uint64_t reply;
} TestRequest;

typedef struct StressContext {
    struct rte_ring *ring;
    uint64_t requests_per_producer;
    uint32_t seed;
    uint64_t consumed;
    uint64_t destroyed_by_producer;
    uint64_t destroyed_by_consumer;
} StressContext;

typedef struct ProducerContext {
    StressContext *stress;
    unsigned producer;
} ProducerContext;

typedef struct ProducerFirstContext {
    TestRequest *request;
    QemuSemaphore producer_put;
    bool consumer_last;
} ProducerFirstContext;

static uint64_t request_sequence(unsigned producer, uint32_t seed,
                                 uint64_t iteration)
{
    return ((uint64_t)(producer + 1) << 56) |
           ((uint64_t)(seed + 1) << 48) |
           (iteration + 1);
}

static uint64_t env_u64(const char *name, uint64_t default_value,
                        uint64_t maximum)
{
    const char *text = g_getenv(name);
    char *end = NULL;
    uint64_t value;

    if (!text || !*text) {
        return default_value;
    }
    value = g_ascii_strtoull(text, &end, 10);
    g_assert(end && *end == '\0');
    g_assert(value > 0 && value <= maximum);
    return value;
}

static void free_on_last(TestRequest *request, uint64_t *counter)
{
    if (cylon_cxl_request_put(&request->lifetime)) {
        qatomic_inc(counter);
        g_free(request);
    }
}

static void *stress_producer(void *opaque)
{
    ProducerContext *producer = opaque;
    StressContext *stress = producer->stress;

    for (uint64_t iteration = 0;
         iteration < stress->requests_per_producer; iteration++) {
        uint64_t sequence = request_sequence(producer->producer,
                                             stress->seed, iteration);
        TestRequest *request = g_new0(TestRequest, 1);
        void *entry = request;

        cylon_cxl_request_lifetime_init(&request->lifetime, sequence);
        g_assert(cylon_cxl_request_mark_enqueued(&request->lifetime));
        g_assert_cmpuint(femu_ring_enqueue(stress->ring, &entry, 1), ==, 1);
        g_assert(cylon_cxl_request_wait(&request->lifetime));
        g_assert_cmpuint(request->reply, ==,
                         sequence ^ UINT64_C(0xd1b54a32d192ed03));
        free_on_last(request, &stress->destroyed_by_producer);
    }
    return NULL;
}

static void *stress_consumer(void *opaque)
{
    StressContext *stress = opaque;
    uint64_t expected[STRESS_PRODUCERS] = { 0 };
    const uint64_t total = stress->requests_per_producer * STRESS_PRODUCERS;

    while (qatomic_read(&stress->consumed) < total) {
        TestRequest *request = NULL;
        unsigned producer;
        uint32_t seed;
        uint64_t iteration;

        if (femu_ring_dequeue(stress->ring, (void **)&request, 1) != 1) {
            _mm_pause();
            continue;
        }
        g_assert(request != NULL);
        g_assert(cylon_cxl_request_mark_dequeued(&request->lifetime));

        producer = (request->lifetime.sequence >> 56) - 1;
        seed = ((request->lifetime.sequence >> 48) & 0xff) - 1;
        iteration = (request->lifetime.sequence &
                     UINT64_C(0x0000ffffffffffff)) - 1;
        g_assert_cmpuint(producer, <, STRESS_PRODUCERS);
        g_assert_cmpuint(seed, ==, stress->seed);
        g_assert_cmpuint(iteration, ==, expected[producer]);
        expected[producer]++;

        request->reply = request->lifetime.sequence ^
                         UINT64_C(0xd1b54a32d192ed03);
        g_assert(cylon_cxl_request_complete(&request->lifetime, NULL, NULL));
        qatomic_inc(&stress->consumed);
        free_on_last(request, &stress->destroyed_by_consumer);
    }

    for (unsigned producer = 0; producer < STRESS_PRODUCERS; producer++) {
        g_assert_cmpuint(expected[producer], ==,
                         stress->requests_per_producer);
    }
    return NULL;
}

static void run_stress_seed(uint64_t requests_per_producer, uint32_t seed)
{
    StressContext stress = {
        .ring = femu_ring_create(FEMU_RING_TYPE_MP_SC, STRESS_RING_SIZE),
        .requests_per_producer = requests_per_producer,
        .seed = seed,
    };
    ProducerContext producers[STRESS_PRODUCERS];
    QemuThread producer_threads[STRESS_PRODUCERS];
    QemuThread consumer_thread;
    const uint64_t total = requests_per_producer * STRESS_PRODUCERS;

    g_assert(stress.ring != NULL);
    qemu_thread_create(&consumer_thread, "cxl-life-consumer",
                       stress_consumer, &stress, QEMU_THREAD_JOINABLE);
    for (unsigned producer = 0; producer < STRESS_PRODUCERS; producer++) {
        producers[producer].stress = &stress;
        producers[producer].producer = producer;
        qemu_thread_create(&producer_threads[producer], "cxl-life-producer",
                           stress_producer, &producers[producer],
                           QEMU_THREAD_JOINABLE);
    }
    for (unsigned producer = 0; producer < STRESS_PRODUCERS; producer++) {
        qemu_thread_join(&producer_threads[producer]);
    }
    qemu_thread_join(&consumer_thread);

    g_assert_cmpuint(qatomic_read(&stress.consumed), ==, total);
    g_assert_cmpuint(qatomic_read(&stress.destroyed_by_producer) +
                     qatomic_read(&stress.destroyed_by_consumer), ==, total);
    g_assert_cmpuint(femu_ring_count(stress.ring), ==, 0);
    femu_ring_free(stress.ring);
}

static void test_mp_sc_stress(void)
{
    uint64_t requests = env_u64("CYLON_CXL_STRESS_REQUESTS",
                                DEFAULT_REQUESTS_PER_PRODUCER,
                                MAX_REQUESTS_PER_PRODUCER);
    uint32_t seeds = env_u64("CYLON_CXL_STRESS_SEEDS",
                             DEFAULT_STRESS_SEEDS, MAX_STRESS_SEEDS);

    for (uint32_t seed = 0; seed < seeds; seed++) {
        run_stress_seed(requests, seed);
    }
}

static void producer_first_hook(void *opaque)
{
    ProducerFirstContext *context = opaque;

    qemu_sem_wait(&context->producer_put);
}

static void *producer_first_consumer(void *opaque)
{
    ProducerFirstContext *context = opaque;

    g_assert(cylon_cxl_request_complete(&context->request->lifetime,
                                        producer_first_hook, context));
    context->consumer_last =
        cylon_cxl_request_put(&context->request->lifetime);
    return NULL;
}

static void test_producer_put_before_ftl_retire(void)
{
    ProducerFirstContext context = {
        .request = g_new0(TestRequest, 1),
    };
    QemuThread consumer;

    qemu_sem_init(&context.producer_put, 0);
    cylon_cxl_request_lifetime_init(&context.request->lifetime, 1);
    g_assert(cylon_cxl_request_mark_enqueued(&context.request->lifetime));
    g_assert(cylon_cxl_request_mark_dequeued(&context.request->lifetime));
    qemu_thread_create(&consumer, "cxl-life-producer-first",
                       producer_first_consumer, &context,
                       QEMU_THREAD_JOINABLE);

    g_assert(cylon_cxl_request_wait(&context.request->lifetime));
    g_assert_cmpuint(cylon_cxl_request_state(&context.request->lifetime), ==,
                     CYLON_CXL_REQ_COMPLETING);
    g_assert(!cylon_cxl_request_put(&context.request->lifetime));
    g_assert_cmpuint(cylon_cxl_request_refs(&context.request->lifetime), ==, 1);
    qemu_sem_post(&context.producer_put);
    qemu_thread_join(&consumer);

    g_assert(context.consumer_last);
    g_assert_cmpuint(cylon_cxl_request_state(&context.request->lifetime), ==,
                     CYLON_CXL_REQ_RETIRED);
    qemu_sem_destroy(&context.producer_put);
    g_free(context.request);
}

static void test_ftl_put_before_producer_wait(void)
{
    TestRequest *request = g_new0(TestRequest, 1);

    cylon_cxl_request_lifetime_init(&request->lifetime, 2);
    g_assert(cylon_cxl_request_mark_enqueued(&request->lifetime));
    g_assert(cylon_cxl_request_mark_dequeued(&request->lifetime));
    request->reply = 99;
    g_assert(cylon_cxl_request_complete(&request->lifetime, NULL, NULL));
    g_assert(!cylon_cxl_request_put(&request->lifetime));
    g_assert(cylon_cxl_request_wait(&request->lifetime));
    g_assert_cmpuint(request->reply, ==, 99);
    g_assert(cylon_cxl_request_put(&request->lifetime));
    g_free(request);
}

static void test_cancel_and_duplicate_dequeue(void)
{
    TestRequest *cancelled = g_new0(TestRequest, 1);
    TestRequest *duplicate = g_new0(TestRequest, 1);

    cylon_cxl_request_lifetime_init(&cancelled->lifetime, 3);
    g_assert(cylon_cxl_request_mark_enqueued(&cancelled->lifetime));
    g_assert(cylon_cxl_request_cancel_unpublished(&cancelled->lifetime));
    g_assert_cmpuint(cylon_cxl_request_state(&cancelled->lifetime), ==,
                     CYLON_CXL_REQ_CANCELLED);
    g_assert_cmpuint(cylon_cxl_request_refs(&cancelled->lifetime), ==, 0);
    g_free(cancelled);

    cylon_cxl_request_lifetime_init(&duplicate->lifetime, 4);
    g_assert(cylon_cxl_request_mark_enqueued(&duplicate->lifetime));
    g_assert(cylon_cxl_request_mark_dequeued(&duplicate->lifetime));
    g_assert(!cylon_cxl_request_mark_dequeued(&duplicate->lifetime));
    g_assert(cylon_cxl_request_complete(&duplicate->lifetime, NULL, NULL));
    g_assert(!cylon_cxl_request_put(&duplicate->lifetime));
    g_assert(cylon_cxl_request_put(&duplicate->lifetime));
    g_free(duplicate);
}

static void test_actual_ring_full_cancel(void)
{
    struct rte_ring *ring = femu_ring_create(FEMU_RING_TYPE_MP_SC, 2);
    TestRequest *candidate = g_new0(TestRequest, 1);
    int sentinel;
    void *entry = &sentinel;
    void *dequeued = NULL;

    g_assert(ring != NULL);
    /* A size-two non-exact ring has one usable slot. */
    g_assert_cmpuint(femu_ring_enqueue(ring, &entry, 1), ==, 1);

    cylon_cxl_request_lifetime_init(&candidate->lifetime, 5);
    g_assert(cylon_cxl_request_mark_enqueued(&candidate->lifetime));
    entry = candidate;
    g_assert_cmpuint(femu_ring_enqueue(ring, &entry, 1), ==, 0);
    g_assert(cylon_cxl_request_cancel_unpublished(&candidate->lifetime));
    g_assert_cmpuint(cylon_cxl_request_state(&candidate->lifetime), ==,
                     CYLON_CXL_REQ_CANCELLED);
    g_assert_cmpuint(cylon_cxl_request_refs(&candidate->lifetime), ==, 0);
    g_free(candidate);

    g_assert_cmpuint(femu_ring_dequeue(ring, &dequeued, 1), ==, 1);
    g_assert(dequeued == &sentinel);
    g_assert_cmpuint(femu_ring_count(ring), ==, 0);
    femu_ring_free(ring);
}

static void test_actual_duplicate_ring_entry_rejected(void)
{
    struct rte_ring *ring = femu_ring_create(FEMU_RING_TYPE_MP_SC, 4);
    TestRequest *request = g_new0(TestRequest, 1);
    TestRequest *dequeued = NULL;
    void *entries[2];
    unsigned payload_accesses = 0;

    g_assert(ring != NULL);
    cylon_cxl_request_lifetime_init(&request->lifetime, 6);
    g_assert(cylon_cxl_request_mark_enqueued(&request->lifetime));

    /* Deliberately simulate a corrupted queue containing one pointer twice. */
    entries[0] = request;
    entries[1] = request;
    g_assert_cmpuint(femu_ring_enqueue(ring, entries, 2), ==, 2);

    g_assert_cmpuint(femu_ring_dequeue(ring, (void **)&dequeued, 1), ==, 1);
    g_assert(dequeued == request);
    g_assert(cylon_cxl_request_mark_dequeued(&dequeued->lifetime));
    payload_accesses++;
    dequeued->reply = 123;
    g_assert(cylon_cxl_request_complete(&dequeued->lifetime, NULL, NULL));
    g_assert(!cylon_cxl_request_put(&dequeued->lifetime));

    /* Keep the producer reference alive so this corruption check is not UAF. */
    dequeued = NULL;
    g_assert_cmpuint(femu_ring_dequeue(ring, (void **)&dequeued, 1), ==, 1);
    g_assert(dequeued == request);
    g_assert(!cylon_cxl_request_mark_dequeued(&dequeued->lifetime));
    g_assert_cmpuint(payload_accesses, ==, 1);
    g_assert_cmpuint(cylon_cxl_request_state(&dequeued->lifetime), ==,
                     CYLON_CXL_REQ_RETIRED);

    g_assert(cylon_cxl_request_wait(&request->lifetime));
    g_assert_cmpuint(request->reply, ==, 123);
    g_assert(cylon_cxl_request_put(&request->lifetime));
    g_free(request);
    g_assert_cmpuint(femu_ring_count(ring), ==, 0);
    femu_ring_free(ring);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/femu/cxl-request-lifetime/producer-first",
                    test_producer_put_before_ftl_retire);
    g_test_add_func("/femu/cxl-request-lifetime/ftl-first",
                    test_ftl_put_before_producer_wait);
    g_test_add_func("/femu/cxl-request-lifetime/cancel-and-duplicate",
                    test_cancel_and_duplicate_dequeue);
    g_test_add_func("/femu/cxl-request-lifetime/actual-ring-full-cancel",
                    test_actual_ring_full_cancel);
    g_test_add_func("/femu/cxl-request-lifetime/actual-duplicate-ring-entry",
                    test_actual_duplicate_ring_entry_rejected);
    g_test_add_func("/femu/cxl-request-lifetime/mp-sc-stress",
                    test_mp_sc_stress);
    return g_test_run();
}
