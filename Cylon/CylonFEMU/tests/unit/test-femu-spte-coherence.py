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
        body = function_body(FTL, "static void *ftl_thread(", last=True)

        control = body.index("case BUF_CLEAR:")
        control_clear = body.index("buffer_clear(buffer)", control)
        control_complete = body.index("complete_cxl_req(creq)", control_clear)
        self.assertLess(control_clear, control_complete)

        for transition in (
            "buffer_insert_entry(buffer, bentry, INSERT_NO_PREFETCH)",
            "buffer_insert_entry(buffer, bentry, INSERT_PREFETCH)",
        ):
            insertion = body.index(transition)
            completion = body.index("complete_cxl_req(creq)", insertion)
            self.assertLess(insertion, completion)

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
