# Cylon asynchronous CPU software prefetch

## 목적과 범위

이 작업 트리는 원본 `/home/ohdh95/graduate_work/Cylon`을 수정하지 않고,
CPU의 x86 `PREFETCHT0/T1/T2/NTA`가 cold CMM-H 페이지를 가리킬 때 Cylon에
비동기 NAND read를 미리 예약할 수 있는지를 검증하기 위한 실험용 구현이다.

기준 소스는 upstream `4c5e196c09676db18114f4d09509b290c7385978`이며,
작업 브랜치는 `experiment/async-sw-prefetch`이다. 이 구현은 실제 CXL 표준
인터페이스가 아니라 Cylon 전용 KVM/QEMU private ABI version 1을 사용한다.

## 요청 경로

```text
Guest PREFETCH instruction
  -> cold CMM-H page의 EPT violation
  -> custom KVM_EXIT_CYLON_PREFETCH (응답 데이터 없음)
  -> QEMU가 Cylon GPA 범위를 검증
  -> bounded prefetch ring에 enqueue 후 vCPU 즉시 재개
  -> FTL thread가 LPN/FTL mapping/cache 상태 확인
  -> modeled NAND read 예약, page 상태 ABSENT -> INFLIGHT
  -> NAND ready time 이후 DRAM cache entry와 Direct SPTE 공개
  -> page 상태 INFLIGHT -> RESIDENT
```

Demand load가 같은 LPN의 INFLIGHT fill을 발견하면 새 NAND read를 만들지 않고
그 fill의 waiter로 합류한다. 동일 page에 대한 여러 hint도 한 NAND read로
병합된다. hint 대상 LPN이 아직 FTL에 매핑되지 않았다면 mapping을 새로 만들지
않고 drop한다.

Demand/control과 hint는 별도 ring을 사용한다. FTL은 demand 한 건과 hint 한
건을 번갈아 분류하여 demand ring 용량을 보장하면서 continuous demand가 hint를
영원히 굶기는 것도 막는다. `cxl_async_max_inflight`는 이름과 달리 안전상
`queued hint + prefetch-origin inflight fill` 전체의 상한으로 적용된다.

## 활성화 조건

기능은 기본적으로 꺼져 있다. 실행할 때 다음 조건을 모두 고정한다.

```bash
export CYLON_ASYNC_SW_PREFETCH=1
export CYLON_ASYNC_MAX_INFLIGHT=4096
export CYLON_PREFETCH_DEGREE=0
export CYLON_CXL_SKIP_FTL=0
```

동일 값은 `configs/async-sw-prefetch.env`에, 100 ms 기능 smoke 전용 값은
`configs/async-sw-prefetch-smoke.env`에 보존한다. 실행 전 `set -a; source
<file>; set +a`로 export할 수 있다.

`CYLON_PREFETCH_DEGREE=0`은 필수다. 기존 Cylon Next-N은 인접 page를 NAND
timing 없이 즉시 resident로 만들기 때문에 async 실험과 함께 사용하면 결과가
무효가 된다. 장치 시작과 runtime degree 변경 경로 모두 nonzero 값을 거부한다.

96 GiB Cylon을 실행하는 예시는 다음과 같다.

```bash
cd /home/ohdh95/graduate_work/Cylon-async-prefetch/CylonFEMU
CYLON_ASYNC_SW_PREFETCH=1 \
CYLON_ASYNC_MAX_INFLIGHT=4096 \
CYLON_PREFETCH_DEGREE=0 \
CYLON_CXL_SKIP_FTL=0 \
./femu-scripts/run-cxlssd.sh 98304
```

QEMU는 host KVM의 `KVM_CAP_CYLON_PREFETCH_EXIT` 값이 정확히 1인지 시작할 때
검사한다. 기존 `6.4.6-cylon` 커널에서는 시작을 거부하며, 이 작업 트리에서
빌드한 matching kernel이 필요하다.

## 빌드

FEMU/QEMU:

```bash
cd /home/ohdh95/graduate_work/Cylon-async-prefetch/CylonFEMU/build-femu
ninja -j16 qemu-system-x86_64 \
  tests/unit/test-femu-cxl-async-fill \
  tests/unit/test-femu-cxl-request-lifetime \
  tests/unit/test-femu-ftl-map-digest
```

