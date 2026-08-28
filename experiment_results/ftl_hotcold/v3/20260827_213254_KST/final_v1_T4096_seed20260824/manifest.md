# V1 T=4096 최종 control run

- 판정: **VALID**
- Branch / commit: `hotcold/v1` / `8bf8e4b01035e6bd15e4c3bc4c2ff6e1c09cc285`
- Parameter: `T=4096`
- Seed: `20260824`
- 목적: V3의 경계 영역을 전부 HOT으로 처리하는 control
- Fresh FEMU process, exposed 6 GiB / raw NAND 8 GiB
- 전체 6 GiB sequential preconditioning 후 accounting counter만 reset
- Preconditioning의 physical FTL state와 classifier history는 유지
- Performance 구간에는 marker와 offset이 없고 중간 stats/reset도 없다.
- Workload: 16 KiB Zipf 0.99 randwrite, QD128, 32 jobs, size 5G, runtime 3600, randrepeat=1
- fio: rc=0, runtime=3600.418 s, aggregate integer IOPS=9464, BW=148 MiB/s
- Final: WAF=6.973143, GC writes=815130507, average GC copy=14036.066174, GC count=58074
- 모든 counter invariant PASS, 정상 poweroff PASS
- 보호 소스의 실행 전/후 SHA-256이 동일하다.
