# FEMU V2.5 correctness·E sweep·최종 비교 manifest

- Run ID: `20260823_030244_KST`
- Branch: `hotcold/v2.5`
- V2.5 implementation commit: `61bab228ab614bb6cbec85246ff442989f35c42f`
- V2 reference source commit: `c7ac5c55a3343bc5f5f4a63532e7848103122dc6`
- 최종 설정: `T=1024`, `A=3`, `C=2`, `D=65536`, decay ON, `E=4096 (4T)`

## 실행 범위

1. V2에서 seed 20260819의 stale-Hot 후보를 측정했다.
2. V2.5 runtime correctness로 `idle_age <= E`와 `idle_age > E`, metadata 불변, Host classifier, TRIM, validation과 counter invariant를 검증했다.
3. seed 20260819 하나로 E=2048/4096/8192 sweep을 수행했다.
4. 가장 낮은 WAF를 보인 E=4096을 선택하고 V2와 V2.5를 seed 20260819/20/21로 paired 비교했다.

## 고정 FEMU·workload 조건

- 6 GiB exposed / 8 GiB raw NAND, OP 25%
- 8 channels × 8 LUNs × 1 plane × 128 blocks
- 4 KiB page, 256 pages/block
- GC thresholds 75% / 95%
- Hot/Cold line pool, dual write pointer, victim selection policy 고정
- KVM, 4 vCPU, Guest RAM 4 GiB
- fresh FEMU process/device per performance run
- 전체 6 GiB 1 MiB sequential preconditioning
- LPN 0 marker write 2회 후 stats-only reset
- offset 1 GiB, size 4 GiB, 16 KiB randwrite, libaio, QD128, 32 jobs
- Zipf 0.99, `randrepeat=1`, 300초, seed별 `randseed`

## 유효성과 correctness

- 모든 performance run에서 device size, 미마운트, 무파티션, Guest root 분리 확인 PASS
- 모든 fill, marker, reset, fio, stats command 종료 코드 0
- 모든 최종 `counter_invariant=PASS`; 별도 파서로 두 counter 관계를 재계산해 PASS
- `historical HOT && idle_age > E` 8회가 effective COLD로 이동하고 metadata 5개 값이 불변
- `historical HOT && idle_age <= E` 6회가 effective HOT으로 이동하고 metadata 5개 값이 불변
- Host `COLD→COLD→HOT`, TRIM 후 첫 write COLD, 재승격 PASS
- E=0과 비정수 E가 exit code 1로 시작 실패 PASS

초기 E=4,000,000 correctness sub-run 하나는 marker 사이 sync가 없어 Guest가 동일 LPN write 하나를 합쳤고 LPN 0이 HOT으로 재승격되지 않았다. 해당 sub-run은 경계 판정에서 제외했으며 marker 사이 sync를 넣은 corrected trace만 `correctness_trace.txt`에 기록했다. 성능 실행에는 영향이 없다.

## sudo와 민감정보

Guest의 `/etc/sudoers.d/femu-experiments`는 계속 `root:root 0440`이며 `visudo -cf`를 통과했다. 기존 정확한 명령을 보존하고 `/dev/nvme0n1`과 모든 인자를 고정한 seed 20260820·20260821 fio 명령만 추가했다. argument-free fio/nvme/blockdev, shell 또는 임의 root 권한은 허용하지 않았다.

전체 console, raw FEMU log, PID, QMP socket, SSH client state와 상세 machine inventory는 ignore 상태다. 최종 Guest에서 임시 SSH 공개키를 제거한 뒤 정상 poweroff했고 host 임시 개인키도 삭제했다.

## 결과 파일

- `stats.txt`: sweep과 최종 3-seed 원시 stats
- `results_summary.txt`: rc, fio, seed별 값, 평균과 표본 표준편차
- `manifest.md`: 재현 조건과 유효성
- `analysis.md`: 인과관계와 결론
- `correctness_trace.txt`: 핵심 runtime trace와 판정
