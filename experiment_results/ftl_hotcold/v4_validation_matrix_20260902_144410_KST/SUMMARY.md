# FEMU V4 Global / ClassGC 실험 최종 분석

작성일: 2026-09-02 KST  
결과 root: `experiment_results/ftl_hotcold/v4_validation_matrix_20260902_144410_KST`

## 1. 결론

이번 단일-seed 실험에서 확인된 핵심 결과는 다음과 같다.

1. **Zipf 0.99의 1800초 normalized media metric 경향은 기존 3600초 결과와 유지됐다.** 1800초 WAF는 Global 6.961641, ClassGC 3.040172였고, 기존 3600초도 각각 7.147977, 3.060527이었다. GC/1M Host pages와 reclaim/GC도 같은 순위와 비슷한 절대 수준을 보였다.
2. **Host write amount를 정확히 맞춘 fixed-write가 가장 명확한 Global/ClassGC 비교다.** 양쪽 모두 134,217,728 Host pages를 쓴 조건에서 ClassGC는 Global보다 WAF가 57.12%, GC/1M이 57.15%, NAND writes가 57.12% 낮았다. IOPS는 140.63% 높고 평균 latency는 58.45% 낮았다.
3. **Cold victim score에서 age를 제거한 효과는 media metric 기준으로 작았다.** Zipf 1.2에서는 Full 대비 no-age의 Host writes 차이가 0.11%에 불과했고 WAF는 0.17% 증가했다. 반면 emergency GC는 35회에서 94회로 증가해, age가 평균 WAF보다는 드문 압박 상황의 victim 선택에 영향을 줄 가능성이 관찰됐다.
4. **skew가 커질수록 두 구현 모두 GC 효율이 좋아졌지만 ClassGC가 전 구간에서 더 낮은 WAF를 유지했다.** Uniform/Zipf 0.7은 거의 같았고, Zipf 0.99부터 개선이 나타나 Zipf 1.2에서 가장 컸다.
5. **관찰된 결과는 `victim invalid ratio 증가 → reclaim/GC 증가 → GC/1M 감소 → WAF 감소` 흐름과 일치한다.** 특히 fixed-write에서 Global의 `14.009% → 2,295.171 → 435.360 → 7.133709`가 ClassGC의 `32.640% → 5,347.783 → 186.555 → 3.058862`로 바뀌었다.
6. **Cold eligibility를 30%에서 12.5%로 완화하면 Full ClassGC의 WAF 개선은 Uniform과 Zipf 1.2 모두에서 거의 사라졌다.** 완화 variant의 Cold victim invalid ratio는 각각 12.504%와 12.535%로 floor 부근에 머물렀고, WAF는 Global과 각각 0.000335, 0.055293 차이였다.

단, 각 조합은 seed `20260824`의 1회 측정이다. 아래 차이는 이 실험에서의 관찰값이며 통계적 우월성, pool 비율의 최적성, 실제 SSD 수명 향상을 입증하지 않는다.

## 2. 실험 범위와 데이터 무결성

- FEMU Blackbox, Raw 8 GiB, Exposed 6 GiB, OP 25%
- 매 attempt마다 fresh FEMU 실행, 6 GiB sequential preconditioning 후 measurement reset
- `randwrite`, `bs=16k`, working set 5 GiB, `iodepth=128`, `numjobs=32`
- `randseed=20260824`, GC threshold 75/95%
- time-based run은 모두 1800초이며, B만 job당 `io_size=16G`인 fixed-write
- C의 디렉터리/파일명에는 요청에 따라 `3600`을 남겼지만 manifest와 실제 fio 설정은 `runtime=1800`
- 14개 selected run 모두 fio `error=0`, `counter_invariant=PASS`
- 모든 run에서 `NAND page writes = Host page writes + GC page writes`
- fio의 총 write bytes도 모든 run에서 `Host page writes × 4096`과 일치
- 모든 ClassGC/no-age run에서 `pool_ownership_invariant=PASS`

소스 버전은 다음과 같다.

