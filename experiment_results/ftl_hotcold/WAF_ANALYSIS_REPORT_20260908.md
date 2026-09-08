# FEMU Hot/Cold FTL: WAF 개선 원인과 보고서용 지표 구성

작성일: 2026-09-08 KST. 대상: A1–G1 12개, threshold-off T1/T2, Baseline H1/H2의 총 16개 실행. 기존 Zipf 0.99 3600초 결과 2개는 A validation의 참조값으로만 사용한다.

## 1. 도출한 결론

이번 구현의 WAF 개선은 **skew가 강할 때 효과가 커지는 Hot/Cold 분리 배치**와 **Cold line을 충분히 invalid 상태가 된 뒤 회수하도록 하는 ClassGC eligibility**가 결합한 결과로 설명된다. `age × ipc`의 age 항은 이번 측정에서 큰 평균 WAF 개선을 설명하는 주된 항이 아니었다.

Uniform에서는 Baseline → V4 Global의 WAF가 7.987633 → 7.967380으로 0.254% 감소한 반면, Zipf 1.2에서는 7.648358 → 5.400359로 29.392% 감소했다. Global Greedy victim policy를 유지한 이 비교는 classifier와 분리 write pointer, line-pool 관리가 결합된 배치 정책의 관찰 효과가 strong skew에서 커졌다는 근거다.

ClassGC를 적용하면 V4 Global 대비 WAF가 Uniform에서 추가로 58.303%, Zipf 1.2에서 53.481% 감소했다. Cold eligibility를 normal 30% → 12.5%, forced 25% → `ipc > 0`으로 완화하면 WAF가 다시 각각 7.967045, 5.345066으로 Global 수준에 가까워졌다. 따라서 ClassGC의 추가 개선을 단순히 분리 배치나 age 점수의 효과만으로 설명할 수 없으며, **Cold eligibility에 의해 낮은-invalid victim의 회수가 제한되는 동작**이 핵심 설명이다.

동일 Host write량 512 GiB를 맞춘 B1/B2에서는 ClassGC가 NAND writes와 WAF를 모두 57.121% 줄였다. 한 번의 GC에서 복사하는 valid page가 줄고, 동일 Host write를 처리하는 데 필요한 GC 횟수도 줄었다는 결과가 원본 counter에서 함께 확인된다.

### 결과를 읽는 단계

| 분포 | Baseline | V4 Global | Full ClassGC | Baseline 대비 Full 감소 |
|---|---:|---:|---:|---:|
| Uniform | 7.987633 | 7.967380 | 3.322130 | 58.409% |
| Zipf 1.2 | 7.648358 | 5.400359 | 2.512177 | 67.154% |

Uniform의 절대 WAF 차이는 separation 단계 0.020253, ClassGC 단계 4.645249이다. Zipf 1.2에서는 각각 2.247999, 2.888182이다. 이것은 선택한 비교 순서의 관찰 차이 분해다. 기능 간 상호작용이 있으므로 독립적인 기여율로 취급하거나 두 단계의 상대 감소율을 더하지 않는다.

각 조합은 단일 seed의 1회 실행이며, 아래 결론은 이 조건에서 관찰한 메커니즘을 설명한다. 통계적 유의성, threshold/pool 비율의 최적성, 실제 SSD 수명 개선은 이 실험만으로 결론 내릴 수 없다.

## 2. 보고서에서 함께 묶을 지표

| 묶음 | 함께 제시할 지표 | 답하는 질문 | 넣을 위치 / 설명 방법 |
|---|---|---|---|
| 실험 조건·검증 | 분포, runtime 또는 io_size, seed, branch/commit, preconditioning/reset, invariant | 같은 조건의 비교인가? | 방법론 표와 부록. raw 수치를 해석하기 전에 분모와 측정 경계를 명시 |
| Write 비용 | Host / GC / NAND page writes, WAF | Host write 하나를 처리하는 데 NAND에 얼마나 더 쓰는가? | 핵심 결과. B1/B2 fixed-write에서 총량과 WAF를 함께 제시 |
| GC 효율 연쇄 | Victim invalid ratio, reclaim/GC, Avg GC copy, GC/1M Host pages, WAF | WAF가 왜 낮아졌는가? | 핵심 원인 표. 같은 행에 다섯 지표를 넣고 공간 회수와 copy 비용을 연결 |
| Hot/Cold 선택 구성 | Hot/Cold victim 수와 비중, class별 invalid ratio와 Avg copy, Host Hot write 비중 | 어느 class가 전체 평균을 바꿨는가? | 메커니즘 표. class 평균과 그 가중치인 victim 비중을 반드시 함께 제시 |
| Pool·압박 대응 | Borrow 수, final ownership, opposite normal/forced, global fallback, emergency GC 및 /1M | 자원 분배와 긴급 경로가 어떤 동작을 보였는가? | 보조 근거. WAF 결과와 혼동하지 않고 선택 정책이 만든 상태 변화로 설명 |
| 서비스 성능 | IOPS, 평균 total latency, P99/P99.9/P99.99 completion latency | media 효율 변화가 Host 응답에 어떻게 나타났는가? | 별도 성능 표. 평균과 tail을 구분하고 A1/A2의 특이값을 함께 명시 |

권장 본문 순서는 **공통 조건 → H1/D1·H2/F1 separation → D1/D2/T1·F1/F2/T2 threshold → F2/G1 age → B1/B2 fixed-write → 전체 skew 추세 → 성능·검증**이다. 아래 각 절의 인용문은 보고서에 그대로 사용할 수 있는 해석 문장이다. 16개 전체값은 부록에 모았다.

## 3. 비교 조건과 코드에서 확인한 의미

### 공통 조건

| 항목 | 설정 |
|---|---|
| 모델 | FEMU Blackbox non-FDP |
| 용량 | Raw 8 GiB, exposed 6 GiB, OP 25% = (8−6)/8 |
| Geometry | 4 KiB/page, 256 pages/block, 8 channels × 8 LUNs, 128 lines |
| GC 단위 | 1 line = 64 blocks = 16,384 pages = 64 MiB |
| 시작 | 각 run fresh FEMU, 6 GiB sequential write 후 measurement reset |
| Preconditioning | `write`, `bs=1M`, `iodepth=32`, `numjobs=1`, `size=6G` |
| 측정 workload | `randwrite`, `bs=16k`, `size=5G`, `iodepth=128`, `numjobs=32`, `direct=1`, `libaio` |
| 재현 설정 | fio 3.16 (JSON 존재 run), `randrepeat=1`, `randseed=20260824` |
| 시간 조건 | A/C/D/E/F/G/T/H: `time_based=1`, `runtime=1800` |
| 쓰기량 조건 | B1/B2: `io_size=16G`/job × 32 jobs = 512 GiB, time_based/runtime 없음 |
| GC 시작 임계값 | 75/95%; Cold victim eligibility 30/25%와 다른 설정 |
| NAND 지연 | read 40 μs, program 200 μs, erase 2 ms, channel transfer 0 |
| VM | 4 vCPU, RAM 4 GiB, KVM |

