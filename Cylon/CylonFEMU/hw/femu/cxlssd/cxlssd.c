#include "../nvme.h"
#include "../bbssd/ftl.h"
#include "../kvm_ext.h"
#include <inttypes.h>
#include "qemu/main-loop.h"
FILE *mem_acc_log_file;
static uint64_t cxl_request_sequence;

static struct cxl_req *cxl_request_new(int command, lpn_t lpn,
                                       uint64_t start_time,
                                       bool map_digest_requested)
{
    struct cxl_req *request = g_new0(struct cxl_req, 1);
    uint64_t sequence = qatomic_fetch_inc(&cxl_request_sequence) + 1;

    if (sequence == 0) {
        femu_err("CXL request sequence wrapped\n");
        abort();
    }

    request->ncmd.type = USER_IO;
    request->ncmd.cmd = command;
    request->ncmd.stime = start_time;
    request->lpn = lpn;
    request->expire_time = start_time;
    request->map_digest_requested = map_digest_requested;
    cylon_cxl_request_lifetime_init(&request->lifetime, sequence);
    return request;
}

static void cxl_request_enqueue(FemuCtrl *n, struct cxl_req *request)
{
    int rc;

    if (!cylon_cxl_request_mark_enqueued(&request->lifetime)) {
        femu_err("CXL request publish state differs: seq=%" PRIu64
                 " state=%u\n", request->lifetime.sequence,
                 cylon_cxl_request_state(&request->lifetime));
        abort();
    }

    rc = femu_ring_enqueue(n->cxl_req, (void *)&request, 1);
    if (rc != 1) {
        femu_err("CXL request enqueue failed: seq=%" PRIu64 " ret=%d\n",
                 request->lifetime.sequence, rc);
        if (!cylon_cxl_request_cancel_unpublished(&request->lifetime)) {
            femu_err("CXL request cancellation state differs\n");
            abort();
        }
        g_free(request);
        abort();
    }
}

static void cxlssd_init_ctrl_str(FemuCtrl *n)
{
    static int fsid_vcxlssd = 0;
    const char *vcxlssdssd_mn = "FEMU CXL-SSD Controller";
    const char *vcxlssdssd_sn = "vSSD";

    nvme_set_ctrl_name(n, vcxlssdssd_mn, vcxlssdssd_sn, &fsid_vcxlssd);
}


/* cxlssd <= [bb <=> black-box] */
static void cxlssd_init(FemuCtrl *n, Error **errp)
{
    femu_kvm_log_init();

    struct ssd *ssd = n->ssd = g_malloc0(sizeof(struct ssd));
    cxlssd_init_ctrl_str(n);

    ssd->dataplane_started_ptr = &n->dataplane_started;
    ssd->ssdname = (char *)n->devname;
    

    femu_debug("Starting FEMU in CXL-SSD mode ...\n");
    
    /* init queue */
    n->cxl_req = femu_ring_create(FEMU_RING_TYPE_MP_SC, FEMU_MAX_INF_REQS);
    if (!n->cxl_req) {
        femu_err("Failed to create ring (n->cxl_req) ...\n");
        abort();
    }
    
    femu_kvm_set_user_memory_region(n);
    ssd_init(n);

    mem_acc_log_file = fopen("/home/necsst/cxlssd_log.txt", "w+");

    n->io_logfile = NULL;//fopen("/home/necsst/cxlssd_io.log", "w+");
    n->lognum = 0;
}

static void cxlssd_exit(FemuCtrl *n)
{

    femu_ring_free(n->cxl_req);

    femu_kvm_del_user_memory_region(n);
    ssd_reset(n);
}

