#include "qemu/osdep.h"

#include "hw/femu/cxlssd/cxl-request-lifetime.h"
#include "qemu/atomic.h"

static bool transition(CylonCxlRequestLifetime *lifetime,
                       CylonCxlRequestState from,
                       CylonCxlRequestState to)
{
    return qatomic_cmpxchg(&lifetime->state, from, to) == from;
}

void cylon_cxl_request_lifetime_init(CylonCxlRequestLifetime *lifetime,
                                     uint64_t sequence)
{
    g_assert(lifetime != NULL);
    g_assert(sequence != 0);

    qemu_event_init(&lifetime->completion, false);
    qatomic_set(&lifetime->refs, 2);
    qatomic_set(&lifetime->state, CYLON_CXL_REQ_ALLOCATED);
    lifetime->sequence = sequence;
}

bool cylon_cxl_request_mark_enqueued(CylonCxlRequestLifetime *lifetime)
{
    return transition(lifetime, CYLON_CXL_REQ_ALLOCATED,
                      CYLON_CXL_REQ_ENQUEUED);
}

bool cylon_cxl_request_mark_dequeued(CylonCxlRequestLifetime *lifetime)
{
    return transition(lifetime, CYLON_CXL_REQ_ENQUEUED,
                      CYLON_CXL_REQ_DEQUEUED);
}

bool cylon_cxl_request_complete(CylonCxlRequestLifetime *lifetime,
                                CylonCxlCompletionHook after_signal,
                                void *opaque)
{
    if (!transition(lifetime, CYLON_CXL_REQ_DEQUEUED,
                    CYLON_CXL_REQ_COMPLETING)) {
        return false;
    }

    /*
     * The event publishes the request result, but waking a waiter is not an
     * ownership ACK.  Keep the FTL reference until qemu_event_set() has
     * returned and the state has advanced to RETIRED.
     */
    qemu_event_set(&lifetime->completion);
    if (after_signal) {
        after_signal(opaque);
    }

    return transition(lifetime, CYLON_CXL_REQ_COMPLETING,
                      CYLON_CXL_REQ_RETIRED);
}

bool cylon_cxl_request_wait(CylonCxlRequestLifetime *lifetime)
{
    uint32_t state;

    qemu_event_wait(&lifetime->completion);
    state = qatomic_read(&lifetime->state);

    /* The producer can wake before the FTL stores RETIRED. */
    return state == CYLON_CXL_REQ_COMPLETING ||
           state == CYLON_CXL_REQ_RETIRED;
}

bool cylon_cxl_request_put(CylonCxlRequestLifetime *lifetime)
{
    uint32_t old_refs = qatomic_fetch_dec(&lifetime->refs);

    g_assert(old_refs == 1 || old_refs == 2);
    if (old_refs != 1) {
        return false;
    }

    g_assert(qatomic_read(&lifetime->refs) == 0);
    g_assert(qatomic_read(&lifetime->state) == CYLON_CXL_REQ_RETIRED);
    qemu_event_destroy(&lifetime->completion);
    return true;
}

bool cylon_cxl_request_cancel_unpublished(CylonCxlRequestLifetime *lifetime)
{
    if (!transition(lifetime, CYLON_CXL_REQ_ENQUEUED,
                    CYLON_CXL_REQ_CANCELLED)) {
        return false;
    }

    /* A failed fixed-size enqueue publishes no ring entry or queue owner. */
    g_assert(qatomic_read(&lifetime->refs) == 2);
    qatomic_set(&lifetime->refs, 0);
    qemu_event_destroy(&lifetime->completion);
    return true;
}

uint32_t cylon_cxl_request_state(const CylonCxlRequestLifetime *lifetime)
{
    return qatomic_read(&lifetime->state);
}

uint32_t cylon_cxl_request_refs(const CylonCxlRequestLifetime *lifetime)
{
    return qatomic_read(&lifetime->refs);
}