Host kernel의 별도 release는 `6.4.6-cylon-asyncpf`다.

```bash
cd /home/ohdh95/graduate_work/Cylon-async-prefetch/CylonLinux
make O=/home/ohdh95/graduate_work/.build/cylon-async-prefetch-kernel-full \
  -j16 bzImage modules
```

Probe:

```bash
mkdir -p /home/ohdh95/graduate_work/.build/async-prefetch-probe
gcc -O2 -g -Wall -Wextra -Werror -fno-lto \
  -o /home/ohdh95/graduate_work/.build/async-prefetch-probe/cylon_async_prefetch_probe \
  /home/ohdh95/graduate_work/Cylon-async-prefetch/tools/cylon_async_prefetch_probe.c
objdump -drwC \
  /home/ohdh95/graduate_work/.build/async-prefetch-probe/cylon_async_prefetch_probe \
  | sed -n '/<raw_prefetcht0>/,+8p'
```

`raw_prefetcht0`에 `0f 18 08`이 정확히 한 번 있어야 한다.

## 통계와 cache clear barrier

기존 guest CXL label 명령을 control channel로 사용한다.

```bash
# 이전 hint ring과 inflight fill까지 drain한 뒤 snapshot을 기록하고 counters reset
cxl read-labels mem0 -s 1 -O 0 >/dev/null

# 이전 hint/fill을 drain한 뒤 Cylon DRAM cache와 counters clear
cxl read-labels mem0 -s 2 -O 0 >/dev/null
```

결과는 host의 `/home/necsst/cxlssd_buffer.txt`에 append된다. 이 snapshot은
수동 관찰용 passive snapshot이 아니라 완료 barrier다. workload producer를
먼저 멈춘 다음 호출해야 하며, control이 진행되는 동안 새 hint admission은
닫힌다.

주요 필드는 다음과 같다.

- `callbacks`: cold/MMIO 상태에서 custom KVM exit로 전달된 hint 수
- `enqueued`, `processed`: ring에 들어간 수와 FTL이 분류한 수
- `nand_reads`: prefetch가 실제로 예약한 mapped NAND read 수
- `deduplicated`: 기존 fill과 합쳐진 hint 수
- `joined_prefetch_fill`: demand가 완료 전 합류한 prefetch-origin fill 수
- `joined_demand_fill`: demand-origin fill에 합류한 추가 demand 수
- `ring_pending`, `outstanding`: barrier snapshot에서는 모두 0이어야 함
- `admission_drops`, `queue_full_drops`, `outstanding_limit_drops`,
  `invalid_drops`, `unmapped_drops`, `inflight_cap_drops`: drop 원인
- `inflight_peak`: 해당 counter window에서 동시에 outstanding이었던 modeled
  NAND fill의 최대 수

`resident_hits`는 전체 prefetch hit 수가 아니다. Direct SPTE가 이미 설치된
resident page의 CPU PREFETCH는 QEMU/FTL을 완전히 우회하므로 Cylon counter에
잡히지 않는다. 따라서 보고용 효과는 end-to-end latency/QPS와 total NAND
reads로 판단하고, join counter는 완료 전 합류한 경우만 설명한다.

## 최초 runtime smoke test

성능 실험 전에 disposable VM과 소수 scratch LPN으로 다음 순서를 통과해야
한다. `tools/cxl_warmup`은 전 96 GiB를 매핑하므로 이 smoke test에서는 실행하지
않는다.

1. `map` mode와 `MAP_POPULATE`만으로 async/path counter가 증가하지 않는지 확인.
2. scratch page를 `seed`하고 cache clear하여 `mapped + cold` 상태 생성.
3. `prefetch` 한 번 후 `callbacks=enqueued=processed=nand_reads=1`,
   `completions=prefetch_unjoined_completions=1`, drop 0,
   `ring_pending=outstanding=0`, FTL mapping digest 불변인지 확인.
4. 같은 resident page에 다시 hint했을 때 custom callback과 NAND read가 모두
   0인지 확인.
