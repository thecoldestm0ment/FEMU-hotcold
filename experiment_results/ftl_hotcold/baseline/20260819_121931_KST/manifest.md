# No-separation baseline manifest

- Run ID: `20260819_121931_KST`
- Source commit: `22ecac90f15bd90894a115c5ad05baba4258be36`
- Source description: Phase1 WAF counters, single `struct write_pointer wp`, classifier/Hot·Cold pool 없음
- Execution: detached clean worktree at a temporary local path
- 기존 작업 보존: main worktree는 계속 `hotcold/v1`; source working tree는 변경하지 않음
- `hotcold/base`를 직접 실행하지 않은 이유: 해당 ref는 `74a95ec68` Phase5이며 이미 dual write pointer와 Hot/Cold classifier를 포함하므로 무분리 baseline이 아님
- Build: QEMU/FEMU 10.1.0, KVM, x86_64-softmmu

## Fixed device configuration

- Exposed device: 6144 MiB (`/dev/nvme0n1`, 6,442,450,944 bytes)
- Raw NAND: 8 GiB, OP 25%
- Page: 4096 bytes; block: 256 pages
- 8 channels × 8 LUNs × 1 plane × 128 blocks
- GC thresholds: 75% / 95%
- Guest: fio 3.16, nvme-cli 1.9

## Workload

- Precondition: entire 6 GiB sequential write, 1 MiB, iodepth 32, direct libaio
- Measurement: `/dev/nvme0n1`, offset 1 GiB, size 4 GiB
- randwrite, 16 KiB, iodepth 128, 32 jobs, direct libaio
- Zipf 0.99, random seed 20260819, runtime 300 seconds, group reporting

V1 correctness용 LPN0 marker 두 번 쓰기는 무분리 baseline에는 classifier/GC-routing 검증 의미가 없어 생략했다. 이는 측정 전 counter 밖의 2 page write 차이이며 결과 비교 시 참고해야 한다.

## Persistent passwordless sudo

Guest image에 `/etc/sudoers.d/femu-experiments`를 root:root, mode 0440으로 영구 설치했고 `visudo -cf`를 통과했다. 규칙은 임의 root 인자를 허용하지 않고 다음 정확한 명령만 허용한다.

- `/dev/nvme0n1` size 조회용 blockdev
- 고정 admin stats reset 명령
- 위의 정확한 fill fio 명령
- 위의 정확한 baseline fio 명령
- `/dev/nvme0n1` 시작 4 bytes marker용 제한된 dd 명령
- 인자 없는 poweroff

이전 `/etc/sudoers.d/femu-v1` broad 임시 규칙은 제거했다. 새 규칙은 VM 종료 후 qcow2 Guest 이미지에 계속 남는다.

## Validity

- device safety check PASS: 정확한 크기, unmounted, partition 없음, OS는 별도 `/dev/sda2`
- fill rc=0, measurement rc=0, stats reset rc=0
- NAND=Host+GC counter invariant PASS
- Guest clean poweroff 완료, QEMU 프로세스 종료 확인
- Phase1에는 gc_count/block_erases가 없어 average_gc_copy를 보고하지 않음
