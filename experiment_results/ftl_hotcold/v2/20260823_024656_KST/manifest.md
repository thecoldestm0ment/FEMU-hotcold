# FEMU V2 공식 seed-1 실험 manifest

- Run ID: `20260823_024656_KST`
- Branch: `hotcold/v2`
- Source commit: `c7ac5c55a3343bc5f5f4a63532e7848103122dc6`
- 목적: V2 stale-Hot 후보를 관찰하고 V2.5 비교용 seed 20260819 기준값을 확보

## 고정 설정

- Classifier: `T=1024`, `A=3`, `C=2`, `D=65536`, decay ON
- Seed: `20260819`
- FEMU: 6 GiB exposed / 8 GiB raw NAND, OP 25%
- Geometry: 8 channels × 8 LUNs × 1 plane × 128 blocks
- Page/block: 4 KiB / 256 pages
- GC thresholds: 75% / 95%
- Hot/Cold line pool과 victim selection policy는 V1/V2 사이에 변경하지 않음
- VM: KVM, 4 vCPU, 4 GiB RAM

## 실행 경계

fresh FEMU device에서 `/dev/nvme0n1`이 정확히 6,442,450,944 bytes이고 파티션과 마운트가 없으며 Guest root가 `/dev/sda2`임을 확인했다. 전체 6 GiB를 1 MiB sequential write로 precondition한 뒤 LPN 0 marker를 두 번 기록하고 통계만 reset했다.

측정 workload는 offset 1 GiB, size 4 GiB, 16 KiB randwrite, libaio, iodepth 128, 32 jobs, Zipf 0.99, `randrepeat=1`, seed 20260819, 300초다. fio 실측 runtime은 300,420 ms이고 모든 명령과 최종 stats 수집이 종료 코드 0이었다.

## 계측 의미

`stale_hot_gc_candidates_gt_kT`는 historical HOT page가 GC relocation될 때 `idle_age > kT`인 page event를 센다. unique LPN 수가 아니며 같은 LPN의 반복 이동은 각각 포함한다. V2에서는 historical state와 destination이 같으므로 `gc_hot_writes`가 historical HOT GC relocation 전체 분모다.

## 안전과 보존

- `counter_invariant=PASS`
- Guest 정상 poweroff PASS
- Guest의 제한 sudoers에는 장치와 모든 인자를 고정한 seed 20260820·20260821 fio 명령만 추가함
- sudoers는 설치 전후 `visudo -cf` PASS, `root:root 0440`
- 전체 console, FEMU raw log, SSH 키와 상세 장치 목록은 ignored `raw_attempt`에만 보존
