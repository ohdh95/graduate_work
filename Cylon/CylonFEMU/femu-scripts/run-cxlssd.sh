#!/bin/bash
# Huaicheng Li <huaicheng@cs.uchicago.edu>
# Run FEMU with no SSD emulation logic, (e.g., for SCM/Optane emulation)
set -euo pipefail

if [[ $# -ne 1 || ! "$1" =~ ^(49152|98304)$ ]]; then
    echo "usage: $0 <49152|98304>  # SSD size in MiB"
    exit 1
fi

# Virtual machine OS disk.  The default preserves the artifact baseline;
# experiments may opt into a disposable overlay and a buffered I/O backend.
default_os_image="$HOME/images/ubuntu22.qcow2"
OSIMGF=${CYLON_OS_IMAGE:-$default_os_image}
os_disk_aio=${CYLON_OS_DISK_AIO:-native}
os_disk_cache=${CYLON_OS_DISK_CACHE:-none}

if [[ "$OSIMGF" != /* ]]; then
    echo "CYLON_OS_IMAGE must be an absolute path" >&2
    exit 1
fi

if [[ ! -f "$OSIMGF" ]]; then
	echo ""
	echo "VM disk image must be a regular file ..."
	echo "Please prepare a usable VM image and place it as $OSIMGF"
	echo "Once VM disk image is ready, please rerun this script again"
	echo ""
	exit
fi
if [[ ! -r "$OSIMGF" || ! -w "$OSIMGF" ]]; then
    echo "CYLON_OS_IMAGE must be readable and writable" >&2
    exit 1
fi

canonical_os_image=$(realpath -e -- "$OSIMGF")
if [[ "$canonical_os_image" != "$OSIMGF" ]]; then
    echo "CYLON_OS_IMAGE must be canonical and symlink-free" >&2
    exit 1
fi
if [[ "$OSIMGF" == *","* || "$OSIMGF" == *$'\n'* || "$OSIMGF" == *$'\r'* ]]; then
    echo "CYLON_OS_IMAGE cannot contain comma or newline characters" >&2
    exit 1
fi
case "${os_disk_aio}:${os_disk_cache}" in
    native:none|threads:writeback) ;;
    *)
        echo "QEMU OS disk must use native/none baseline or threads/writeback safe mode" >&2
        exit 1
        ;;
esac


# CXL-SSD backend memory parameters.  The defaults preserve the validated
# node-0 baseline.  A paper-like remote-NUMA run overrides all three values
# from configs/paper-remote-node1-96g.env without changing this launcher.
backend_dev=${CYLON_BACKEND_DEV:-/dev/mem}
bdev_offset=${CYLON_BDEV_OFFSET_HEX:-${CYLON_HPA_BASE_HEX:-0x6880000000}}
hpa_base=${CYLON_HPA_BASE_HEX:-$bdev_offset}

if [[ "$backend_dev" != /* ]]; then
    echo "CYLON_BACKEND_DEV must be an absolute device path" >&2
    exit 1
fi
if [[ ! "$bdev_offset" =~ ^0x[0-9a-fA-F]+$ ]]; then
    echo "CYLON_BDEV_OFFSET_HEX must be a hexadecimal address" >&2
    exit 1
fi
if [[ ! "$hpa_base" =~ ^0x[0-9a-fA-F]+$ ]]; then
    echo "CYLON_HPA_BASE_HEX must be a hexadecimal address" >&2
    exit 1
fi
if (( bdev_offset % 4096 != 0 || hpa_base % 4096 != 0 )); then
    echo "Cylon backend addresses must be 4 KiB aligned" >&2
    exit 1
fi
if [[ "$backend_dev" == "/dev/mem" ]] && (( bdev_offset != hpa_base )); then
    echo "For /dev/mem, CYLON_BDEV_OFFSET_HEX and CYLON_HPA_BASE_HEX must match" >&2
    exit 1
fi

# CXL-SSD DRAM buffer parameters. Defaults preserve the public artifact.
policy=${CYLON_REPLACEMENT_POLICY:-2} # [1:LIFO 2:FIFO 3:CLOCK 4:S3FIFO]
prf_dg=${CYLON_PREFETCH_DEGREE:-0}   # Next-N prefetch degree
buffer_way=${CYLON_BUFFER_WAY:-0}    # [0:1 1:2 2:4 3:8 4:16 5:full]

if [[ ! "$policy" =~ ^[1-4]$ ]]; then
    echo "CYLON_REPLACEMENT_POLICY must be in [1,4]" >&2
    exit 1
fi
if [[ ! "$prf_dg" =~ ^[0-9]+$ ]] || (( prf_dg > 255 )); then
    echo "CYLON_PREFETCH_DEGREE must be in [0,255]" >&2
    exit 1
fi
if [[ ! "$buffer_way" =~ ^[0-5]$ ]]; then
    echo "CYLON_BUFFER_WAY must be in [0,5]" >&2
    exit 1
fi

# Configurable SSD Controller layout parameters (must be power of 2)
ssd_size=$1		# in MegaBytes
bufsz=${CYLON_CACHE_SIZE_MIB:-$((ssd_size/20))}
skip_ftl=${CYLON_CXL_SKIP_FTL:-0}

if [[ ! "$bufsz" =~ ^[0-9]+$ ]] || (( bufsz <= 0 || bufsz > ssd_size )); then
    echo "CYLON_CACHE_SIZE_MIB must be in [1, SSD size]" >&2
    exit 1
fi
if [[ ! "$skip_ftl" =~ ^[01]$ ]]; then
    echo "CYLON_CXL_SKIP_FTL must be 0 or 1" >&2
    exit 1
fi

# Guest DRAM sizing. Keep the 16 GiB artifact baseline as the default, allow
# the paper's 96 GiB evaluation setup, and retain EXP-11's 128 GiB variant.
n_threads=8
dram_size=${CYLON_GUEST_DRAM:-16G}

case "$dram_size" in
    16G|96G|128G) ;;
    *)
        echo "CYLON_GUEST_DRAM must be one of: 16G, 96G, 128G" >&2
        exit 1
        ;;
esac

qemu_memory="${dram_size},maxmem=128G,slots=8"
qemu_memory_backend="memory-backend-ram,size=${dram_size},policy=bind,host-nodes=0,id=ram-node0,prealloc=on,prealloc-threads=${n_threads}"

# Optional read-only 9p directory used to stage the EXP-11 dataset in the guest.
# Keep this as an argv array so paths containing spaces remain one QEMU option.
stage_source_dir=${CYLON_STAGE_SOURCE_DIR:-}
stage_args=()

if [[ -n "$stage_source_dir" ]]; then
    if [[ "$stage_source_dir" != /* ]]; then
        echo "CYLON_STAGE_SOURCE_DIR must be an absolute path" >&2
        exit 1
    fi
    if [[ ! -e "$stage_source_dir" ]]; then
        echo "CYLON_STAGE_SOURCE_DIR must name an existing directory" >&2
        exit 1
    fi
    if [[ ! -d "$stage_source_dir" ]]; then
        echo "CYLON_STAGE_SOURCE_DIR must name a directory" >&2
        exit 1
    fi

    # QEMU parses -fsdev as a comma-separated key/value string.  Require the
    # caller to provide the one canonical, symlink-free spelling so the path
    # recorded in provenance is exactly the path QEMU exports.
    canonical_stage_source_dir=$(realpath -e -- "$stage_source_dir")
    if [[ "$canonical_stage_source_dir" != "$stage_source_dir" ]]; then
        echo "CYLON_STAGE_SOURCE_DIR must be canonical and symlink-free" >&2
        exit 1
    fi
    if [[ "$stage_source_dir" == *","* ||
          "$stage_source_dir" == *$'\n'* ||
          "$stage_source_dir" == *$'\r'* ]]; then
        echo "CYLON_STAGE_SOURCE_DIR cannot contain comma or newline characters" >&2
        exit 1
    fi

    stage_args=(
        -fsdev "local,id=exp11stage,path=${stage_source_dir},security_model=none,readonly=on"
        -device "virtio-9p-pci,fsdev=exp11stage,mount_tag=exp11stage"
    )
fi

# 96GB
if [ $ssd_size -eq 98304 ]
then
    secsz=512		
    secs_per_pg=8
    pgs_per_blk=256
    blks_per_pl=1536
    pls_per_lun=1       # still not support multiplanes		
    luns_per_ch=8		
    nchs=8  			
fi

# 48GB
if [ $ssd_size -eq 49152 ]
then
    secsz=512		
    secs_per_pg=8
    pgs_per_blk=256
    blks_per_pl=768
    pls_per_lun=1       # still not support multiplanes		
    luns_per_ch=8		
    nchs=8  			
fi


# Latency in nanoseconds
pg_rd_lat=${CYLON_NAND_READ_LAT_NS:-40000}
pg_wr_lat=200000
blk_er_lat=2000000
ch_xfer_lat=0

if [[ ! "$pg_rd_lat" =~ ^[0-9]+$ ]] ||
   (( pg_rd_lat <= 0 || pg_rd_lat > 2147483647 )); then
    echo "CYLON_NAND_READ_LAT_NS must be in [1,2147483647]" >&2
    exit 1
fi

# GC Threshold (1-100)
gc_thres_pcent=75
gc_thres_pcent_high=95

#-----------------------------------------------------------------------

#Compose the entire FEMU BBSSD command line options
FEMU_OPTIONS="-device femu,id=femu-cxlssd"
FEMU_OPTIONS=${FEMU_OPTIONS}",backend_dev=${backend_dev}"
FEMU_OPTIONS=${FEMU_OPTIONS}",bdev_offset=${bdev_offset}"
FEMU_OPTIONS=${FEMU_OPTIONS}",hpa_base=${hpa_base}"
FEMU_OPTIONS=${FEMU_OPTIONS}",devsz_mb=${ssd_size}"
FEMU_OPTIONS=${FEMU_OPTIONS}",namespaces=1"
FEMU_OPTIONS=${FEMU_OPTIONS}",femu_mode=6"
FEMU_OPTIONS=${FEMU_OPTIONS}",secsz=${secsz}"
FEMU_OPTIONS=${FEMU_OPTIONS}",secs_per_pg=${secs_per_pg}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pgs_per_blk=${pgs_per_blk}"
FEMU_OPTIONS=${FEMU_OPTIONS}",blks_per_pl=${blks_per_pl}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pls_per_lun=${pls_per_lun}"
FEMU_OPTIONS=${FEMU_OPTIONS}",luns_per_ch=${luns_per_ch}"
FEMU_OPTIONS=${FEMU_OPTIONS}",nchs=${nchs}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pg_rd_lat=${pg_rd_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pg_wr_lat=${pg_wr_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",blk_er_lat=${blk_er_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",ch_xfer_lat=${ch_xfer_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",gc_thres_pcent=${gc_thres_pcent}"
FEMU_OPTIONS=${FEMU_OPTIONS}",gc_thres_pcent_high=${gc_thres_pcent_high}"
FEMU_OPTIONS=${FEMU_OPTIONS}",bufsz_mb=${bufsz}"
FEMU_OPTIONS=${FEMU_OPTIONS}",replacement=${policy}"
FEMU_OPTIONS=${FEMU_OPTIONS}",prefetch_degree=${prf_dg}"
FEMU_OPTIONS=${FEMU_OPTIONS}",buffer_way=${buffer_way}"
FEMU_OPTIONS=${FEMU_OPTIONS}",cxl_skip_ftl=${skip_ftl}"
FEMU_OPTIONS=${FEMU_OPTIONS}",multipoller_enabled=1"

printf 'FEMU_OPTIONS=%s\n' "$FEMU_OPTIONS"
printf 'CYLON_BACKEND=%s,offset=%s,hpa_base=%s\n' \
    "$backend_dev" "$bdev_offset" "$hpa_base"
printf 'QEMU_MEMORY=-m %s\n' "$qemu_memory"
printf 'QEMU_MEMORY_BACKEND=-object %s\n' "$qemu_memory_backend"
printf 'QEMU_OS_IMAGE=%s\n' "$OSIMGF"
printf 'QEMU_OS_DISK=aio=%s,cache=%s\n' "$os_disk_aio" "$os_disk_cache"

if [[ "${CYLON_DRY_RUN:-0}" == "1" ]]; then
    printf 'QEMU_STAGE_ARGC=%d\n' "${#stage_args[@]}"
    for ((i = 0; i < ${#stage_args[@]}; i++)); do
        printf 'QEMU_STAGE_ARGV[%d]=%q\n' "$i" "${stage_args[$i]}"
    done
    exit 0
fi

nr_hugepages=$((ssd_size/2))

echo 0 | sudo tee /proc/sys/kernel/numa_balancing
# echo $nr_hugepages | sudo tee /sys/devices/system/node/node1/hugepages/hugepages-2048kB/nr_hugepages
echo never | sudo tee /sys/kernel/mm/transparent_hugepage/enabled

sudo numactl \
    --physcpubind=0-23 \
    --membind=0 \
    build-femu/qemu-system-x86_64 \
    -name "FEMU-CXLSSD-VM" \
    -machine type=q35,accel=kvm,nvdimm=on,cxl=on -enable-kvm \
    -cpu host \
    -smp $n_threads \
    -m "$qemu_memory" \
    -object "$qemu_memory_backend" \
    -numa node,nodeid=0,cpus=0-$((n_threads-1)),memdev=ram-node0 \
    --overcommit cpu-pm=on \
    -device virtio-scsi-pci,id=scsi0 \
    -device scsi-hd,drive=hd0 \
    -drive "file=${OSIMGF},if=none,aio=${os_disk_aio},cache=${os_disk_cache},format=qcow2,id=hd0" \
    "${stage_args[@]}" \
    ${FEMU_OPTIONS} \
    -device pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.1 \
    -device cxl-rp,port=0,bus=cxl.1,id=root_port13,chassis=0,slot=2 \
    -device cxl-type3,bus=root_port13,femu=femu-cxlssd,id=cxl-ssd0 \
    -M cxl-fmw.0.targets.0=cxl.1,cxl-fmw.0.size=${ssd_size}M \
    -net user,hostfwd=tcp:127.0.0.1:8080-:22 \
    -net nic,model=e1000 \
    -nographic \
    -qmp unix:./qmp-sock,server,nowait 2>&1 | tee log