static void cxlssd_flip(FemuCtrl *n, NvmeCmd *cmd)
{
    struct ssd *ssd = n->ssd;
    int64_t cdw10 = le64_to_cpu(cmd->cdw10);

    switch (cdw10) {
    case FEMU_ENABLE_GC_DELAY:
        ssd->sp.enable_gc_delay = true;
        femu_log("%s,FEMU GC Delay Emulation [Enabled]!\n", n->devname);
        break;
    case FEMU_DISABLE_GC_DELAY:
        ssd->sp.enable_gc_delay = false;
        femu_log("%s,FEMU GC Delay Emulation [Disabled]!\n", n->devname);
        break;
    case FEMU_ENABLE_DELAY_EMU:
        ssd->sp.pg_rd_lat = NAND_READ_LATENCY;
        ssd->sp.pg_wr_lat = NAND_PROG_LATENCY;
        ssd->sp.blk_er_lat = NAND_ERASE_LATENCY;
        ssd->sp.ch_xfer_lat = 0;
        femu_log("%s,FEMU Delay Emulation [Enabled]!\n", n->devname);
        break;
    case FEMU_DISABLE_DELAY_EMU:
        ssd->sp.pg_rd_lat = 0;
        ssd->sp.pg_wr_lat = 0;
        ssd->sp.blk_er_lat = 0;
        ssd->sp.ch_xfer_lat = 0;
        femu_log("%s,FEMU Delay Emulation [Disabled]!\n", n->devname);
        break;
    case FEMU_RESET_ACCT:
        n->nr_tt_ios = 0;
        n->nr_tt_late_ios = 0;
        femu_log("%s,Reset tt_late_ios/tt_ios,%lu/%lu\n", n->devname,
                n->nr_tt_late_ios, n->nr_tt_ios);
        break;
    case FEMU_ENABLE_LOG:
        n->print_log = true;
        femu_log("%s,Log print [Enabled]!\n", n->devname);
        break;
    case FEMU_DISABLE_LOG:
        n->print_log = false;
        femu_log("%s,Log print [Disabled]!\n", n->devname);
        break;
    default:
        printf("FEMU:%s,Not implemented flip cmd (%lu)\n", n->devname, cdw10);
    }
}

static uint16_t cxlssd_nvme_rw(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                        NvmeRequest *req)
{
    return nvme_rw(n, ns, cmd, req);
}

