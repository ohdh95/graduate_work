#ifndef CYLON_CXL_REQUEST_LIFETIME_H
#define CYLON_CXL_REQUEST_LIFETIME_H

#include "qemu/thread.h"

typedef enum CylonCxlRequestState {
    CYLON_CXL_REQ_ALLOCATED = 1,
    CYLON_CXL_REQ_ENQUEUED,
    CYLON_CXL_REQ_DEQUEUED,
    CYLON_CXL_REQ_COMPLETING,
    CYLON_CXL_REQ_RETIRED,
    CYLON_CXL_REQ_CANCELLED,
} CylonCxlRequestState;

typedef struct CylonCxlRequestLifetime {
    QemuEvent completion;
    uint32_t refs;
    uint32_t state;
    uint64_t sequence;
} CylonCxlRequestLifetime;

typedef void (*CylonCxlCompletionHook)(void *opaque);

void cylon_cxl_request_lifetime_init(CylonCxlRequestLifetime *lifetime,
                                     uint64_t sequence);
bool cylon_cxl_request_mark_enqueued(CylonCxlRequestLifetime *lifetime);
bool cylon_cxl_request_mark_dequeued(CylonCxlRequestLifetime *lifetime);
bool cylon_cxl_request_complete(CylonCxlRequestLifetime *lifetime,
                                CylonCxlCompletionHook after_signal,
                                void *opaque);
bool cylon_cxl_request_wait(CylonCxlRequestLifetime *lifetime);
bool cylon_cxl_request_put(CylonCxlRequestLifetime *lifetime);
bool cylon_cxl_request_cancel_unpublished(CylonCxlRequestLifetime *lifetime);
uint32_t cylon_cxl_request_state(const CylonCxlRequestLifetime *lifetime);
uint32_t cylon_cxl_request_refs(const CylonCxlRequestLifetime *lifetime);

#endif
