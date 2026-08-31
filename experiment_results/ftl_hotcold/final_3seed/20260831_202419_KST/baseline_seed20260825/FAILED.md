# Invalid attempt

- Stage: preconditioning 전 guest safety check
- Cause: guest `lsblk` 버전이 `MOUNTPOINTS` column을 지원하지 않아 검사 명령이 종료 코드 1을 반환함
- Device checks completed before the command error: 6 GiB, no partition, no mount, root device `/dev/sda2`
- Preconditioning started: no
- Performance fio started: no
- Result status: INVALID; final comparison에 사용하지 않음
- Recovery: guest가 지원하는 `MOUNTPOINT` column으로 helper만 수정하고 fresh FEMU process에서 새 디렉터리로 재실행
