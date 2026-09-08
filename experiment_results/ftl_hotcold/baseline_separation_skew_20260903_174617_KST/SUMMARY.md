# Baseline versus V4 Global: skew-dependent separation effect

Baseline is `hotcold/base` at `c0237bb12f183d87ea1abf2bf52e92f900f6c587`; it has no Hot/Cold classifier and uses one write pointer. V4 Global keeps the same Global Greedy victim policy but adds classifier-driven separated write pointers. D1/F1 are reused existing V4 Global results; H1/H2 are the new Baseline runs. Each row is one seed/run (`20260824`), so the figures are observations rather than statistical estimates.

| Distribution | Implementation | WAF | Victim invalid | Reclaim/GC | GC/1M Host pages | Avg GC copy |
|---|---|---:|---:|---:|---:|---:|
| Uniform | Baseline H1 | 7.987633 | 12.503% | 2,048.525 | 487.436 | 14,335.475 |
| Uniform | V4 Global D1 | 7.967380 | 12.534% | 2,053.554 | 486.194 | 14,330.446 |
| Zipf 1.2 | Baseline H2 | 7.648358 | 13.051% | 2,138.312 | 466.693 | 14,245.688 |
| Zipf 1.2 | V4 Global F1 | 5.400359 | 18.495% | 3,030.300 | 329.524 | 13,353.700 |

## Comparison

| Distribution | V4 Global WAF change | Invalid-ratio change | Reclaim/GC change | GC/1M change | Avg-copy change |
|---|---:|---:|---:|---:|---:|
| Uniform | -0.254% | +0.031%p | +0.245% | -0.255% | -0.035% |
| Zipf 1.2 | -29.392% | +5.444%p | +41.715% | -29.392% | -6.261% |

Uniform에서 Baseline과 V4 Global의 media metric 차이는 약 0.3% 이내다. 반면 Zipf 1.2에서 V4 Global은 Baseline보다 invalid ratio가 5.444%p 높고, reclaim/GC가 41.715% 높으며, GC/1M과 WAF가 각각 약 29.39% 낮다. 따라서 이 조건에서는 Hot/Cold classifier + separated write pointer의 관찰된 이득이 Uniform보다 strong skew에서 훨씬 크다는 counter chain과 일치한다.

이는 Global Greedy victim policy를 양쪽에서 유지한 비교다. 다만 branch 간 차이는 classifier와 separated pointer 및 이를 지지하는 line-pool/measurement instrumentation을 포함하므로 classifier만의 고립된 효과로 해석하지 않는다. 두 새 run은 fio `error=0`, `counter_invariant=PASS`, `NAND=Host+GC`, 그리고 fio bytes=`Host pages × 4096`을 만족한다.

Raw artifacts are under `runs/H1_baseline_uniform_1800/attempt_01/` and `runs/H2_baseline_zipf120_1800/attempt_01/`; exact fio files and comparison mapping are in `fio_settings/` and `campaign_manifest.csv`.