| 구현 | Branch | Commit |
|---|---|---|
| V4 Global | `hotcold/v4` | `032d29b83e3906593ef9f79e5d2739bdab27ef5e` |
| V4 ClassGC | `hotcold/v4-classgc` | `d067a64de2974949807a6a6652cf2a08a76d1c4c` |
| V4 ClassGC without age | `hotcold/v4-classgc-noage` | `ea289e975127f2e63e510c0ce4ed22081ffec293` |
| V4 ClassGC threshold-off | `hotcold/v4-classgc-threshold-off` | `fd5967d4bfa8da8f5e374028e6acbce1c6537de5` |

ClassGC와 no-age commit 사이의 `hw/femu/bbssd/ftl.c` 차이는 Cold victim score를 `age × ipc`에서 `ipc`로 바꾸는 것뿐이다. Cold threshold, Hot victim policy, pressure-aware class selection, borrowing 및 fallback 경로는 같다.

## 3. 전체 결과표

### 3.1 Write 및 GC counter

page는 FEMU 4 KiB page 기준이다.

| Run | Workload | 구현 | Host pages | GC pages | NAND pages | GC count | WAF | Invariant |
|---|---|---|---:|---:|---:|---:|---:|---|
| A1 | Zipf 0.99, 1800s | Global | 56,301,836 | 335,651,350 | 391,953,186 | 23,916 | 6.961641 | PASS |
| A2 | Zipf 0.99, 1800s | ClassGC | 109,656,200 | 223,717,548 | 333,373,748 | 20,328 | 3.040172 | PASS |
| B1 | Zipf 0.99, fixed 512 GiB | Global | 134,217,728 | 823,252,518 | 957,470,246 | 58,433 | 7.133709 | PASS |
| B2 | Zipf 0.99, fixed 512 GiB | ClassGC | 134,217,728 | 276,335,831 | 410,553,559 | 25,039 | 3.058862 | PASS |
| C1 | Zipf 0.99, no-age, 1800s | ClassGC no-age | 159,814,972 | 329,434,190 | 489,249,162 | 29,842 | 3.061347 | PASS |
| D1 | Uniform, 1800s | Global | 59,441,268 | 414,149,890 | 473,591,158 | 28,900 | 7.967380 | PASS |
| D2 | Uniform, 1800s | ClassGC | 141,644,864 | 328,917,854 | 470,562,718 | 28,705 | 3.322130 | PASS |
| E1 | Zipf 0.7, 1800s | Global | 59,781,972 | 414,655,135 | 474,437,107 | 28,951 | 7.936123 | PASS |
| E2 | Zipf 0.7, 1800s | ClassGC | 147,148,488 | 340,352,166 | 487,500,654 | 29,739 | 3.312984 | PASS |
| F1 | Zipf 1.2, 1800s | Global | 88,600,636 | 389,874,613 | 478,475,249 | 29,196 | 5.400359 | PASS |
| F2 | Zipf 1.2, 1800s | ClassGC | 195,989,740 | 296,371,108 | 492,360,848 | 30,030 | 2.512177 | PASS |
| G1 | Zipf 1.2, no-age, 1800s | ClassGC no-age | 196,205,604 | 297,537,107 | 493,742,711 | 30,113 | 2.516456 | PASS |
| T1 | Uniform, threshold-off, 1800s | ClassGC threshold-off | 57,276,852 | 399,050,391 | 456,327,243 | 27,846 | 7.967045 | PASS |
| T2 | Zipf 1.2, threshold-off, 1800s | ClassGC threshold-off | 88,810,168 | 385,886,031 | 474,696,199 | 28,966 | 5.345066 | PASS |

### 3.2 GC 효율

전체 victim invalid ratio는 구현 간 같은 정의를 쓰기 위해 `1 - Avg GC copy / 16384`로 계산했다. Reclaim/GC는 `16384 - Avg GC copy`다.

