# FEMU V3 erase-survival 실험 Manifest

## 1. 실험 범위

이 디렉터리는 V3의 correctness, erase-survival histogram, `V1 T=4096` control 및 V3 최종 3600초 성능 결과를 보존한다.

- 실험 루트 생성: 2026-08-27 21:32:54 KST
- 최종 performance seed: `20260824`
- R 선택용 seed: `20260823` — 최종 성능 비교에서 제외
- 최종 비교: 기존 Baseline, 기존 V1 T=1024, 신규 V1 T=4096, 신규 V3 R=1
- 최종 run은 각 버전 1회이며 평균/표준편차를 계산하지 않는다.

## 2. 소스 버전과 비교 의도

| 비교군 | Branch | Commit | 설정 | 역할 |
|---|---|---|---|---|
| Baseline | `hotcold/base` | `c0237bb12f183d87ea1abf2bf52e92f900f6c587` | 무분리 | 기존 FTL 기준점 |
| V1 | `hotcold/v1` | `8bf8e4b01035e6bd15e4c3bc4c2ff6e1c09cc285` | `T=1024` | 경계 영역 전체 COLD control |
| V1-wide | `hotcold/v1` | `8bf8e4b01035e6bd15e4c3bc4c2ff6e1c09cc285` | `T=4096` | 경계 영역 전체 HOT control |
| V3 | `hotcold/v3` | `cbb044e32ea4714037e956aaba653b301a25779d` | `T_fast=1024, T_slow=4096, R=1` | 경계 영역만 erase-survival로 선택 |

V3는 V1에서 분기했다. `interval <= 1024`는 HOT, `interval > 4096`는 COLD로 판정하고, `1024 < interval <= 4096`에서만 current LPN version의 erase-survival count를 사용한다. `R=1`이면 survival이 0인 경계 write는 HOT, 1 이상은 COLD다.

Baseline과 V1 T=1024 원시 결과는 이전 최종 실험의 동일 seed 자료를 재사용했다.

```text
hotcold/v2.5:
experiment_results/ftl_hotcold/final_single_seed/20260824_211618_KST
```

## 3. 공통 환경

- Host: WSL2, KVM acceleration
- Guest: Ubuntu 20.04 계열, fio 3.16
- Guest vCPU / memory: 4 vCPU / 4 GiB
- Guest root: `/dev/sda2`
- Target: `/dev/nvme0n1`, 6 GiB (`6442450944` bytes), partition/mount 없음
- Raw NAND: 8 GiB
- FTL page: 4 KiB
- Geometry: 8 channels × 8 LUNs × 128 blocks × 256 pages × 4 KiB
- GC thresholds: 75% / 95%

## 4. 최종 performance 절차

신규 `V1 T=4096`과 V3 run은 각각 다음 순서로 수행했다.

1. 해당 branch checkout 및 기존 소스 build
2. fresh FEMU/QEMU 시작
3. target 크기, partition, mount, root disk 분리 검사
4. 전체 6 GiB sequential preconditioning
5. `sync`
6. `cdw10=5`로 preconditioning stats 출력 및 accounting counter reset
7. 3600초 fio를 중단 없이 실행
8. fio 종료 후 `cdw10=5`로 final stats 수집
9. 정상 poweroff, QEMU 완전 종료 확인

Preconditioning 후 physical mapping/page/line/write-pointer state와 classifier metadata는 현재 구현대로 유지했다. Performance 구간에는 marker, offset, 중간 snapshot 및 중간 reset이 없다.

```text
filename=/dev/nvme0n1
direct=1
time_based=1
runtime=3600
ioengine=libaio
rw=randwrite
iodepth=128
numjobs=32
size=5G
bs=16k
random_distribution=zipf:0.99
randrepeat=1
randseed=20260824
group_reporting=0
```

## 5. Run 목록

| Directory | 목적 | 판정 |
|---|---|---|
| `correctness_routing` | first/fast/boundary/slow/TRIM routing | VALID |
| `correctness_survival_valid` | 실제 erase survival과 경계 COLD 보정 | VALID |
| `histogram_seed20260823_R1` | 300초 survival histogram, R 선택 | VALID, final 비교 제외 |
| `final_v1_T4096_seed20260824` | 경계 전체 HOT control, 3600초 | VALID |
| `final_v3_R1_seed20260824` | V3 최종, 3600초 | VALID |
| `correctness_default` | boot-only setup attempt | EXCLUDED |
| `correctness_survival` | stale launcher parameter attempt | EXCLUDED |

## 6. 유효성 검증

신규 성능 run은 모두 다음을 만족했다.

- FEMU build 성공
- target 정확히 6 GiB, partition/mount 없음, root disk와 분리
- 6 GiB preconditioning rc=0
- measurement reset rc=0
- fio rc=0, runtime 약 3600초, size=5G, offset/marker 없음
- final stats command rc=0
- `nand_page_writes = host_page_writes + gc_page_writes`
- `gc_hot_writes + gc_cold_writes = gc_page_writes`
- reason 및 survival histogram 합계 invariant PASS
- `counter_invariant=PASS`
- 정상 poweroff 및 QEMU 종료

V3 final은 fio 종료 뒤 Guest idle 상태에서 final stats 수집이 지연됐지만, target은 unmounted raw device였고 fio issued I/O 수와 final host counter가 정확히 일치했다.

```text
34,065,304 fio writes × 4 FTL pages/write = 136,261,216 host_page_writes
```

따라서 지연 시간 동안 target write가 추가되지 않았다.

## 7. 소스 무결성

V1 T=4096과 V3 run 모두 `ftl.c`, `ftl.h`, `bb.c`, source launcher의 실행 전/후 SHA-256이 동일하다. 최종 `git diff --exit-code`도 보호 소스에 차이가 없었다. 즉 성능 run 도중 소스 수정은 없었다.

## 8. 핵심 결과 파일

- `analysis.md`: 최종 비교 및 설계 해석
- `correctness_summary.md`: correctness 판정
- `comparison_stats.txt`: 네 비교군의 원시 BBSSD-STATS
- 각 final directory의 `fio_raw.txt`, `stats.txt`, `results_summary.txt`, `manifest.md`
- histogram directory의 `stats.txt`, `histogram_summary.txt`, `results_summary.txt`, `manifest.md`
