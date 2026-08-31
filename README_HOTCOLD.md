# 🔥❄️ FEMU Hot/Cold FTL

**FEMU Blackbox FTL에 Hot/Cold 데이터 분리를 적용해 WAF 12.64% 감소**

Advanced System Programming Final Project · FEMU Blackbox Mode

> LBA rewrite frequency를 주 판단 신호로 사용하고, 경계 영역에서 LPN의 block-erase survival feedback을 추가하여 Hot/Cold 데이터를 서로 다른 line에 배치합니다.

## 📊 Results

### Primary comparison

| Metric | Baseline FTL | V3 Hot/Cold FTL | Improvement |
|---|---:|---:|---:|
| **WAF** | 7.990327 | **6.980748** | **↓ 12.635%** |
| GC overhead¹ | 699.0327% | **598.0748%** | **↓ 14.443%** |
| Average GC copy | 14335.056199 | **14038.413585** | **↓ 2.069%** |
| GC / 10⁶ Host pages | 487.638635 | **426.027315** | **↓ 12.635%** |
| IOPS | 8,224 | **9,442** | **↑ 14.810%** |

¹ `GC overhead = GC page writes / Host page writes × 100 = (WAF − 1) × 100`

```text
Workload: fio randwrite, 16 KiB, Zipf 0.99, QD128, 32 jobs
Runtime: 3600 seconds
Range:   size=5G, no offset
Seed:    20260824
Device:  8 GiB raw NAND / 6 GiB exposed / 25% OP
```

### Classifier ablation

| Version | Classification | WAF |
|---|---|---:|
| Baseline | Hot/Cold 분리 없음 | 7.990327 |
| V1 T=1024 | `interval ≤ 1024`만 HOT | 7.547217 |
| **V1 T=4096** | `interval ≤ 4096` 전체 HOT | **6.973143** |
| V3 | 경계 영역에서 erase survival로 선택 | 6.980748 |

V3는 Baseline보다 WAF를 12.635% 낮췄지만, 단순 V1 T=4096보다 0.109% 높았습니다. Erase-survival feedback은 실제 placement를 변경했으나 적용 대상이 전체 Host writes의 0.133%로 작아, 이 single-seed workload에서는 추가 WAF 개선으로 이어지지 않았습니다.

> 이 결과는 버전별 1회, 단일 seed 결과입니다. 작은 차이의 통계적 유의성은 주장하지 않습니다.

## 📌 Overview

기존 FEMU Blackbox FTL은 데이터 온도를 구분하지 않고 하나의 write pointer로 모든 page를 기록합니다. Hot page와 Cold page가 같은 line에 혼합되면 Hot page가 빠르게 invalidation되면서 GC를 유발하고, 같은 line에 남은 Cold valid page까지 반복 복사될 수 있습니다.

```text
Hot/Cold lifetime mixing
        ↓
Cold valid page의 반복 GC copy
        ↓
NAND writes와 block erase 증가
        ↓
WAF 증가 및 SSD 수명 감소
```

본 프로젝트는 다음 기능을 FEMU Blackbox FTL에 추가합니다.

- Host-write sequence 기반 LBA update-frequency 추정
- Current LPN version의 block-erase survival feedback
- Hot/Cold free-line pool과 dual write pointer
- Pool 고갈 시 반대 pool에서 line borrowing
- Host/GC/NAND write 기반 WAF 계측
- Classification reason 및 counter invariant

## 🏗️ Architecture

### 1. Hot/Cold classification

각 Host 4 KiB page write마다 `host_write_seq`를 증가시키고, 동일 LPN의 직전 write와 현재 write 사이의 sequence 차이로 update frequency를 추정합니다.

```text
interval = current_host_write_seq - last_write_seq[lpn]
```

최종 V3의 기본 rule은 다음과 같습니다.

```text
First write
└── COLD

interval ≤ 1024
└── HOT                              # fast rewrite

interval > 4096
└── COLD                             # slow rewrite

1024 < interval ≤ 4096
├── erase_survival_count = 0  → HOT
└── erase_survival_count ≥ 1  → COLD
```

Wall-clock 시간당 요청 수를 직접 세는 방식은 아닙니다. 정확한 표현은 다음과 같습니다.

> Host-write sequence 기준 rewrite interval을 이용해 LBA update frequency를 추정합니다.

### 2. LPN metadata

```c
typedef struct LpnMeta {
    uint64_t last_write_seq;       // 마지막 Host page-write sequence
    uint64_t update_interval;      // 직전 write와 현재 write의 sequence 차이
    uint32_t write_count;          // 관찰한 Host write 수
    uint32_t erase_survival_count; // 현재 version이 살아남은 block erase 수
    LpnState state;                // UNSEEN / COLD / HOT
} LpnMeta;
```

`erase_survival_count`는 LPN의 영구 누적값이 아니라 **현재 Host-written version의 lifetime**에만 속합니다.

- Host write로 새 version 생성 → `0`으로 reset
- Current version이 GC relocation과 source block erase를 생존 → `+1`
- 다음 Host write의 경계 classification에서 사용
- TRIM → metadata 전체 초기화