`size=5G`는 32 jobs가 같은 장치의 같은 5 GiB 영역을 대상으로 접근하는 조건이다. 서로 다른 5 GiB 영역 32개를 뜻하지 않는다. 16 KiB fio I/O 1개는 FEMU 4 KiB Host page write 4개에 해당한다. GC/1M의 분모는 fio I/O 횟수가 아니라 이 4 KiB Host pages다.

C1의 경로와 job filename에는 `3600`이 남아 있지만 현재 manifest, fio job, fio JSON에서 모두 실제 `runtime=1800`을 확인했다. B1/B2의 실제 시간은 각각 3,628.802초와 1,508.021초이며, 1800초 실험으로 묶지 않는다.

### 구현별 차이

| 구현 | Branch / commit | Placement | Victim policy |
|---|---|---|---|
| Baseline | `hotcold/base` / `c0237bb12f183d87ea1abf2bf52e92f900f6c587` | single pointer, classifier 없음 | Global Greedy, normal invalid ≥ 12.5% |
| V4 Global | `hotcold/v4` / `032d29b83e3906593ef9f79e5d2739bdab27ef5e` | classifier + Hot/Cold pointers/pools | Baseline과 같은 Global Greedy 선택 함수 |
| Full ClassGC | `hotcold/v4-classgc` / `d067a64de2974949807a6a6652cf2a08a76d1c4c` | V4 분리 배치 유지 | class pressure, Hot greedy, Cold eligibility 30/25%, Cold score age×ipc |
| No-age | `hotcold/v4-classgc-noage` / `ea289e975127f2e63e510c0ce4ed22081ffec293` | Full 유지 | Cold score만 ipc로 변경 |
| Threshold-off | `hotcold/v4-classgc-threshold-off` / `fd5967d4bfa8da8f5e374028e6acbce1c6537de5` | Full 유지 | Cold normal ≥12.5%, forced ipc>0; age×ipc 유지 |

고정 commit의 `ftl.c`, `ftl.h`, `bb.c`를 대조했다. Baseline → V4 비교는 classifier만이 아니라 dual write pointer, class별 free-line 관리, borrowing 및 GC relocation 배치를 포함한다. V4 Global → Full ClassGC는 pressure 기반 class 선택, eligibility, Cold score와 fallback 체계가 함께 바뀌는 비교다. 반면 Full → no-age와 Full → threshold-off는 각각 요청한 score/eligibility만 바뀐 FTL code ablation이다.

이 버전의 classifier는 LPN별로 **global Host page-write sequence 4096**을 기준으로 관찰 window를 관리한다. 현재 write까지 포함해 1회 이하면 Cold, 4회 이상이면 Hot이며, 중간 빈도는 실제 erase event 관찰값으로 나눈다. Cold score의 age도 **현재 Host-write sequence − 해당 line의 마지막 Host-write sequence**이다. 벽시계 시간이나 block erase 횟수 자체가 아니다.

measurement reset은 통계를 초기화하고 V4의 classifier 및 ClassGC age 관찰 이력을 초기화한다. mapping, 유효/무효 page, line ownership과 write pointer 등 물리 상태는 유지한다. 따라서 동일한 preconditioning 절차를 사용해도 서로 다른 placement 정책의 초기 물리 배치까지 동일하다고 주장하지 않는다.

Full ClassGC는 `(owned lines − free lines)/owned lines`가 큰 class를 먼저 고른다. 해당 class에 적격 victim이 없으면 opposite class를 시도하고, forced 경로에서도 선택하지 못하면 global fallback을 사용한다. Cold 30%는 16,384-page line에서 최소 4,916 invalid pages, 즉 약 30.0049%를 뜻한다. threshold-off에도 normal 12.5% gate가 남아 있어 모든 threshold가 제거된 구현은 아니다.

## 4. WAF와 GC 지표를 연결하는 방법

Host pages를 H, GC copy pages를 G, NAND pages를 N, GC 횟수를 K, line당 pages를 E=16,384라 두면 다음 관계가 성립한다.

```text
N = H + G
WAF = N/H = 1 + G/H
Avg GC copy C = G/K
Reclaim/GC R = E − C
Victim invalid ratio r = R/E
GC/1M Host pages q = K/H × 1,000,000
WAF = 1 + q × C / 1,000,000
```

`R`은 line 전체를 erase해 얻는 16,384 pages에서 valid-page relocation에 필요한 `C` pages를 뺀 **순수 공간 회수량**이다. 따라서 invalid ratio가 높을수록 한 번의 GC에서 복사할 valid page는 적고 순수 회수량은 많다.

다섯 지표는 서로 독립적인 다섯 증거가 아니다. invalid ratio/reclaim/Avg copy는 같은 victim 효율의 서로 다른 표현이고, WAF는 GC/1M과 Avg copy로 정확히 재구성된다. 독립적인 설득력은 이런 계산식 자체보다 **조건을 통제한 비교·ablation·raw invariant**에서 나온다.

긴 구간에서 시작/끝의 free-page 차이가 Host write량에 비해 작으면 `K×R ≈ H`, 따라서 `q ≈ 1,000,000/R`, `WAF ≈ 1/r`로 이해할 수 있다. 다만 유한 측정 구간에서는 시작/끝 free space 차이가 있으므로 `1/r`을 측정 WAF와 항상 같은 항등식으로 사용하지 않는다. CSV에는 `E×K−N`의 공간 수지 잔차를 별도로 저장했다.

> 본 실험의 WAF 개선은 높은-invalid victim 선택에 따른 valid-page copy 감소와 회수 공간 증가로 설명된다. 동일 Host write량을 처리하는 데 필요한 GC 횟수까지 감소하면서 Host page당 GC copy 비용 G/H가 줄었다. 다만 victim 선택이 GC 빈도를 바꿨다는 설명은 code와 비교 실험을 함께 사용한 해석이며, 계산식만으로 정책의 인과 효과가 증명되는 것은 아니다.

## 5. 묶음 A: separation의 이득이 skew에 따라 커지는가?

H1/D1은 Uniform Baseline/Global, H2/F1은 Zipf 1.2 Baseline/Global이다. 각 쌍의 fio job 파일은 byte-for-byte 동일하고, Global Greedy victim policy도 같다.

| Run | WAF | Victim invalid (%) | Reclaim/GC (pages) | GC/1M Host pages | Avg copy (pages) |
| --- | --- | --- | --- | --- | --- |
| H1 | 7.987633 | 12.503 | 2,048.525 | 487.436 | 14,335.475 |
| D1 | 7.967380 | 12.534 | 2,053.554 | 486.194 | 14,330.446 |
| H2 | 7.648358 | 13.051 | 2,138.312 | 466.693 | 14,245.688 |
| F1 | 5.400359 | 18.495 | 3,030.300 | 329.524 | 13,353.700 |

