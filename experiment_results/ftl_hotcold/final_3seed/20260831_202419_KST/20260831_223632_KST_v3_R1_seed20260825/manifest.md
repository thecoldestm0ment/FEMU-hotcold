# FEMU final three-seed extension run

- version: V3
- branch: hotcold/v3
- commit: fa7b9bb03f28703c1af37d559ce521b2f10554d3
- seed: 20260825
- classifier config: T=1024 B=4096 R=1
- fresh FEMU process/device: yes
- exposed capacity: 6 GiB
- raw NAND geometry: 8 GiB
- preconditioning: full-device 6 GiB sequential write
- pre-measurement command: cdw10=5 (accounting reset; classifier metadata follows existing implementation)
- measurement runtime: 3600 seconds
- workload: 16 KiB Zipf 0.99 random write, 32 jobs, iodepth 128, size 5 GiB/job address range
- offset: none
- marker: none
- intermediate snapshots: none
- fio randrepeat: 1
- fio randseed: 20260825