### 3. Dual line pool

```text
Total 128 lines
├── Hot Pool  (initial 64 lines)  ── wp_hot
└── Cold Pool (initial 64 lines)  ── wp_cold
```

```text
HOT Host/GC write  → wp_hot  → Hot line
COLD Host/GC write → wp_cold → Cold line
```

초기 비율은 50:50이지만 고정 partition으로만 동작하지 않습니다. 요청한 pool이 비면 반대 pool에서 free line을 빌리고 해당 line의 class를 변경합니다.

| 상황 | 처리 |
|---|---|
| Hot pool 고갈 | Cold pool에서 free line borrowing |
| Cold pool 고갈 | Hot pool에서 free line borrowing |
| 두 pool 모두 고갈 | 기존 foreground GC 후에도 불가하면 실패 |

Final V3 run에서는 Cold pool 고갈/borrowing이 10회, Hot pool 고갈과 emergency GC는 0회였습니다.

### 4. GC placement and erase feedback

GC relocation은 classifier를 다시 실행하지 않습니다. 저장된 LPN state만 읽어 Hot/Cold write pointer를 선택하며, `last_write_seq`와 `write_count` 같은 Host frequency history를 변경하지 않습니다.

```text
GC victim valid page
        ↓
저장된 state로 Hot/Cold destination 선택
        ↓
Page relocation
        ↓
Source block logical erase
        ↓
Current LPN version의 erase_survival_count++
```

GC victim selection은 Hot/Cold별 별도 정책으로 나누지 않았습니다. 기존 global valid-page-count 기반 greedy victim queue를 유지하고, **placement만 분리**하여 lifetime mixing을 줄이는 구조입니다.

### 5. WAF accounting

```text
NAND page writes = Host page writes + GC page writes

WAF = NAND page writes / Host page writes
    = 1 + GC page writes / Host page writes

Average GC copy = GC page writes / GC count
```

최종 stats는 다음 관계를 함께 검증합니다.

```text
NAND = Host + GC
Host Hot + Host Cold = Host
GC Hot + GC Cold = GC
Classification reason sum = Host classification sum
```

모든 최종 run에서 `counter_invariant=PASS`를 확인했습니다.

## 📁 Project Files

```text
FEMU/
├── hw/femu/bbssd/
│   ├── ftl.c              # Classifier, line pool, placement, GC, WAF stats
│   ├── ftl.h              # LPN metadata, line class, counters, write pointers
│   └── bb.c               # cdw10=5 stats/reset admin command
├── hw/femu/scripts/
│   └── run-blackbox.sh    # 8 GiB NAND, 6 GiB exposed, V3 environment
└── experiment_results/ftl_hotcold/v3/20260827_213254_KST/
    ├── analysis.md
    ├── manifest.md
    ├── correctness_summary.md
    ├── comparison_stats.txt
    ├── final_v1_T4096_seed20260824/
    └── final_v3_R1_seed20260824/
```

핵심 문서:

- [최종 결과 분석](experiment_results/ftl_hotcold/v3/20260827_213254_KST/analysis.md)
- [실험 Manifest](experiment_results/ftl_hotcold/v3/20260827_213254_KST/manifest.md)
- [Correctness 검증](experiment_results/ftl_hotcold/v3/20260827_213254_KST/correctness_summary.md)
- [원시 비교 Stats](experiment_results/ftl_hotcold/v3/20260827_213254_KST/comparison_stats.txt)

## 🚀 Getting Started

### Prerequisites

- Linux 또는 WSL2
- KVM 지원 x86_64 CPU
- Host memory 12 GiB 이상 권장
- Swap 4 GiB 이상 권장
- FEMU Guest image (`$HOME/images/u20s.qcow2`)
- Guest utilities: `fio`, `nvme-cli`

WSL2에서 KVM을 확인합니다.

```bash
sudo modprobe kvm
sudo modprobe kvm_intel   # AMD CPU는 kvm_amd

lsmod | grep kvm
ls -l /dev/kvm
```

### 1. Clone

```bash
git clone --branch hotcold/v3 --single-branch \
  https://github.com/thecoldestm0ment/FEMU-hotcold.git
cd FEMU-hotcold
```

### 2. Build FEMU

```bash
mkdir -p build-femu
cd build-femu

cp ../femu-scripts/femu-copy-scripts.sh .
./femu-copy-scripts.sh .
sudo ./pkgdep.sh
./femu-compile.sh
```

빌드 결과는 `build-femu/qemu-system-x86_64` 또는 FEMU build configuration에 따라 `build-femu/x86_64-softmmu/qemu-system-x86_64`에 생성됩니다.

### 3. Prepare Guest image

`run-blackbox.sh`는 다음 image를 사용합니다.

```text
$HOME/images/u20s.qcow2
```

FEMU 공식 README의 Guest image 준비 절차에 따라 image를 배치합니다.

### 4. Run V3 Blackbox FEMU