| V4 Global의 Baseline 대비 변화 | Uniform | Zipf 1.2 |
|---|---:|---:|
| WAF | −0.254% | −29.392% |
| Victim invalid ratio | +0.031%p | +5.444%p |
| Reclaim/GC | +0.245% | +41.715% |
| GC/1M Host pages | −0.255% | −29.392% |
| Avg GC copy | −0.035% | −6.261% |

Uniform은 거의 같은 write cost를 보인다. Zipf 1.2에서는 Baseline보다 V4 Global이 훨씬 높은-invalid victim을 얻으며, copy 감소와 reclaim 증가가 GC/1M 감소로 이어진다. 자주 갱신되는 data와 그렇지 않은 data가 다른 line에 배치되어, 높은 invalid 비율의 line이 형성되기 쉬워졌다는 설명과 일치한다. 실제 line별 lifetime 분포나 classifier 정확도를 측정한 것은 아니므로 이 부분은 code와 counter에 근거한 메커니즘 해석이다.

절대 WAF 개선폭은 Uniform 0.020253, Zipf 1.2 2.247999이며, 개선폭의 차이는 **2.227746 WAF units**다. 상대 감소율의 차이는 29.138%p다. 이 수치는 분포에 따라 효과가 달라진다는 관찰 대비값이며, 반복실험 없는 통계적 interaction 검정이나 시간에 따른 difference-in-differences 추정량으로 해석하지 않는다.

> Global Greedy를 유지한 채 Hot/Cold 분리 배치를 적용했을 때, Uniform의 WAF 변화는 0.254%에 그쳤으나 Zipf 1.2에서는 29.392% 감소했다. Zipf 1.2에서 증가한 victim invalid ratio와 reclaim/GC, 감소한 GC/1M은 분리 배치의 효율 이득이 strong skew에서 더 크게 나타났음을 뒷받침한다. 이 결과는 classifier와 분리 write pointer 및 관련 pool 관리가 결합된 배치 정책의 효과로 해석한다.

## 6. 묶음 B: ClassGC는 무엇을 추가로 개선하는가?

### Cold eligibility ablation

| Run | WAF | Victim invalid (%) | Reclaim/GC (pages) | GC/1M Host pages | Avg copy (pages) |
| --- | --- | --- | --- | --- | --- |
| D1 | 7.967380 | 12.534 | 2,053.554 | 486.194 | 14,330.446 |
| D2 | 3.322130 | 30.063 | 4,925.444 | 202.655 | 11,458.556 |
| T1 | 7.967045 | 12.533 | 2,053.382 | 486.165 | 14,330.618 |
| F1 | 5.400359 | 18.495 | 3,030.300 | 329.524 | 13,353.700 |
| F2 | 2.512177 | 39.763 | 6,514.832 | 153.222 | 9,869.168 |
| T2 | 5.345066 | 18.689 | 3,061.966 | 326.156 | 13,322.034 |

Uniform D1 → D2는 WAF 7.967380 → 3.322130, Zipf 1.2 F1 → F2는 5.400359 → 2.512177이다. 이미 두 쪽 모두 Hot/Cold 배치를 사용하므로 이 차이는 separation을 새로 추가한 효과가 아니다. ClassGC victim policy 변경의 추가 효과다.

Full → threshold-off에서는 Uniform reclaim/GC가 4,925.444 → 2,053.382(−58.311%), Zipf 1.2가 6,514.832 → 3,061.966(−53.000%)으로 감소한다. 동시에 GC/1M은 각각 139.898%, 112.865% 증가하고, WAF는 각각 139.817%, 112.766% 증가한다.

| 분포 | Full Cold invalid | Threshold-off Cold invalid | Full 대비 off WAF 증가량 | Global→Full 관찰 개선폭 대비 되돌아간 비율 |
|---|---:|---:|---:|---:|
| Uniform | 30.010% | 12.504% | 4.644914 | 99.993% |
| Zipf 1.2 | 30.010% | 12.535% | 2.832889 | 98.086% |

마지막 열의 정의는 `(WAF_off − WAF_full)/(WAF_global − WAF_full)`이다. WAF 자체가 99.993% 증가했다는 뜻도, threshold의 독립 인과 기여율이 99.993%라는 뜻도 아니다.

Cold invalid가 30% 부근에서 12.5% 부근으로 내려가며 Cold 평균 copy는 약 11,467 pages에서 14,330–14,335 pages로 증가했다. 이는 eligibility 완화로 낮은-invalid Cold line이 회수되기 쉬워진 동작과 맞는다. Full의 높은 gate는 normal 경로에서 해당 line의 회수를 보류하거나 opposite class를 탐색하게 하므로, invalid page가 더 누적된 상태의 victim을 사용하게 되는 것으로 해석할 수 있다.

이 ablation은 **normal과 forced Cold gate를 동시에 변경**했다. 따라서 normal 30% 단독의 정확한 효과와 forced 25% 단독의 효과를 분리하지 못한다. normal 30% 부근에 모인 Full Cold 평균과 적은 emergency 횟수는 normal eligibility가 중요하다는 해석을 지지하지만, 남은 gate 효과·pressure 선택·borrowing의 상호작용까지 독립적으로 분해한 것은 아니다.

> ClassGC의 WAF 개선은 Cold score에 age를 추가했다는 사실보다 Cold victim eligibility 변화와 강하게 연결됐다. 동일 score를 유지하고 Cold gate만 완화했을 때 Cold victim의 평균 invalid ratio가 약 30%에서 약 12.5%로 낮아졌고, 공간 회수 효율과 WAF도 V4 Global 수준으로 되돌아갔다. 따라서 이 구현에서 Cold eligibility는 ClassGC 추가 개선의 핵심 구성요소로 판단된다.

### Uniform에서도 개선되는 이유

D2 Uniform의 Hot victims는 28,705회 중 33회, 즉 0.115%뿐이다. 나머지 대부분을 차지하는 Cold victims의 invalid ratio가 약 30.010%이므로, Uniform ClassGC의 낮은 WAF를 strong locality나 뛰어난 Hot 검출만으로 설명할 수 없다. 이 구간에서는 다수 Cold victim의 높은 eligibility가 전체 평균을 결정하는 설명이 더 직접적이다.

## 7. 묶음 C: Hot/Cold 선택 비중과 전체 skew 추세

