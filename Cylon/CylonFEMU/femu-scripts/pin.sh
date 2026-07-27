#!/bin/bash
# Huaicheng Li <huaicheng@cs.uchicago.edu>
# pin vcpu and qemu main thread to certain set of physical CPUs
#

NRCPUS="$(cat /proc/cpuinfo | grep "vendor_id" | wc -l)"

# Pin eight vCPUs to host node 0 physical cores.
sudo ./ftk/qmp-vcpu-pin -s ./qmp-sock 2 3 4 5 6 7 8 9

# Pin the eight FEMU pollers to host node 0 CPUs 23 down to 16.
POLLCPU=23
for poller in $(grep FEMU-NVMe-Poller-TID log | awk -F:\  '{print $3}'); do
    FEMU_POLLER_TID=$poller
    [[ -z $FEMU_POLLER_TID ]] && echo -e "\t===> FEMU NVMe Poller thread ID not found from log file..." && exit
    echo -e "===> Pinning FEMU NVMe poller thread to pCPU: $POLLCPU"
    sudo taskset -cp ${POLLCPU} $FEMU_POLLER_TID
    ((POLLCPU-=1))
done


# pin main thread to the rest of pCPUs
#qemu_pid=$(ps -ef | grep qemu | grep -v grep | tail -n 1 | awk '{print $2}')

#sudo taskset -cp 1-$NRCPUS ${qemu_pid}