| Run | 구현 | Avg GC copy | Victim invalid | Reclaim/GC | GC/1M Host pages |
|---|---|---:|---:|---:|---:|
| A1 | Global | 14,034.594 | 14.340% | 2,349.406 | 424.782 |
| A2 | ClassGC | 11,005.389 | 32.828% | 5,378.611 | 185.379 |
| B1 | Global | 14,088.829 | 14.009% | 2,295.171 | 435.360 |
| B2 | ClassGC | 11,036.217 | 32.640% | 5,347.783 | 186.555 |
| C1 | ClassGC no-age | 11,039.280 | 32.622% | 5,344.720 | 186.728 |
| D1 | Global | 14,330.446 | 12.534% | 2,053.554 | 486.194 |
| D2 | ClassGC | 11,458.556 | 30.063% | 4,925.444 | 202.655 |
| E1 | Global | 14,322.653 | 12.581% | 2,061.347 | 484.276 |
| E2 | ClassGC | 11,444.641 | 30.147% | 4,939.359 | 202.102 |
| F1 | Global | 13,353.700 | 18.495% | 3,030.300 | 329.524 |
| F2 | ClassGC | 9,869.168 | 39.763% | 6,514.832 | 153.222 |
| G1 | ClassGC no-age | 9,880.686 | 39.693% | 6,503.314 | 153.477 |
| T1 | ClassGC threshold-off | 14,330.618 | 12.533% | 2,053.382 | 486.165 |
| T2 | ClassGC threshold-off | 13,322.034 | 18.689% | 3,061.966 | 326.156 |

### 3.3 fio 성능

| Run | 구현 | IOPS | Avg latency (ms) | P99 (ms) | P99.9 (ms) | P99.99 (ms) |
|---|---|---:|---:|---:|---:|---:|
| A1 | Global | 7,815.6 | 524.040 | 2,332.033 | 5,804.917 | 12,280.922 |
| A2 | ClassGC | 15,228.0 | 268.929 | 2,332.033 | 8,153.727 | 17,112.760 |
| B1 | Global | 9,246.7 | 442.838 | 834.666 | 1,166.017 | 2,021.655 |
| B2 | ClassGC | 22,250.6 | 183.985 | 400.556 | 530.579 | 1,266.680 |
| C1 | ClassGC no-age | 22,192.3 | 184.560 | 400.556 | 675.283 | 1,249.903 |
| D1 | Global | 8,253.1 | 496.276 | 918.553 | 1,736.442 | 3,003.122 |
| D2 | ClassGC | 19,670.4 | 208.220 | 530.579 | 1,468.006 | 2,264.924 |
| E1 | Global | 8,300.1 | 493.453 | 1,266.680 | 1,686.110 | 3,103.785 |
| E2 | ClassGC | 20,434.6 | 200.437 | 425.722 | 675.283 | 1,484.784 |
| F1 | Global | 12,303.3 | 332.896 | 658.506 | 876.610 | 2,164.261 |
| F2 | ClassGC | 27,217.2 | 150.482 | 341.836 | 599.785 | 1,333.789 |
| G1 | ClassGC no-age | 27,248.6 | 150.306 | 333.447 | 438.305 | 1,233.125 |
| T1 | ClassGC threshold-off | 7,952.6 | 514.975 | 1,216.348 | 2,499.805 | 3,841.982 |
| T2 | ClassGC threshold-off | 12,331.8 | 332.079 | 666.894 | 1,010.827 | 1,669.333 |

A1/A2는 fixed-write와 기존 3600초 결과보다 양쪽 구현 모두 IOPS가 낮고 tail latency가 두드러지게 크게 관찰됐다. 따라서 A로는 normalized media metric의 방향을 검증하되, A의 절대 성능값이나 A2/C1의 성능 차이를 정책 효과로 해석하지 않는다.

## 4. 1800초와 기존 3600초 비교

비교 대상은 같은 Zipf 0.99, seed, geometry, workload의 기존 3600초 결과다.

| 구현 | Runtime | WAF | GC/1M | Avg copy | Reclaim/GC | Invalid ratio | IOPS |
|---|---:|---:|---:|---:|---:|---:|---:|
| Global | 1800s | 6.961641 | 424.782 | 14,034.594 | 2,349.406 | 14.340% | 7,815.6 |
| Global | 3600s | 7.147977 | 436.227 | 14,093.524 | 2,290.476 | 13.980% | 약 9,240 |
| ClassGC | 1800s | 3.040172 | 185.379 | 11,005.389 | 5,378.611 | 32.828% | 15,228.0 |
| ClassGC | 3600s | 3.060527 | 186.742 | 11,034.089 | 5,349.911 | 32.653% | 약 22,200 |