| 분포 | Global WAF | ClassGC WAF | Global Hot writes (%) | ClassGC Hot writes (%) | ClassGC Hot victims (%) | ClassGC Cold invalid (%) |
| --- | --- | --- | --- | --- | --- | --- |
| Uniform | 7.967380 | 3.322130 | 0.301 | 0.299 | 0.115 | 30.010 |
| Zipf 0.7 | 7.936123 | 3.312984 | 0.758 | 0.766 | 0.306 | 30.010 |
| Zipf 0.99 | 6.961641 | 3.040172 | 15.445 | 14.355 | 6.346 | 30.009 |
| Zipf 1.2 | 5.400359 | 2.512177 | 37.892 | 38.690 | 18.848 | 30.010 |

| Run | Hot victims | Cold victims | Hot 비중 (%) | Hot invalid (%) | Cold invalid (%) | Hot copy | Cold copy |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A2 | 1290 | 19038 | 6.346 | 74.445 | 30.009 | 4,186.893 | 11,467.405 |
| B2 | 1410 | 23629 | 5.631 | 76.743 | 30.009 | 3,810.468 | 11,467.395 |
| C1 | 1661 | 28181 | 5.566 | 76.958 | 30.008 | 3,775.217 | 11,467.427 |
| D2 | 33 | 28672 | 0.115 | 75.432 | 30.010 | 4,025.212 | 11,467.112 |
| E2 | 91 | 29648 | 0.306 | 74.940 | 30.010 | 4,105.802 | 11,467.166 |
| F2 | 5660 | 24370 | 18.848 | 81.759 | 30.010 | 2,988.528 | 11,467.215 |
| G1 | 5576 | 24537 | 18.517 | 82.320 | 30.006 | 2,896.764 | 11,467.773 |
| T1 | 13 | 27833 | 0.047 | 75.265 | 12.504 | 4,052.615 | 14,335.419 |
| T2 | 2570 | 26396 | 8.872 | 81.898 | 12.535 | 2,965.864 | 14,330.344 |

Host Hot write 비중은 전체 Host write 중 classifier가 Hot으로 보낸 비율이다. Hot victim 비중은 전체 GC 중 Hot line을 회수한 비율이다. 분모가 다르며, 어느 것도 고유 Hot LPN의 비율이나 classifier 정확도를 뜻하지 않는다.

전체 invalid ratio는 class별 invalid ratio를 **victim 횟수 비중으로 가중 평균**한 값이다.

```text
r_all = (K_hot/K) × r_hot + (K_cold/K) × r_cold
```

Uniform D2는 Hot victim 비중 0.115%, Hot invalid 75.432%, Cold invalid 30.010%가 결합해 전체 invalid가 30.063%다. Zipf 1.2 F2는 Hot victim 비중이 18.848%로 늘고 Hot invalid도 약 81.759%여서 전체 invalid가 39.763%까지 높아진다. Cold invalid 평균은 두 workload 모두 약 30.010%로 거의 같다.

따라서 ClassGC 내부의 Uniform → Zipf 1.2 개선은 **Cold 평균 자체의 추가 상승보다는 높은-invalid Hot victim의 선택 비중 증가**로 산술적으로 설명된다. Full ClassGC의 WAF는 3.322130 → 2.512177로 24.381% 감소한다. Uniform과 Zipf 0.7은 Hot 비중과 media metric이 모두 가깝고, Zipf 0.99부터 차이가 커진다.

> ClassGC는 workload 전반에서 Cold victim의 invalid ratio를 약 30% 부근으로 유지했다. Strong skew에서는 높은 invalid ratio를 가진 Hot victim의 선택 비중이 증가하여 전체 reclaim/GC가 더 커졌다. 따라서 Cold eligibility가 기본 GC 효율을 높이고, strong skew에서 형성되는 Hot victim의 높은 회수 효율이 추가 개선을 제공한 것으로 해석된다.

Baseline 비교는 Uniform과 Zipf 1.2에만 존재한다. Zipf 0.7/0.99에서는 V4 Global/ClassGC 내부의 skew 추세를 관찰할 수 있지만 Baseline 대비 separation 효과의 정확한 크기는 이 자료에 없다.

## 8. 묶음 D: age는 큰 WAF 개선의 원인인가?

| Run | WAF | Victim invalid (%) | Reclaim/GC (pages) | GC/1M Host pages | Avg copy (pages) |
| --- | --- | --- | --- | --- | --- |
| A2 | 3.040172 | 32.828 | 5,378.611 | 185.379 | 11,005.389 |
| C1 | 3.061347 | 32.622 | 5,344.720 | 186.728 | 11,039.280 |
| F2 | 2.512177 | 39.763 | 6,514.832 | 153.222 | 9,869.168 |
| G1 | 2.516456 | 39.693 | 6,503.314 | 153.477 | 9,880.686 |

| 비교 | No-age WAF 변화 | GC/1M 변화 | Reclaim/GC 변화 | Host pages 변화 |
|---|---:|---:|---:|---:|
| A2 → C1, Zipf 0.99 | +0.697% | +0.728% | −0.630% | +45.742% |
| F2 → G1, Zipf 1.2 | +0.170% | +0.166% | −0.177% | +0.110% |

A2/C1은 Host write량이 크게 다르고 A2의 IOPS/tail latency도 다른 run들과 차이가 커서 성능 차이를 age의 효과로 해석하기 어렵다. F2/G1은 Host write량 차이가 0.110%로 작으며, 평균 WAF 차이도 0.170%에 그쳤다. Cold invalid는 각각 약 30.010%, 30.006%로 gate 부근에 남았다.

F2 → G1의 emergency GC는 35 → 94회, Host 백만 pages당 0.179 → 0.479회다. age가 pressure 상황에 영향을 줄 가능성은 있지만 emergency count는 성공한 forced GC의 횟수이며, 실패·stall 시간 또는 latency 개선의 직접 측정값은 아니다. 한 번의 관찰만으로 age가 반드시 emergency를 줄인다고 단정할 수 없다.

> Age 제거 후 평균 WAF 차이는 Zipf 1.2에서 0.170%에 그쳤다. 반면 Cold eligibility 완화의 WAF 영향은 훨씬 컸다. 따라서 이번 데이터에서 큰 평균 WAF 개선의 주된 원인을 age 항으로 설명하기는 어렵다. Age의 잠재적 역할은 평균 효율보다 victim 순서 및 드문 forced GC 상황에서 추가 검토할 수 있다.

## 9. 묶음 E: 동일 쓰기량에서 개선이 유지되는가?

| 지표 | B1 Global | B2 ClassGC | ClassGC 상대 변화 (%) |
| --- | --- | --- | --- |
| Host pages | 134,217,728 | 134,217,728 | 0.00 |
| GC pages | 823,252,518 | 276,335,831 | -66.43 |
| NAND pages | 957,470,246 | 410,553,559 | -57.12 |
| WAF | 7.133709 | 3.058862 | -57.12 |
| GC count | 58,433 | 25,039 | -57.15 |
| GC/1M | 435.360 | 186.555 | -57.15 |
| Avg copy | 14,088.829 | 11,036.217 | -21.67 |
| Reclaim/GC | 2,295.171 | 5,347.783 | 133.00 |
| IOPS | 9,246.7 | 22,250.6 | 140.63 |
| Runtime (ms) | 3,628,802 | 1,508,021 | -58.44 |

