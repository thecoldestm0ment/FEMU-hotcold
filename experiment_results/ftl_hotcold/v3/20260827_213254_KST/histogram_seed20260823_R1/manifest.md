# V3 erase-survival histogram run

- 판정: **VALID**
- 목적: 큰 R sweep 전에 경계 rewrite의 survival 분포를 보고 R을 정한다.
- Branch / commit: `hotcold/v3` / `cbb044e32ea4714037e956aaba653b301a25779d`
- Seed: `20260823` — 최종 성능 비교에는 포함하지 않음
- 설정: `T_fast=1024`, `T_slow=4096`, `R=1`
- Fresh FEMU, 6 GiB preconditioning, accounting reset, 300초 workload
- Workload shape는 최종 run과 같고 runtime만 300초다. Offset과 marker는 사용하지 않았다.
- 경계 event 887,012건 중 survival=0이 98.4420%, survival=1이 1.5580%, survival>=2는 0건이었다.
- 따라서 추가 performance sweep 없이 `R=1`을 최종값으로 선택했다.
- fio rc=0, `counter_invariant=PASS`, 정상 poweroff PASS
