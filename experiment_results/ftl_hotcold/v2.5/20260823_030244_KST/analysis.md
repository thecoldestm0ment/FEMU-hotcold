# V2.5 stale-Hot expiration 최종 분석

## 결론

V2.5의 GC expiration 로직 자체는 의도대로 동작했다. E=4T에서 historical HOT GC page의 평균 `52.71%`를 Cold destination으로 전환했고, 실제 Hot destination GC writes는 V2 평균 12,600.67에서 V2.5 평균 6,604.67로 `47.58%` 감소했다.

그러나 이 placement 변화가 WAF 개선으로 안정적으로 이어졌다고 보기는 어렵다. 3-seed 평균 WAF는 `7.462095 → 7.457715`로 0.0587% 낮아졌지만 seed별 paired 변화가 감소, 감소, 증가로 갈렸고 변화량의 표본 표준편차가 평균 변화보다 훨씬 크다. 이 결과에서는 “expiration mechanism은 작동하지만 성능 이점은 확정되지 않음”이 적절한 결론이다.

## Correctness

- Host classifier `COLD → COLD → HOT` 유지
- TRIM 후 첫 write COLD, 이후 재승격 유지
- `historical HOT && idle_age <= E → effective HOT` PASS
- `historical HOT && idle_age > E → effective COLD` PASS
- expiration 전후 `state`, `access_count`, `short_interval_count`, `last_write_seq`, `last_decay_epoch` 불변 PASS
- NAND write와 GC destination counter invariant PASS
- E의 positive integer validation PASS

경계별 원시 trace는 `correctness_trace.txt`에 있다.

## E sensitivity

seed 20260819에서 E만 바꾼 결과는 다음과 같다.

| E | Demotions | Historical HOT 중 demotion | Average GC copy | GC count | GC page writes | WAF |
|---:|---:|---:|---:|---:|---:|---:|
| 2T = 2048 | 9,611 | 69.05% | 14,198.514351 | 4,843 | 68,763,405 | 7.461601 |
| 4T = 4096 | 7,746 | 53.95% | 14,201.295699 | 4,836 | 68,677,466 | **7.458357** |
| 8T = 8192 | 3,683 | 29.86% | 14,201.117028 | 4,845 | 68,804,412 | 7.463951 |

우선순위에 따라 WAF가 가장 낮고 total GC writes와 GC count도 세 설정 중 가장 적은 E=4T를 선택했다. 다만 세 E의 WAF 범위는 0.005594에 불과하며 최종 3-seed의 seed 간 변동보다 작다. 따라서 E=4T 선택은 현재 workload에서의 provisional 선택이다.

## 최종 3-seed paired 비교

| Seed | V2 WAF | V2.5 WAF | Δ WAF | Δ GC writes | Δ GC count | Δ avg GC copy | Demotions |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 20260819 | 7.463094 | 7.458357 | -0.004737 | +874,701 | +61 | +1.763762 | 7,746 |
| 20260820 | 7.463259 | 7.437430 | -0.025829 | -53,646 | -1 | -8.142996 | 8,859 |
| 20260821 | 7.459931 | 7.477359 | +0.017428 | +40,720 | 0 | +8.395876 | 5,772 |

| Metric | V2 평균 ± 표본 SD | V2.5 평균 ± 표본 SD | 평균 변화 |
|---|---:|---:|---:|
| WAF | 7.462095 ± 0.001876 | 7.457715 ± 0.019972 | -0.004379 (-0.0587%) |
| GC page writes | 68,490,961.67 ± 596,731.08 | 68,778,220.00 ± 116,238.14 | +287,258.33 (+0.4194%) |
| GC count | 4,823.33 ± 41.93 | 4,843.33 ± 7.02 | +20.00 (+0.4147%) |
| Average GC copy | 14,199.9214 ± 1.2603 | 14,200.5936 ± 7.0813 | +0.6722 (+0.0047%) |
| Effective Hot GC writes | 12,600.67 ± 908.89 | 6,604.67 ± 593.03 | -5,996.00 (-47.58%) |

paired WAF 변화의 평균은 -0.004379, 표본 SD는 0.021631이다. n=3의 참고용 95% paired interval은 약 `[-0.0581, +0.0494]`로 0을 포함한다. 세 seed 중 하나는 WAF가 악화됐으므로 작은 평균 감소만으로 일반적인 개선을 주장하지 않는다.

## 인과관계 해석

관찰된 흐름은 다음과 같다.

```text
stale_hot_gc_demotions 발생
        ↓
stale page의 Hot destination 이동 감소: 명확히 관찰
        ↓
Hot/Cold lifetime composition 변화: 발생한 것으로 추론 가능
        ↓
average_gc_copy + gc_count 개선: seed마다 방향이 달라 미확인
        ↓
total GC writes 감소: 3-seed 평균에서는 감소하지 않음
        ↓
WAF 개선: 평균은 미세하게 감소했지만 일관되지 않음
```

V2.5는 placement 방향을 바꾸는 데는 성공했지만, Cold line으로 보낸 stale page가 Cold lifetime과 충분히 잘 맞았다는 증거는 없다. expiration은 historical HOT이라는 사실만 보고 Cold로 보내므로, demoted page의 남은 lifetime이 다른 Cold data와 다르면 새로운 mixing이 생길 수도 있다.

또한 workload가 300초 time-based이므로 seed와 버전마다 처리한 host page 수가 다르다. V2.5는 평균 host writes가 약 0.49% 많았고, 이 때문에 raw total GC writes는 WAF와 다른 방향으로 움직일 수 있다. normalized 결과인 WAF를 최종 판단 기준으로 두되, 후속 검증에서는 fixed-I/O 실행을 추가하면 GC writes와 GC count의 인과관계를 더 직접적으로 비교할 수 있다.

## 최종 판단

- 구현 correctness: PASS
- stale-Hot demotion 발생: PASS
- Hot GC relocation 감소: PASS
- E sweep 선택: E=4T
- 3-seed WAF 개선 재현성: 미확인
- 현재 결과로 V2.5가 V2보다 우수하다고 단정: 불가

다음 판단력을 높이는 가장 직접적인 방법은 동일한 E=4T에서 seed 수를 늘리거나, 동일 host write 수를 강제한 fixed-I/O paired 실험을 추가하는 것이다.
