# FEMU V3 erase-survival 최종 결과 분석

## 1. 결론 요약

V3의 두 입력 신호와 routing은 의도대로 작동했다. Host-write sequence 기반 rewrite interval이 주 판단을 맡고, 경계 구간 `1024 < interval <= 4096`에서만 current LPN version의 block-erase survival count가 COLD 보정 신호로 사용됐다.

seed `20260824`의 3600초 최종 결과에서 V3 WAF는 `6.980748`이었다. Baseline `7.990327`보다 **12.6350% 낮고**, 기존 V1 T=1024 `7.547217`보다 **7.5057% 낮다**. 따라서 Hot/Cold 분리와 넓어진 rewrite 범위는 확실한 이득을 보였다.

그러나 erase 신호 자체의 효과를 분리하는 핵심 control인 V1 T=4096의 WAF는 `6.973143`으로 V3보다 **0.1091% 낮았다**. 즉 이번 단일-seed 결과에서는 V3 메커니즘이 정상 작동했지만, 경계 영역을 전부 HOT으로 두는 단순 control을 성능으로 넘지 못했다.

```text
WAF 순위
V1 T=4096 (6.973143)
< V3 R=1 (6.980748)
< V1 T=1024 (7.547217)
< Baseline (7.990327)
```

이 차이는 V3 실패나 bug를 뜻하지 않는다. V3가 실제로 방향을 바꾼 `ERASE_SURVIVAL_COLD` write는 전체 Host writes의 0.1332%뿐이어서, secondary signal의 영향 범위가 매우 작았기 때문이다.

## 2. 최종 비교 결과

네 비교군은 동일 seed `20260824`, fresh FEMU, 전체 6 GiB preconditioning, accounting reset, 5G/no-offset/3600초 Zipf workload 조건이다. Baseline과 V1 T=1024는 이전 최종 실험의 유효 결과를 재사용했다.

| Version | WAF | GC writes / Host writes | Avg GC copy | GC count | GC / 10⁶ Host writes | IOPS |
|---|---:|---:|---:|---:|---:|---:|
| Baseline | 7.990327 | 6.990327 | 14335.056199 | 57,919 | 487.638635 | 8,224 |
| V1 T=1024 | 7.547217 | 6.547217 | 14214.948053 | 57,982 | 460.586777 | 8,733 |
| **V1 T=4096** | **6.973143** | **5.973143** | **14036.066174** | 58,074 | **425.556736** | **9,464** |
| V3 T=1024/4096, R=1 | 6.980748 | 5.980748 | 14038.413585 | **58,051** | 426.027315 | 9,442 |

IOPS는 `group_reporting=0`인 fio 원문에서 32개 job의 표시 IOPS를 합산한 값이다. 모든 버전에서 다음 관계가 성립한다.

```text
NAND writes = Host writes + GC writes
WAF = NAND writes / Host writes
    = 1 + GC writes / Host writes
GC writes = average_gc_copy × gc_count
```

## 3. Baseline 대비 효과

| Version | WAF 변화 | Total GC writes 변화 | Avg GC copy 변화 | Normalized GC frequency 변화 | IOPS 변화 |
|---|---:|---:|---:|---:|---:|
| V1 T=1024 | -5.5456% | -0.7300% | -0.8379% | -5.5475% | +6.1892% |
| V1 T=4096 | **-12.7302%** | -1.8237% | **-2.0857%** | **-12.7311%** | **+15.0778%** |
| V3 | -12.6350% | **-1.8462%** | -2.0694% | -12.6346% | +14.8103% |

V3는 baseline보다 total GC writes를 15,328,173 page 줄였고, average GC copy를 약 2.07% 줄였다. 같은 3600초 동안 Host writes와 IOPS는 약 14.8% 증가했다. 따라서 baseline 대비 개선은 단순 counter 착시가 아니라, 정규화된 GC frequency와 한 번당 copy 비용이 함께 낮아진 결과다.

Time-based workload에서는 더 빠른 버전이 같은 시간에 더 많은 Host writes를 처리한다. 그러므로 `gc_count` 절대값만 보면 안 되고 `GC count / Host writes`를 함께 비교해야 한다. V3의 절대 GC count는 baseline보다 0.2279% 많지만, 백만 Host page writes당 GC는 487.64회에서 426.03회로 12.63% 줄었다.

## 4. R=1 선택 근거

최종 seed와 분리된 seed `20260823`으로 300초 histogram run을 먼저 수행했다.

| Current-version erase survival | Boundary events | 비율 |
|---|---:|---:|
| 0 | 873,192 | 98.4420% |
| 1 | 13,820 | 1.5580% |
| 2 | 0 | 0% |
| 3+ | 0 | 0% |
| 합계 | 887,012 | 100% |

