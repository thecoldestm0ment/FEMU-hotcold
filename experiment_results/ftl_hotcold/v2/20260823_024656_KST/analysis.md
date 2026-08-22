# V2 seed-1 stale-Hot 분석

## 핵심 결과

V2의 seed 20260819 실행은 WAF `7.463094`, total GC writes `67,802,765`, GC count `4,775`, average GC copy `14,199.531937`이었다. fio와 통계 명령은 모두 성공했고 counter invariant도 PASS다.

historical HOT GC relocation은 11,944 page event였다. 이 중 idle age가 T보다 큰 후보는 9,454개로 `79.15%`, 2T 초과는 `65.24%`, 4T 초과는 `46.85%`, 8T 초과는 `26.93%`였다. 따라서 V2가 과거 HOT 상태를 그대로 GC placement에 사용하면서 stale page를 Hot destination으로 보내는 현상은 실제 workload에서 충분히 존재한다.

| 기준 | 후보 event | historical HOT GC 대비 | 전체 GC copy 대비 |
|---|---:|---:|---:|
| `>T` | 9,454 | 79.152713% | 0.013943384% |
| `>2T` | 7,792 | 65.237776% | 0.011492157% |
| `>4T` | 5,596 | 46.851976% | 0.008253351% |
| `>8T` | 3,216 | 26.925653% | 0.004743169% |

## 기존 결과와의 배경 비교

동일 seed의 corrected baseline WAF `7.949892`보다 V2는 6.12% 낮다. V1 T=1024의 WAF `7.297968`보다는 V2가 2.26% 높다. 단일 seed 비교이므로 알고리즘 우열의 최종 결론이 아니라 V2.5 sweep의 기준값으로만 사용한다.

## V2.5에 대한 해석

candidate 비율은 historical HOT relocation 안에서는 크지만 전체 GC copy에 대한 비율은 최대 0.014%로 작다. 따라서 expiration이 직접 바꾸는 page 수만으로 WAF가 크게 변한다고 가정하면 안 된다. V2.5의 효과는 demoted page가 Cold line의 lifetime composition을 바꾸고 이후 victim valid-page 수와 GC 빈도에 연쇄적으로 영향을 주는지 `average_gc_copy`, `gc_count`, total `gc_page_writes`, WAF 순으로 확인해야 한다.
