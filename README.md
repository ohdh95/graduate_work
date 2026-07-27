# CMM-H 특성 분석 및 CMM-H 특화 DiskANN

이 디렉터리는 Cylon을 이용해 CXL-SSD/CMM-H의 특성을 분석하고, Starling과 MARGO의 아이디어를 참고한 page-aware DiskANN 레이아웃을 설계·평가하기 위한 졸업작품 작업공간이다.

## 1. 연구 목표

이 프로젝트는 다음 세 질문에 답하는 것을 목표로 한다.

1. CMM-H의 4 KiB DRAM cache line, NAND miss latency, 교체 정책, prefetch가 DiskANN 탐색에 어떤 영향을 주는가?
2. 탐색 중 함께 방문되는 그래프 노드를 같은 4 KiB page에 배치하면 Cylon cache miss와 query latency를 줄일 수 있는가?
3. Page-aware layout과 Cylon cache policy를 함께 설계하면 각각을 독립적으로 최적화할 때보다 더 좋은 결과를 얻을 수 있는가?

핵심 가설은 다음과 같다.

> DiskANN의 그래프 탐색 경로를 기준으로 노드를 4 KiB page에 함께 배치하면 CMM-H의 page miss 수가 감소하고, 이 효과는 cache replacement와 prefetch policy에 따라 달라진다.

## 2. 디렉터리 구성

```text
graduate_work/
├── Cylon/                # Cylon 소스, 빌드 및 실험 코드
├── configs/              # 실험 설정과 실행 시 사용한 환경 기록
├── notes/                # 설계 메모 및 문제 기록
├── results/
│   ├── raw/              # 프로그램이 출력한 원본 결과
│   └── processed/        # 표와 그래프 생성용 정리 결과
├── scripts/              # 반복 실행, 검증, 결과 수집 스크립트
└── README.md
```

`Cylon`은 아래 원본 커밋에서 시작했으며, 원본 baseline은 commit과 `master` branch로 보존한다.

```text
Repository: https://github.com/MoatLab/Cylon
Commit:     4c5e196c09676db18114f4d09509b290c7385978
```

현재 작업 branch와 원본 대비 변경사항은 다음 명령으로 확인한다.

```bash
cd ~/graduate_work/Cylon
git branch --show-current
git status --short
git diff 4c5e196c09676db18114f4d09509b290c7385978
```

현재 `experiment/node0-baseline` branch에는 이 서버에서 node 0만 사용하기 위한 backend 주소, QEMU NUMA binding, FEMU poller CPU 변경이 적용되어 있다.

## 3. 고정할 Cylon 원본 baseline

최초 재현 실험에서는 SSD 크기와 NAND geometry를 변경하지 않는다.

| 항목 | 원본 baseline |
|---|---:|
| CXL-SSD/NAND capacity | 96 GiB (`98304 MiB`) |
| Cylon DRAM cache parameter | `4915 MiB`, 약 4.8 GiB |
| Cache line/page | 4 KiB |
| Guest 일반 DRAM | 16 GiB |
| Guest vCPU | 8 |
| Replacement policy | FIFO (`2`) |
| Prefetch degree | `0` |
| NAND read latency | 40 us |
| NAND program latency | 200 us |
| NAND erase latency | 2 ms |

원본 실행 형태는 다음과 같다.

```bash
./run-cxlssd.sh 98304
```

원본은 cache 크기를 NAND 크기의 5%로 계산한다.

```bash
bufsz=$((ssd_size/20))
```

`maxmem=128G`는 최대 확장 가능 크기일 뿐, 원본 부팅 시 128 GiB를 할당한다는 뜻은 아니다. 원본 guest DRAM은 16 GiB이다.

원본이 명시적으로 정의하는 NAND geometry는 48 GiB와 96 GiB이다. 다른 SSD 크기는 baseline이 완전히 재현된 뒤 별도 실험으로만 추가한다.

## 4. Cylon 논문 기준 커널 수정

