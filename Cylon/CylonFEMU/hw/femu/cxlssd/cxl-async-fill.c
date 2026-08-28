#include "qemu/osdep.h"

#include "hw/femu/cxlssd/cxl-async-fill.h"

static gint fill_lpn_compare(gconstpointer left, gconstpointer right)
{
    const CylonCxlFill *a = left;
    const CylonCxlFill *b = right;

    return a->lpn < b->lpn ? -1 : a->lpn > b->lpn;
}

static gint fill_ready_compare(const CylonCxlFill *a, const CylonCxlFill *b)
{
    if (a->ready_time_ns != b->ready_time_ns) {
        return a->ready_time_ns < b->ready_time_ns ? -1 : 1;
    }
    return fill_lpn_compare(a, b);
}

static void fill_heap_swap(CylonCxlFillTracker *tracker, size_t left,
                           size_t right)
{
    CylonCxlFill *a = g_ptr_array_index(tracker->ready_heap, left);
    CylonCxlFill *b = g_ptr_array_index(tracker->ready_heap, right);

    g_ptr_array_index(tracker->ready_heap, left) = b;
    g_ptr_array_index(tracker->ready_heap, right) = a;
    b->heap_index = left;
    a->heap_index = right;
}

static void fill_heap_sift_up(CylonCxlFillTracker *tracker, size_t index)
{
    while (index) {
        size_t parent = (index - 1) / 2;
        CylonCxlFill *fill = g_ptr_array_index(tracker->ready_heap, index);
        CylonCxlFill *parent_fill =
            g_ptr_array_index(tracker->ready_heap, parent);

        if (fill_ready_compare(parent_fill, fill) <= 0) {
            break;
        }
        fill_heap_swap(tracker, parent, index);
        index = parent;
    }
}

static void fill_heap_sift_down(CylonCxlFillTracker *tracker, size_t index)
{
    size_t length = tracker->ready_heap->len;

    while (true) {
        size_t left = index * 2 + 1;
        size_t right = left + 1;
        size_t smallest = index;

        if (left < length &&
            fill_ready_compare(g_ptr_array_index(tracker->ready_heap, left),
                               g_ptr_array_index(tracker->ready_heap,
                                                 smallest)) < 0) {
            smallest = left;
        }
        if (right < length &&
            fill_ready_compare(g_ptr_array_index(tracker->ready_heap, right),
                               g_ptr_array_index(tracker->ready_heap,
                                                 smallest)) < 0) {
            smallest = right;
        }
        if (smallest == index) {
            break;
        }
        fill_heap_swap(tracker, index, smallest);
        index = smallest;
    }
}

void cylon_cxl_fill_tracker_init(CylonCxlFillTracker *tracker)
{
    g_assert(tracker != NULL);

    tracker->by_lpn = g_tree_new(fill_lpn_compare);
    tracker->ready_heap = g_ptr_array_new();
    tracker->count = 0;
    tracker->prefetch_count = 0;
    tracker->peak = 0;
}

void cylon_cxl_fill_tracker_destroy(CylonCxlFillTracker *tracker)
{
    g_assert(tracker != NULL);
    g_assert(tracker->count == 0);
    g_assert(tracker->prefetch_count == 0);
    g_assert(tracker->ready_heap->len == 0);

    g_tree_destroy(tracker->by_lpn);
    g_ptr_array_unref(tracker->ready_heap);
    tracker->by_lpn = NULL;
    tracker->ready_heap = NULL;
}

CylonCxlFill *cylon_cxl_fill_lookup(CylonCxlFillTracker *tracker,
                                    uint64_t lpn)
{
    CylonCxlFill key = { .lpn = lpn };

    return g_tree_lookup(tracker->by_lpn, &key);
}

CylonCxlFill *cylon_cxl_fill_get_or_create(CylonCxlFillTracker *tracker,
                                           uint64_t lpn,
                                           uint64_t ready_time_ns,
                                           bool prefetch_origin,
                                           void *payload,
                                           bool *created)
{
    CylonCxlFill *fill = cylon_cxl_fill_lookup(tracker, lpn);

    if (fill) {
        if (created) {
            *created = false;
        }
        return fill;
    }

    fill = g_new0(CylonCxlFill, 1);
    fill->lpn = lpn;
    fill->ready_time_ns = ready_time_ns;
    fill->prefetch_origin = prefetch_origin;
    fill->payload = payload;
    g_queue_init(&fill->waiters);
    fill->heap_index = tracker->ready_heap->len;

    g_tree_insert(tracker->by_lpn, fill, fill);
    g_ptr_array_add(tracker->ready_heap, fill);
    fill_heap_sift_up(tracker, fill->heap_index);
    tracker->count++;
    if (prefetch_origin) {
        tracker->prefetch_count++;
    }
    tracker->peak = MAX(tracker->peak, tracker->count);

    if (created) {
        *created = true;
    }
    return fill;
}

CylonCxlFill *cylon_cxl_fill_peek(CylonCxlFillTracker *tracker)
{
    if (!tracker->ready_heap->len) {
        return NULL;
    }
    return g_ptr_array_index(tracker->ready_heap, 0);
}

static CylonCxlFill *cylon_cxl_fill_pop_head(CylonCxlFillTracker *tracker)
{
    CylonCxlFill *fill;
    CylonCxlFill *last;
    size_t length = tracker->ready_heap->len;
    CylonCxlFill key;

    if (!length) {
        return NULL;
    }

    fill = g_ptr_array_index(tracker->ready_heap, 0);
    last = g_ptr_array_index(tracker->ready_heap, length - 1);
    g_ptr_array_set_size(tracker->ready_heap, length - 1);
    if (length > 1) {
        g_ptr_array_index(tracker->ready_heap, 0) = last;
        last->heap_index = 0;
        fill_heap_sift_down(tracker, 0);
    }
    fill->heap_index = SIZE_MAX;

    key.lpn = fill->lpn;
    g_assert(g_tree_remove(tracker->by_lpn, &key));
    g_assert(tracker->count > 0);
    tracker->count--;
    if (fill->prefetch_origin) {
        g_assert(tracker->prefetch_count > 0);
        tracker->prefetch_count--;
    }
    return fill;
}

CylonCxlFill *cylon_cxl_fill_pop_ready(CylonCxlFillTracker *tracker,
                                       uint64_t now_ns)
{
    CylonCxlFill *fill = cylon_cxl_fill_peek(tracker);

    if (!fill || fill->ready_time_ns > now_ns) {
        return NULL;
    }
    return cylon_cxl_fill_pop_head(tracker);
}

CylonCxlFill *cylon_cxl_fill_pop_next(CylonCxlFillTracker *tracker)
{
    return cylon_cxl_fill_pop_head(tracker);
}

uint64_t cylon_cxl_fill_reset_peak(CylonCxlFillTracker *tracker)
{
    uint64_t previous_peak;

    g_assert(tracker != NULL);
    previous_peak = tracker->peak;
    tracker->peak = tracker->count;
    return previous_peak;
}

void cylon_cxl_fill_add_waiter(CylonCxlFill *fill, void *waiter)
{
    g_assert(fill != NULL);
    g_assert(waiter != NULL);
    g_queue_push_tail(&fill->waiters, waiter);
}

void *cylon_cxl_fill_pop_waiter(CylonCxlFill *fill)
{
    g_assert(fill != NULL);
    return g_queue_pop_head(&fill->waiters);
}

void cylon_cxl_fill_free(CylonCxlFill *fill)
{
    g_assert(fill != NULL);
    g_assert(g_queue_is_empty(&fill->waiters));
    g_free(fill);
}