- Global의 1800초 값은 3600초 대비 WAF -2.61%, GC/1M -2.62%, reclaim/GC +2.57%, invalid ratio +0.360%p다.
- ClassGC는 WAF -0.67%, GC/1M -0.73%, reclaim/GC +0.54%, invalid ratio +0.175%p다.
- 두 runtime 모두 ClassGC가 Global보다 victim invalid ratio와 reclaim/GC가 높고 GC/1M과 WAF가 낮다. 즉 **요청한 normalized metric 경향은 유지됐으며, ClassGC의 절대 normalized 값은 특히 가깝다.**
- 그러나 IOPS는 Global -15.42%, ClassGC -31.41%이고 A의 tail latency가 매우 크다. 따라서 이 validation은 **media efficiency 경향의 재현**으로 한정하며 성능 재현으로 보지 않는다.

기존 3600초 ClassGC commit과 이번 ClassGC commit의 FTL 기능 차이는 없고 주석만 달라, 이 비교의 정책 의미는 동일하다.

## 5. Fixed-write: Global 대 ClassGC

B는 양쪽 모두 정확히 134,217,728 Host pages, 즉 총 512 GiB를 기록했다. time-based 비교에서 발생하는 Host write량 차이를 제거했기 때문에 이번 campaign의 가장 강한 직접 비교다.

| Metric | Global | ClassGC | ClassGC 상대 변화 |
|---|---:|---:|---:|
| Host page writes | 134,217,728 | 134,217,728 | 0.00% |
| GC page writes | 823,252,518 | 276,335,831 | -66.43% |
| NAND page writes | 957,470,246 | 410,553,559 | -57.12% |
| WAF | 7.133709 | 3.058862 | -57.12% |
| GC/1M Host pages | 435.360 | 186.555 | -57.15% |
| Avg GC copy | 14,088.829 | 11,036.217 | -21.67% |
| Reclaim pages/GC | 2,295.171 | 5,347.783 | +133.00% |
| Victim invalid ratio | 14.009% | 32.640% | +18.631%p |
| IOPS | 9,246.7 | 22,250.6 | +140.63% |
| Avg latency | 442.838 ms | 183.985 ms | -58.45% |
| P99 | 834.666 ms | 400.556 ms | -52.01% |
| P99.9 | 1,166.017 ms | 530.579 ms | -54.50% |
| P99.99 | 2,021.655 ms | 1,266.680 ms | -37.34% |

ClassGC는 한 번의 GC에서 평균 2.33배 많은 page를 reclaim했고, Host page 백만 개당 GC 횟수는 약 43% 수준으로 줄었다. 그 결과 동일 Host write량에서 GC copy는 약 5.47억 page, 총 NAND write는 약 5.47억 page 적었다. 이 데이터는 ClassGC의 낮은 WAF가 단순히 더 적은 Host I/O를 처리했기 때문이 아님을 확인한다.

## 6. Cold victim age 제거 효과

### Zipf 0.99

| Metric | Full ClassGC A2 | No-age C1 | No-age 상대 변화 |
|---|---:|---:|---:|
| Host pages | 109,656,200 | 159,814,972 | +45.74% |
| WAF | 3.040172 | 3.061347 | +0.70% |
| GC/1M | 185.379 | 186.728 | +0.73% |
| Reclaim/GC | 5,378.611 | 5,344.720 | -0.63% |
| Victim invalid ratio | 32.828% | 32.622% | -0.207%p |
| Emergency GC | 18 | 35 | +17회 |

normalized media metric은 no-age에서 소폭 나빠졌지만, A2와 C1의 Host writes가 45.74% 다르고 A2에 큰 성능 이상치가 있으므로 처리량·latency 차이는 age 제거 효과로 해석할 수 없다.

### Zipf 1.2

