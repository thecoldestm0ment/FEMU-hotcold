# FEMU BBSSD FTL V1 실험 manifest

## 식별 정보

- Run ID: `20260819_025314_KST`
- 실험 버전: V1 (latest rewrite interval)
- 작업 브랜치: `hotcold/v1`
- 기준 커밋: `74a95ec68700dc80334883f033ce34ae350176bc`
- Git ref 제약상 기존 local `hotcold` 브랜치는 같은 커밋을 가리키는 `hotcold/base`로 보존했다 (`hotcold`와 `hotcold/v1`은 file/directory ref로 공존 불가).
- 커밋 생성 여부: 없음. 소스 수정과 결과는 working tree에 남겨 두었다.
- Host: x86_64 Linux with KVM (machine-specific details retained only in ignored raw logs)
- QEMU/FEMU binary: QEMU 10.1.0
- Guest: Ubuntu 20.04 image, `fio-3.16`, `nvme-cli 1.9`

## 변경 범위

V1 classifier/placement 정책은 유지하고 실험 가능성과 관측성을 추가했다.

- `FEMU_HOT_REWRITE_WINDOW`로 V1 임계값 설정(미설정 기본값 1024, 0/잘못된 값 즉시 실패)
- 통계 reset 시 LPN history/state는 유지하고 측정 counter만 초기화
- host/GC hot·cold write, 전이, pool 고갈/borrow, emergency GC, GC count 계측
- WAF, average GC copy, hot write ratio, counter invariant 출력
- `run-blackbox.sh`에서 임계값 환경 변수를 sudo 경계 너머 QEMU에 전달

전체 패치는 `source_changes.patch`에 있다.

## 고정 FEMU 조건

- Exposed device: 6144 MiB (`/dev/nvme0n1`, 정확히 6,442,450,944 bytes)
- Raw NAND: 8 GiB; OP 25%
- Page: 4096 bytes (`secsz=512`, `secs_per_pg=8`)
- Block: 256 pages
- Geometry: 8 channels × 8 LUNs/channel × 1 plane/LUN × 128 blocks/plane
- Hot/Cold line pool: 50:50, dual write pointers
- GC thresholds: 75% / 95%
- GC policy와 victim selection: 버전 간 변경 없음
- VM: KVM, 4 vCPU, 4 GiB RAM; 매 임계값마다 fresh process/device state

## 임계값

| T (page writes) | Logical write distance |
| ---: | ---: |
| 64 | 256 KiB |
| 256 | 1 MiB |
| 1024 | 4 MiB |
| 4096 | 16 MiB |

## preconditioning 및 측정

Preconditioning은 각 fresh device 전체 6 GiB에 1 MiB sequential direct write를 수행했다. 그 뒤 LPN 0에 `V1GC` marker를 두 번 써서 GC relocation 확인점을 만들고, 통계만 reset했다.

측정 workload:

- Target: `/dev/nvme0n1`, offset 1 GiB, size 4 GiB
- `randwrite`, 16 KiB, `libaio`, direct I/O
- iodepth 128, numjobs 32
- time based 300 seconds
- Zipf 0.99
- `randrepeat=1`, `randseed=20260819`
- group reporting

시간 기반 측정이므로 처리량이 높은 설정은 동일한 300초 동안 더 많은 host page를 기록한다. WAF와 비율 지표를 주 비교 기준으로 사용했다.

## 안전 및 유효성

각 실행 전 다음을 확인했다.

- raw write 대상이 정확히 `/dev/nvme0n1`
- 크기 6,442,450,944 bytes
- partition 없음, `findmnt` 결과 없음
- Guest OS root는 별도 80 GiB `/dev/sda2`

네 valid run 모두 precondition/measurement 종료 코드 0, 통계 불변식 PASS, GC 발생 및 marker LPN relocation을 만족한다. `runs/T64_attempt1`은 fio 종료 후 최종 통계 전 host-attached QEMU가 끊긴 무효 실행이며 분석에서 제외했다.

실험용 SSH 공개키와 제한된 sudoers 항목은 마지막 Guest에서 제거했으며 VM은 정상 종료했다. 결과 디렉터리의 키 파일은 raw provenance 보존용으로 남기지 않는다.
