# Baseline versus V4 Global skew experiment

Prepared only; no FEMU or fio workload was started during preparation.

- Baseline source: `hotcold/base`
- Commit: `c0237bb12f183d87ea1abf2bf52e92f900f6c587`
- Isolated source/build tree: `/tmp/femu_baseline_v4_env`
- Run order: Uniform 1800s, then Zipf 1.2 1800s
- Existing comparisons reused: D1 V4 Global Uniform and F1 V4 Global Zipf 1.2
- Fresh FEMU, 6 GiB sequential preconditioning, measurement reset for every attempt
- Raw 8 GiB, exposed 6 GiB, OP 25%, GC 75/95%
- `randwrite`, `bs=16k`, `size=5G`, `iodepth=128`, `numjobs=32`, `direct=1`, `libaio`, seed `20260824`

Run both sequentially:

```bash
cd /root/workspace/FEMU
FEMU_GUEST_PASSWORD='<guest-password>' \
  experiment_results/ftl_hotcold/baseline_separation_skew_20260903_174617_KST/run_campaign.sh
```

Optional selection for recovery/testing: `RUN_ONLY=uniform` or `RUN_ONLY=zipf120`.
Failed attempts are retained and a run is retried automatically up to three attempts per invocation.