| Metric | Full ClassGC F2 | No-age G1 | No-age 상대 변화 |
|---|---:|---:|---:|
| Host pages | 195,989,740 | 196,205,604 | +0.11% |
| WAF | 2.512177 | 2.516456 | +0.17% |
| GC/1M | 153.222 | 153.477 | +0.17% |
| Reclaim/GC | 6,514.832 | 6,503.314 | -0.18% |
| Victim invalid ratio | 39.763% | 39.693% | -0.070%p |
| IOPS | 27,217.2 | 27,248.6 | +0.12% |
| Avg latency | 150.482 ms | 150.306 ms | -0.12% |
| Emergency GC | 35 | 94 | +59회 |

F2/G1은 Host writes와 IOPS가 거의 같으므로 더 깨끗한 ablation이다. WAF, GC/1M, reclaim/GC, 평균 latency 차이는 모두 0.2% 안팎으로, 이번 run에서 `age × ipc`가 `ipc`보다 평균 효율을 크게 개선했다는 근거는 없다. 반면 emergency GC rate는 Host 백만 page당 약 0.179회에서 0.479회로 늘었다. 전체 GC 중 비율은 각각 약 0.117%, 0.312%로 작지만, **age 항이 고-skew의 드문 긴급 상황을 줄이는 데 기여했을 가능성**은 후속 반복실험 대상으로 남는다.

G1의 P99/P99.9/P99.99가 F2보다 낮았지만 단일 run의 tail 차이이므로 no-age의 latency 개선으로 결론 내리지 않는다.

## 7. Cold eligibility threshold ablation

별도 `hotcold/v4-classgc-threshold-off` branch에서 Cold normal eligibility만 `invalid ≥ 30%`에서 `invalid ≥ 12.5%`로 완화했고, Cold forced는 기존 `invalid ≥ 25%` 대신 `ipc > 0`인 Cold line을 허용했다. Cold score는 그대로 `age × ipc`이며 Hot policy, pressure-aware selection, borrowing, classifier 및 fallback은 Full ClassGC와 같다. 따라서 아래는 **Cold threshold만의 code ablation**이다.

| Workload / Metric | Global | Full ClassGC | Threshold-off | Off 대 Full |
|---|---:|---:|---:|---:|
| Uniform WAF | 7.967380 | 3.322130 | 7.967045 | +4.644915 (+139.82%) |
| Uniform Cold invalid | — | 30.010% | 12.504% | -17.507%p |
| Uniform reclaim/GC | 2,053.554 | 4,925.444 | 2,053.382 | -58.31% |
| Uniform GC/1M | 486.194 | 202.655 | 486.165 | +139.90% |
| Zipf 1.2 WAF | 5.400359 | 2.512177 | 5.345066 | +2.832889 (+112.77%) |
| Zipf 1.2 Cold invalid | — | 30.010% | 12.535% | -17.475%p |
| Zipf 1.2 reclaim/GC | 3,030.300 | 6,514.832 | 3,061.966 | -52.999% |
| Zipf 1.2 GC/1M | 329.524 | 153.222 | 326.156 | +112.87% |

- Uniform에서 Full ClassGC의 Global 대비 WAF 감소량은 `4.645250`인데 threshold-off의 Full 대비 WAF 증가는 `4.644915`였다. 이 단일 run에서는 관찰된 WAF 감소량의 **99.99%**가 사라졌고, threshold-off는 Global보다 WAF가 0.000335 낮을 뿐이다.
- Zipf 1.2에서도 Full ClassGC의 Global 대비 WAF 감소량 `2.888182` 중 threshold-off가 Full 대비 되돌린 값은 `2.832889`(관찰된 감소량의 **98.09%**)였다. threshold-off WAF는 Global보다 1.02% 낮지만 Full ClassGC와는 크게 다르다.
- 두 workload에서 threshold-off의 Cold victim invalid ratio가 12.5% floor 부근이고, Full ClassGC의 약 30.01%와 달랐다. 그에 따라 reclaim/GC는 Global 수준으로 낮고 GC/1M과 WAF도 Global 수준으로 돌아왔다. 이는 이 구현·조건에서 **30% Cold eligibility가 관찰된 WAF 개선에 크게 기여했다는 counter chain**과 일치한다.
- T1/T2는 각각 Full ClassGC보다 IOPS가 낮고 평균 latency가 높았지만, time-based run의 Host write량 차이와 단일 seed 때문에 성능 차이를 threshold의 일반적 효과로 확정하지 않는다.

