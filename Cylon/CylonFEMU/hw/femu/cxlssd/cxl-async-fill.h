#ifndef CYLON_CXL_ASYNC_FILL_H
#define CYLON_CXL_ASYNC_FILL_H

#include "qemu/queue.h"

typedef struct CylonCxlFill {
    uint64_t lpn;
    uint64_t ready_time_ns;
    bool dirty;
    bool prefetch_origin;
    bool demand_joined;
    bool holds_prefetch_token;
    void *payload;
    GQueue waiters;
    size_t heap_index;
} CylonCxlFill;

typedef struct CylonCxlFillTracker {
    GTree *by_lpn;
    GPtrArray *ready_heap;
    uint64_t count;
    uint64_t prefetch_count;
    uint64_t peak;
} CylonCxlFillTracker;

void cylon_cxl_fill_tracker_init(CylonCxlFillTracker *tracker);
void cylon_cxl_fill_tracker_destroy(CylonCxlFillTracker *tracker);

CylonCxlFill *cylon_cxl_fill_lookup(CylonCxlFillTracker *tracker,
                                    uint64_t lpn);
CylonCxlFill *cylon_cxl_fill_get_or_create(CylonCxlFillTracker *tracker,
                                           uint64_t lpn,
                                           uint64_t ready_time_ns,
                                           bool prefetch_origin,
                                           void *payload,
                                           bool *created);
CylonCxlFill *cylon_cxl_fill_peek(CylonCxlFillTracker *tracker);
CylonCxlFill *cylon_cxl_fill_pop_ready(CylonCxlFillTracker *tracker,
                                       uint64_t now_ns);
CylonCxlFill *cylon_cxl_fill_pop_next(CylonCxlFillTracker *tracker);
uint64_t cylon_cxl_fill_reset_peak(CylonCxlFillTracker *tracker);

void cylon_cxl_fill_add_waiter(CylonCxlFill *fill, void *waiter);
void *cylon_cxl_fill_pop_waiter(CylonCxlFill *fill);
void cylon_cxl_fill_free(CylonCxlFill *fill);

#endif
