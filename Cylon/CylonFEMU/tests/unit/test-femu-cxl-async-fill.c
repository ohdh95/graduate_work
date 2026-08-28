#include "qemu/osdep.h"

#include "hw/femu/cxlssd/cxl-async-fill.h"

static void test_state_and_duplicate_merge(void)
{
    CylonCxlFillTracker tracker;
    CylonCxlFill *fill;
    CylonCxlFill *duplicate;
    bool created;
    int waiter_a;
    int waiter_b;

    cylon_cxl_fill_tracker_init(&tracker);

    fill = cylon_cxl_fill_get_or_create(&tracker, 42, 1000, true,
                                        &waiter_a, &created);
    g_assert_true(created);
    g_assert_nonnull(fill);
    g_assert_cmpuint(tracker.count, ==, 1);
    g_assert_cmpuint(tracker.prefetch_count, ==, 1);
    g_assert_cmpuint(tracker.peak, ==, 1);
    g_assert_true(fill->prefetch_origin);
    g_assert_true(fill->payload == &waiter_a);

    duplicate = cylon_cxl_fill_get_or_create(&tracker, 42, 2000, false,
                                             &waiter_b, &created);
    g_assert_false(created);
    g_assert_true(duplicate == fill);
    g_assert_cmpuint(duplicate->ready_time_ns, ==, 1000);
    g_assert_cmpuint(tracker.count, ==, 1);

    cylon_cxl_fill_add_waiter(fill, &waiter_a);
    cylon_cxl_fill_add_waiter(fill, &waiter_b);
    g_assert_true(cylon_cxl_fill_pop_waiter(fill) == &waiter_a);
    g_assert_true(cylon_cxl_fill_pop_waiter(fill) == &waiter_b);
    g_assert_null(cylon_cxl_fill_pop_waiter(fill));

    g_assert_null(cylon_cxl_fill_pop_ready(&tracker, 999));
    g_assert_true(cylon_cxl_fill_lookup(&tracker, 42) == fill);
    g_assert_true(cylon_cxl_fill_pop_ready(&tracker, 1000) == fill);
    g_assert_null(cylon_cxl_fill_lookup(&tracker, 42));
    g_assert_cmpuint(tracker.count, ==, 0);
    g_assert_cmpuint(tracker.prefetch_count, ==, 0);

    cylon_cxl_fill_free(fill);
    cylon_cxl_fill_tracker_destroy(&tracker);
}

static void test_completion_order_and_peak(void)
{
    CylonCxlFillTracker tracker;
    CylonCxlFill *fill;
    bool created;

    cylon_cxl_fill_tracker_init(&tracker);
    cylon_cxl_fill_get_or_create(&tracker, 30, 300, false, NULL, &created);
    g_assert_true(created);
    cylon_cxl_fill_get_or_create(&tracker, 20, 100, false, NULL, &created);
    g_assert_true(created);
    cylon_cxl_fill_get_or_create(&tracker, 10, 200, false, NULL, &created);
    g_assert_true(created);
    cylon_cxl_fill_get_or_create(&tracker, 5, 200, false, NULL, &created);
    g_assert_true(created);
    g_assert_cmpuint(tracker.peak, ==, 4);
    g_assert_cmpuint(tracker.prefetch_count, ==, 0);
    g_assert_cmpuint(cylon_cxl_fill_reset_peak(&tracker), ==, 4);
    g_assert_cmpuint(tracker.peak, ==, tracker.count);

    fill = cylon_cxl_fill_pop_ready(&tracker, 100);
    g_assert_cmpuint(fill->lpn, ==, 20);
    cylon_cxl_fill_free(fill);

    fill = cylon_cxl_fill_pop_ready(&tracker, 200);
    g_assert_cmpuint(fill->lpn, ==, 5);
    cylon_cxl_fill_free(fill);
    fill = cylon_cxl_fill_pop_ready(&tracker, 200);
    g_assert_cmpuint(fill->lpn, ==, 10);
    cylon_cxl_fill_free(fill);

    g_assert_null(cylon_cxl_fill_pop_ready(&tracker, 299));
    fill = cylon_cxl_fill_pop_next(&tracker);
    g_assert_cmpuint(fill->lpn, ==, 30);
    cylon_cxl_fill_free(fill);

    g_assert_cmpuint(tracker.count, ==, 0);
    g_assert_cmpuint(tracker.peak, ==, 4);
    g_assert_cmpuint(cylon_cxl_fill_reset_peak(&tracker), ==, 4);
    g_assert_cmpuint(tracker.peak, ==, 0);
    cylon_cxl_fill_tracker_destroy(&tracker);
}

struct expected_fill {
    uint64_t lpn;
    uint64_t ready_time_ns;
};

static int expected_fill_compare(const void *left, const void *right)
{
    const struct expected_fill *a = left;
    const struct expected_fill *b = right;

    if (a->ready_time_ns != b->ready_time_ns) {
        return a->ready_time_ns < b->ready_time_ns ? -1 : 1;
    }
    return a->lpn < b->lpn ? -1 : a->lpn > b->lpn;
}

static void test_randomized_heap_order(void)
{
    enum { FILL_COUNT = 4096 };
    CylonCxlFillTracker tracker;
    struct expected_fill *expected = g_new(struct expected_fill, FILL_COUNT);
    GRand *random = g_rand_new_with_seed(0xc7102026U);
    bool created;

    cylon_cxl_fill_tracker_init(&tracker);
    for (uint64_t lpn = 0; lpn < FILL_COUNT; ++lpn) {
        uint64_t ready_time_ns = g_rand_int_range(random, 0, 10000);

        expected[lpn] = (struct expected_fill) {
            .lpn = lpn,
            .ready_time_ns = ready_time_ns,
        };
        cylon_cxl_fill_get_or_create(&tracker, lpn, ready_time_ns,
                                     lpn % 3 == 0, NULL, &created);
        g_assert_true(created);
    }
    g_assert_cmpuint(tracker.count, ==, FILL_COUNT);
    g_assert_cmpuint(tracker.peak, ==, FILL_COUNT);

    qsort(expected, FILL_COUNT, sizeof(*expected), expected_fill_compare);
    for (size_t index = 0; index < FILL_COUNT; ++index) {
        CylonCxlFill *fill = cylon_cxl_fill_pop_next(&tracker);

        g_assert_nonnull(fill);
        g_assert_cmpuint(fill->ready_time_ns, ==,
                         expected[index].ready_time_ns);
        g_assert_cmpuint(fill->lpn, ==, expected[index].lpn);
        cylon_cxl_fill_free(fill);
    }
    g_assert_cmpuint(tracker.count, ==, 0);
    g_assert_cmpuint(tracker.prefetch_count, ==, 0);
    cylon_cxl_fill_tracker_destroy(&tracker);
    g_rand_free(random);
    g_free(expected);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/femu/cxl-async-fill/state-and-duplicate-merge",
                    test_state_and_duplicate_merge);
    g_test_add_func("/femu/cxl-async-fill/completion-order-and-peak",
                    test_completion_order_and_peak);
    g_test_add_func("/femu/cxl-async-fill/randomized-heap-order",
                    test_randomized_heap_order);
    return g_test_run();
}