이 절은 [Cylon 논문 §4.3-§4.8](https://www.usenix.org/system/files/fast26-yoon.pdf)과 현재 repository의 `CylonLinux` 및 `CylonFEMU` 코드를 함께 기준으로 작성했다. 논문은 Cylon을 QEMU/FEMU와 **host Linux KVM kernel**을 함께 수정한 시스템으로 설명하며, Linux 6.4.6에 약 1,261 LOC, FEMU 8.0.0에 약 6,282 LOC를 추가했다고 보고한다.

### 4.1 Host kernel을 수정하는 이유

일반 QEMU CXL 장치는 MMIO 경로를 사용하므로 guest의 load/store마다 VM-exit가 발생한다. 이 경로는 수 µs 이상의 hypervisor overhead를 발생시켜 수백 ns 수준이어야 하는 CXL-SSD DRAM cache hit까지 느리게 만든다. 그러면 cache hit와 수십 µs NAND miss의 비대칭을 정확하게 재현할 수 없다.

Cylon은 이를 해결하기 위해 다음 세 기법을 사용한다.

| 기법 | 역할 | 수정 위치 |
|---|---|---|
| Dynamic EPT Remapping (DER) | cache hit는 host DRAM에 직접 매핑하고 miss만 trap | Host KVM과 FEMU |
| Shared EPT Memory | EPTE를 LPN으로 O(1) 접근해 residency 전환 비용 감소 | Host KVM과 FEMU |
| Pluggable cache framework | FIFO, CLOCK, S3FIFO, prefetch 등의 정책 실험 | 주로 FEMU |

따라서 `6.4.6-cylon`은 host용 커널이다. Guest는 CXL Type-3 장치와 DAX를 인식할 수 있는 별도의 CXL-enabled kernel을 사용한다.

```text
Host modified kernel: KVM/EPT dual-mode 지원
Guest CXL kernel:      CXL Type-3, region, DAX/system-ram 지원
```

Host의 `CONFIG_CXL_MEM` 또는 `CONFIG_DEV_DAX_CXL`이 꺼져 있어도 Cylon host 실행 자체에는 문제가 아니다. CXL 장치를 인식해야 하는 쪽은 guest이며, host는 KVM과 `/dev/mem` backend를 제공한다.

### 4.2 Dynamic EPT Remapping 동작

Cylon은 CXL-SSD의 각 4 KiB page를 EPT leaf entry인 EPTE의 두 상태로 표현한다.

```text
Guest load/store
├── Direct EPTE
│   └── [reserved HPA | DIRECT_MASK]
│       └── host DRAM에서 직접 load/store, VM-exit 없음
└── Trap EPTE
    └── R/W/X 비활성 또는 MMIO SPTE
        └── EPT violation → KVM → QEMU/FEMU
            └── NAND timing + cache policy 처리
                └── cache fill 후 Direct EPTE로 전환

cache eviction
└── Direct EPTE → Trap EPTE
```

논문상 clean eviction은 EPTE만 Trap 상태로 바꾸고, dirty eviction은 FEMU backend에 write-back을 완료한 뒤 Trap으로 바꾼다. 변경된 translation이 남지 않도록 INVEPT/INVVPID를 사용하며, 논문은 여러 page 전환을 batching/range invalidation하는 설계를 설명한다.

현재 코드에서 Direct와 Trap EPTE는 다음처럼 구성된다.

```c
#define DIRECT_MASK 0x600000000000977
#define MMIO_MASK   0x0000000586

direct_spte = hpa_base + (lpn << 12) | DIRECT_MASK;
trap_spte   = (gfn << 12) | MMIO_MASK;
```

여기서 `hpa_base`가 GRUB로 예약한 host physical memory의 시작 주소와 일치해야 한다. 현재 실험 설정에서는 `0x6880000000`이다. 이 주소는 kernel에 하드코딩하지 않고 `run-cxlssd.sh`에서 FEMU로 전달한다.

### 4.3 Linux KVM에 추가된 주요 인터페이스

현재 artifact의 핵심 kernel 변경은 다음과 같다.

| 변경 | 역할 | 코드 |
|---|---|---|
| `KVM_MEMSLOT_DUAL_MODE` | 일반 guest RAM과 구분되는 Cylon 전용 memslot | [`include/linux/kvm_host.h`](Cylon/CylonLinux/include/linux/kvm_host.h) |
| `aux`와 leaf SPT 저장소 | dual-mode memslot별 shared leaf page table 보관 | [`include/linux/kvm_host.h`](Cylon/CylonLinux/include/linux/kvm_host.h) |
| `KVM_GET_LINEAR_SPT` | kernel leaf SPT를 QEMU userspace에 매핑 | [`include/linux/kvm_ext.h`](Cylon/CylonLinux/include/linux/kvm_ext.h) |
| `KVM_SET_SPTE_FLAG` | ioctl 방식으로 특정 GPA의 SPTE 상태 변경 및 TLB flush | [`include/linux/kvm_ext.h`](Cylon/CylonLinux/include/linux/kvm_ext.h) |
| dual-mode TDP MMU path | 최초 fault에서 MMIO SPTE와 shared leaf SPT 설치 | [`arch/x86/kvm/mmu/tdp_mmu.c`](Cylon/CylonLinux/arch/x86/kvm/mmu/tdp_mmu.c) |
| shared SPT mapping | `remap_pfn_range()`로 leaf SPT를 QEMU에 노출 | [`arch/x86/kvm/mmu/mmu.c`](Cylon/CylonLinux/arch/x86/kvm/mmu/mmu.c) |
| emulation restart | miss 처리 후 동일 guest 명령을 다시 실행 | [`arch/x86/kvm/kvm_emulate.h`](Cylon/CylonLinux/arch/x86/kvm/kvm_emulate.h) |

VM 생성 시의 실제 순서는 다음과 같다.

1. FEMU가 CXL 영역을 `KVM_MEMSLOT_DUAL_MODE` flag가 있는 memslot으로 등록한다.
2. KVM이 memslot 크기에 맞춰 leaf SPT 영역을 미리 할당한다.
3. `KVM_GET_LINEAR_SPT` ioctl이 해당 SPT page들을 QEMU의 anonymous mapping에 연결한다.
4. Cylon TDP MMU fault handler가 dual-mode 영역에 MMIO SPTE를 설치한다.
5. FEMU cache controller가 page fill/eviction에 맞춰 LPN에 해당하는 EPTE를 Direct/Trap 상태로 전환한다.

### 4.4 논문 설명과 현재 공개 artifact 코드의 차이

논문은 userspace가 `<index, desired state, cookie>` descriptor를 전달하고, kernel이 허용된 PFN과 R/W/X bit만 검증·수정하며, per-EPTE synchronization 및 TLB invalidation batching을 수행하는 설계를 설명한다.

그러나 현재 repository commit `4c5e196c09676db18114f4d09509b290c7385978`의 실행 hot path는 코드 검토상 다음과 같다.

- `KVM_GET_LINEAR_SPT`가 kernel leaf SPT page를 QEMU userspace에 직접 매핑한다.
- [`femu_kvm_spte_set_mmio_flag()`](Cylon/CylonFEMU/hw/femu/kvm_ext.c)와 `femu_kvm_spte_clear_mmio_flag()`는 공유된 raw SPTE pointer에 직접 값을 쓴다.
- 두 함수는 직접 쓰기 직후 `return`하므로 뒤쪽의 `KVM_SET_SPTE_FLAG` ioctl 경로는 실행되지 않는다.
- Kernel ioctl 구현에는 `kvm_flush_remote_tlbs_range()`가 있지만, 현재 direct-write hot path가 논문에서 설명한 validation, locking, batching을 모두 수행하는지는 별도 검증이 필요하다.

따라서 졸업작품에서는 다음을 구분해 기록한다.

```text
paper design:    논문 §4.3의 의도와 보장
artifact path:   현재 commit에서 실제 실행되는 코드
our extension:   이후 직접 수정한 kernel/FEMU patch
```

논문의 safety 또는 batching을 그대로 구현했다고 주장하기 전에는 SPTE update 경로, TLB invalidation 횟수, concurrent eviction/prefetch 동작을 trace로 검증한다. Kernel/FEMU를 수정하면 해당 patch와 commit을 실험 결과에 함께 보존한다.

### 4.5 논문 원본 배치와 현재 서버 배치의 차이

논문은 guest vCPU를 host NUMA node 0에 두고 Cylon backend memory를 host node 1에 예약해 약 150 ns remote-NUMA hit latency를 사용한다. 현재 프로젝트는 사용자의 실험 조건에 따라 host node 1을 전혀 사용하지 않고 vCPU와 backend를 모두 node 0에 둔다.

| 항목 | Cylon 논문 | 현재 node0 baseline |
|---|---|---|
| Host vCPU 위치 | NUMA node 0 | NUMA node 0 |
| Host backend 위치 | NUMA node 1 | NUMA node 0 |
| Hit-path DRAM | remote NUMA | local NUMA |
| Guest 일반 DRAM | 96 GiB | 16 GiB |
| CXL-SSD/NAND | 96 GiB | 96 GiB |
| 논리 DRAM cache | 약 4.8 GiB | 약 4.8 GiB |

따라서 현재 설정은 **논문 원본 재현**이 아니라 **node0-isolated Cylon baseline**이다. Local DRAM hit latency가 논문보다 낮을 수 있으므로 MLC/pointer-chasing으로 실제 hit latency를 측정하고 모든 결과에 함께 기록한다. 논문 재현 실험이 필요하면 별도 실험으로 node 0 vCPU + node 1 backend 구성을 사용해야 한다.

### 4.6 현재 설치한 kernel에서 우리가 바꾼 것

현재 부팅된 `6.4.6-cylon`은 Cylon이 이미 수정한 kernel source를 빌드한 것이다. 이번 환경 구성에서 Cylon의 KVM C source를 추가로 수정하지 않았고, 주로 다음 build/configuration만 적용했다.

- `CONFIG_LOCALVERSION="-cylon"`
- `CONFIG_KVM=y`, `CONFIG_KVM_INTEL=m`
- `CONFIG_MEMORY_HOTPLUG=y`, `CONFIG_MEMORY_HOTREMOVE=y`
- `CONFIG_MEMORY_HOTPLUG_DEFAULT_ONLINE` 비활성화
- `CONFIG_DEFERRED_STRUCT_PAGE_INIT` 비활성화
- `CONFIG_IO_STRICT_DEVMEM` 비활성화

현재 실행 kernel 설정은 `/boot/config-6.4.6-cylon`이며 SHA-256은 다음과 같다.

```text
ca1ea7b65cf4345c3296fe56be4e34a4649b14d6872d3222d966cee895f41b18
```

`memhp_default_state=online_movable`, `ibt=off`, `memmap=...`은 kernel source 변경이 아니라 GRUB kernel command line이다. 특히 `memmap`과 `run-cxlssd.sh`의 `hpa_base`는 같은 물리 영역을 가리켜야 하지만, 주소가 바뀌어도 Cylon kernel을 다시 빌드할 필요는 없다.

## 5. 이 서버에서 사용할 host 배치

이 프로젝트에서는 host NUMA node 1을 실험에 사용하지 않고, Cylon과 모든 실험을 host node 0에서 수행한다.

### 5.1 현재 서버 topology

```text
Host node 0 CPUs: 0-23,48-71
Host node 1 CPUs: 24-47,72-95

Host node 0 physical end: 514 GiB = 0x8080000000
Host node 1 physical range: 514-1026 GiB
```

목표 배치는 다음과 같다.

```text
Host node 0
├── Linux와 실험 프로그램의 일반 메모리
├── Cylon VM 일반 DRAM 16 GiB
└── Cylon backend 96 GiB

Host node 1
├── RAM 전체 reserved
└── CPU는 offline 또는 실험 process에서 제외
```

### 5.2 GRUB 설정

node 0 끝의 96 GiB는 Cylon backend로 예약하고, node 1 전체는 사용하지 않도록 예약한다.

```bash
GRUB_CMDLINE_LINUX="ibt=off memmap=96G\\\$0x0000006880000000 memmap=512G\\\$0x0000008080000000"
```

두 `memmap`은 모두 물리 주소 범위를 Linux의 일반 RAM allocator에서 제외한다. 첫 번째 영역이 Cylon용인 이유는 Cylon의 `hpa_base`가 그 주소를 선택하기 때문이다.

```text
node 0 Cylon range: 0x6880000000-0x807fffffff (96 GiB)
node 1 reserved:    0x8080000000-0x1007fffffff (512 GiB)
```

적용 절차:

```bash
sudo update-grub
grep -F 'memmap=96G' /boot/grub/grub.cfg | head
grep -F 'memmap=512G' /boot/grub/grub.cfg | head
sudo reboot
```

재부팅 후 반드시 확인한다.

```bash
uname -r
cat /proc/cmdline
numactl -H
lsmem
ls -l /dev/kvm
```

통과 조건:

- 커널이 `6.4.6-cylon`이다.
- `/proc/cmdline`에 두 `memmap`이 모두 있다.
- host node 0 메모리가 이전보다 96 GiB 감소했다.
- host node 1의 일반 메모리 크기가 0이다.
- `/dev/kvm`이 존재한다.

`$` 형식은 generic reserved memory를 생성하므로 `/dev/pmem0`이 없어도 정상이다. Cylon은 `/dev/mem`으로 물리 주소를 매핑한다.

> 현재 상태 기록(2026-07-27): `/etc/default/grub`과 `/boot/grub/grub.cfg`에는 위 설정이 들어갔지만 현재 `/proc/cmdline`에는 아직 `memmap`이 없다. 재부팅 전에는 Cylon을 실행하지 않는다.

### 5.3 Host node 1 CPU 제외

실험 process는 항상 다음 형태로 실행한다.

```bash
numactl --cpunodebind=0 --membind=0 <command>
```

node 1 CPU까지 offline하려면 다음 스크립트를 사용할 수 있다.

```bash
sudo bash ~/numa_util/cpu_online.sh offline
lscpu -e=CPU,NODE,ONLINE
```

복구:

```bash
sudo bash ~/numa_util/cpu_online.sh online
```

CPU를 offline하지 않는 경우에도 QEMU와 benchmark를 node 0에 명시적으로 bind해야 한다.

## 6. Node 0 실행본

단일 `Cylon` 저장소의 실험 branch에서 아래 host 전용 변경을 사용한다. 이 변경들은 실험 알고리즘 변경이 아니라 서버 topology에 맞추기 위한 실행환경 변경이다.

### 6.1 FEMU 빌드

```bash
cd ~/graduate_work/Cylon/CylonFEMU
mkdir -p build-femu
cd build-femu
cp ../femu-scripts/femu-copy-scripts.sh .
./femu-copy-scripts.sh .
sudo ./pkgdep.sh
./femu-compile.sh
```

빌드 결과 확인:

```bash
test -x ~/graduate_work/Cylon/CylonFEMU/build-femu/qemu-system-x86_64
```

### 6.2 Cylon backend 주소

`build-femu/run-cxlssd.sh`의 backend 설정을 다음처럼 바꾼다.

```bash
backend_dev="/dev/mem"
bdev_offset=0x6880000000
hpa_base=$bdev_offset
```

다음 세 값은 항상 일치해야 한다.

```text
GRUB 예약 시작 주소
Cylon bdev_offset
Cylon hpa_base
```

`ssd_size=98304`이므로 Cylon은 `0x6880000000`부터 96 GiB만 매핑한다.

### 6.3 QEMU와 FEMU thread를 node 0에 고정

QEMU 실행 줄은 다음 형태로 감싼다.

```bash
sudo numactl \
    --physcpubind=0-23,48-71 \
    --membind=0 \
    x86_64-softmmu/qemu-system-x86_64 \
```

Guest RAM 설정은 원본을 유지한다.

```bash
dram_size=16G

-object memory-backend-ram,size=$dram_size,policy=bind,host-nodes=0,...
```

`pin.sh`에서는:

```text
vCPU:   host CPU 2-9
poller: host CPU 16-23
```

을 사용한다. 원본의 `POLLCPU=29`는 host node 1이므로 `POLLCPU=23`으로 변경한다. 현재 poller는 8개이므로 23부터 16까지 사용한다.

### 6.4 최초 실행

```bash
cd ~/graduate_work/Cylon/CylonFEMU/build-femu
./run-cxlssd.sh 98304
```

다른 터미널에서:

```bash
cd ~/graduate_work/Cylon/CylonFEMU/build-femu
./pin.sh
```

실행 중 host 배치 확인:

```bash
qemu_pid=$(pgrep -n qemu-system-x86_64)
numastat -p "$qemu_pid"
ps -L -o pid,tid,psr,comm -p "$qemu_pid"
```

QEMU의 anonymous/guest RAM과 실행 CPU가 host node 0에 있어야 한다. Guest 내부에서 CXL 장치가 `node 1`로 보일 수 있는데, 이는 guest의 가상 NUMA 번호이며 host node 1을 사용한다는 뜻이 아니다.

## 7. Guest에서 Cylon 장치 준비

Guest 요구사항:

- CXL을 지원하는 Linux kernel
- `cxl`, `ndctl`, `daxctl`
- Cylon warm-up 도구

Guest에서 먼저 확인한다.

```bash
uname -r
sudo cxl list -vv
sudo ndctl list -R -D
```

CXL region과 DAX device를 만든다.

```bash
sudo cxl create-region -m -t ram -d decoder0.0 -w 1 -g 4096 mem0
sudo daxctl reconfigure-device --mode=devdax --force dax0.0
```

이 프로젝트의 page-aware DiskANN에는 `devdax`를 기본 모드로 사용한다.

```text
Guest /dev/dax0.0 offset
        ↓
Guest CXL physical address
        ↓
Cylon cache hit: direct EPT mapping
Cylon cache miss: FEMU trap + NAND latency
        ↓
Host node 0의 예약된 96 GiB
```

중요:

- qcow2 파일시스템에 있는 DiskANN index를 단순히 `pread()`하면 Cylon이 아니라 가상 디스크 I/O를 측정한다.
- Index를 `/dev/dax0.0`에 `mmap()`하고 데이터를 복사한 뒤, 탐색 코드가 그 mapping을 load/store해야 한다.
- Host RAM을 backend로 사용하므로 host 재부팅 후에는 index를 다시 적재한다.

## 8. 실험 단계

모든 조합을 한 번에 실행하지 않는다. 각 단계의 correctness gate를 통과한 뒤 다음 단계로 이동한다.

### Phase 0. 원본 Cylon 재현

목적:

- Cylon VM 부팅
- CXL region/devdax 생성
- 96 GiB 주소 범위 read/write 검증
- Cache hit/miss latency가 분리되는지 확인

실험:

1. 4 KiB 단위 sequential read/write
2. 4 KiB 단위 uniform random read/write
3. Working set을 cache보다 작게/크게 설정
4. Cylon original FIFO, cache 약 4.8 GiB, prefetch 0 유지

권장 working set:

```text
1 GiB   : cache에 충분히 들어감
4 GiB   : cache 경계 부근
8 GiB   : cache 초과
32 GiB  : 명확한 capacity miss
80 GiB  : NAND 전체에 가까운 대규모 접근
```

수집:

- 평균 bandwidth
- p50/p95/p99 latency
- cache read/write hit와 miss
- eviction 수
- FEMU가 부여한 NAND latency
- VM exit 수
- host CPU utilization

통과 조건:

- 같은 seed에서 데이터 검증 오류가 없다.
- Working set이 cache를 초과할 때 miss와 latency가 증가한다.
- 반복 실행 결과의 변동 원인을 설명할 수 있다.

### Phase 1. DiskANN baseline

비교군:

1. DRAM-only DiskANN
2. Cylon + 원본 DiskANN layout
3. Cylon + 원본 DiskANN layout + warm cache
4. Cylon + 원본 DiskANN layout + cold cache

고정할 DiskANN 파라미터:

- Index build의 `R`, `L`, `alpha`
- Search의 `L`, beam width, query thread 수
- PQ 사용 여부와 chunk 수
- Dataset, base/query/ground-truth checksum
- Query 순서와 random seed

핵심 결과:

- Recall@10-QPS curve
- Recall@10-p50/p95/p99 latency curve
- Query당 방문 node 수
- Query당 접근한 unique 4 KiB page 수
- Query당 cache miss 수

### Phase 2. Page-aware layout

Page 크기는 Cylon cacheline과 맞춰 4 KiB로 고정한다.

비교할 layout:

1. `ID-order`: 기존 DiskANN node ID 순서
2. `Neighbor-pack`: 한 node와 자주 접근하는 이웃을 같은 page에 greedy packing
3. `Starling-inspired`: graph locality를 높이는 reorder/block shuffle
4. `MARGO-inspired`: monotonic search path에서 중요한 edge에 높은 weight를 주고 해당 node를 근접 배치

이 단계에서는 Starling이나 MARGO 전체 시스템을 재구현했다고 주장하지 않는다. 논문에서 제안한 layout 목적함수를 CMM-H 4 KiB cache page에 맞춰 적용한 별도 설계로 정의한다.

Index format에 최소한 다음 정보를 기록한다.

```text
Header
├── magic/version
├── page_size = 4096
├── vector dimension/type
├── node_count
├── node-id → page-id/offset table
└── page payloads
    ├── node record
    ├── exact or compressed vector
    └── adjacency list
```

Layout 품질 지표:

- 같은 page에서 소비된 유효 node 수
- 읽었지만 사용하지 않은 byte 비율
- search path edge의 same-page 비율
- query당 unique page 수
- page reuse distance

### Phase 3. Layout와 cache policy co-design

Phase 2에서 좋은 layout 두 개만 선택해 다음 소규모 matrix를 실행한다.

| 축 | 값 |
|---|---|
| Layout | DiskANN, best page-aware 1, best page-aware 2 |
| Policy | FIFO, CLOCK, S3FIFO |
| Cache ratio | 2.5%, 5%, 10% |
| Prefetch degree | 0, 1, 2 |
| Workload | low/high recall, low/high concurrency |

원본 기준점은 항상 다음이다.

```text
FIFO + cache 5% + prefetch 0 + NAND 96 GiB
```

확인할 상호작용:

- Page-aware layout이 FIFO에서도 충분한가?
- S3FIFO가 반복 방문되는 navigation page를 더 오래 유지하는가?
- Layout이 순차성을 만들었을 때 next-page prefetch가 효과적인가?
- 높은 concurrency에서 cache pollution이 증가하는가?

### Phase 4. 최종 ablation

최종 기법에서 한 요소씩 제거한다.

1. Reordering 제거
2. Monotonic-path weight 제거
3. Page packing 제거
4. Prefetch 제거
5. Cache policy를 FIFO로 복원

최종 주장은 전체 성능뿐 아니라 각 요소가 miss 수와 tail latency를 어떻게 바꾸는지로 설명한다.

## 9. 측정 규칙

각 설정은:

1. 환경 검증
2. warm-up
3. 최소 5회 측정
4. median과 분산 또는 95% confidence interval 보고

순서로 실행한다.

Cold와 warm 결과를 섞지 않는다.

- Cold: Cylon VM/cache를 초기화하고 동일한 순서로 index를 적재
- Warm: 측정 전 고정된 warm-up query set을 1회 이상 실행

실험 사이에 변경하지 않을 것:

- CPU affinity
- guest DRAM
- NAND geometry
- query set 및 순서
- compiler와 optimization flag
- background service 상태

각 raw result에 다음 metadata를 저장한다.

```text
timestamp
host kernel
/proc/cmdline
Cylon commit
DiskANN commit
dataset checksum
layout
policy
cache size
prefetch degree
NAND latency
guest DRAM
CPU binding
full command line
```

권장 CSV schema:

```text
run_id,dataset,layout,policy,cache_mib,prefetch,ssd_mib,guest_dram_gib,
threads,search_l,beam_width,recall_at_10,qps,p50_us,p95_us,p99_us,
visited_nodes,unique_pages,read_hits,read_misses,write_hits,write_misses,
evictions,notes
```

## 10. 실험 결과로 인정하기 위한 correctness gate

다음 조건을 모두 만족해야 성능 결과로 사용한다.

- 원본 baseline commit과 현재 branch 및 변경 patch를 식별할 수 있다.
- 실행한 소스 commit과 변경 patch를 기록했다.
- Host `/proc/cmdline`에 의도한 memory reservation이 있다.
- QEMU, vCPU, FEMU poller가 host node 0 CPU에서 실행된다.
- Guest DRAM이 host node 0에서 할당된다.
- Cylon backend HPA가 host node 0 예약 영역에 있다.
- 현재 실행 경로가 paper design, 공개 artifact, 자체 수정본 중 무엇인지 기록했다.
- Direct/Trap EPTE 전환과 필요한 TLB invalidation이 실제로 동작함을 trace 또는 counter로 검증했다.
- Node 0 local-DRAM hit latency를 MLC 또는 pointer-chasing으로 측정해 결과와 함께 기록했다.
- DiskANN index 데이터가 qcow2가 아니라 CXL devdax mapping에 있다.
- Page-aware layout과 baseline의 검색 결과가 동일한 index 의미를 보존한다.
- Recall 저하 없이 비교하거나, 동일 Recall 지점에서 성능을 비교한다.

## 11. 알려진 주의사항

1. 원본 `run-cxlssd.sh`는 48/96 GiB geometry만 정의한다. 초기 실험에서 128 GiB로 변경하지 않는다.
2. 원본 launcher의 `/dev/cmahog`는 이 서버에서 사용할 수 없으므로 실험 branch에서 `/dev/mem`과 실제 HPA를 사용한다.
3. 일부 코드에는 `/home/necsst/...` log path가 하드코딩되어 있다. 실험 branch에서 사용자 경로 또는 `/tmp`로 변경하고 patch를 기록한다.
4. 원본 `pin.sh`의 poller CPU 29는 이 서버의 node 1이다. node 0 CPU로 변경한다.
5. Cylon의 cache policy 번호는 활성 코드 기준으로 확인한다. 현재 property 정의는 `1=LIFO, 2=FIFO, 3=CLOCK, 4=S3FIFO`이다.
6. Cylon backend의 실제 byte는 host의 예약 DRAM에 있고, NAND와 DRAM cache 동작은 timing 및 page-residency 모델이다.
7. Guest에서 보이는 CPU-less NUMA node 번호와 host NUMA node 번호를 혼동하지 않는다.
8. Cylon warm-up 도구가 전체 96 GiB 범위를 정상적으로 touch하는지 코드와 실행 시간을 확인한다.
9. 현재 node0-only 배치는 논문의 remote-NUMA 배치와 다르므로 논문 원본 재현이라고 부르지 않는다.
10. 공개 artifact의 raw shared-SPTE write 경로가 논문에 기술된 validation과 TLB batching을 그대로 구현한다고 가정하지 않는다.

## 12. 논문 및 코드

- [Cylon: Fast and Accurate Full-System Emulation of CXL-SSDs](https://www.usenix.org/conference/fast26/presentation/yoon)
- [Cylon artifact repository](https://github.com/MoatLab/Cylon)
- [DiskANN: Fast Accurate Billion-point Nearest Neighbor Search on a Single Node](https://papers.nips.cc/paper_files/paper/2019/hash/09853c7fb1d3f8ee67a61b6bf4a7f8e6-Abstract.html)
- [Microsoft DiskANN](https://github.com/microsoft/DiskANN)
- [Starling: An I/O-Efficient Disk-Resident Graph Index Framework](https://doi.org/10.1145/3639269)
- [MARGO: Select Edges Wisely—Monotonic Path Aware Graph Layout Optimization](https://www.vldb.org/pvldb/vol18/p4337-zheng.pdf)

## 13. 바로 다음 작업

1. Host를 재부팅해 GRUB memory reservation을 적용한다.
2. Host 검증 스크립트가 통과하는지 확인한다.
3. `Cylon`에서 FEMU를 빌드한다.
4. 원본 96 GiB Cylon을 부팅한다.
5. Guest에서 devdax와 warm-up을 검증한다.
6. 4 KiB random/sequential microbenchmark로 Phase 0 결과를 수집한다.
7. DiskANN baseline을 devdax load/store 경로에 연결한다.