B1/B2는 모두 134,217,728 Host pages = 512 GiB를 썼다. ClassGC는 GC copy를 823,252,518 → 276,335,831 pages로 줄였으며, 총 NAND 쓰기도 같은 절대량인 **546,916,687 pages**만큼 줄었다. GC copy의 감소율은 66.434%, WAF/NAND의 감소율은 57.121%다. WAF에는 피할 수 없는 Host write 항 1이 포함되므로 두 감소율은 다르다.

```text
GC copy 총량 비율 = GC 횟수 비율 × Avg copy 비율
                = 0.428508 × 0.783331
                ≈ 0.335664
```

GC 횟수와 회당 copy가 함께 줄었기 때문에 G/H가 줄었다. IOPS 증가율은 140.633%, 실제 완료시간 감소율은 58.444%로 같은 방향이다. 이 fixed-write 비교는 time-based 비교의 Host write량 차이라는 제한을 보완한다. 다만 32-job 실행의 요청 interleaving이나 모든 물리 상태가 두 run에서 동일했다는 뜻은 아니다.

> 동일한 512 GiB Host write를 처리했을 때 ClassGC는 더 큰 reclaim/GC를 확보해 GC 횟수를 57.149% 줄였고, GC당 copy도 21.667% 줄였다. 이에 따라 총 GC copy가 66.434%, 총 NAND 쓰기와 WAF가 57.121% 감소했다. 따라서 관찰된 WAF 개선은 단순히 처리한 Host write량이 달라서 생긴 결과만으로 설명되지 않는다.

## 10. 묶음 F: Pool과 pressure counter는 어떻게 해석하는가?

| Run | Borrow | Final Hot/Cold | Opposite normal | Opposite forced | Global fallback | Emergency | Emergency/1M |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A2 | 18 | 7/121 | 1290 | 0 | 0 | 18 | 0.164 |
| B2 | 18 | 7/121 | 1410 | 0 | 0 | 19 | 0.142 |
| C1 | 18 | 7/121 | 1661 | 0 | 0 | 35 | 0.219 |
| D2 | 18 | 7/121 | 33 | 0 | 0 | 13 | 0.092 |
| E2 | 18 | 7/121 | 91 | 0 | 0 | 13 | 0.088 |
| F2 | 18 | 7/121 | 5660 | 0 | 0 | 35 | 0.179 |
| G1 | 18 | 7/121 | 5576 | 0 | 0 | 94 | 0.479 |
| T1 | 4 | 21/107 | 13 | 0 | 0 | 0 | 0.000 |
| T2 | 4 | 21/107 | 2570 | 0 | 0 | 0 | 0.000 |

Full/no-age는 초기 Hot/Cold ownership 25/103에서 최종 7/121, threshold-off는 21/107이다. Full/no-age의 최종 Hot ownership은 7/128=5.469%, threshold-off는 21/128=16.406%다. 설정값 Hot 20%는 초기 line 수를 정하는 요청값이며 128 lines의 정수 분할로 실제 초기 Hot 비율은 25/128=19.531%다.

동일 borrowing 코드를 유지해도 선택 정책 변경에 따라 빈 pool과 요청되는 free line의 상황이 달라져 borrow count와 ownership 결과가 달라질 수 있다. 따라서 threshold 효과에는 이러한 상태 변화가 매개하는 영향도 들어 있다. 18회 borrowing이나 최종 7 lines가 최적이라는 뜻은 아니다. 또한 Global의 최종 ownership은 해당 stats에 직접 출력되지 않아 CSV에 추정값을 채우지 않았다.

ClassGC 계열 9개 run 모두 opposite forced와 global emergency fallback은 0이다. 이는 해당 성공 경로가 사용되지 않았음을 뜻한다. Emergency GC는 Full/no-age에서 0이 아니므로 **fallback 0 = forced GC 0**으로 해석하면 안 된다.

Opposite normal 수와 Hot victim 수가 각 run에서 같지만, 두 값은 서로 다른 주변 집계다. 이 일치만으로 모든 Hot victim이 opposite-normal 경로에서 선택됐다는 일대일 대응을 증명할 수 없다. 경로×class 교차 counter나 event trace가 있어야 그 주장을 직접 확인할 수 있다.

> Borrowing과 final ownership은 victim 정책이 자원 분배에 미친 동작 결과를 보여준다. Fallback이 0인 가운데도 Full/no-age에는 일부 forced GC가 관찰됐다. 이러한 counter는 WAF 개선의 보조 설명으로 사용하되, pool 비율 최적성이나 긴급 상황이 완전히 사라졌다는 결론으로 확대하지 않는다.

## 11. 1800초 결과는 기존 3600초와 일관적인가?

L1/L2는 기존 Zipf 0.99 Global/ClassGC 3600초 참조 결과다. L2 commit `c277fbcc99c016db265674cac7ce81c98addbdfc`와 Full commit의 FTL 차이는 주석/표현 정리로 확인했다.

| Run | WAF | Victim invalid (%) | Reclaim/GC (pages) | GC/1M Host pages | Avg copy (pages) |
| --- | --- | --- | --- | --- | --- |
| A1 | 6.961641 | 14.340 | 2,349.406 | 424.782 | 14,034.594 |
| L1 | 7.147977 | 13.980 | 2,290.476 | 436.227 | 14,093.524 |
| A2 | 3.040172 | 32.828 | 5,378.611 | 185.379 | 11,005.389 |
| L2 | 3.060527 | 32.653 | 5,349.911 | 186.742 | 11,034.089 |

1800초는 3600초 대비 Global WAF가 약 2.61%, ClassGC가 약 0.67% 낮다. 두 시간 모두 ClassGC의 invalid ratio/reclaim이 더 높고 GC/1M과 WAF가 더 낮다. 따라서 평균 media 효율의 순위와 대략적인 수준은 유지됐다.

A1/A2는 B 및 다른 1800초 run에 비해 IOPS가 낮고 tail latency가 크게 나타난다. 이 자료만으로 원인이 정책인지 Host 실행환경인지 특정하지 않는다. 누적값 두 시점 비교만으로 시간에 따른 steady state 도달을 증명할 수도 없다.

> Zipf 0.99의 1800초와 기존 3600초 비교에서 normalized media metric의 우열과 대략적인 수준이 유지됐다. 이를 짧은 실행에서도 GC 효율 경향이 관찰됐다는 근거로 사용한다. A1/A2의 절대 성능과 tail latency는 다른 run 대비 차이가 커서 성능 재현성이나 steady state의 증거로는 사용하지 않는다.

