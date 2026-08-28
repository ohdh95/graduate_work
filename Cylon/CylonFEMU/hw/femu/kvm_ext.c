#include "./kvm_ext.h"
#include "hw/femu/backend/dram.h"

#include "hw/core/cpu.h"
#include <sys/ioctl.h>

// static FemuCtrl *femu = NULL;
FILE *kvm_ioctl_log_file;
static struct kvm_memslot_get_linear_spt spt;
struct kvm_userspace_memory_region mem;

static bool init = false;

FILE *pagemap = NULL;
FemuCtrl *femu = NULL;

static int init_spt(struct kvm_userspace_memory_region mem)
{
    int ret;
    u64 size = (mem.memory_size >> PAGE_SHIFT) * sizeof(u64*);
    // u64 size = (femu->mbe->size >> PAGE_SHIFT) * sizeof(u64*);

    int idx = 0;
    while (size > 0) {
        u64 sz = (size > MAX_CONT_ALLOC_SZ)? MAX_CONT_ALLOC_SZ:size;

        //allocate anonymous memory
        spt.spt_list[idx].spt = mmap(NULL, sz, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_SHARED, -1, 0);
        // printf("idx:%d mmap addr: 0x%llx\n",idx, (u64)spt.base_leaf_spt[idx].spt);

        if (spt.spt_list[idx].spt == MAP_FAILED) {
            perror("mmap failed!");
            abort();
        }

        size -= sz;
        idx++;
    }
    
    spt.gfn = mem.guest_phys_addr >> 12;
    spt.backend_ptr = femu->mbe->logical_space;
    // spt.gfn = femu->base_gpa >> 12;
    KVMState *s = kvm_state;

    printf(">init_spt %ld\n", KVM_GET_LINEAR_SPT);

    ret = kvm_vm_ioctl(s, KVM_GET_LINEAR_SPT, &spt);
    if (ret < 0) {
        perror("get liner");
        printf("FEMU kvm ioctl KVM_GET_LINEAR_SPT error\n");
        abort();
    }

    // for (int i=0; i<idx; i++) {
    //     printf("%02d npg:0x%x, off:0x%x\n",i ,spt.spt_list[i].npages, spt.spt_list[i].offset);
    // }

    printf(">>>>>>>>>>>>>\n");

    // pagemap = fopen("/proc/self/pagemap", "r");
    // if (!pagemap) {
    //     perror("fopen pagemap");
    //     abort();
    // }

    return ret;
}

static inline u64 dualslot_get_leaf_spt_idx(u64 lpn)
{
	return (lpn * sizeof(u64*)) >> (MAX_ORDER + PAGE_SHIFT);
}

/*
 * KVM_SET_SPTE_FLAG with flag == 0 is the only userspace ABI that this
 * file can use to synchronously invalidate a hardware translation.  The
 * custom kernel currently falls back to a VM-wide shootdown on bare-metal
 * Intel, but the ABI promises only the single GFN passed in data.gpa.
 * Consequently callers must issue this once for every direct->MMIO
 * transition; coalescing unrelated GFNs into one ioctl would be incorrect
 * on a host that implements range-aware invalidation.
 */
static int kvm_flush_spte_tlb(uint64_t gfn, lpn_t lpn)
{
    struct kvm_set_spte_flag data = (struct kvm_set_spte_flag) {
        .gpa = gfn << PAGE_SHIFT,
        .flag = 0,
        .lpn = lpn,
    };
    struct CPUState *cpu = qemu_get_cpu(0);
    int ret;

    if (!cpu) {
        return -ENODEV;
    }

    ret = kvm_vcpu_ioctl(cpu, KVM_SET_SPTE_FLAG, &data);
    return ret;
}

#define DIRECT_MASK  0x600000000000977
#define MMIO_MASK  0x0000000586

static void spte_fail_closed(const char *operation, uint64_t gfn, lpn_t lpn,
                             int error)
{
    if (error < 0) {
        femu_err("%s failed for gfn=0x%" PRIx64 ", lpn=0x%" PRIx64
                 ": %s (%d); stopping to avoid a stale direct mapping\n",
                 operation, gfn, lpn, strerror(-error), error);
    } else {
        femu_err("%s failed for gfn=0x%" PRIx64 ", lpn=0x%" PRIx64
                 "; stopping to avoid a stale direct mapping\n",
                 operation, gfn, lpn);
    }
    abort();
}