5. mapped cold page 여러 개에 hint하여 `inflight_peak > 1`인지 확인.
6. 기능 smoke 전용 VM은 `CYLON_NAND_READ_LAT_NS=100000000`(100 ms)로
   시작한다. mapped page를 다시 clear한 다음
   `prefetch-demand --lead-us 1000`을 실행한다. async
   `callbacks=enqueued=processed=nand_reads=1`, normal
   `mmio_read_callbacks=1`, total `mapped_nand_reads=1`,
   `demand_joins=joined_prefetch_fill=completions=1`,
   `prefetch_unjoined_completions=0`, drop/dedup 0과 marker 검증 성공을
   확인한다. 이 100 ms 설정은 기능 검증 전용이며 성능 결과에는 쓰지 않는다.
   `nand_reads=0`, `deduplicated=1`, `joined_prefetch_fill=0`이면 demand가
   hint보다 먼저 분류된 timing-inconclusive run이므로 lead와 CPU 배치를
   점검해 다시 실행한다.
7. untouched/unmapped LPN은
   `callbacks=enqueued=processed=dropped=unmapped_drops=1`, NAND read와
   completion 0, mapping digest 불변인지 확인한다. untouched LPN은 fresh
   QEMU에서 한번도 seed/demand touch하지 않은 별도 범위를 사용한다.
8. 100 ms 기능 VM에서 mapped+cold 한 page에 `--repeat 8`을 사용한다.
   `callbacks=enqueued=processed=8`, `nand_reads=completions=1`,
   `deduplicated=7`, `prefetch_unjoined_completions=1`, drop 0,
   `inflight_peak=1`, pending/outstanding 0인지 확인한다.

각 cell은 반드시 다음 순서로 분리한다.

```text
workload stop -> cache clear(-s 2) -> empty/reset snapshot(-s 1)
-> probe -> drain/result snapshot(-s 1)
```

앞 cell의 prefetch가 page를 Direct/resident로 만든 상태에서 곧바로 join cell을
실행하면 hint와 demand가 모두 QEMU를 우회하므로 join을 검증할 수 없다.

예시 probe 호출:

```bash
./cylon_async_prefetch_probe map --offset-pages 4096 --pages 8 --cpu 0
./cylon_async_prefetch_probe seed --offset-pages 4096 --pages 8 --cpu 0
# 여기서 guest control 명령으로 cache clear
./cylon_async_prefetch_probe prefetch --offset-pages 4096 --pages 8 \
  --settle-us 1000 --cpu 0
# 다시 cache clear한 뒤 100 ms 기능검증 VM에서 실행
./cylon_async_prefetch_probe prefetch-demand --offset-pages 4096 --pages 1 \
  --lead-us 1000 --expect-markers --cpu 0
# 다시 cache clear한 뒤 duplicate merge 확인
./cylon_async_prefetch_probe prefetch --offset-pages 4096 --pages 1 \
  --repeat 8 --settle-us 110000 --cpu 0
```

`seed`는 scratch LPN의 첫 8 bytes를 marker로 실제 덮어쓴다. disposable VM과
본 인덱스 밖으로 예약한 LPN에서만 사용한다. `seed` 자체는 marker를 다시 읽어
검증하며, demand/join cell은 `--expect-markers`로 data correctness까지 검사한다.

## 중요한 한계

x86 PREFETCH는 architecturally non-faulting hint다. 현재 kernel 코드는 CPU가
실제로 EPT violation을 전달한 경우에만 opcode를 식별할 수 있다. 따라서
컴파일과 단위 테스트가 성공해도 실제 host CPU가 cold CMM-H page의 PREFETCH를
무시하면 `callbacks=0`일 수 있다. 이 경우 transparent `_mm_prefetch()` 경로는
성립하지 않으며, 명시적 paravirtual hypercall 또는 doorbell 방식으로 전환해야
한다. 본 실험 결과를 수집하기 전에 위 runtime smoke test가 반드시 필요하다.

이 실험 경로는 run script로 시작한 단일 Cylon 장치의 정상 VM 종료를 대상으로
하며, 실행 중 CXL/NVMe 장치 hot-unplug는 지원 범위가 아니다. 종료할 때는 guest
workload를 먼저 멈추고 VM을 종료한다.