분포가 사실상 0과 1로만 나뉘었으므로 R=1이면 erase feedback이 존재한 current version과 그렇지 않은 version을 가장 간단히 구분한다. R=2나 R=4는 이 workload에서 secondary signal을 거의 사용하지 않는 설정이 되므로 추가 performance sweep 없이 R=1을 선택했다. Tuning seed는 최종 결과에 포함하지 않았다.

## 5. V3 메커니즘이 실제로 한 일

V3 final에서 Host classification reason은 다음과 같다.

| Reason | Count | Host writes 대비 |
|---|---:|---:|
| `FAST_REWRITE` → HOT | 9,351,692 | 6.8631% |
| `BOUNDARY_HOT` → HOT | 11,234,028 | 8.2445% |
| `ERASE_SURVIVAL_COLD` → COLD | 181,436 | 0.1332% |
| `SLOW_REWRITE` → COLD | 115,494,060 | 84.7593% |
| `FIRST_WRITE` → COLD | 0 | 0% |

`FIRST_WRITE=0`인 이유는 preconditioning 후 classifier metadata를 유지했고, performance의 5G 범위가 이미 채워진 overwrite 대상이기 때문이다.

경계 영역만 보면 다음과 같다.

```text
boundary total = 11,234,028 + 181,436 = 11,415,464
erase-survival COLD 보정률 = 181,436 / 11,415,464
                            = 1.5894%
```

즉 경계 write의 98.41%는 survival=0이라 V1 T=4096과 똑같이 HOT으로 처리됐고, 1.59%만 V3의 erase signal 때문에 COLD로 바뀌었다. 이 때문에 V3와 V1 T=4096의 전체 Hot ratio도 15.1075%와 15.2191%로 0.1116%p밖에 차이나지 않는다.

GC destination에서도 V3의 `gc_hot_writes`는 V1 T=4096보다 57,672 page, 즉 0.9304% 줄었다. 따라서 erase feedback이 Host classification과 이후 GC placement에 실제 영향을 준 것은 counter로 확인된다.

## 6. V3와 V1 T=4096: erase signal의 순수 비교

V1 T=4096은 `interval <= 4096`을 모두 HOT으로 처리한다. V3는 그중 `1024 < interval <= 4096`만 survival을 보고 일부를 COLD로 보낸다. 두 버전의 차이가 erase feedback의 효과에 가장 가까운 ablation이다.

| Metric | V1 T=4096 | V3 | V3 변화 |
|---|---:|---:|---:|
| WAF | **6.973143** | 6.980748 | +0.1091% |
| Host page writes | 136,465,940 | 136,261,216 | -0.1500% |
| GC page writes | 815,130,507 | **814,943,947** | -0.0229% |
| Avg GC copy | **14036.066174** | 14038.413585 | +0.0167% |
| GC count | 58,074 | **58,051** | -0.0396% |
| GC / 10⁶ Host writes | **425.556736** | 426.027315 | +0.1106% |
| GC Hot writes | 6,198,556 | **6,140,884** | -0.9304% |
| IOPS | **9,464** | 9,442 | -0.2325% |

V3는 GC page writes 절대량과 GC count를 아주 조금 줄였지만, average GC copy가 아주 조금 늘었고 같은 시간에 처리한 Host writes가 0.15% 적었다. 최종 비율인 WAF와 normalized GC frequency는 V1 T=4096보다 약 0.11% 나빠졌다.

따라서 이번 결과로 주장할 수 있는 범위는 다음과 같다.

- erase-survival signal은 실제 classification과 GC destination을 바꿨다.
- V3는 V1 T=1024와 baseline을 크게 개선했다.
- 하지만 그 개선 대부분은 boundary window를 4096까지 넓힌 효과다.
- erase signal만의 추가 성능 이득은 이 단일 seed에서는 관찰되지 않았다.
- 0.109% 차이는 단일 run의 작은 차이이므로 V3가 본질적으로 열등하다고 일반화할 수도 없다.

## 7. Correctness와 과제 요구사항 연결

과제의 두 요구 신호는 다음과 같이 구현·검증됐다.

```text
LBA request frequency
→ wall-clock 대신 host-write sequence의 rewrite interval로 update frequency 추정

SSD 내부 block erase activity
→ aggregate erase count가 아니라 current LPN version이 살아남은 실제 block erase 횟수
```

Rewrite interval은 주 신호이고 erase survival은 애매한 경계에서만 사용된다. 빠른 rewrite와 느린 rewrite를 secondary signal이 뒤집지 않으므로 설계 의도가 명확하고 설명 가능하다.

Correctness trace는 first write COLD, fast rewrite HOT, boundary survival=0 HOT, slow rewrite COLD, TRIM 후 first write COLD를 확인했다. 별도의 GC trace에서는 host가 해당 marker LPN을 다시 쓰지 않은 동안 current version이 34번의 source block erase를 살아남았고, 다음 경계 write가 `ERASE_SURVIVAL_COLD`로 판정됐다. 모든 trace와 성능 run에서 invariant가 PASS였다.