static u64 *dualslot_get_sptep(u64 lpn)
{
    const u64 entries_per_page = (1ULL << PAGE_SHIFT) / sizeof(u64);
    u64 chunk_base;
    u64 chunk_entries;
    u64 off;
    size_t idx;
    u64 *sptep;

    if (!init || !femu || !femu->mbe) {
        spte_fail_closed("SPTE lookup before initialization", 0, lpn, 0);
    }

    if (lpn >= (femu->mbe->size >> PAGE_SHIFT)) {
        spte_fail_closed("out-of-range SPTE lookup", 0, lpn, -ERANGE);
    }

    idx = dualslot_get_leaf_spt_idx(lpn);
    if (spt.n <= 0 || idx >= (size_t)spt.n ||
        idx >= ARRAY_SIZE(spt.spt_list) ||
        !spt.spt_list[idx].spt || spt.spt_list[idx].npages <= 0) {
        spte_fail_closed("invalid linear-SPT chunk", 0, lpn, -EINVAL);
    }

    chunk_base = (u64)spt.spt_list[idx].offset * entries_per_page;
    chunk_entries = (u64)spt.spt_list[idx].npages * entries_per_page;
    if (lpn < chunk_base || lpn - chunk_base >= chunk_entries) {
        spte_fail_closed("linear-SPT offset mismatch", 0, lpn, -ERANGE);
    }

    off = lpn - chunk_base;
    sptep = spt.spt_list[idx].spt + off;
    if ((uintptr_t)sptep % sizeof(*sptep)) {
        spte_fail_closed("unaligned SPTE", 0, lpn, -EFAULT);
    }

    return sptep;
}

static void validate_spte_gfn(uint64_t gfn, lpn_t lpn)
{
    uint64_t expected_gfn = (femu->mbe->base_gpa >> PAGE_SHIFT) + lpn;

    if (gfn != expected_gfn) {
        spte_fail_closed("GFN/LPN mismatch", gfn, lpn, -EINVAL);
    }
}

static inline u64 make_mmio_spte(u64 gfn)
{
    return (gfn << PAGE_SHIFT) | MMIO_MASK;
}

static inline u64 make_direct_spte(u64 lpn)
{
    return (femu->hpa_base + (lpn << PAGE_SHIFT)) | DIRECT_MASK;


    // u64 paddr = 0;
    // u64 vaddr = (lpn << PAGE_SHIFT) + (u64)femu->mbe->logical_space;
    // off_t offset = vaddr >> 9;
    // uint64_t e;

    // if (lseek(fileno(pagemap), offset, SEEK_SET) == offset) {
    //     if (fread(&e, sizeof(uint64_t), 1, pagemap)) {
    //         if (e & (1ULL << 63)) { // page present ?
    //             paddr = e & ((1ULL << 54) - 1); // pfn mask
    //             paddr = paddr * sysconf(_SC_PAGESIZE);
    //             // add offset within page
    //             // paddr = paddr | (vaddr & (sysconf(_SC_PAGESIZE) - 1));
    //         }   
    //     }   
    // }

    // if (paddr == 0) {
    //     printf("pagemap error\n");
    //     abort();
    // }

    // return paddr | DIRECT_MASK;

}


int femu_kvm_spte_set_mmio_flag(uint64_t gfn, lpn_t lpn)
{
    u64 *sptep = dualslot_get_sptep(lpn);
    int ret;

    validate_spte_gfn(gfn, lpn);

    /*
     * SPTEs are naturally aligned 64-bit words.  Publish the non-present
     * MMIO SPTE atomically and with a full barrier before asking KVM to
     * invalidate remote hardware TLBs.  The ioctl is synchronous: returning
     * from this function is therefore the eviction visibility point.
     */
    qatomic_set_mb(sptep, make_mmio_spte(gfn));
    ret = kvm_flush_spte_tlb(gfn, lpn);
    if (ret) {
        spte_fail_closed("KVM direct-to-MMIO TLB shootdown", gfn, lpn, ret);
    }

    return 0;
}

int femu_kvm_spte_clear_mmio_flag(uint64_t gfn, lpn_t lpn)
{
    u64 *sptep = dualslot_get_sptep(lpn);

    validate_spte_gfn(gfn, lpn);

    /*
     * MMIO SPTEs are non-present, so the CPU cannot retain a successful
     * MMIO->direct hardware translation.  An atomic, ordered store is enough
     * for the next page walk.  Re-flushing here would add a global shootdown
     * to every cache insertion/hit on the current host without strengthening
     * the direct->MMIO eviction guarantee above.
     */
    qatomic_set_mb(sptep, make_direct_spte(lpn));
    return 0;
}

static struct kvm_userspace_memory_region femu_get_memory_region(FemuCtrl *n)
{
    struct kvm_userspace_memory_region m;
    SsdDramBackend *b = n->mbe;

    m.slot = 0xFE;
    m.guest_phys_addr = n->base_gpa;
    m.userspace_addr = (uint64_t)b->buf_space;
    m.flags = KVM_MEMSLOT_DUAL_MODE;
    // mem.flags = 0;
    m.memory_size = b->size;

    return m;
}

int femu_kvm_set_user_memory_region(FemuCtrl *n)
{
    femu = n;
    // return 0;

    KVMState *s = kvm_state;
    struct kvm_userspace_memory_region mem = femu_get_memory_region(n);
    int ret;
	
    printf("\n%s: base_gpa:0x%llx, hva:0x%llx, size:%.1fGB\n\n",__func__,mem.guest_phys_addr,mem.userspace_addr,mem.memory_size/1024/1024/1024.);
    
    
    ret = kvm_vm_ioctl(s, KVM_SET_USER_MEMORY_REGION, &mem);
    if (ret < 0) {
        printf("FEMU kvm ioctl KVM_SET_USER_MEMORY_REGION(0x%lx) error (%d), addr:0x%lx\n",KVM_SET_USER_MEMORY_REGION, ret, (uint64_t)&mem);
        abort();
    }

    init_spt(mem);
    init = true;

    return ret;
}