## 12. 묶음 G: 서비스 성능과 총량을 별도로 읽기

| Run | IOPS | Avg lat (ms) | P99 clat (ms) | P99.9 clat (ms) | P99.99 clat (ms) | fio 근거 |
| --- | --- | --- | --- | --- | --- | --- |
| A1 | 7,815.6 | 524.040 | 2,332.033 | 5,804.917 | 12,280.922 | JSON |
| A2 | 15,228.0 | 268.929 | 2,332.033 | 8,153.727 | 17,112.760 | JSON |
| B1 | 9,246.7 | 442.838 | 834.666 | 1,166.017 | 2,021.655 | JSON |
| B2 | 22,250.6 | 183.985 | 400.556 | 530.579 | 1,266.680 | JSON |
| C1 | 22,192.3 | 184.560 | 400.556 | 675.283 | 1,249.903 | JSON |
| D1 | 8,253.1 | 496.276 | 918.553 | 1,736.442 | 3,003.122 | JSON |
| D2 | 19,670.4 | 208.220 | 530.579 | 1,468.006 | 2,264.924 | JSON |
| E1 | 8,300.1 | 493.453 | 1,266.680 | 1,686.110 | 3,103.785 | JSON |
| E2 | 20,434.6 | 200.437 | 425.722 | 675.283 | 1,484.784 | JSON |
| F1 | 12,303.3 | 332.896 | 658.506 | 876.610 | 2,164.261 | JSON |
| F2 | 27,217.2 | 150.482 | 341.836 | 599.785 | 1,333.789 | JSON |
| G1 | 27,248.6 | 150.306 | 333.447 | 438.305 | 1,233.125 | JSON |
| T1 | 7,952.6 | 514.975 | 1,216.348 | 2,499.805 | 3,841.982 | 기존 요약† |
| T2 | 12,331.8 | 332.079 | 666.894 | 1,010.827 | 1,669.333 | JSON |
| H1 | 8,196.0 | 499.716 | 994.050 | 1,702.887 | 2,634.023 | JSON |
| H2 | 8,621.7 | 475.049 | 801.112 | 985.661 | 1,233.125 | JSON |

평균 latency는 fio `write.lat_ns.mean`(submission을 포함하는 total latency), P99/P99.9/P99.99는 `write.clat_ns.percentile`(completion latency)이다. 모두 ms로 변환했지만 서로 같은 분포의 평균/백분위가 아니므로 표에 `lat`와 `clat`을 구분했다. †T1 성능값은 기존 요약에 남은 값이고 현재 fio JSON으로 재검증할 수 없다.

F1/F2는 NAND 총량이 478,475,249 → 492,360,848 pages로 2.902% 늘고 GC 횟수도 29,196 → 30,030회로 2.857% 늘었다. 그러나 Host pages가 121.206% 증가했기 때문에 Host page당 GC 횟수는 53.502% 줄고 WAF는 53.481% 낮아졌다. **Time-based run의 GC 횟수나 NAND 총량만 보고 효율이 나빠졌다고 판단하면 안 되는 사례**다.

Configured aggregate queue depth는 최대 128×32=4096이다. 같은 높은 부하에서 IOPS와 평균 latency는 queueing 관계로 연결되므로 두 값의 개선을 완전히 독립적인 두 증거로 세지 않는다. Tail은 A2가 A1보다 P99.9/P99.99가 나쁘고, Baseline H2보다 V4 Global F1의 P99.99가 큰 예도 있어, 낮은 WAF가 모든 tail percentile 개선을 보장한다고 쓰지 않는다.

> ClassGC의 낮은 WAF는 다수 조건에서 더 높은 IOPS와 낮은 평균 latency와 함께 나타났다. 그러나 total Host write량이 다른 time-based 실험에서는 raw NAND/GC 횟수보다 normalized media metric을 우선 비교해야 한다. 일부 tail latency의 방향은 평균 효율과 달랐으므로, WAF 개선을 일관된 tail 개선으로 일반화하지 않는다.

## 13. 보고서에 넣을 최종 결론 문단

> 본 실험은 FEMU Blackbox 환경에서 Hot/Cold FTL의 WAF 개선을 배치 정책과 GC victim 정책으로 나누어 분석하였다. Global Greedy를 고정한 Baseline–V4 Global 비교에서 Uniform의 WAF 개선은 0.254%에 그쳤으나 Zipf 1.2에서는 29.392%로 커졌다. 이는 강한 접근 편향에서 Hot/Cold classifier와 분리 write pointer를 사용하는 배치가 높은-invalid victim 형성에 유리했음을 뒷받침한다. 여기에 ClassGC를 적용하면 Cold victim의 eligibility를 높게 유지하고 높은 회수 효율의 Hot victim을 함께 활용하여 reclaim/GC를 늘리고 Host write당 GC 비용을 줄였다. Cold gate를 완화한 ablation에서는 이 추가 WAF 개선의 대부분이 사라진 반면, age 제거의 평균 WAF 영향은 Zipf 1.2에서 0.170%로 작았다. 동일 Host write량 512 GiB 비교에서도 NAND 쓰기와 WAF가 57.121% 감소하여, 개선이 처리량 차이만으로 설명되지 않음을 확인하였다. 따라서 이번 조건의 큰 WAF 개선은 strong skew에서 효과가 커지는 분리 배치와 Cold eligibility 중심의 회수 정책의 결합으로 설명하는 것이 타당하다.

> 이 결론은 단일 seed의 관찰 결과이며, classifier와 pool 관리의 개별 기여, normal/forced Cold gate 각각의 영향, threshold나 pool 비율의 최적성은 분리해 측정하지 않았다. 또한 평균 WAF 감소는 실제 SSD 수명 또는 모든 tail latency의 개선을 직접 입증하지 않는다.

## 부록 A. 16개 전체 GC 효율과 write counter