ClassGC counter도 threshold relaxation의 선택 동작을 뒷받침한다.

| Run | Hot victims | Cold victims | Hot invalid | Cold invalid | Borrow | Final Hot ownership | Opposite normal / forced / fallback |
|---|---:|---:|---:|---:|---:|---:|---:|
| T1 Uniform threshold-off | 13 | 27,833 | 75.265% | 12.504% | 4 | 21/128 (16.41%) | 13 / 0 / 0 |
| T2 Zipf 1.2 threshold-off | 2,570 | 26,396 | 81.898% | 12.535% | 4 | 21/128 (16.41%) | 2,570 / 0 / 0 |

두 threshold-off run 모두 `counter_invariant=PASS`, `pool_ownership_invariant=PASS`, `NAND=Host+GC`였다. T2는 report-file diff check에 의해 fio 시작 전 attempt 01/02가 실패했으며, selected `attempt_03`만 fresh FEMU에서 정상 완료됐다.

## 8. Skew 변화

동일한 1800초 Full 정책만 비교했다. time-based이므로 구현 간 Host write량은 다르며, skew 효과는 각 구현 내부의 normalized metric 흐름으로 본다.

| 분포 | Global WAF | ClassGC WAF | Global GC/1M | ClassGC GC/1M | Global reclaim/GC | ClassGC reclaim/GC | Global/ClassGC hot write ratio |
|---|---:|---:|---:|---:|---:|---:|---:|
| Uniform | 7.967380 | 3.322130 | 486.194 | 202.655 | 2,053.554 | 4,925.444 | 0.301% / 0.299% |
| Zipf 0.7 | 7.936123 | 3.312984 | 484.276 | 202.102 | 2,061.347 | 4,939.359 | 0.758% / 0.766% |
| Zipf 0.99 | 6.961641 | 3.040172 | 424.782 | 185.379 | 2,349.406 | 5,378.611 | 15.445% / 14.355% |
| Zipf 1.2 | 5.400359 | 2.512177 | 329.524 | 153.222 | 3,030.300 | 6,514.832 | 37.892% / 38.690% |

- Uniform과 Zipf 0.7은 두 구현 모두 거의 같은 WAF, GC/1M, reclaim/GC를 보였다. 이 classifier 기준에서는 Zipf 0.7의 locality가 media metric을 의미 있게 바꿀 만큼 강하지 않았다.
- Zipf 0.99부터 hot write ratio와 reclaim/GC가 증가하고 GC/1M과 WAF가 감소했다. Zipf 1.2에서 이 변화가 가장 크다.
- ClassGC의 Hot victim 횟수도 Uniform 33, Zipf 0.7 91, Zipf 0.99 1,290, Zipf 1.2 5,660으로 증가했다. skew가 커질수록 높은 invalid ratio의 Hot victim을 활용하는 경로가 더 자주 실행됐다는 counter 근거다.
- ClassGC의 WAF는 Global 대비 Uniform -58.30%, Zipf 0.7 -58.25%, Zipf 0.99 -56.33%, Zipf 1.2 -53.48%였다. skew가 커질수록 Global도 개선돼 상대 격차는 줄지만, ClassGC의 WAF 우위 자체는 전 구간에서 유지됐다.
- A의 성능 이상치 때문에 IOPS의 skew 추세는 이 표에서 결론 내리지 않는다.

## 9. Victim invalid ratio에서 WAF까지

측정값 사이에는 다음 항등식이 성립한다.

1. `Reclaim pages/GC = 16384 - Avg GC copy`
2. `Avg victim invalid ratio = Reclaim pages/GC / 16384`
3. `GC page writes = GC count × Avg GC copy`
4. `WAF = 1 + GC page writes / Host page writes`

따라서 출력 반올림 오차를 제외하면 다음처럼 결합된다.