int femu_kvm_del_user_memory_region(FemuCtrl *n)
{
    KVMState *s = kvm_state;
    struct kvm_userspace_memory_region mem = femu_get_memory_region(n);
    int ret;

    ret = kvm_vm_ioctl(s, KVM_SET_USER_MEMORY_REGION, &mem);
    if (ret < 0) {
        printf("FEMU kvm ioctl error\n");
    }
    return ret;
}


static bool do_direct(int hitrate, lpn_t lpn)
{
	// if(r_cxl == 1)		return (lpn%10==1);
	// else if(r_cxl == 3)	return (lpn%10==1 || lpn%10==4 || lpn%10==7);
	// else if(r_cxl == 5)	return (lpn%2==1);
	// else if(r_cxl == 7)	return (!(lpn%10==1 || lpn%10==4 || lpn%10==7));
	// else if(r_cxl == 9)	return (lpn%10!=1);
    
	// if(r_cxl == 1)		r_cxl=99; 
	// else if(r_cxl == 3)	r_cxl=98;
	// else if(r_cxl == 5)	r_cxl=97;
    // else if(r_cxl == 7)	r_cxl=995;
    // else if(r_cxl == 9)	r_cxl=999;
    int r_cxl = hitrate;

    if (r_cxl==999)
        return (lpn%1000!=0);
    if (r_cxl==995)
        return (lpn%200!=0);
    if (r_cxl==99)
        return (lpn%100!=0);
    if (r_cxl==98)
        return (lpn%50!=0);
    if (r_cxl==97)
        return (lpn%33!=0);
    if (r_cxl==95)
        return (lpn%20!=0);
    if (r_cxl==90)
        return (lpn%10!=0);
    if (r_cxl==75)
        return (lpn%4!=0);    
    if (r_cxl==50) 
        return (lpn%2!=0);        

    return true;
}

int femu_kvm_set_spte_by_cxl_mmio_rate(FemuCtrl *n, int hitrate)
{
    // SsdDramBackend *b = n->mbe;
    lpn_t lpn, max_lpn = n->mbe->size >> 12;//b->size >> 12;
    // lpn_t lpn, max_lpn = n->mbe->buf_size >> 12;//b->size >> 12;
    uint64_t gfn_base = n->base_gpa >> 12;
    // uint64_t hvn_base = (uint64_t)b->buf_space >> 12;
    printf("max lpn: 0x%lx, idx: %lld\n", max_lpn, dualslot_get_leaf_spt_idx(max_lpn));
    int ret = 0;
    int cnt = 0;
    for (lpn=0; lpn<max_lpn; lpn++) {
        if (do_direct(hitrate, lpn)) {
            cnt++;
            ret+=femu_kvm_spte_clear_mmio_flag(gfn_base+lpn, lpn);
        }
    }

    printf("\nSET: %d/%d pgs done\n",ret,cnt);
    return ret;
}

int femu_kvm_reset_spte_by_cxl_mmio_rate(FemuCtrl *n, int r_cxl)
{

    // SsdDramBackend *b = n->mbe;
    lpn_t lpn, max_lpn = n->mbe->size >> 12;//b->size >> 12;
    // lpn_t lpn, max_lpn = n->mbe->buf_size >> 12;//b->size >> 12;
    uint64_t gfn_base = n->base_gpa >> 12;
    // uint64_t hvn_base = (uint64_t)b->buf_space >> 12;
    int ret = 0;
    int cnt = 0;
    for (lpn=0; lpn<max_lpn; lpn++) {
        if (do_direct(r_cxl, lpn)) {
            cnt++;
            ret+=femu_kvm_spte_set_mmio_flag(gfn_base+lpn, lpn);
        }
    }

    printf("\nRESET: %d/%d pgs done\n",ret,cnt);
    return ret;
}

void femu_kvm_log_init(void)
{
    kvm_ioctl_log_file = fopen("kvm_ioctl_log.txt", "w+");
}

void test_spte_modify(void)
{
    if (init == false) {
        printf("%s spt not initialized\n", __func__);
        abort();
        // init_spt(mem);
        // init = true;

        // test_spte_modify();
    }

    for (int i=0; i<60; i++) {
        if (spt.spt_list[i].spt == NULL)
            continue;

        printf("%02d 0x%llx |",i ,(u64)spt.spt_list[i].spt);
        for (int j=0; j<8; j++) {
            printf("0x%llx ", (u64)spt.spt_list[i].spt[j]);
        }
        printf("\n");
    }
        
}

void print_spte(uint64_t gpa) {
    u64 lpn = gpa - femu->base_gpa;
    u64* sptep = dualslot_get_sptep(lpn);
    printf("gpa:0x%lx, lpn:0x%llx, sptep:0x%llx\n", gpa, lpn, *sptep);
}
