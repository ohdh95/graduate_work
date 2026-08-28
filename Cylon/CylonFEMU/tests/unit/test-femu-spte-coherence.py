#!/usr/bin/env python3
"""Static contract tests for FEMU's userspace-managed CXL SPTE transitions."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
KVM_EXT = (ROOT / "hw/femu/kvm_ext.c").read_text(encoding="utf-8")
BUFFER = (ROOT / "hw/femu/bbssd/buffer.c").read_text(encoding="utf-8")
BUFFER_H = (ROOT / "hw/femu/bbssd/buffer.h").read_text(encoding="utf-8")
FTL = (ROOT / "hw/femu/bbssd/ftl.c").read_text(encoding="utf-8")
CXLSSD = (ROOT / "hw/femu/cxlssd/cxlssd.c").read_text(encoding="utf-8")
KVM_ALL = (ROOT / "accel/kvm/kvm-all.c").read_text(encoding="utf-8")
FEMU_H = (ROOT / "include/hw/femu/femu.h").read_text(encoding="utf-8")
KERNEL_KVM_UAPI = (
    ROOT.parent / "CylonLinux/include/uapi/linux/kvm.h"
).read_text(encoding="utf-8")
KERNEL_TOOLS_KVM_UAPI = (
    ROOT.parent / "CylonLinux/tools/include/uapi/linux/kvm.h"
).read_text(encoding="utf-8")
KERNEL_X86 = (
    ROOT.parent / "CylonLinux/arch/x86/kvm/x86.c"
).read_text(encoding="utf-8")
QEMU_KVM_UAPI = (ROOT / "linux-headers/linux/kvm.h").read_text(encoding="utf-8")


def function_body(source: str, signature: str, *, last: bool = False) -> str:
    start = source.rindex(signature) if last else source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : pos]
    raise AssertionError(f"unterminated function: {signature}")


class SpteCoherenceContract(unittest.TestCase):
    def test_private_prefetch_uapi_copies_stay_in_sync(self) -> None:
        for source in (
            KERNEL_KVM_UAPI,
            KERNEL_TOOLS_KVM_UAPI,
            QEMU_KVM_UAPI,
        ):
            for required in (
                "KVM_MEM_CYLON_PREFETCH",
                "KVM_EXIT_CYLON_PREFETCH",
                "KVM_CAP_CYLON_PREFETCH_EXIT",
                "} cylon_prefetch;",
            ):
                self.assertIn(required, source)

    def test_async_prefetch_uses_a_separate_priority_ring_and_gate(self) -> None:
        for field in (
            "struct rte_ring *cxl_req;",
            "struct rte_ring *cxl_prefetch_req;",
            "QemuMutex       cxl_req_gate;",
            "QemuMutex       cxl_control_gate;",
            "bool            cxl_accept_requests;",
            "bool            cxl_accept_prefetch;",
            "uint32_t        cxl_prefetch_outstanding;",
        ):
            self.assertIn(field, FEMU_H)

        enqueue = function_body(
            CXLSSD, "static CxlEnqueueResult cxl_request_try_enqueue"
        )
        self.assertLess(
            enqueue.index("qemu_mutex_lock(&n->cxl_req_gate)"),
            enqueue.index("femu_ring_enqueue(ring"),
        )
        self.assertLess(
            enqueue.index("qatomic_inc(&n->cxl_prefetch_outstanding)"),
            enqueue.index("femu_ring_enqueue(ring"),
        )
        self.assertGreater(
            enqueue.index("qatomic_dec(&n->cxl_prefetch_outstanding)"),
            enqueue.index("femu_ring_enqueue(ring"),
        )
        shutdown = function_body(CXLSSD, "static void cxlssd_exit")
        self.assertLess(
            shutdown.index("n->cxl_accept_requests = false"),
            shutdown.index("ssd_reset(n)"),
        )

        ftl = function_body(FTL, "static void *ftl_thread(", last=True)
        self.assertIn("prefer_prefetch", ftl)
        self.assertIn("cxl_ring = ssd->cxl_req", ftl)
        self.assertIn("cxl_ring = ssd->cxl_prefetch_req", ftl)

        barrier = function_body(
            CXLSSD, "static void cxl_prefetch_barrier_begin"
        )
        self.assertLess(
            barrier.index("n->cxl_accept_prefetch = false"),
            barrier.index("femu_ring_count(n->cxl_prefetch_req)"),
        )

    def test_prefetch_exit_dispatches_only_to_registered_cylon(self) -> None:
        start = KVM_ALL.index("case KVM_EXIT_CYLON_PREFETCH:")
        end = KVM_ALL.index("case KVM_EXIT_IRQ_WINDOW_OPEN:", start)
        handler = KVM_ALL[start:end]
        self.assertIn("femu_kvm_dispatch_cylon_prefetch", handler)
        self.assertNotIn("address_space_rw", handler)

        unregister = function_body(
            KVM_EXT, "int femu_kvm_del_user_memory_region"
        )
        self.assertIn("cylon_dispatch_registered = false", unregister)
        self.assertIn("while (cylon_dispatch_active)", unregister)
        self.assertIn("mem.memory_size = 0", unregister)

    def test_kernel_prefetch_exit_is_one_way_and_preserves_singlestep(
        self,
    ) -> None:
        self.assertIn("complete_cylon_prefetch_singlestep", KERNEL_X86)
        self.assertIn("KVM_EXIT_CYLON_PREFETCH", KERNEL_X86)
        self.assertIn(
            "complete_userspace_io =\n"
            "\t\t\t\tcomplete_cylon_prefetch_singlestep",
            KERNEL_X86,
        )
        self.assertIn(
            "vcpu->run->cylon_prefetch.phys_addr = cr2_or_gpa",
            KERNEL_X86,
        )

    def test_direct_to_mmio_store_precedes_synchronous_flush(self) -> None:
        body = function_body(KVM_EXT, "int femu_kvm_spte_set_mmio_flag")
        validation = body.index("validate_spte_gfn(gfn, lpn)")
        store = body.index("qatomic_set_mb(sptep, make_mmio_spte(gfn))")
        flush = body.index("kvm_flush_spte_tlb(gfn, lpn)")
        failure = body.index("spte_fail_closed", flush)
        self.assertLess(validation, store)
        self.assertLess(store, flush)
        self.assertLess(flush, failure)

    def test_flush_ioctl_requests_single_gfn_invalidation(self) -> None:
        body = function_body(KVM_EXT, "static int kvm_flush_spte_tlb")
        self.assertIn(".gpa = gfn << PAGE_SHIFT", body)
        self.assertIn(".flag = 0", body)
        self.assertIn("KVM_SET_SPTE_FLAG", body)
        self.assertNotIn("qatomic_set_mb", body)

    def test_mmio_to_direct_is_an_ordered_atomic_store(self) -> None:
        body = function_body(KVM_EXT, "int femu_kvm_spte_clear_mmio_flag")
        self.assertLess(
            body.index("validate_spte_gfn(gfn, lpn)"),
            body.index("qatomic_set_mb"),
        )
        self.assertIn("qatomic_set_mb(sptep, make_direct_spte(lpn))", body)
        self.assertNotIn("kvm_flush_spte_tlb", body)

    def test_no_plain_spte_store_remains(self) -> None:
        self.assertIsNone(re.search(r"(?m)^\s*\*sptep\s*=", KVM_EXT))

    def test_spte_lookup_checks_bounds_and_alignment(self) -> None:
        body = function_body(KVM_EXT, "static u64 *dualslot_get_sptep")
        for required in (
            "lpn >= (femu->mbe->size >> PAGE_SHIFT)",
            "idx >= (size_t)spt.n",
            "lpn - chunk_base >= chunk_entries",
            "(uintptr_t)sptep % sizeof(*sptep)",
        ):
            self.assertIn(required, body)

    def test_cache_clear_invalidates_before_free_and_destroys_old_trees(
        self,
    ) -> None:
        helper = function_body(BUFFER, "static void buffer_clear_active_entry")
        self.assertLess(helper.index("direct_mr_del"), helper.index("free(entry)"))

        clear = function_body(BUFFER, "void buffer_clear(")
        self.assertIn("QTAILQ_EMPTY(&set->small)", clear)
        self.assertIn("QTAILQ_EMPTY(&set->ghost)", clear)
        self.assertIn("g_tree_destroy(buffer->tree)", clear)
        self.assertIn("g_tree_destroy(buffer->ghost_tree)", clear)

    def test_ftl_never_completes_before_cache_transition_returns(self) -> None:
        completion = function_body(FTL, "static void complete_cxl_fill(")
        publish = completion.index("buffer_insert_entry(buffer, bentry")
        wake_waiter = completion.index("complete_cxl_req(waiter)")
        self.assertLess(publish, wake_waiter)

        body = function_body(FTL, "static void *ftl_thread(", last=True)

        control = body.index("case BUF_CLEAR:")
        control_drain = body.rindex(
            "drain_cxl_fills(n, buffer, &fills)", 0, control
        )
        control_clear = body.index("buffer_clear(buffer)", control)
        control_complete = body.index("complete_cxl_req(creq)", control_clear)
        self.assertLess(control_drain, control_clear)
        self.assertLess(control_clear, control_complete)

        schedule = body.index("cylon_cxl_fill_get_or_create(&fills")
        add_waiter = body.index("cylon_cxl_fill_add_waiter(fill, creq)", schedule)
        self.assertLess(schedule, add_waiter)
        self.assertNotIn("buffer_entry_init(buffer, lpn)", body[schedule:])

    def test_cxl_path_counters_cover_and_close_the_stats_window(self) -> None:
        for field in (
            "cxl_mmio_read_callbacks",
            "cxl_skip_ftl_bypasses",
            "cxl_mapped_nand_reads",
            "cxl_unmapped_read_alloc_writes",
            "cxl_modeled_nand_wait_ns",
        ):
            self.assertIn(f"uint64_t {field};", BUFFER_H)

        read = function_body(CXLSSD, "static MemTxResult cxlssd_mem_read")
        self.assertIn("qatomic_inc(&buffer->cxl_mmio_read_callbacks)", read)
        self.assertIn("qatomic_inc(&buffer->cxl_skip_ftl_bypasses)", read)

        ftl = function_body(FTL, "static void *ftl_thread(", last=True)
        self.assertIn("qatomic_inc(&buffer->cxl_mapped_nand_reads)", ftl)
        self.assertIn("&buffer->cxl_unmapped_read_alloc_writes", ftl)
        self.assertIn("qatomic_add(&buffer->cxl_modeled_nand_wait_ns, lat)", ftl)

        stats = function_body(CXLSSD, "static uint16_t get_lsa")
        stats_case = stats[stats.index("case 1:") : stats.index("case 2:")]
        self.assertLess(
            stats_case.index("cxl_prefetch_barrier_begin(n)"),
            stats_case.index("qatomic_xchg"),
        )
        self.assertGreater(
            stats_case.index("cxl_prefetch_barrier_end(n)"),
            stats_case.rindex("qatomic_xchg"),
        )
        for field in (
            "cxl_mmio_read_callbacks",
            "cxl_skip_ftl_bypasses",
            "cxl_mapped_nand_reads",
            "cxl_unmapped_read_alloc_writes",
            "cxl_modeled_nand_wait_ns",
        ):
            self.assertIn(f"qatomic_xchg(&buffer->{field}, 0)", stats)

        clear = function_body(BUFFER, "void buffer_clear(")
        self.assertIn("qatomic_set(&buffer->cxl_mmio_read_callbacks, 0)", clear)
        self.assertIn("qatomic_set(&buffer->cxl_modeled_nand_wait_ns, 0)", clear)


if __name__ == "__main__":
    unittest.main()
