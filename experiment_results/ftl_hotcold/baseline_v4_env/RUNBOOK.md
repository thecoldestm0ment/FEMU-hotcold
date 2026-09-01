# V4 동일 환경 baseline 실행

이 스크립트는 Hot/Cold 기능이 없는 `hotcold/base`를 V4 최종 실험과
동일한 장치 구성과 fio workload로 실행한다.

```bash
cd /root/workspace/FEMU
FEMU_GUEST_PASSWORD=femu \
  experiment_results/ftl_hotcold/baseline_v4_env/run_baseline_v4_env.sh
```

실행 조건은 raw NAND 8 GiB, exposed 6 GiB, OP 25%, GC 75/95,
6 GiB sequential preconditioning, measurement reset, seed `20260824`,
5 GiB Zipf 0.99 randwrite, 16 KiB, iodepth 128, numjobs 32, 3600초다.

결과는 다음 형식의 새 디렉터리에 보존된다.

```text
experiment_results/ftl_hotcold/baseline_v4_env/
└── YYYYMMDD_HHMMSS_KST_baseline_seed20260824/
    ├── manifest.txt
    ├── preflight.txt
    ├── precondition_fio.txt
    ├── precondition_reset.txt
    ├── fio_raw.txt
    ├── final_stats_command.txt
    ├── host_femu.log
    ├── stats.txt
    ├── results_summary.txt
    └── status.txt
```

정상 완료 기준은 `status.txt=PASS`, `counter_invariant=PASS`, 그리고
`nand_page_writes = host_page_writes + gc_page_writes`이다.
