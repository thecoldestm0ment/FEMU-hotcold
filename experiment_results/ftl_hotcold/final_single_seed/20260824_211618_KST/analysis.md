# FEMU Hot/Cold FTL 최종 단일-seed 결과 분석

## 1. 결론 요약

seed `20260824`의 3600초 결과에서는 세 Hot/Cold 버전이 모두 baseline보다 낮은 WAF와 높은 IOPS를 보였다. 가장 낮은 WAF는 V1의 `7.547217`이며, baseline 대비 5.5456% 감소했다.

V2는 더 보수적인 classifier를 사용하면서 V1보다 Hot write 비율이 낮아졌고, WAF는 V1보다 0.7021% 높았다. V2.5의 stale-Hot expiration은 실제 GC placement를 변경했으며 V2보다 WAF를 0.2016% 낮췄지만, V1을 넘지는 못했다. 따라서 이 실행이 지지하는 결론은 “V2.5 메커니즘은 동작하고 V2를 소폭 보정했지만, 현재 고정 parameter에서는 V1이 가장 좋았다”이다.

이 결과는 버전별 1회, 단일 seed 비교다. 차이의 재현성과 통계적 유의성까지 입증한 결과로 해석해서는 안 된다.

## 2. 핵심 결과

| Version | WAF | GC writes / Host writes | Avg GC copy | GC count | GC / 10⁶ Host writes | IOPS |
|---|---:|---:|---:|---:|---:|---:|
| Baseline | 7.990327 | 6.990327 | 14335.056199 | 57,919 | 487.638635 | 8,224 |
| V1 | **7.547217** | **6.547217** | **14214.948053** | 57,982 | **460.586777** | **8,733** |
| V2 | 7.600206 | 6.600206 | 14230.212451 | 57,971 | 463.816400 | 8,670 |
| V2.5 | 7.584884 | 6.584884 | 14225.724621 | 57,971 | 462.885644 | 8,673 |

계측 관계는 다음과 같다.

```text
WAF = NAND page writes / Host page writes
    = 1 + GC page writes / Host page writes

average_gc_copy = GC page writes / GC count
GC page writes = average_gc_copy × GC count
```

표의 `GC writes / Host writes`가 정확히 `WAF - 1`인 것은 counter가 위 관계를 만족하기 때문이다.

## 3. Baseline 대비 효과

| Version | WAF 변화 | GC writes 변화 | Avg GC copy 변화 | GC count 변화 | IOPS 변화 |
|---|---:|---:|---:|---:|---:|
| V1 | **-5.5456%** | -0.7300% | -0.8379% | +0.1088% | **+6.1892%** |
| V2 | -4.8824% | -0.6423% | -0.7314% | +0.0898% | +5.4232% |
| V2.5 | -5.0742% | -0.6736% | -0.7627% | +0.0898% | +5.4596% |

세 버전 모두 GC 횟수 자체는 baseline보다 약 0.09~0.11% 많았다. 그럼에도 총 GC writes가 0.64~0.73% 줄어든 이유는 GC 한 번당 복사량인 average GC copy가 0.73~0.84% 감소했기 때문이다. 즉 이번 결과에서 baseline 대비 이득은 “GC를 훨씬 덜 호출했다”기보다 “각 GC에서 복사하는 유효 page를 조금 줄였다”는 쪽에 가깝다.

3600초 동안 처리한 Host writes는 baseline보다 V1 5.99%, V2 5.23%, V2.5 5.44% 많았다. fio IOPS 증가 방향과 일치한다. 다만 time-based 실행이라 각 버전이 처리한 총 Host writes가 같지 않으므로, GC count의 절대값만 비교하기보다 `GC count / Host writes`도 함께 보아야 한다. 정규화 GC 빈도는 baseline의 487.64회/백만 Host page writes에서 V1 460.59, V2 463.82, V2.5 462.89로 낮아졌다.

## 4. V1이 가장 낮은 WAF를 보인 이유

V1의 Hot write ratio는 6.4128%로 V2의 5.5919%보다 0.8209%p 높다. V2는 `A=3`, `C=2`, decay 조건을 함께 사용하므로 단순 rewrite-window 기반 V1보다 Hot 판정이 보수적이다. 이번 Zipf workload에서는 그 보수성이 classifier 안정성은 높였지만, 분리했을 때 이득이 있었을 rewrite 일부를 COLD로 남겼을 가능성이 있다.

이를 뒷받침하는 관찰은 다음과 같다.

