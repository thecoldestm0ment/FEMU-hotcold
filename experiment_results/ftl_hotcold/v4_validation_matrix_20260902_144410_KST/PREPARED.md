# V4 Global / ClassGC validation matrix

Preparation completed on 2026-09-02 KST. No experiment run has been started in
this result root.

## Decisions fixed for execution

- Geometry: raw 8 GiB, exposed 6144 MiB, OP 25%, 4 KiB FEMU page.
- GC thresholds: 75% / 95%.
- Every attempt starts a fresh FEMU process, performs a 6 GiB sequential fill,
  then issues `opcode=0xef, cdw10=5` to reset measurement counters and
  classifier metadata while preserving physical state.
- Measurement: `randwrite`, 16 KiB, 5 GiB working set, libaio, iodepth 128,
  numjobs 32, randrepeat 1, seed 20260824.
- Fixed-write B uses `io_size=16G` per job. With 32 jobs this is exactly 512 GiB
  total, or 134,217,728 FEMU 4 KiB Host pages. This is close to the prior
  3600-second Global run's 133,084,356 Host pages.
- C and G both use 1800 seconds. The C job file keeps its original filename for
  compatibility with the already-running campaign process; its actual
  `runtime` setting is 1800.
- Retry limit is three attempts per run. Failed attempt directories are retained.
- Liveness polling records only QEMU survival, fio survival, and increasing fio
  elapsed time. It does not parse intermediate performance or FTL statistics.

## Exact source versions

- V4 Global: `hotcold/v4` at `032d29b83e3906593ef9f79e5d2739bdab27ef5e`
- V4 ClassGC: `hotcold/v4-classgc` at `d067a64de2974949807a6a6652cf2a08a76d1c4c`
- V4 ClassGC without age: `hotcold/v4-classgc-noage` at
  `ea289e975127f2e63e510c0ce4ed22081ffec293`

The without-age commit changes only Cold victim scoring from `age * ipc` to
`ipc`; thresholds, Hot selection, pressure selection, borrowing, and fallback
paths are inherited unchanged from ClassGC.

## Execution

Run as root from anywhere:

```bash
/root/workspace/FEMU/experiment_results/ftl_hotcold/v4_validation_matrix_20260902_144410_KST/run_campaign.sh
```

The runner validates KVM, the source commits, image exclusivity, guest SSH,
`/dev/nvme0n1` identity/size/unmounted state, and fio version before destructive
I/O. On complete success it generates `summary.csv` and `SUMMARY.md`.

Expected run order is recorded in `campaign_manifest.csv`. Exact fio settings
are also materialized in `fio_settings/` before execution and copied into every
attempt directory as `fio_command.txt`.

## Reference result

The 3600-second comparison source is:

`experiment_results/ftl_hotcold/v4_global_vs_classgc_20260901_181151_KST/`

It is used only by the final summarizer; it is not modified.
