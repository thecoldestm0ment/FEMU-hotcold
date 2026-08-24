# FEMU final 3600-second performance run

- Run directory: `20260824_211940_KST_baseline_seed20260824`
- Phase: `final`
- Version / branch: `baseline` / `hotcold/base`
- Source commit: `c0237bb12f183d87ea1abf2bf52e92f900f6c587`
- Seed: `20260824`
- E: `none`
- Fresh FEMU process and in-memory device state
- Exposed device: 6 GiB; raw NAND: 8 GiB
- Full 6 GiB sequential preconditioning
- Preconditioning physical FTL state and classifier history retained
- Accounting counters reset once before measurement with cdw10=5
- No marker, no offset, no intermediate stats reset
- Workload: randwrite, 16 KiB, QD128, 32 jobs, Zipf 0.99, size 5G, runtime 3600, randrepeat=1, group_reporting=0
- Final stats collected once after fio completed with cdw10=5
- Valid: `YES`
