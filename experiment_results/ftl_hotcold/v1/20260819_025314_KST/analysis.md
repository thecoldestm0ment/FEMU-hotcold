# FEMU BBSSD FTL V1 threshold 실험 분석

## 결론

이번 workload에서는 임계값을 64에서 4096 page writes로 늘릴수록 Hot 판정 비율이 4.41%에서 16.48%로 단조 증가했고, WAF는 7.6464에서 6.8613으로 단조 감소했다. 관측한 네 설정 중 **T=4096이 가장 좋았으며**, T=64 대비 WAF 10.27%, 기본값 T=1024 대비 5.98% 낮았다.

다만 이 결과는 V1 내부의 threshold 선택 결과다. 이번 범위에는 Hot/Cold separation을 끈 별도 baseline FTL 실행이 없으므로, “V1 분리가 무분리 FTL보다 낫다”는 질문까지는 결론 내릴 수 없다.

## 결과

| T | Hot write ratio | WAF | GC writes / host | Avg. GC copy | GC count | fio IOPS |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 4.4104% | 7.646401 | 6.646401 | 14253.105737 | 4776 | 8514 |
| 256 | 5.3446% | 7.589861 | 6.589861 | 14235.529412 | 4794 | 8604 |
| 1024 | 9.5534% | 7.297968 | 6.297968 | 14151.711918 | 4766 | 8908 |
| 4096 | 16.4796% | 6.861301 | 5.861301 | 14007.325765 | 4801 | 9544 |

원자료와 모든 counter는 `results.csv`에 있다.

## 해석

임계값이 커질수록 최근 overwrite를 Hot으로 인정하는 범위가 넓어졌고, 실제 Hot write ratio가 예상대로 증가했다. 이는 sweep가 classifier에 실질적인 변화를 주었음을 확인한다. 동시에 GC page writes/host가 6.6464에서 5.8613으로 줄어 WAF 개선의 직접 원인이 GC copy 부담 감소임을 보여준다.

평균 GC copy는 14253.11에서 14007.33 page/GC로 1.72% 감소했다. 감소 폭이 WAF 변화보다 작은 이유는 GC count가 4776~4801로 비슷하고, 시간 기반 fio에서 빠른 설정이 300초 동안 더 많은 host writes를 처리했기 때문이다. 따라서 총 NAND writes만 비교하면 안 되고 WAF, GC writes/host, average GC copy를 함께 봐야 한다.

GC가 옮긴 데이터의 대부분은 Cold였다. T=4096에서는 GC hot writes가 560,836까지 늘었지만 전체 GC writes 67,249,171 중 약 0.83%다. threshold를 더 늘리면 warm/cold 데이터를 Hot으로 잘못 묶는 지점이 나타날 수 있으나, 이번 범위에서는 아직 WAF가 악화되는 turning point가 관측되지 않았다. 후속 sweep를 한다면 8192와 16384를 추가하되 같은 workload/seed/preconditioning을 유지하는 것이 적절하다.

## 구조와 계측 검증

- 별도 write pointer: 초기 로그에서 `hot_line=64`, `cold_line=0`을 확인했다.
- Host routing: LPN 0 첫 write는 cold block 0, 바로 이은 rewrite는 hot block 64로 갔다.
- Reset semantics: 통계 reset 뒤 같은 LPN의 세 번째 write가 계속 hot block 64로 가서 history/state가 보존됨을 확인했다.
- GC routing: 모든 threshold의 `gc_marker.txt`에서 LPN 0이 GC로 여러 번 relocation되었다. 소스상 GC는 history를 갱신하지 않고 현재 LPN state로 `ssd_select_write_pointer()`를 호출한다.
- Counter: 네 실행 모두 NAND=Host+GC, HostHot+HostCold=Host, GCHot+GCCold=GC가 성립했다.

## 이상 징후와 영향

측정 구간마다 `cold_pool_empty_count=6`, `borrow_count=6`이 동일했고 hot pool 고갈 및 emergency GC는 0이었다. Cold pool borrow가 존재하므로 완전한 비혼합이라고 볼 수는 없지만, 모든 임계값에서 횟수가 같아 threshold 비교를 특정 설정에만 유리하게 만든 증거는 없다. preconditioning 구간의 cold pool borrow도 네 실행에서 동일하게 33회였다.

T=64 첫 시도는 fio 자체는 끝났지만 QEMU가 host의 대화형 실행 세션에 붙어 있어 최종 stats 회수 전에 종료됐다. 이 실행은 `runs/T64_attempt1`에 보존하고 결과에서 제외했으며, fresh VM/device로 같은 명령을 재실행했다. 유효 T=64 재실행과 이후 실행은 detached launch를 사용했다.

## 한계

- 임계값별 1회 실행이므로 run-to-run 분산과 신뢰구간은 알 수 없다.
- 단일 Zipf 0.99, 단일 seed, 단일 random-write workload 결과다.
- time-based 방식이라 host write 수가 설정별로 다르다. 비율 비교에는 적합하지만 동일 I/O 개수에서의 비교는 별도 실험이 필요하다.
- 무분리 baseline 및 V2/V2.5는 이번 실험 범위에 포함하지 않았다.

따라서 V1의 현재 채택 후보는 T=4096이지만, 최종 기본값 결정 전에는 각 설정을 최소 3회 반복하고 무분리 baseline을 추가하는 편이 안전하다.
