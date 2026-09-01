# FEMU Hot/Cold FTL

FEMU Blackbox SSD의 non-FDP FTL에 **Hot/Cold 데이터 분류, 분리 배치,
Class-aware GC**를 구현하고 Write Amplification Factor(WAF)를 비교한
프로젝트입니다.

## What Was Changed

### 1. LPN별 Hot/Cold 분류

Host의 4 KiB page write가 발생할 때마다 logical sequence를 증가시키고,
각 LPN의 observation window 안에서 write 빈도를 측정합니다.

| 조건 | 분류 |
|---|---|
| `writes_in_window <= 1` | COLD |
| `writes_in_window >= 4` | HOT |
| `writes_in_window = 2~3` | erase-event feedback으로 결정 |

```text
Logical window       = 4096 Host page writes
Erase-event threshold = 1

erase_event_count >= 1 → COLD
erase_event_count == 0 → HOT
```

GC relocation은 Host access로 세지 않으며 classifier history도 갱신하지
않습니다.

### 2. Hot/Cold 분리 배치

Hot과 Cold 데이터가 서로 다른 line에 기록되도록 두 개의 write pointer와
line pool을 사용합니다.

```text
Hot write  → wp_hot  → Hot line
Cold write → wp_cold → Cold line
```

전체 128개 line은 Hot 25개, Cold 103개로 시작합니다. 요청한 pool의 free
line이 부족하면 반대 pool에서 borrowing하고, 빌린 line의 `data_class`를
실제 사용 class로 변경합니다.

### 3. Class-aware GC

기존 Global Greedy 대신 Hot과 Cold의 특성에 맞는 victim selection을
적용했습니다.

| Class | Normal candidate | Forced candidate | 선택 기준 |
|---|---:|---:|---|
| Hot | invalid ratio >= 12.5% | `ipc > 0` | `max(ipc)` |
| Cold | invalid ratio >= 30% | invalid ratio >= 25% | `max(age × ipc)` |

Cold의 `age`는 실제 시간이 아니라 해당 line의 마지막 Host write 이후
진행된 Host page-write sequence입니다. GC relocation은 이 age를 갱신하지
않습니다.

GC할 class는 borrowing 이후 실제 Hot/Cold line ownership을 기준으로
normalized pressure를 비교해 선택합니다. Class selector로 진행할 수 없는
forced GC에서만 기존 Global Greedy를 emergency fallback으로 사용합니다.

### 4. 통계와 불변식 검증

다음 지표를 FTL 내부에서 측정합니다.

- Host, GC, NAND page writes와 WAF
- GC count, block erase, 평균 valid-page copy
- Hot/Cold Host writes와 GC relocation
- Hot/Cold victim count, 평균 copy, 평균 invalid ratio
- borrowing, opposite-class selection, emergency fallback

최종 실험에서는 다음 관계가 모두 PASS였습니다.

```text
NAND writes = Host writes + GC writes
Host Hot + Host Cold = Host writes
GC Hot + GC Cold = GC writes
Hot victim + Cold victim = GC count
Current Hot lines + Current Cold lines = Total lines
```

Hot/Cold 기능은 non-FDP Blackbox 경로에 적용했으며, FDP 경로와 기존
mapping/rmap, TRIM, NAND timing 경로는 유지했습니다.

## Final Results

동일한 장치 구성, preconditioning, GC threshold와 fio workload에서
Hot/Cold 분리가 없는 Baseline, V4 Global Greedy, V4 ClassGC를 실행했습니다.

| Metric | Baseline | V4 Global | V4 ClassGC | ClassGC vs Baseline |
|---|---:|---:|---:|---:|
| **WAF** | 7.989174 | 7.147977 | **3.060527** | **-61.69%** |
| Avg. GC copy | 14,334.94 | 14,093.52 | **11,034.09** | **-23.03%** |
| GC / 1M Host writes | 487.6 | 436.2 | **186.7** | **-61.70%** |
| IOPS | 8,127 | 9,240 | **약 22,200** | 약 2.73배 |
| Avg. latency | 503.88 ms | 443.24 ms | **184.22 ms** | **-63.44%** |
| Counter invariant | PASS | PASS | PASS | 정상 |

Baseline 대비 수치는 전체 Hot/Cold FTL의 결과입니다. GC 정책만의 효과는
classifier, 20:80 초기 pool, borrowing과 GC trigger가 동일한 V4 Global과
V4 ClassGC를 비교해야 하며, 이 비교에서 WAF는 57.18% 감소했습니다.

ClassGC victim 결과:

| Victim | GC count | Avg. copy | Avg. invalid ratio |
|---|---:|---:|---:|
| Hot | 3,396 | 3,841.34 | 76.55% |
| Cold | 56,399 | 11,467.19 | 30.01% |

Global Greedy는 GC 한 번에 평균 14,093.52개의 valid page를 복사했습니다.
한 line이 16,384 page이므로 평균 invalid ratio는 약 13.98%로
역산됩니다. ClassGC는 invalid page가 더 많이 누적된 victim을 선택하여
GC copy와 Host write당 GC 발생 빈도를 함께 낮췄습니다.

## Experiment Setup

| 항목 | 설정 |
|---|---|
| FEMU mode | Blackbox |
| Raw NAND / Exposed | 8 GiB / 6 GiB |
| Over-Provisioning | 25% |
| NAND page | 4 KiB |
| Initial Hot:Cold lines | 25:103 |
| Preconditioning | 6 GiB sequential write |
| Workload | fio 5 GiB randwrite |
| Block size | 16 KiB |
| iodepth / numjobs | 128 / 32 |
| Distribution | Zipf 0.99 |
| Runtime / Seed | 3600 s / 20260824 |

Preconditioning 후 measurement reset으로 통계와 classifier observation
history를 초기화했습니다. Mapping, page/line state와 write pointer는
유지하므로 실제 측정은 물리적으로 채워진 SSD에서 시작합니다.

## Main Files

```text
hw/femu/
├── bbssd/
│   ├── bb.c
│   ├── ftl.c
│   └── ftl.h
└── scripts/
    └── run-blackbox.sh
```

## Branches and Results

| Version | Branch | Commit |
|---|---|---|
| Baseline | `hotcold/base` | `c0237bb12f183d87ea1abf2bf52e92f900f6c587` |
| Global Control | `hotcold/v4` | `032d29b83e3906593ef9f79e5d2739bdab27ef5e` |
| ClassGC | `hotcold/v4-classgc` | `c277fbcc99c016db265674cac7ce81c98addbdfc` |

```text
experiment_results/ftl_hotcold/
├── baseline_v4_env/
│   └── 20260902_042054_KST_baseline_seed20260824/
└── v4_global_vs_classgc_20260901_181151_KST/
    ├── final_global/
    └── final_classgc/
```
