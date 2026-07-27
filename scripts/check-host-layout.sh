#!/usr/bin/env bash
set -uo pipefail

expected_kernel="6.4.6-cylon"
expected_cylon_reservation='memmap=96G$0x0000006880000000'
expected_node1_reservation='memmap=512G$0x0000008080000000'
expected_hpa="0x6880000000"
default_build_launcher="$HOME/graduate_work/Cylon/CylonFEMU/build-femu/run-cxlssd.sh"
default_source_launcher="$HOME/graduate_work/Cylon/CylonFEMU/femu-scripts/run-cxlssd.sh"

if [[ $# -gt 0 ]]; then
    launcher="$1"
elif [[ -f "$default_build_launcher" ]]; then
    launcher="$default_build_launcher"
else
    launcher="$default_source_launcher"
fi
failed=0

pass() {
    printf 'PASS: %s\n' "$1"
}

fail() {
    printf 'FAIL: %s\n' "$1" >&2
    failed=1
}

kernel="$(uname -r)"
if [[ "$kernel" == "$expected_kernel" ]]; then
    pass "kernel=$kernel"
else
    fail "kernel=$kernel, expected $expected_kernel"
fi

cmdline="$(cat /proc/cmdline)"
if [[ "$cmdline" == *"$expected_cylon_reservation"* ]]; then
    pass "$expected_cylon_reservation"
else
    fail "$expected_cylon_reservation is absent from /proc/cmdline"
fi

if [[ "$cmdline" == *"$expected_node1_reservation"* ]]; then
    pass "$expected_node1_reservation"
else
    fail "$expected_node1_reservation is absent from /proc/cmdline"
fi

if [[ -c /dev/kvm ]]; then
    pass "/dev/kvm exists"
else
    fail "/dev/kvm is missing"
fi

if command -v numactl >/dev/null 2>&1; then
    node1_size="$(numactl -H 2>/dev/null | awk '/^node 1 size:/ {print $4}')"
    if [[ "$node1_size" == "0" ]]; then
        pass "host node 1 memory size is 0 MB"
    else
        fail "host node 1 memory size is ${node1_size:-unknown} MB, expected 0 MB"
    fi
else
    fail "numactl is not installed"
fi

if [[ -f "$launcher" ]]; then
    configured_hpa="$(sed -n 's/^bdev_offset=//p' "$launcher" | head -n 1)"
    if [[ "$configured_hpa" == "$expected_hpa" ]]; then
        pass "Cylon bdev_offset=$configured_hpa"
    else
        fail "Cylon bdev_offset=${configured_hpa:-missing}, expected $expected_hpa"
    fi
else
    fail "launcher not found: $launcher"
fi

if (( failed != 0 )); then
    exit 1
fi

printf 'Host layout is ready for the node-0-only Cylon baseline.\n'
