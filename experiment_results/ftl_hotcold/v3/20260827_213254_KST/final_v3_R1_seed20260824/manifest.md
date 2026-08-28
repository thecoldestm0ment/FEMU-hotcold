# V3 R=1 최종 3600초 performance run

- 판정: **VALID**
- Branch / commit: `hotcold/v3` / `cbb044e32ea4714037e956aaba653b301a25779d`
- Parameter: `T_fast=1024`, `T_slow=4096`, `R=1`
- Seed: `20260824`
- Fresh FEMU process, exposed 6 GiB / raw NAND 8 GiB
- `/dev/nvme0n1`은 정확히 6 GiB이고 partition/mount가 없으며 root `/dev/sda2`와 분리됨
- 전체 6 GiB sequential preconditioning 후 `cdw10=5`로 accounting counter만 reset
- Preconditioning의 physical FTL state와 classifier history는 유지
- Performance 구간에는 marker와 offset이 없고 중간 stats/reset도 없다.
- Workload: 16 KiB Zipf 0.99 randwrite, QD128, 32 jobs, size 5G, runtime 3600, randrepeat=1
- fio: rc=0, runtime=3600.623 s, aggregate integer IOPS=9442, BW=148 MiB/s
- Final: WAF=6.980748, GC writes=814943947, average GC copy=14038.413585, GC count=58051
- 모든 counter invariant PASS, 정상 poweroff PASS
- 보호 소스의 실행 전/후 SHA-256이 동일하다.

fio 종료 후 세션 재접속 전까지 Guest가 idle 상태로 남아 최종 stats 수집이 지연되었다. 실험 장치는 unmounted raw device였고, `fio issued writes × 4 = 34,065,304 × 4 = 136,261,216`이 최종 `host_page_writes`와 정확히 같다. 따라서 fio 종료 후 final stats 수집 사이에 target write가 추가되지 않았음을 확인했다.
