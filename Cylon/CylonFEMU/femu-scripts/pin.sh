#!/bin/bash
# Huaicheng Li <huaicheng@cs.uchicago.edu>
# Pin QEMU control, vCPU, FTL, and poller threads to disjoint host CPUs.
#
set -euo pipefail

mapfile -t qemu_pids < <(
    pgrep -f '^build-femu/qemu-system-x86_64 -name FEMU-CXLSSD-VM( |$)'
)
if [[ ${#qemu_pids[@]} -ne 1 ]]; then
    echo "Expected exactly one FEMU-CXLSSD-VM process, found ${#qemu_pids[@]}" >&2
    exit 1
fi
qemu_pid=${qemu_pids[0]}

mapfile -t ftl_tids < <(
    awk -F': ' '/FEMU-FTL-Thread-TID:/ {print $NF}' log | sort -u
)
if [[ ${#ftl_tids[@]} -ne 1 ||
      ! -d "/proc/$qemu_pid/task/${ftl_tids[0]}" ]]; then
    echo "Expected exactly one live FEMU FTL thread in log" >&2
    exit 1
fi
ftl_tid=${ftl_tids[0]}

mapfile -t poller_tids < <(
    awk -F': ' '/FEMU-NVMe-Poller-TID:/ {print $NF}' log | sort -u
)
if [[ ${#poller_tids[@]} -ne 8 ]]; then
    echo "Expected exactly eight FEMU poller threads, found ${#poller_tids[@]}" >&2
    exit 1
fi
for poller_tid in "${poller_tids[@]}"; do
    if [[ ! -d "/proc/$qemu_pid/task/$poller_tid" ]]; then
        echo "Poller TID $poller_tid is not part of QEMU PID $qemu_pid" >&2
        exit 1
    fi
done

# Start with every QEMU thread in the control pool.  The commands below then
# move measured workers to dedicated, non-overlapping CPUs.
sudo taskset -apc 0-1,11-15 "$qemu_pid"

# Pin eight vCPUs to host node 0 physical cores.
sudo ./femu-scripts/ftk/qmp-vcpu-pin -s ./qmp-sock 2 3 4 5 6 7 8 9

# Pin the single CXL-SSD FTL thread away from vCPUs and pollers.
echo "===> Pinning FEMU FTL thread $ftl_tid to pCPU: 10"
sudo taskset -cp 10 "$ftl_tid"

# Pin the eight FEMU pollers to host node 0 CPUs 23 down to 16.
POLLCPU=23
for poller_tid in "${poller_tids[@]}"; do
    echo "===> Pinning FEMU NVMe poller thread $poller_tid to pCPU: $POLLCPU"
    sudo taskset -cp "$POLLCPU" "$poller_tid"
    ((POLLCPU-=1))
done

echo "===> Final QEMU thread affinities"
for task_path in "/proc/$qemu_pid"/task/*; do
    task_tid=${task_path##*/}
    task_name=$(<"$task_path/comm")
    task_cpus=$(awk '/^Cpus_allowed_list:/ {print $2}' "$task_path/status")
    printf '%s\t%s\t%s\n' "$task_tid" "$task_name" "$task_cpus"
done | sort -n