| Run | WAF | Victim invalid (%) | Reclaim/GC (pages) | GC/1M Host pages | Avg copy (pages) |
| --- | --- | --- | --- | --- | --- |
| A1 | 6.961641 | 14.340 | 2,349.406 | 424.782 | 14,034.594 |
| A2 | 3.040172 | 32.828 | 5,378.611 | 185.379 | 11,005.389 |
| B1 | 7.133709 | 14.009 | 2,295.171 | 435.360 | 14,088.829 |
| B2 | 3.058862 | 32.640 | 5,347.783 | 186.555 | 11,036.217 |
| C1 | 3.061347 | 32.622 | 5,344.720 | 186.728 | 11,039.280 |
| D1 | 7.967380 | 12.534 | 2,053.554 | 486.194 | 14,330.446 |
| D2 | 3.322130 | 30.063 | 4,925.444 | 202.655 | 11,458.556 |
| E1 | 7.936123 | 12.581 | 2,061.347 | 484.276 | 14,322.653 |
| E2 | 3.312984 | 30.147 | 4,939.359 | 202.102 | 11,444.641 |
| F1 | 5.400359 | 18.495 | 3,030.300 | 329.524 | 13,353.700 |
| F2 | 2.512177 | 39.763 | 6,514.832 | 153.222 | 9,869.168 |
| G1 | 2.516456 | 39.693 | 6,503.314 | 153.477 | 9,880.686 |
| T1 | 7.967045 | 12.533 | 2,053.382 | 486.165 | 14,330.618 |
| T2 | 5.345066 | 18.689 | 3,061.966 | 326.156 | 13,322.034 |
| H1 | 7.987633 | 12.503 | 2,048.525 | 487.436 | 14,335.475 |
| H2 | 7.648358 | 13.051 | 2,138.312 | 466.693 | 14,245.688 |

| Run | Host pages | GC pages | NAND pages | GC count | Host GiB |
| --- | --- | --- | --- | --- | --- |
| A1 | 56,301,836 | 335,651,350 | 391,953,186 | 23,916 | 214.774 |
| A2 | 109,656,200 | 223,717,548 | 333,373,748 | 20,328 | 418.305 |
| B1 | 134,217,728 | 823,252,518 | 957,470,246 | 58,433 | 512.000 |
| B2 | 134,217,728 | 276,335,831 | 410,553,559 | 25,039 | 512.000 |
| C1 | 159,814,972 | 329,434,190 | 489,249,162 | 29,842 | 609.646 |
| D1 | 59,441,268 | 414,149,890 | 473,591,158 | 28,900 | 226.750 |
| D2 | 141,644,864 | 328,917,854 | 470,562,718 | 28,705 | 540.332 |
| E1 | 59,781,972 | 414,655,135 | 474,437,107 | 28,951 | 228.050 |
| E2 | 147,148,488 | 340,352,166 | 487,500,654 | 29,739 | 561.327 |
| F1 | 88,600,636 | 389,874,613 | 478,475,249 | 29,196 | 337.985 |
| F2 | 195,989,740 | 296,371,108 | 492,360,848 | 30,030 | 747.642 |
| G1 | 196,205,604 | 297,537,107 | 493,742,711 | 30,113 | 748.465 |
| T1 | 57,276,852 | 399,050,391 | 456,327,243 | 27,846 | 218.494 |
| T2 | 88,810,168 | 385,886,031 | 474,696,199 | 28,966 | 338.784 |
| H1 | 59,029,232 | 412,474,610 | 471,503,842 | 28,773 | 225.179 |
| H2 | 62,087,968 | 412,783,050 | 474,871,018 | 28,976 | 236.847 |

표는 반올림 전 integer counter에서 직접 계산했다. 예전 요약의 반올림된 Avg copy를 다시 계산하는 방법과 마지막 소수자리에서 차이가 날 수 있다. 원본 `waf`/`average_gc_copy` 출력값과의 차이는 출력 반올림 범위 이내인지 검사했다.

## 부록 B. 원본 검증과 자료 한계

| Run | FEMU counter | fio | Manifest | Source hash |
| --- | --- | --- | --- | --- |
| A1 | PASS | RAW_JSON | PASS | PASS |
| A2 | PASS | RAW_JSON | PASS | PASS |
| B1 | PASS | RAW_JSON | PASS | PASS |
| B2 | PASS | RAW_JSON | PASS | PASS |
| C1 | PASS | RAW_JSON | PASS | PASS |
| D1 | PASS | RAW_JSON | PASS | PASS |
| D2 | PASS | RAW_JSON | PASS | PASS |
| E1 | PASS | RAW_JSON | PASS | PASS |
| E2 | PASS | RAW_JSON | PASS | PASS |
| F1 | PASS | RAW_JSON | PASS | PASS |
| F2 | PASS | RAW_JSON | PASS | PASS |
| G1 | PASS | RAW_JSON | PASS | PASS |
| T1 | PASS | ARCHIVED_SUMMARY_ONLY | MISSING; commit from archived summary | MISSING |
| T2 | PASS | RAW_JSON | PASS | PASS |
| H1 | PASS | RAW_JSON | PASS | PASS |
| H2 | PASS | RAW_JSON | PASS | PASS |
| L1 | PASS | LEGACY_TEXT | PASS | MISSING |
| L2 | PASS | LEGACY_TEXT | PASS | MISSING |

16개 본 실험과 2개 legacy 참조의 raw FEMU counter에서 `NAND=Host+GC`, `block_erases=64×GC count`, 출력 WAF/Avg copy와 재계산값 일치를 검증했다. V4의 Host/GC class 합계, ClassGC의 victim 수·copy 합계와 pool ownership 합계도 확인했다. 15개 본 실험은 fio JSON의 error=0, bytes=`Host pages×4096`, total_ios=`Host pages/4`, job 설정, runtime/512 GiB 조건을 확인했으며 source_hashes를 해당 commit의 `ftl.c/ftl.h/bb.c`와 대조했다.

T1은 `stats.txt`가 없어 남아 있는 `host_femu.log`의 마지막 BBSSD-STATS에서 media counter를 재확인했다. 이 로그에는 preconditioning 1,572,864 Host pages(6 GiB)와 최종 measurement counter가 별도로 남아 있다. 다만 현재 T1의 fio JSON, attempt manifest, source_hashes가 없어 fio 성능·정확한 실행 설정·빌드 commit은 이번 감사에서 직접 재확인하지 못했다. 성능과 commit 표기는 기존 summary에 따른 것이며, 이 누락을 전체 PASS로 덮지 않았다. 같은 ablation의 T2에는 원본 JSON/manifest/hash가 모두 남아 있어 threshold 비교의 직접 검증 근거를 보완한다.

L1/L2는 3600초 참조용 원본 FEMU 로그를 재확인했으며 fio는 legacy text 형식이다. 본 실험의 JSON 검증과 같은 수준의 자동검증으로 표시하지 않았다. 과거 SUMMARY 중 “모든 T1 원본 파일이 보존돼 있다”거나 marginal count 일치만으로 Hot의 경로를 단정하는 문장은 이 보고서의 검증 수준에 맞춰 제한했다.

