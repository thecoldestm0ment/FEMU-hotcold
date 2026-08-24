# FEMU Hot/Cold FTL 최종 단일-seed 성능 실험 Manifest

## 1. 실험 범위

이 디렉터리는 baseline, V1, V2, V2.5를 동일한 workload와 동일한 seed로 비교한 최종 3600초 성능 실험을 보존한다.

- 실행 시각: 2026-08-24 21:19 KST ~ 2026-08-25 01:24 KST
- 공통 seed: `20260824`
- 반복 횟수: 버전별 1회
- 실행 순서: baseline → V1 → V2 → V2.5
- 각 실행은 이전 QEMU가 종료된 뒤 새 FEMU/QEMU process로 순차 실행했다.
- E sweep, 중간 snapshot, marker, offset, correctness workload는 수행하지 않았다.
- 실험 중 FTL 소스와 launcher를 수정하지 않았다.

## 2. 비교군과 소스 버전

| Version | Branch | Commit | Runtime parameters |
|---|---|---|---|
| Baseline | `hotcold/base` | `c0237bb12f183d87ea1abf2bf52e92f900f6c587` | 무분리 FTL |
| V1 | `hotcold/v1` | `8bf8e4b01035e6bd15e4c3bc4c2ff6e1c09cc285` | `T=1024` |
| V2 | `hotcold/v2` | `6e394b433833f5839cdea6fad5b84e3371053e9e` | `T=1024, A=3, C=2, D=65536, decay=on` |
| V2.5 | `hotcold/v2.5` | `c056f18b523ab7220d8ce1b297fe2ba476355c03` | V2 설정 + `E=4096` |

보호 대상인 `hw/femu/bbssd/ftl.c`, `ftl.h`, `bb.c`, `hw/femu/scripts/run-blackbox.sh`는 실험 완료 후 `git diff`가 없음을 확인했다.

## 3. 실행 환경과 FEMU geometry

- Host: WSL2, Linux `6.18.33.2-microsoft-standard-WSL2`
- WSL memory limit: 12 GiB 설정값(`free`에서는 약 11 GiB로 표시)
- WSL swap: 8 GiB
- Acceleration: KVM
- Guest: Ubuntu 20.04 계열, Linux 5.4.0-64, fio 3.16
- Guest vCPU: 4
- Guest memory: 4 GiB
- Guest OS disk: `/dev/sda2`
- 실험 장치: `/dev/nvme0n1`, 정확히 6 GiB(`6442450944` bytes)
- Raw NAND: 8 GiB

FEMU device geometry와 timing은 네 비교군에서 동일하다.

```text
devsz_mb=6144
secsz=512
secs_per_pg=8
pgs_per_blk=256
blks_per_pl=128
pls_per_lun=1
luns_per_ch=8
nchs=8
pg_rd_lat=40000
pg_wr_lat=200000
blk_er_lat=2000000
ch_xfer_lat=0
gc_thres_pcent=75
gc_thres_pcent_high=95
```

따라서 FTL page 크기는 4 KiB이고, raw NAND 용량은 `4 KiB × 256 × 128 × 1 × 8 × 8 = 8 GiB`다.

## 4. 공통 실행 절차

각 버전은 다음 순서로 실행했다.

1. 해당 branch checkout
2. 기존 소스로 FEMU build
3. fresh FEMU/QEMU 시작
4. target safety 검사
5. 전체 6 GiB sequential preconditioning
6. `sync`
7. `cdw10=5`로 preconditioning stats 출력 및 accounting counter reset
8. fio workload를 중단 없이 3600초 실행
9. fio가 완전히 종료된 후 `cdw10=5`를 한 번 호출해 최종 stats 수집
10. 정상 poweroff 후 QEMU 종료 확인

Safety 검사에서 네 실행 모두 다음을 확인했다.

- `/dev/nvme0n1` 크기: `6442450944` bytes
- target partition: 없음
- target mount: 없음
- Guest root: `/dev/sda2`, 즉 target과 분리