- V1 WAF: 7.547217
- V2 WAF: 7.600206, V1보다 0.7021% 높음
- V1 average GC copy: 14214.95
- V2 average GC copy: 14230.21
- V1 normalized GC frequency: 460.59/백만 Host writes
- V2 normalized GC frequency: 463.82/백만 Host writes

V2는 average GC copy와 정규화 GC 빈도 모두 V1보다 조금 높았다. 이 두 차이가 합쳐져 total GC writes/Host writes와 WAF가 V1보다 나빠졌다. 따라서 “더 많은 history를 쓰는 복잡한 classifier가 항상 더 좋은 분리를 만든다”는 주장은 이 결과로는 성립하지 않는다.

다만 이 결과는 현재 고정값 `T=1024, A=3, C=2, D=65536`에 대한 것이다. V2 구조 전체가 V1보다 열등하다고 일반화할 수는 없으며, parameter sensitivity를 수행하지 않았다는 점을 명시해야 한다.

## 5. Classifier 안정성

Host write 중 상태 전이가 발생한 비율을 `(Cold→Hot + Hot→Cold) / Host writes`로 계산하면 다음과 같다.

| Version | Cold → Hot | Hot → Cold | 전체 전이 비율 |
|---|---:|---:|---:|
| V1 | 5,477,976 | 5,476,908 | 8.7021% |
| V2 | 4,451,120 | 4,450,920 | 7.1224% |
| V2.5 | 4,556,428 | 4,556,208 | 7.2762% |

V2는 V1보다 전이 비율이 약 1.58%p 낮아 classifier가 덜 흔들렸다. 그러나 promotion과 demotion이 거의 같은 횟수로 반복되므로 상태 변화 자체는 여전히 빈번하다. 안정성 향상만으로 WAF가 개선되지는 않았고, V2의 낮은 Hot 판정률과 함께 보아야 한다.

여기서 `C=2`는 “연속된 short interval 두 번”이라고 표현하면 안 된다. 현재 의미는 decay가 유지하는 history 안에서 short interval count가 threshold를 만족했다는 뜻이다. long interval마다 즉시 0으로 만드는 연속성 조건과는 다르다.

## 6. V2 stale-Hot 후보

V2는 placement를 바꾸지 않고 historical HOT page가 GC될 때 idle age를 관찰했다.

| Threshold | Candidate events | V2 GC Hot writes 대비 | 전체 GC writes 대비 |
|---|---:|---:|---:|
| `>T` | 65,446 | 76.1425% | 0.007933% |
| `>2T` | 52,031 | 60.5350% | 0.006307% |
| `>4T` | 35,383 | 41.1660% | 0.004289% |
| `>8T` | 19,366 | 22.5312% | 0.002348% |

Historical HOT relocation 안에서는 오래 idle한 page의 비율이 높다. 특히 `>4T` 후보가 GC Hot writes의 41.17%였으므로 stale-Hot 문제와 V2.5 expiration을 시험할 이유는 충분했다.

반면 전체 GC writes에서 이 후보들이 차지하는 비율은 매우 작다. V2의 GC Hot writes 자체가 전체 GC writes의 약 0.0104%뿐이기 때문이다. 따라서 expiration이 historical HOT placement를 크게 바꿔도 전체 WAF에 미치는 효과가 작을 가능성이 처음부터 존재했다.

## 7. V2.5 expiration 효과

V2.5는 `E=4096=4T`를 사용했다.

- `stale_hot_gc_demotions=44,006`
- `stale_hot_gc_candidates_gt_4t=44,006`
- effective `gc_hot_writes=54,676`
- historical HOT GC relocation 추정값: `54,676 + 44,006 = 98,682`
- historical HOT relocation 중 demotion 비율: 44.5937%
- demotion이 전체 GC writes에서 차지하는 비율: 0.005336%

`idle_age > E`인 historical HOT page 44,006건이 COLD write pointer로 이동했으며, E와 동일한 `>4T` 후보 counter도 44,006으로 관찰됐다. 이는 expiration 조건과 placement counter가 의도대로 작동했다는 강한 sanity evidence다.

V2.5와 V2의 최종 지표 차이는 다음과 같다.

| Metric | V2 | V2.5 | V2.5−V2 |
|---|---:|---:|---:|
| WAF | 7.600206 | 7.584884 | -0.2016% |
| Avg GC copy | 14230.212451 | 14225.724621 | -0.0315% |
| GC count | 57,971 | 57,971 | 0% |
| GC page writes | 824,939,646 | 824,679,482 | -0.0315% |
| Host page writes | 124,986,956 | 125,238,276 | +0.2011% |
| IOPS | 8,670 | 8,673 | +0.0346% |