| Run | 분포/방식 | 구현 | 원본 counter | fio JSON |
| --- | --- | --- | --- | --- |
| A1 | zipf:0.99 / 1800s | V4 Global | [원본](v4_validation_matrix_20260902_144410_KST/runs/A1_zipf099_1800_global/attempt_04/stats.txt) | [JSON](v4_validation_matrix_20260902_144410_KST/runs/A1_zipf099_1800_global/attempt_04/fio_raw.json) |
| A2 | zipf:0.99 / 1800s | V4 ClassGC | [원본](v4_validation_matrix_20260902_144410_KST/runs/A2_zipf099_1800_classgc/attempt_01/stats.txt) | [JSON](v4_validation_matrix_20260902_144410_KST/runs/A2_zipf099_1800_classgc/attempt_01/fio_raw.json) |
| B1 | zipf:0.99 / 512GiB | V4 Global | [원본](v4_validation_matrix_20260902_144410_KST/runs/B1_zipf099_fixed_global/attempt_01/stats.txt) | [JSON](v4_validation_matrix_20260902_144410_KST/runs/B1_zipf099_fixed_global/attempt_01/fio_raw.json) |
| B2 | zipf:0.99 / 512GiB | V4 ClassGC | [원본](v4_validation_matrix_20260902_144410_KST/runs/B2_zipf099_fixed_classgc/attempt_01/stats.txt) | [JSON](v4_validation_matrix_20260902_144410_KST/runs/B2_zipf099_fixed_classgc/attempt_01/fio_raw.json) |
| C1 | zipf:0.99 / 1800s | V4 ClassGC without age | [원본](v4_validation_matrix_20260902_144410_KST/runs/C1_zipf099_3600_classgc_noage/attempt_01/stats.txt) | [JSON](v4_validation_matrix_20260902_144410_KST/runs/C1_zipf099_3600_classgc_noage/attempt_01/fio_raw.json) |
| D1 | uniform / 1800s | V4 Global | [원본](v4_validation_matrix_20260902_144410_KST/runs/D1_uniform_1800_global/attempt_01/stats.txt) | [JSON](v4_validation_matrix_20260902_144410_KST/runs/D1_uniform_1800_global/attempt_01/fio_raw.json) |
| D2 | uniform / 1800s | V4 ClassGC | [원본](v4_validation_matrix_20260902_144410_KST/runs/D2_uniform_1800_classgc/attempt_01/stats.txt) | [JSON](v4_validation_matrix_20260902_144410_KST/runs/D2_uniform_1800_classgc/attempt_01/fio_raw.json) |
| E1 | zipf:0.7 / 1800s | V4 Global | [원본](v4_validation_matrix_20260902_144410_KST/runs/E1_zipf070_1800_global/attempt_01/stats.txt) | [JSON](v4_validation_matrix_20260902_144410_KST/runs/E1_zipf070_1800_global/attempt_01/fio_raw.json) |
| E2 | zipf:0.7 / 1800s | V4 ClassGC | [원본](v4_validation_matrix_20260902_144410_KST/runs/E2_zipf070_1800_classgc/attempt_01/stats.txt) | [JSON](v4_validation_matrix_20260902_144410_KST/runs/E2_zipf070_1800_classgc/attempt_01/fio_raw.json) |
| F1 | zipf:1.2 / 1800s | V4 Global | [원본](v4_validation_matrix_20260902_144410_KST/runs/F1_zipf120_1800_global/attempt_01/stats.txt) | [JSON](v4_validation_matrix_20260902_144410_KST/runs/F1_zipf120_1800_global/attempt_01/fio_raw.json) |
| F2 | zipf:1.2 / 1800s | V4 ClassGC | [원본](v4_validation_matrix_20260902_144410_KST/runs/F2_zipf120_1800_classgc/attempt_01/stats.txt) | [JSON](v4_validation_matrix_20260902_144410_KST/runs/F2_zipf120_1800_classgc/attempt_01/fio_raw.json) |
| G1 | zipf:1.2 / 1800s | V4 ClassGC without age | [원본](v4_validation_matrix_20260902_144410_KST/runs/G1_zipf120_1800_classgc_noage/attempt_01/stats.txt) | [JSON](v4_validation_matrix_20260902_144410_KST/runs/G1_zipf120_1800_classgc_noage/attempt_01/fio_raw.json) |
| T1 | uniform / 1800s | V4 ClassGC threshold-off | [원본](v4_classgc_threshold_off_20260903_001558_KST/runs/T1_uniform_1800_classgc_threshold_off/attempt_01/host_femu.log) | 없음† |
| T2 | zipf:1.2 / 1800s | V4 ClassGC threshold-off | [원본](v4_classgc_threshold_off_20260903_001558_KST/runs/T2_zipf120_1800_classgc_threshold_off/attempt_03/stats.txt) | [JSON](v4_classgc_threshold_off_20260903_001558_KST/runs/T2_zipf120_1800_classgc_threshold_off/attempt_03/fio_raw.json) |
| H1 | uniform / 1800s | Baseline | [원본](baseline_separation_skew_20260903_174617_KST/runs/H1_baseline_uniform_1800/attempt_01/stats.txt) | [JSON](baseline_separation_skew_20260903_174617_KST/runs/H1_baseline_uniform_1800/attempt_01/fio_raw.json) |
| H2 | zipf:1.2 / 1800s | Baseline | [원본](baseline_separation_skew_20260903_174617_KST/runs/H2_baseline_zipf120_1800/attempt_01/stats.txt) | [JSON](baseline_separation_skew_20260903_174617_KST/runs/H2_baseline_zipf120_1800/attempt_01/fio_raw.json) |

기존 3600초 원본: [Global](v4_global_vs_classgc_20260901_181151_KST/final_global/host_femu.log), [ClassGC](v4_global_vs_classgc_20260901_181151_KST/final_classgc/host_femu.log).

## 부록 C. 파일 활용 및 재생성

| 파일 | 용도 |
|---|---|
| [WAF_ALL_RUNS_20260908.csv](WAF_ALL_RUNS_20260908.csv) | 16개 run 원본 계수, normalized metric, 성능, class counter, provenance 통합 |
| [WAF_COMPARISONS_20260908.csv](WAF_COMPARISONS_20260908.csv) | 비교쌍별 절대차·상대차; 부호는 variant−reference |
| [WAF_REPORT_TABLES_20260908.md](WAF_REPORT_TABLES_20260908.md) | 문서에 옮기기 위한 묶음별 표 |
| [WAF_EVIDENCE_AUDIT_20260908.csv](WAF_EVIDENCE_AUDIT_20260908.csv) | 원본 경로, hash, 확인/누락 상태 |
| [WAF_LEGACY_3600_20260908.csv](WAF_LEGACY_3600_20260908.csv) | 기존 3600초 참조값 |
| [build_waf_analysis_20260908.py](build_waf_analysis_20260908.py) | 원본 parsing·무결성 검사·표/CSV/본문 재생성 |

```bash
cd /root/workspace/FEMU
python3 experiment_results/ftl_hotcold/build_waf_analysis_20260908.py
```

재생성은 결과 파일만 읽어 표와 CSV를 만들며 실험을 실행하지 않는다. 본문은 같은 폴더의 `WAF_REPORT_20260908.template.md`와 생성 표를 결합한다.
