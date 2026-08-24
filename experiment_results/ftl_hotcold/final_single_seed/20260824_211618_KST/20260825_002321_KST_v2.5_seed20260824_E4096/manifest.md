# FEMU final 3600-second performance run

- Run directory: `20260825_002321_KST_v2.5_seed20260824_E4096`
- Phase: `final`
- Version / branch: `v2.5` / `hotcold/v2.5`
- Source commit: `c056f18b523ab7220d8ce1b297fe2ba476355c03`
- Seed: `20260824`
- E: `4096`
- Fresh FEMU process and in-memory device state
- Exposed device: 6 GiB; raw NAND: 8 GiB
- Full 6 GiB sequential preconditioning
- Preconditioning physical FTL state and classifier history retained
- Accounting counters reset once before measurement with cdw10=5
- No marker, no offset, no intermediate stats reset
- Workload: randwrite, 16 KiB, QD128, 32 jobs, Zipf 0.99, size 5G, runtime 3600, randrepeat=1, group_reporting=0
- Final stats collected once after fio completed with cdw10=5
- Valid: `YES`
