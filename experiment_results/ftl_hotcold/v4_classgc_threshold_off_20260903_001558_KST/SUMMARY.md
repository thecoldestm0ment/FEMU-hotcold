# V4 ClassGC Cold-threshold-off ablation

결과 root: `experiment_results/ftl_hotcold/v4_classgc_threshold_off_20260903_001558_KST`  
Variant branch/commit: `hotcold/v4-classgc-threshold-off` / `fd5967d4bfa8da8f5e374028e6acbce1c6537de5`

Cold victim eligibility만 변경했다. normal Cold는 invalid `>= 12.5%`, forced Cold는 `ipc > 0`을 허용한다. Cold score는 기존대로 `age * ipc`이고 Hot policy, pressure-aware selection, borrowing, classifier와 fallback은 Full ClassGC와 동일하다.

| Workload | Global WAF | Full ClassGC WAF | Threshold-off WAF | Threshold-off Cold invalid | Threshold-off reclaim/GC | Threshold-off GC/1M |
|---|---:|---:|---:|---:|---:|---:|
| Uniform | 7.967380 | 3.322130 | 7.967045 | 12.504% | 2,053.382 | 486.165 |
| Zipf 1.2 | 5.400359 | 2.512177 | 5.345066 | 12.535% | 3,061.966 | 326.156 |

두 workload에서 relaxed eligibility는 Full ClassGC의 약 30% Cold victim invalid ratio 대신 12.5% floor 부근 victim을 선택했다. 그 결과 reclaim/GC와 GC/1M, WAF는 Global에 가까워졌다. Full ClassGC의 Global 대비 관찰 WAF 감소량 중 threshold-off가 되돌린 비율은 Uniform 99.99%, Zipf 1.2 98.09%다. 이는 단일 seed/run의 counter-based 관찰이며 통계적 우월성이나 실제 SSD 수명 향상을 뜻하지 않는다.

두 selected run 모두 fio `error=0`, `counter_invariant=PASS`, `pool_ownership_invariant=PASS`, `NAND=Host+GC`를 만족한다. exact fio 설정과 raw fio JSON/FEMU stats는 각 selected attempt 경로에 보존했다. Zipf 1.2 attempt_01/02는 fio 시작 전 report-file diff check 실패 로그이며, fresh FEMU에서 완료된 attempt_03을 selected로 사용했다.

전체 14-run 비교와 상세 해석은 `../v4_validation_matrix_20260902_144410_KST/SUMMARY.md` 및 `summary.csv`에 반영했다.