`WAF ≈ 1 + (GC/1M Host pages × Avg GC copy) / 1,000,000`

정책이 invalid page가 더 많은 line을 victim으로 선택하면 한 GC에서 더 많은 공간을 회수하고 copy해야 하는 valid page는 줄어든다. 같은 Host write를 감당하는 데 필요한 GC 횟수가 줄고, `GC count × Avg copy`가 감소하면 최종 WAF도 낮아진다.

| 대표 조건 | Invalid ratio | Reclaim/GC | GC/1M | Avg copy | WAF |
|---|---:|---:|---:|---:|---:|
| Fixed Global | 14.009% | 2,295.171 | 435.360 | 14,088.829 | 7.133709 |
| Fixed ClassGC | 32.640% | 5,347.783 | 186.555 | 11,036.217 | 3.058862 |
| Uniform Global | 12.534% | 2,053.554 | 486.194 | 14,330.446 | 7.967380 |
| Uniform ClassGC | 30.063% | 4,925.444 | 202.655 | 11,458.556 | 3.322130 |
| Zipf 1.2 Global | 18.495% | 3,030.300 | 329.524 | 13,353.700 | 5.400359 |
| Zipf 1.2 ClassGC | 39.763% | 6,514.832 | 153.222 | 9,869.168 | 2.512177 |
| Uniform threshold-off | 12.533% | 2,053.382 | 486.165 | 14,330.618 | 7.967045 |
| Zipf 1.2 threshold-off | 18.689% | 3,061.966 | 326.156 | 13,322.034 | 5.345066 |

첫 두 식은 정의상 항등식이고, WAF 식도 counter 정의에서 직접 성립한다. 다만 중간의 `victim 선택 → GC 빈도 변화`는 시스템 동작에 대한 해석이며, 단일 run만으로 독립적인 인과 효과를 확정하지 않는다.

## 10. ClassGC 상세 counter

| Run | Hot victims | Cold victims | Hot invalid | Cold invalid | Borrow | Final Hot ownership | Opposite normal | Opposite forced | Global fallback | Emergency GC |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A2, Zipf 0.99 | 1,290 | 19,038 | 74.445% | 30.008% | 18 | 7/128 (5.47%) | 1,290 | 0 | 0 | 18 |
| B2, Zipf 0.99 fixed | 1,410 | 23,629 | 76.743% | 30.009% | 18 | 7/128 (5.47%) | 1,410 | 0 | 0 | 19 |
| C1, Zipf 0.99 no-age | 1,661 | 28,181 | 76.958% | 30.008% | 18 | 7/128 (5.47%) | 1,661 | 0 | 0 | 35 |
| D2, Uniform | 33 | 28,672 | 75.432% | 30.010% | 18 | 7/128 (5.47%) | 33 | 0 | 0 | 13 |
| E2, Zipf 0.7 | 91 | 29,648 | 74.940% | 30.010% | 18 | 7/128 (5.47%) | 91 | 0 | 0 | 13 |
| F2, Zipf 1.2 | 5,660 | 24,370 | 81.760% | 30.010% | 18 | 7/128 (5.47%) | 5,660 | 0 | 0 | 35 |
| G1, Zipf 1.2 no-age | 5,576 | 24,537 | 82.320% | 30.006% | 18 | 7/128 (5.47%) | 5,576 | 0 | 0 | 94 |
| T1, Uniform threshold-off | 13 | 27,833 | 75.265% | 12.504% | 4 | 21/128 (16.41%) | 13 | 0 | 0 | 0 |
| T2, Zipf 1.2 threshold-off | 2,570 | 26,396 | 81.898% | 12.535% | 4 | 21/128 (16.41%) | 2,570 | 0 | 0 | 0 |