계획했던 인과관계에 대한 관찰은 다음과 같이 정리할 수 있다.

```text
stale-Hot demotion 44,006건 발생
→ GC Hot destination 감소
→ lifetime mixing 변화 가능성
→ average GC copy 0.0315% 감소, GC count 변화 없음
→ total GC writes 0.0315% 감소
→ WAF 0.2016% 감소
```

직접 입증된 구간은 demotion, destination counter 변화, GC 지표 변화다. “lifetime mixing이 개선됐다”는 별도 mixing metric이 없으므로 후속 GC 결과로부터 추론한 설명이어야 한다.

또한 V2.5의 WAF 감소율 0.2016%가 total GC writes 감소율 0.0315%보다 크다. V2.5가 같은 시간에 0.2011% 더 많은 Host writes를 처리했고 NAND writes는 사실상 같았기 때문에 분모 효과도 포함된다. 따라서 단일 실행만으로 0.2016% 전체를 expiration의 순수한 인과 효과라고 주장하면 안 된다. 직접적인 GC 비용 개선 신호는 average GC copy와 total GC writes의 약 0.0315% 감소다.

## 8. Pool 동작

Sequential preconditioning에서는 모든 첫 write가 COLD로 분류되어 각 Hot/Cold 버전에서 `cold_pool_empty_count=33`, `borrow_count=33`이 발생했다. Measurement counter reset 이후에도 세 버전 모두 Cold pool 고갈과 borrowing이 10회 발생했고, Hot pool 고갈과 emergency GC는 0회였다.

따라서 구현은 정적인 50:50 partition으로만 동작한 것이 아니라 Cold 수요가 커질 때 Hot pool의 line을 빌리는 구조로 동작했다. 실제 Host Hot 비율이 5.59~6.41%라는 점을 고려하면 borrowing은 필수적인 완충 장치였다. 동시에 “왜 초기 pool을 50:50으로 두었는가”에 대한 최적성은 이번 실험에서 검증되지 않았다.

## 9. 결과의 한계

1. **단일 seed, 단일 반복**: seed `20260824` 한 번씩만 실행했으므로 평균, 표준편차, 신뢰구간이 없다. 0.2% 수준인 V2와 V2.5 차이는 run-to-run noise와 분리해 확정할 수 없다.
2. **Steady-state 추적 없음**: 3600초로 runtime은 늘렸지만 중간 snapshot을 수집하지 않았다. 최종값은 measurement 전체 누적값이며 WAF-vs-written-data가 평탄해졌는지는 증명하지 못한다.
3. **Time-based 비교**: 버전별 처리량이 달라 Host writes가 동일하지 않다. WAF는 정규화 비율이지만 GC count와 total writes의 절대값 비교에는 처리량 차이가 섞인다.
4. **Classifier metadata 유지**: preconditioning 후 accounting counter만 reset했다. 물리 FTL 상태뿐 아니라 classifier history도 유지되므로, 측정은 학습 상태가 없는 clean classifier가 아니라 sequential fill을 경험한 overwrite 상태를 나타낸다.
5. **Parameter tuning 없음**: V2의 T/A/C/D와 V2.5의 E는 한 조합만 사용했다. V1/V2/V2.5 각각의 최적점을 비교한 결과가 아니다.
6. **실행 순서 고정**: baseline → V1 → V2 → V2.5 순서였으며 무작위화하지 않았다. 각 run은 fresh FEMU였지만 host 환경의 시간 순서 효과는 완전히 배제할 수 없다.

## 10. 최종 판단

이번 단일-seed 결과에서 순위는 다음과 같다.

```text
V1 (7.547217) < V2.5 (7.584884) < V2 (7.600206) < Baseline (7.990327)
```

- Hot/Cold 분리는 baseline 대비 WAF와 IOPS를 개선했다.
- V1은 현재 parameter에서 가장 낮은 WAF를 기록했다.
- V2는 classifier transition을 줄였지만 Hot 판정도 줄었고, V1보다 WAF가 높았다.
- V2.5 expiration은 44,006건의 stale-Hot GC placement를 COLD로 전환해 V2를 소폭 개선했다.
- 그러나 V2.5의 직접적인 GC 비용 감소는 약 0.0315%로 작았고 V1보다 WAF가 0.4991% 높았다.

따라서 보고서에는 “V2.5가 V2의 stale-Hot 문제를 메커니즘 수준에서 보정했다”와 “현재 단일-seed 고정 parameter의 최종 WAF는 V1이 가장 낮다”를 동시에 기술하는 것이 정확하다.