```bash
cd build-femu

FEMU_HOT_REWRITE_WINDOW=1024 \
FEMU_HOT_BOUNDARY_WINDOW=4096 \
FEMU_ERASE_SURVIVAL_THRESHOLD=1 \
./run-blackbox.sh
```

기본값도 `T_fast=1024`, `T_slow=4096`, `R=1`입니다. 잘못된 값과 `T_slow <= T_fast`는 FEMU 시작 시 즉시 거부됩니다.

### 5. Verify target device

Guest에서 실험 장치가 OS disk와 분리되어 있는지 반드시 확인합니다.

```bash
lsblk -b -o NAME,KNAME,TYPE,SIZE,PKNAME,MOUNTPOINT
findmnt -n -o SOURCE /
sudo blockdev --getsize64 /dev/nvme0n1
```

기대 조건:

```text
/dev/nvme0n1 = 6442450944 bytes
Partition 없음
Mount 없음
Guest root disk가 아님
```

### 6. Full-device preconditioning

```bash
sudo fio \
  --name=fill \
  --filename=/dev/nvme0n1 \
  --direct=1 \
  --ioengine=libaio \
  --rw=write \
  --bs=1M \
  --iodepth=32 \
  --numjobs=1 \
  --size=6G \
  --group_reporting=1

sync
```

### 7. Reset measurement accounting

```bash
sudo nvme admin-passthru /dev/nvme0 \
  --opcode=0xef --cdw10=5
```

이 command는 현재 stats를 Host FEMU log에 출력한 뒤 accounting counter만 reset합니다. Physical mapping/page/line/write-pointer state와 classifier history는 유지됩니다.

### 8. Run the 3600-second workload

```bash
sudo fio \
  --name=test \
  --filename=/dev/nvme0n1 \
  --direct=1 \
  --time_based=1 \
  --runtime=3600 \
  --ioengine=libaio \
  --rw=randwrite \
  --iodepth=128 \
  --numjobs=32 \
  --size=5G \
  --bs=16k \
  --random_distribution=zipf:0.99 \
  --randrepeat=1 \
  --randseed=20260824 \
  --group_reporting=0
```

Performance workload에는 offset과 marker를 사용하지 않습니다. fio 실행 중에는 stats/reset command를 호출하지 않습니다.

### 9. Collect final WAF

fio가 완전히 종료된 후 한 번만 실행합니다.

```bash
sudo nvme admin-passthru /dev/nvme0 \
  --opcode=0xef --cdw10=5
```

> ⚠️ 이 command는 stats를 출력한 직후 counter를 reset합니다. 반드시 Host FEMU log를 보존하고 workload 중간에는 호출하지 마세요.

V3 출력 예시:

```text
BBSSD-STATS version=V3
hot_rewrite_window=1024
hot_boundary_window=4096
erase_survival_threshold=1
host_page_writes=136261216
nand_page_writes=951205163
gc_page_writes=814943947
waf=6.980748
gc_count=58051
average_gc_copy=14038.413585
hot_write_ratio=0.151075
host_cold_survival_writes=181436
counter_invariant=PASS
```

## ✅ Correctness Evidence

다음 항목을 marker 기반 correctness test로 검증했습니다.

- First write → COLD
- Fast rewrite → HOT
- Boundary + survival=0 → HOT
- Boundary + survival≥R → COLD
- Slow rewrite → COLD
- TRIM 후 first write → COLD
- GC는 frequency metadata를 변경하지 않음
- Invalid runtime parameter 즉시 실패
- 모든 accounting invariant PASS

상세 trace는 [correctness_summary.md](experiment_results/ftl_hotcold/v3/20260827_213254_KST/correctness_summary.md)를 참고하세요.

## ⚠️ Limitations & Future Work

### Current limitations

- 최종 비교는 단일 seed, 버전별 1회 실행
- 중간 snapshot이 없어 steady-state WAF curve를 직접 확인하지 못함
- Sequence-based frequency는 wall-clock 요청률의 근사
- Preconditioning 후 classifier history 유지
- 초기 Hot/Cold pool 비율 50:50 고정
- LPN metadata 약 64 MiB
- V3 state transition 비율 11.83%
- Erase-survival signal이 전체 Host writes의 0.133%에만 적용
- V3 WAF가 V1 T=4096보다 0.109% 높음

### Future work

- 3개 이상 seed와 반복 실행으로 평균·표준편차 계산
- Reset 없는 periodic stats로 steady-state WAF 확인
- Dynamic Hot/Cold pool resizing
- Adaptive `T_fast`, `T_slow`, `R`
- Wall-clock/epoch frequency와 sequence interval 비교
- LPN metadata bit packing
- Erase-survival이 더 자주 발생하는 workload 평가

## 📚 References

- Huaicheng Li et al., **“The CASE of FEMU: Cheap, Accurate, Scalable and Extensible Flash Emulator,”** FAST '18.
- [FEMU upstream repository](https://github.com/MoatLab/FEMU)
- [Project experiment analysis](experiment_results/ftl_hotcold/v3/20260827_213254_KST/analysis.md)

## License

This project is based on FEMU/QEMU and follows the licenses included in the upstream repository.