엄밀히 말하면 V3의 frequency는 고정된 wall-clock 시간 동안의 요청 횟수가 아니라 write sequence 간격을 이용한 추정값이다. 보고서에는 “host-write sequence 기준 rewrite interval로 LBA update frequency를 추정했다”고 표현하는 것이 정확하다.

## 8. 코드·설계 피드백

좋았던 점:

- V1에서 최소 변경으로 분기해 Hot/Cold pool과 routing을 그대로 재사용했다.
- 함수와 enum 이름이 버전명보다 역할을 드러내므로 코드가 읽기 쉽다.
- `erase_survival_count`는 host write마다 0으로 reset되어 “새 current version”의 생존 횟수만 센다.
- GC는 `last_write_seq` 같은 frequency history를 갱신하지 않고 survival count만 올린다.
- TRIM은 metadata를 초기화한다.
- 실제 block을 free/erase 처리하는 경로에서 valid current version을 세므로 relocation 횟수를 일반적으로 세는 것보다 과제의 erase 신호에 가깝다.
- reason counter와 histogram invariant 덕분에 최종 통계만으로 분기 결과를 감사할 수 있다.
- V1 T=1024와 T=4096을 양쪽 control로 둔 실험 설계가 매우 좋다. 단순 threshold 확장 효과와 erase feedback 효과를 분리했다.

주의할 점:

- `mark_block_free()`에서 logical erase 처리와 1:1로 survival을 기록하고, 그 직후 FEMU NAND erase latency를 진행한다. 발표에서는 “source block의 logical erase event에 결합했다”고 설명하면 가장 정확하다.
- Histogram상 survival signal이 매우 희소하다. Final에서도 secondary signal이 전체 Host writes의 0.133%만 바꿨으므로 큰 WAF 차이를 기대하기 어려웠다.
- LPN metadata는 entry당 32 bytes, 전체 약 64 MiB다. 시뮬레이터에는 허용 가능하지만 실제 SSD controller 관점의 metadata 비용은 한계로 적어야 한다.
- Measurement 중 Cold pool empty/borrow가 두 신규 run 모두 10회 발생했다. 50:50 초기 pool은 borrowing으로 보정되지만 pool ratio 자체는 최적화하지 않았다.
- V3 state transition 비율은 `(Cold→Hot + Hot→Cold) / Host writes = 11.8345%`로 낮지 않다. erase signal이 classifier 안정성까지 개선했다고 주장해서는 안 된다.

## 9. 결과의 한계

1. **단일 seed, 단일 반복**: 0.109%인 V1 T=4096과 V3 차이를 통계적으로 확정할 수 없다.
2. **Steady-state curve 없음**: runtime은 3600초지만 중간 snapshot을 수집하지 않아 WAF-vs-written-data 평탄화를 직접 보이지 못한다.
3. **Time-based 비교**: 버전별 Host writes가 다르므로 total GC writes와 GC count 절대값에는 처리량 차이가 섞인다.
4. **Classifier metadata 유지**: preconditioning의 학습 상태가 measurement 시작점에 남는다. 이는 채워진 SSD의 overwrite workload라는 의미지만 명시해야 한다.
5. **R sensitivity 생략**: histogram이 0/1로 분리돼 R=1을 합리적으로 택했지만, 여러 seed의 R sensitivity를 수행한 것은 아니다.
6. **기존 control 재사용**: Baseline과 V1 T=1024는 동일 workload/seed의 이전 fresh run이지만 신규 V1 T=4096/V3와 실행 날짜가 다르다.

## 10. 최종 총평

V3는 과제에서 요구한 LBA update-frequency 신호와 internal erase 신호를 모두 실제 classifier 입력으로 사용하는 간결하고 설명 가능한 최종 설계다. Correctness, reason counter, histogram, 두 개의 V1 control까지 포함한 실험 구조도 탄탄하다.

성능 결과는 과장하지 않는 것이 중요하다. V3는 baseline 대비 WAF를 12.64% 줄이고 IOPS를 14.81% 높였으며 V1 T=1024보다도 크게 개선됐다. 그러나 V1 T=4096이 WAF 0.109%와 IOPS 0.233%만큼 앞섰다. 따라서 최종 보고서의 가장 정확한 결론은 다음과 같다.

> Erase-survival feedback은 경계 classification을 의도대로 보정했고 과제의 두 번째 입력 신호를 충족했다. 다만 이 Zipf workload와 단일 seed에서는 보정 대상이 전체 write의 0.133%로 희소하여, 경계 영역을 전부 HOT으로 처리한 V1 T=4096보다 추가적인 WAF 이득은 나타나지 않았다.

이 결과는 충분히 좋은 과제 결과다. “복잡한 신호가 무조건 더 좋다”가 아니라, 요구사항을 정확히 구현하고 적절한 control로 신호의 실제 기여가 작았음을 분리해 보였기 때문이다.