static uint16_t cxlssd_io_cmd(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                        NvmeRequest *req)
{
    switch (cmd->opcode) {
    case NVME_CMD_READ:
    case NVME_CMD_WRITE:
        return cxlssd_nvme_rw(n, ns, cmd, req);
    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

static uint16_t cxlssd_admin_cmd(FemuCtrl *n, NvmeCmd *cmd)
{
    switch (cmd->opcode) {
    case NVME_ADM_CMD_FEMU_FLIP:
        cxlssd_flip(n, cmd);
        return NVME_SUCCESS;
    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

static void wait_for_buf_update(FemuCtrl *n, uint64_t addr, int c)
{   
    bool had_bql;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    lpn_t lpn = addr >> 12;
    struct cxl_req *myreq = cxl_request_new(c, lpn, now, false);

    /* Send the request to the single FTL consumer. */
    cxl_request_enqueue(n, myreq);

    /*
     * Each producer waits only for its own request.  qemu_event_set/wait
     * publishes expire_time without a shared response queue or dispatcher.
     *
     * KVM MMIO enters this callback with the BQL held.  Do not retain that
     * global lock while waiting for the FTL and the modeled NAND completion:
     * doing so limits the CXL request queue to one outstanding vCPU request
     * and hides channel/LUN parallelism.  Device lookup, backing-memory
     * access, enqueue, and request destruction remain under the caller's
     * original BQL scope.
     */
    had_bql = qemu_mutex_iothread_locked();
    if (had_bql) {
        qemu_mutex_unlock_iothread();
    }
    if (!cylon_cxl_request_wait(&myreq->lifetime)) {
        femu_err("CXL request wait state differs: seq=%" PRIu64
                 " state=%u\n", myreq->lifetime.sequence,
                 cylon_cxl_request_state(&myreq->lifetime));
        abort();
    }
    const uint64_t expire_time = myreq->expire_time;
    do {
        now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    } while (now < expire_time);
    if (had_bql) {
        qemu_mutex_lock_iothread();
    }

    cylon_cxl_req_put(myreq);
}

// #include <execinfo.h>
// static void print_backtrace(void) {
//     void *bt[32];
//     int size = backtrace(bt, 32);
//     backtrace_symbols_fd(bt, size, STDERR_FILENO);
// }


int cnt = 0;
char str[128];
uint64_t prev = 0;
static MemTxResult cxlssd_mem_read(void *opaque, uint64_t addr, uint64_t *data, unsigned size, MemTxAttrs attrs)
{
    // fflush(stdout);
    // fflush(stdin);
    // // prev = scanf("%d",&cnt);
    // char* ss = fgets(str, sizeof(str), stdin);
    // if (ss) {
    //     ;
    // }

    FemuCtrl *n = (FemuCtrl*)opaque;
    struct buffer *buffer = &n->ssd->dram_buffer;
    assert(addr < n->mbe->size);
    qatomic_inc(&buffer->cxl_mmio_read_callbacks);
    // qemu_mutex_lock(&n->mutex);
    // printf("[tid %d] read 0x%lx, size: %u\n", gettid(), addr, size);
    // if (cnt < 10) {
    //     printf("read 0x%lx\n", addr);
    //     cnt++;
    // }
    // if (attrs.dual_mode_dma) {
    //     // printf("DMA map addr: 0x%lx, size: 0x%x\n",addr, size);
    //     femu_kvm_spte_clear_mmio_flag((uint64_t)(n->mbe->base_gpa >> 12) + (addr >> 12), addr >> 12);
    //     return MEMTX_OK;
    // }
    
    // void *backend_addr = get_backend_nand_ptr(n->mbe, addr>>12, 1<<12);
    void *backend_addr = n->mbe->logical_space + addr;
    // void *backend_addr = n->mbe->buf_space + addr;
    // lpn_t lpn = addr >> 12;

    memcpy(data, backend_addr, size);
    // uint64_t result = 0;
    // for (unsigned i = 0; i < size; ++i) {
    //     result |= ((uint64_t)((uint8_t *)backend_addr)[i]) << (i * 8);
    // }
    // *data = result;
    // address_space_read(&n->cxl_as, addr, attrs, data, size);
    // printf("Read addr: 0x%lx, size: 0x%x, val: 0x%lx\n",addr, size, *data);
    // print_backtrace();  
    // printf("Read addr: %lx, lpn: %lx, size: %u, src:%lx, dest:%lx\n", addr, addr>>12,size, *(uint64_t *)backend_addr, *data);
    // fprintf(mem_acc_log_file, "0x%lx %d\n", addr, size);
    // if (!femu_kvm_spte_clear_mmio_flag((uint64_t)(n->mbe->base_gpa >> 12) + lpn, lpn)) {
    //     // printf("Fail addr: %lx, size: %u\n", addr, size);
    // }
    // if (attrs.dual_mode_dma) {
    //     return MEMTX_OK;
    // }

    if (!n->cxl_skip_ftl) {
        wait_for_buf_update(n, addr, CXL_READ);
    } else {
        qatomic_inc(&buffer->cxl_skip_ftl_bypasses);
    }
    // qemu_mutex_unlock(&n->mutex);
    return MEMTX_OK;
}

static MemTxResult cxlssd_mem_write(void *opaque, uint64_t addr, uint64_t data, unsigned size, MemTxAttrs attrs)
{
    
    // printf("Write addr: 0x%lx, size: 0x%x, val: 0x%lx\n",addr, size, data);
    // fflush(stdout);
    // fflush(stdin);
    // // prev = scanf("%d",&cnt);
    // char* ss = fgets(str, sizeof(str), stdin);
    // if (ss) {
    //     ;
    // }
    
    FemuCtrl *n = (FemuCtrl*)opaque;
    assert(addr < n->mbe->size);
    // qemu_mutex_lock(&n->mutex);
    // printf("[tid %d] write 0x%lx, size: %u\n", gettid(), addr, size);
    // printf("write 0x%lx\n", addr);
    // void *backend_addr = get_backend_nand_ptr(n->mbe, addr>>12, 1<<12);
    void *backend_addr = n->mbe->logical_space + addr;
    // void *backend_addr = n->mbe->buf_space + addr;
    // lpn_t lpn = addr >> 12;
    // if (attrs.dual_mode_dma) {
    //     // printf("DMA unmap addr: 0x%lx, size: 0x%x\n",addr, size);
    //     femu_kvm_spte_set_mmio_flag((uint64_t)(n->mbe->base_gpa >> 12) + (addr >> 12), addr >> 12);
    //     return MEMTX_OK;
    // }
    // if (cnt < 10) {
    //     printf("write 0x%lx\n", addr);
    //     cnt++;
    // }
    
    // if (attrs.unspecified) {
    memcpy(backend_addr, &data, size);
    // }
    // else {
    // memcpy(backend_addr, &data, size);
    // for (unsigned i = 0; i < size; ++i)
    //     ((uint8_t *)backend_addr)[i] = (data >> (i * 8)) & 0xff;
    // }
    // address_space_write(&n->cxl_as, addr, attrs, &data, size);
    
    // if (attrs.dual_mode_dma) {
    //     printf("DMA Write addr: 0x%lx, size: 0x%x\n",addr, size);
    //     return MEMTX_OK;
    // }

    // printf("Write addr: %lx, lpn: %lx, size: %u, src:%lx, dest:%lx\n", addr, addr>>12,size, data, *(uint64_t *)backend_addr);
    // print_backtrace();
    
    // if (!femu_kvm_spte_clear_mmio_flag((uint64_t)(n->mbe->base_gpa >> 12) + lpn, lpn)) {
    //     // printf("Fail addr: %lx, size: %u\n", addr, size);
    // }
    
    if(!n->cxl_skip_ftl)
        wait_for_buf_update(n, addr, CXL_WRITE);
    // qemu_mutex_unlock(&n->mutex);
    return MEMTX_OK;
}

#ifdef LSA_TROLL
static void req_ftl(FemuCtrl *n, int c, CylonFtlMapDigest *map_digest)
{   
    struct cxl_req *myreq = cxl_request_new(c, 0, 0,
                                            map_digest != NULL);

    /* Send the control request and wait for the FTL-side barrier. */
    cxl_request_enqueue(n, myreq);
    if (!cylon_cxl_request_wait(&myreq->lifetime)) {
        femu_err("CXL control wait state differs: seq=%" PRIu64
                 " state=%u\n", myreq->lifetime.sequence,
                 cylon_cxl_request_state(&myreq->lifetime));
        abort();
    }

    if (map_digest) {
        *map_digest = myreq->map_digest;
    }
    cylon_cxl_req_put(myreq);
}
#endif

static const char policy_str[5][10] = {
    "NONE", "LIFO", "FIFO", "CLOCK", "S3FIFO"
};

static void format_buffer_way(const struct buffer *buffer, char *text,
                              size_t text_size)
{
    if (buffer->way == WAY_FULL) {
        snprintf(text, text_size, "full");
    } else {
        snprintf(text, text_size, "%u", 1U << buffer->way);
    }
}

static void print_buffer_policy(FILE *stream, const FemuCtrl *n,
                                const struct buffer *buffer, uint64_t offset)
{
    char way[32];
    format_buffer_way(buffer, way, sizeof(way));
    fprintf(stream,
            "NAND size: %u MB, Buffer size: %u MB, eviction: %s, "
            "prefetch: %d, way: %s, == %" PRIu64 " ==\n",
            n->memsz, n->bufsz,
            buffer->policy < POLICY_MAX ?
                policy_str[buffer->policy] : "INVALID",
            buffer->degree, way, offset);
}


static FILE *f = NULL;
static uint16_t get_lsa(struct FemuCtrl *n, void *buf, uint64_t size, uint64_t offset)
{
    // printf("get lsa\n");
#ifdef LSA_TROLL    
    int rc = 0;
    
    char new_logfilename[100];
    uint64_t time;
    f = fopen("/home/necsst/cxlssd_buffer.txt", "a+");
    assert(f != NULL);
    struct buffer *buffer = &n->ssd->dram_buffer;
    switch(size) {
    case 1: { //print hit/miss count
        CylonFtlMapDigest map_digest;
        uint64_t mmio_read_callbacks;
        uint64_t skip_ftl_bypasses;
        uint64_t mapped_nand_reads;
        uint64_t unmapped_read_alloc_writes;
        uint64_t modeled_nand_wait_ns;

        req_ftl(n, BUF_PRINT_STAT, NULL);
        req_ftl(n, FTL_MAP_DIGEST, &map_digest);
        mmio_read_callbacks =
            qatomic_xchg(&buffer->cxl_mmio_read_callbacks, 0);
        skip_ftl_bypasses =
            qatomic_xchg(&buffer->cxl_skip_ftl_bypasses, 0);
        mapped_nand_reads =
            qatomic_xchg(&buffer->cxl_mapped_nand_reads, 0);
        unmapped_read_alloc_writes =
            qatomic_xchg(&buffer->cxl_unmapped_read_alloc_writes, 0);
        modeled_nand_wait_ns =
            qatomic_xchg(&buffer->cxl_modeled_nand_wait_ns, 0);
        print_buffer_policy(f, n, buffer, offset);
        fprintf(f,"Entry cnt: %" PRIu64 "/%" PRIu64 "\n",
                buffer->entry_cnt, buffer->size);
        fprintf(f,"Buffer read: %" PRIu64 " hit/ %" PRIu64 " miss\n",
                buffer->read_hit, buffer->read_miss);
        fprintf(f,"Buffer write: %" PRIu64 " hit/ %" PRIu64 " miss\n",
                buffer->write_hit, buffer->write_miss);
        fprintf(f,"Buffer ops: %" PRIu64 " insert/ %" PRIu64 " evict\n",
                buffer->ins_cnt, buffer->evict_cnt);
        fprintf(f,"Victim LPNs (%" PRIu64 " captured, %" PRIu64 " dropped):",
                buffer->victim_trace_count, buffer->victim_trace_dropped);
        for (uint64_t i = 0; i < buffer->victim_trace_count; i++) {
            fprintf(f," %" PRIu64, buffer->victim_trace[i]);
        }
        fprintf(f,"\n");
        fprintf(f,
                "CXL path: mmio_read_callbacks=%" PRIu64
                " skip_ftl_bypasses=%" PRIu64
                " mapped_nand_reads=%" PRIu64
                " unmapped_read_alloc_writes=%" PRIu64
                " modeled_nand_wait_ns=%" PRIu64 "\n",
                mmio_read_callbacks, skip_ftl_bypasses,
                mapped_nand_reads, unmapped_read_alloc_writes,
                modeled_nand_wait_ns);
        /* Append after the legacy seven-line stats block. */
        fprintf(f,
                "FTL mapping digest: schema=%s sha256=%s"
                " lpn_count=%" PRIu64 " mapped=%" PRIu64
                " unmapped=%" PRIu64 " ppa_bytes=%u\n",
                CYLON_FTL_MAP_DIGEST_SCHEMA, map_digest.sha256,
                map_digest.lpn_count, map_digest.mapped_count,
                map_digest.unmapped_count,
                CYLON_FTL_MAP_DIGEST_PPA_BYTES);
        buffer->ins_cnt = buffer->evict_cnt = buffer->read_hit = buffer->read_miss = buffer->write_hit = buffer->write_miss = 0;
        buffer->victim_trace_count = 0;
        buffer->victim_trace_dropped = 0;

        break;
    }
    case 2://flush buffer
        printf("flush buffer ");
        req_ftl(n, BUF_CLEAR, NULL);
        // printf("done\n");
        break;
    case 3: { //set way
        // fprintf(f,"====================\n\n");
        if (offset > WAY_FULL) {
            fprintf(f, "[Set way rejected] invalid way code: %" PRIu64 "\n",
                    offset);
            break;
        }
        /*
         * Runtime way changes are debug-only and must be issued while no
         * workload is running.  Drain earlier FTL requests before replacing
         * the set array.
         */
        req_ftl(n, BUF_PRINT_STAT, NULL);
        buffer_clear(buffer);
        buffer_destroy_set(buffer);

        buffer->way = offset;
        buffer_init_set(buffer);
        char way[32];
        format_buffer_way(buffer, way, sizeof(way));
        fprintf(f,"[Set way] eviction: %s, prefetch: %d, way: %s\n",
                buffer->policy < POLICY_MAX ?
                    policy_str[buffer->policy] : "INVALID",
                buffer->degree, way);

        break;
    }
    case 5: //set prefetch degree
        buffer->degree = offset;
        fprintf(f,"[Set degree] \n\t");
        print_buffer_policy(f, n, buffer, offset);
        break;
    case 7: //set prefetch stride
        buffer->stride = offset;
        fprintf(f,"[Set stride] \n\t");
        print_buffer_policy(f, n, buffer, offset);
        break;
    case 9:
        // femu_kvm_spte_clear_mmio_flag(0x1200, (uint64_t)(n->mbe->logical_space) + (0x1200<<12));
        printf("turn on ioctl flag set\n");
        req_ftl(n, BUF_CLEAR, NULL);
        ioctl_flag = true;
        break;
    case 11:
        // femu_kvm_spte_set_mmio_flag(0x1200, (uint64_t)(n->mbe->logical_space) + (0x1200<<12));
        printf("turn off ioctl flag set\n");
        req_ftl(n, BUF_CLEAR, NULL);
        ioctl_flag = false;
        break;
    
    case 13:
        n->lognum++;
        sprintf(new_logfilename, "/home/necsst/log/cxlssd_log%d", n->lognum);
        n->io_logfile = fopen(new_logfilename, "w+");
        break;
    case 15:
        if (n->io_logfile) {
            fclose(n->io_logfile);
            n->io_logfile = NULL;
        }
        break;
    case 17:
        test_spte_modify();
        break;


    // case 99:
    // case 97:
    // case 95:
    // case 93:
    case 91:
        rc = system("echo > /sys/kernel/tracing/trace");
        rc = system("echo 1 > /sys/kernel/tracing/tracing_on");
        printf("Turn on ftrace\n");
        break;
    case 90:
        printf("\nSET direct flags. ratio: %ld\n", offset);
        // req_ftl(n, BUF_CLEAR);
        // buffer->r_cxl = size%10;
        time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        femu_kvm_set_spte_by_cxl_mmio_rate(n, offset);
        time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - time;
        printf("SET direct flags. ratio: %ld, time: %ld\n", offset, time);
        break;
    // case 89:
    // case 87:
    // case 85:
    // case 83:
    case 81:
        // system("echo > /sys/kernel/tracing/trace");
        rc = system("echo 0 > /sys/kernel/tracing/tracing_on");
        char cmd[128];
        uint64_t nt = offset >> 16;
        int hr = (offset <<16) >> 16;
        printf("Turn off ftrace %ld %d %d offset: 0x%lx\n", nt, hr, rc, offset);
        sprintf(cmd, "cat /sys/kernel/tracing/trace >> ~/ftrace_result_%ld_%d", nt, hr);
        rc = system(cmd);
        break;
    case 80:
        printf("\nRESET mmio flags. ratio: %ld:%ld\n", size%10, 10-size%10);
        // req_ftl(n, BUF_CLEAR);
        time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        femu_kvm_reset_spte_by_cxl_mmio_rate(n, size%10);
        time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - time;
        printf("RESET mmio flags. ratio: %ld:%ld, time: %ld\n", size%10, 10-size%10, time);
        break;
    }
    fclose(f);
#endif
    // printf("get lsa done\n");

    void *backend_buf = n->mbe->buf_space + offset;
    memcpy(buf, backend_buf, size);
    return size;
}
static uint16_t set_lsa(struct FemuCtrl *n, const void *buf, uint64_t size, uint64_t offset) {
    // printf("set_lsa: offset:%lx, buf:%lx, size:%lx\n", offset, (uint64_t)buf, size);

    // if (size == 13) {
    //     char *data = (char *)buf;
    //     int hitrate = atoi(data);
    //     printf("\nSET direct flags. ratio: %d\n", hitrate);
    //     femu_kvm_set_spte_by_cxl_mmio_rate(n, hitrate);
    // }

    void *backend_buf = n->mbe->buf_space + offset;
    memcpy(backend_buf, buf, size);
    return size;
}

int nvme_register_cxlssd(FemuCtrl *n)
{
    // int devmemfd;
    // void *ptr;
    /* NVME ops */
    // Error *errp;
    n->ext_ops = (FemuExtCtrlOps) {
        .state            = NULL,
        .init             = cxlssd_init,
        .exit             = cxlssd_exit,
        .rw_check_req     = NULL,
        .admin_cmd        = cxlssd_admin_cmd,
        .io_cmd           = cxlssd_io_cmd,
        .get_log          = NULL,
    };

    /* CXL support */
    SsdDramBackend *mbe = n->mbe;
    // devmemfd = open("/dev/mem", O_RDWR);
    // if (devmemfd < 0) {
    //     femu_err("%s,Open [%s] failed, not able to allocate FEMU Backend DRAM using huagepages", __func__, "/dev/mem");
    //     abort();
    // }

    // ptr = mmap(NULL, mbe->size, PROTECTION, FLAGS, devmemfd, DEVMEM_OFFSET);
    // if (ptr == MAP_FAILED || ptr == NULL) {
    //     femu_err("DRAM backend [%s],mmap [%s] failed", __func__, "/dev/mem");
    //     abort();
    // }
    // printf("[%s] backend address: 0x%lx\n",__func__, (uint64_t)ptr);

    // memory_region_init_ram_ptr_dual_mode(&n->cxl_mr, OBJECT(n), "femu-cxlssd", mbe->size, mbe->logical_space);
    memory_region_init_ram_ptr(&n->cxl_mr, OBJECT(n), "femu-cxlssd", mbe->size, mbe->logical_space);
    // memory_region_init_ram_ptr_nomigrate(&n->cxl_mr, OBJECT(n), "femu-cxlssd", mbe->size, mbe->logical_space);
    // memory_region_init_ram_from_file(&n->cxl_mr, OBJECT(n), "femu-cxlssd", mbe->size, 0, RAM_SHARED, "/dev/mem/", DEVMEM_OFFSET, false, &errp);
    // memory_region_add_subregion_overlap(get_system_memory(), n->base_gpa, &n->cxl_mr, 99);
    address_space_init(&n->cxl_as, &n->cxl_mr, "cxlssd address space");
    // memory_region_init_ram_ptr(&n->cxl_mr, OBJECT(n), "femu-cxlssd", mbe->buf_size, mbe->buf_space);

    n->cxl_mem_ops = (FemuCXLOps) {
        .get_lsa          = get_lsa,
        .set_lsa          = set_lsa,
        .read             = cxlssd_mem_read,
        .write            = cxlssd_mem_write
    };
    return 0;
}
