# Corrected no-separation baseline manifest

- Run ID: `20260819_134044_KST`
- Source commit: `f28e6fbb4e91406e0ccb6027fdc2f454bd2bb5ce`
- Base source: `22ecac90f15bd90894a115c5ad05baba4258be36`
- FTL: single write pointer, classifier/Hot·Cold pool 없음
- Correction: `gc_count`, `block_erases`, `average_gc_copy`, counter invariant 계측 추가
- Build: QEMU/FEMU 10.1.0, KVM, x86_64-softmmu

## Fixed conditions

- Exposed device: 6144 MiB (`/dev/nvme0n1`, 6,442,450,944 bytes)
- Raw NAND: 8 GiB, OP 25%
- Page/block: 4096 bytes / 256 pages
- Geometry: 8 channels × 8 LUNs × 1 plane × 128 blocks
- GC thresholds: 75% / 95%
- Guest: fio 3.16, nvme-cli 1.9

## Workload and boundary

- Fresh FEMU process/device state
- Full-device 6 GiB sequential fill, 1 MiB, QD32
- V1과 동일한 LPN 0 marker write 2회 후 accounting reset
- 4 GiB range at offset 1 GiB, randwrite 16 KiB, QD128, 32 jobs
- Zipf 0.99, seed 20260819, time based 300 seconds
- Marker를 포함한 reset 전 host write는 `1,572,866 = 1,572,864 + 2`로 확인됨

## Safety and validity

- target size, unmounted state, partition absence, separate OS root 확인 PASS
- fill, marker 2회, accounting reset, measurement의 실제 명령 rc는 모두 0
- preconditioning wrapper의 마지막 검사는 pipeline subshell 지역변수를 다시 참조해 종료했으나, reset 명령과 기록은 이미 rc=0으로 완료됨. 측정 데이터에는 영향 없음
- measurement stats: `gc_count=4814`, `average_gc_copy=14334.293103`, invariant PASS
- Guest의 제한 sudoers는 root:root 0440이며 `visudo -cf` PASS
- 임시 SSH 공개키 제거 후 Guest clean poweroff PASS

이 결과가 공식 baseline이다. `20260819_121931_KST` 결과는 gc_count 미계측으로 인해 감사 추적용으로만 유지한다.