### Preconditioning

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
```

모든 실행에서 6144 MiB가 기록되었고 fio exit code는 0이었다. Preconditioning 직후 stats는 공통으로 `host_page_writes=1572864`, `nand_page_writes=1572864`, `gc_page_writes=0`, `WAF=1.0`, invariant PASS였다.

`cdw10=5`는 stats를 출력한 뒤 accounting counter만 reset한다. 물리 mapping, page/line/write-pointer 상태와 Hot/Cold classifier metadata는 유지된다. 따라서 Hot/Cold 버전의 sequential fill에서 학습된 COLD 상태와 history가 measurement 시작점에 남고, measured workload는 이미 데이터가 채워진 장치에 대한 overwrite로 처리된다.

### Performance workload

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

- Offset: 없음
- Marker: 없음
- 실행 중 stats command 또는 reset: 없음
- IOPS는 fio의 `Run status group 0 (all jobs)` 결과를 사용했다.

## 5. Run 디렉터리

| Version | Run directory | fio runtime |
|---|---|---:|
| Baseline | `20260824_211940_KST_baseline_seed20260824` | 3600.565 s |
| V1 | `20260824_222043_KST_v1_seed20260824` | 3600.541 s |
| V2 | `20260824_232212_KST_v2_seed20260824` | 3600.233 s |
| V2.5 | `20260825_002321_KST_v2.5_seed20260824_E4096` | 3600.399 s |

각 디렉터리의 핵심 파일은 다음과 같다.

- `fio_raw.txt`: 32-job fio 원본 출력
- `stats.txt`: measurement 종료 후 최종 BBSSD-STATS
- `results_summary.txt`: run 설정과 핵심 결과
- `manifest.md`: 개별 run manifest
- `precondition_fio.txt`: sequential fill 원본 출력
- `precondition_reset.txt`: measurement 시작 boundary command 결과
- `all_stats_boundaries.txt`: preconditioning 및 최종 stats 원문
- `safety.txt`: target 장치 크기, partition, mount, root 분리 검사
- `build.txt`: 해당 commit build 기록
- `host_femu.log`: Host FEMU log
- `poweroff.txt`: Guest 정상 종료 요청 결과

## 6. 원시 결과

| Version | Host writes | NAND writes | GC writes | Block erases | GC count | Avg GC copy | WAF | IOPS |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Baseline | 118,774,428 | 949,046,548 | 830,272,120 | 3,706,816 | 57,919 | 14335.056199 | 7.990327 | 8,224 |
| V1 | 125,887,244 | 950,098,362 | 824,211,118 | 3,710,848 | 57,982 | 14214.948053 | 7.547217 | 8,733 |
| V2 | 124,986,956 | 949,926,602 | 824,939,646 | 3,710,144 | 57,971 | 14230.212451 | 7.600206 | 8,670 |
| V2.5 | 125,238,276 | 949,917,758 | 824,679,482 | 3,710,144 | 57,971 | 14225.724621 | 7.584884 | 8,673 |

Hot/Cold 관련 원시 결과:

| Metric | V1 | V2 | V2.5 |
|---|---:|---:|---:|
| Host Hot writes | 8,072,956 | 6,989,084 | 7,328,672 |
| Host Cold writes | 117,814,288 | 117,997,872 | 117,909,604 |
| Hot write ratio | 6.4128% | 5.5919% | 5.8518% |
| GC Hot writes | 574,728 | 85,952 | 54,676 |
| GC Cold writes | 823,636,390 | 824,853,694 | 824,624,806 |
| Cold → Hot | 5,477,976 | 4,451,120 | 4,556,428 |
| Hot → Cold | 5,476,908 | 4,450,920 | 4,556,208 |
| Cold pool empty / borrow | 10 / 10 | 10 / 10 | 10 / 10 |
| Hot pool empty | 0 | 0 | 0 |
| Emergency GC | 0 | 0 | 0 |

V2.5 expiration 관련 원시 결과:

| Metric | Count |
|---|---:|
| Candidates `>T` | 76,948 |
| Candidates `>2T` | 62,810 |
| Candidates `>4T` | 44,006 |
| Candidates `>8T` | 25,450 |
| Stale-Hot GC demotions | 44,006 |

## 7. 유효성 판정

네 실행은 모두 다음 조건을 만족하여 valid result로 판정했다.

- build 성공
- preconditioning rc=0
- measurement reset command 성공
- fio rc=0
- runtime 약 3600초
- 최종 stats command 성공
- `nand_page_writes = host_page_writes + gc_page_writes`
- Hot/Cold 버전에서 `gc_hot_writes + gc_cold_writes = gc_page_writes`
- `counter_invariant=PASS`
- 정상 poweroff 및 다음 실행 전 QEMU 종료 확인