- Cold victim invalid ratio가 모든 workload에서 약 30.01%로 유지된 반면 Hot victim은 74.45~82.32%였다. 전체 victim efficiency 개선은 소수의 매우 높은-invalid Hot victim과 다수의 threshold 근처 Cold victim이 결합한 결과다.
- 모든 run에서 `opposite_normal == Hot victim count`, `opposite_forced=0`, `global_emergency_fallback=0`이다. 관찰된 Hot victim은 전부 normal opposite-class 경로로 선택됐고 global fallback은 사용되지 않았다.
- 초기 Hot ownership 25/128에서 18개를 Cold에 빌려 최종 7/128이 됐으며 모든 ClassGC/no-age run에서 같았다. 이는 borrowing이 실제 동작했다는 증거이지 20:80 초기 pool이나 7/128 최종 비율이 최적이라는 증거는 아니다.

## 11. 해석 한계

- 각 workload/정책은 단일 seed, 단일 run이다. 반복 측정의 분산, 신뢰구간, 유의성 검정이 없다.
- time-based run의 Host write량은 정책 성능에 따라 달라진다. Global/ClassGC의 가장 직접적인 write-cost 비교는 B fixed-write를 우선해야 한다.
- A의 IOPS와 tail latency는 다른 동일 계열 run보다 나빠 성능 재현성이 약하다. 원인은 이 데이터만으로 특정하지 않는다.
- lower WAF는 이 FEMU workload에서 NAND page write amplification이 낮았다는 뜻이다. 실제 장치의 수명 개선을 직접 입증하지 않는다.
- hot/cold ownership 및 fallback counter는 해당 실행에서의 정책 동작을 보여줄 뿐 pool ratio의 최적성을 입증하지 않는다.

## 12. 재현 자료

- 전체 machine-readable 결과: `summary.csv`
- 실행 순서·branch·commit·workload: `campaign_manifest.csv`
- exact fio job: `fio_settings/`
- 각 run의 selected attempt: `runs/<run_id>/selected_attempt.txt`
- raw FEMU counter: `runs/<run_id>/attempt_<NN>/stats.txt`
- raw fio JSON: `runs/<run_id>/attempt_<NN>/fio_raw.json`
- VM/fio 생존 확인: `runs/<run_id>/attempt_<NN>/liveness.log`
- A1은 재시도 후 `attempt_04`가 selected result이며 나머지는 `attempt_01`이다. A1의 실패한 `attempt_01~03`은 최종 분석 완료 후 사용자 요청에 따라 정리했다.
- threshold-off raw artifacts는 `../v4_classgc_threshold_off_20260903_001558_KST/`에 있으며, T1은 `attempt_01`, T2는 `attempt_03`이 selected다.

## 13. 예상 질문과 답변

**Q. 왜 ClassGC가 좋다고 볼 수 있는가?**  
A. 동일 Host write량의 B에서 더 높은 victim invalid ratio, 더 큰 reclaim/GC, 더 적은 GC/1M과 GC copy, 더 낮은 WAF가 하나의 일관된 counter chain으로 확인됐다. 단, 통계적 우월성은 반복실험이 필요하다.

**Q. 1800초로 줄여도 결론이 유지되는가?**  
A. WAF, GC/1M, reclaim/GC의 Global/ClassGC 순위와 절대 수준은 유지됐다. 다만 A의 IOPS/tail latency는 재현되지 않아 media metric 경향에 한정해 답해야 한다.

**Q. age는 필요 없는가?**  
A. 평균 WAF와 throughput에는 이번 Zipf 1.2 run에서 영향이 매우 작았다. 하지만 no-age에서 emergency GC가 증가했으므로 필요 없다고 단정할 수 없다.

**Q. Cold 30% eligibility는 왜 필요한가?**
A. eligibility를 12.5%로 완화한 code ablation에서 Cold victim invalid ratio가 약 12.5%로 낮아지고 WAF가 두 workload 모두 Global 수준으로 돌아왔다. 이 단일-seed 관찰은 30% gate가 이번 조건의 WAF 개선에 크게 기여했음을 뒷받침하지만, 모든 workload에서의 일반적 효과를 확정하려면 반복실험이 필요하다.

**Q. skew가 클수록 왜 좋아지는가?**  
A. hot write 비율과 높은-invalid Hot victim 선택이 증가하면서 reclaim/GC가 커지고 GC/1M이 줄어든 흐름이 counter에서 관찰됐다. 다만 이 설명은 실험 counter에 근거한 해석이며 독립 인과실험은 아니다.
