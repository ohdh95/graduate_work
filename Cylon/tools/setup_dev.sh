#!/usr/bin/env bash
set -euo pipefail

mode=${1:-}
if [[ "$mode" != "system_ram" && "$mode" != "devdax" ]]; then
    echo "Usage: $0 [system_ram|devdax]" >&2
    exit 1
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

if ! cxl list -R | grep -q '"region"'; then
    cxl create-region -m -t ram -d decoder0.0 -w 1 -g 4096 mem0
fi

udevadm settle
for attempt in {1..30}; do
    [[ -e /sys/bus/dax/devices/dax0.0 ]] && break
    if (( attempt == 30 )); then
        echo "Timed out waiting for dax0.0" >&2
        exit 1
    fi
    sleep 1
done

# Touch all pages to trigger cold EPT faults and populate the EPT tables.
for attempt in {1..5}; do
    if daxctl reconfigure-device --mode=devdax --force dax0.0; then
        break
    fi
    if (( attempt == 5 )); then
        echo "Failed to reconfigure dax0.0 as devdax" >&2
        exit 1
    fi
    udevadm settle
    sleep 1
done
"$script_dir/cxl_warmup"

if [[ "$mode" == "system_ram" ]]; then
    echo "Reconfiguring DAX device to system RAM mode..."
    # The device appears as a CPU-less NUMA node.
    daxctl reconfigure-device --mode=system-ram --force dax0.0
else
    echo "Configured DAX device to devdax mode..."
fi
