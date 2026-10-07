# FEMU Blackbox FTL의 Hot/Cold Data Placement와 Class-Aware GC를 통한 Write Amplification 감소

**Advanced System Programming Final Project**



---

## 핵심 결과

Hot/Cold 분리 배치는 Baseline 대비 V3 실험에서 WAF를 약 **12.64%** 낮췄다.  
최종 V4에서는 동일한 classifier와 placement 조건에서 GC policy만 Class-aware 방식으로 변경했을 때 WAF가 **7.148 → 3.061**로 **57.18% 추가 감소**하였다.

---

## 요약

본 프로젝트는 FEMU Blackbox FTL에서 LBA의 접근 빈도와 erase 관련 정보를 이용해 Hot/Cold 데이터를 분류하고, 서로 다른 line pool에 분리 배치하여 Write Amplification Factor(WAF)를 줄이는 것을 목표로 한다. 초기에는 Host-write sequence 기반 update frequency를 이용해 데이터를 분류하였고, 이후 erase feedback과 LPN별 logical window를 도입해 분류 기준을 구체화하였다.

분리 배치만 적용한 이전 V3 실험에서는 Baseline WAF 7.990 대비 6.981으로 약 12.64% 감소하였다. 그러나 결과를 분석한 결과, Host는 Hot/Cold 데이터를 분리해 기록하는 반면 GC는 여전히 전체 victim line을 대상으로 Global Greedy를 수행하여 분류 정보를 공간 회수 단계에서 충분히 활용하지 못했다. 실제 최종 V4 Global control에서 GC victim의 평균 invalid 비율은 약 13.98%에 불과해, 한 번의 GC마다 평균 14,094개의 valid page를 다시 복사하고 있었다.

이에 Hot line은 Greedy, Cold line은 Host-write logical age × invalid ratio를 이용하는 Class-aware GC를 추가하였다. 동일한 1시간 workload에서 V4 Global Greedy의 WAF는 7.148, ClassGC는 3.061으로 57.18% 감소하였다. 이때 GC 1회당 평균 copy는 14,094 → 11,034 pages로 21.71% 감소했고, Host 100만 page write당 GC 횟수도 436.2 → 186.7로 감소했다.

> **보고서의 핵심 메시지**  
> Hot/Cold 분류의 효과는 데이터를 서로 다른 line에 배치하는 것에서 끝나지 않는다. 분류된 수명 특성을 GC victim selection까지 일관되게 활용해야 valid-page relocation을 실질적으로 줄이고 WAF 감소 효과를 크게 만들 수 있다.

---

# 1. Project Goal

NAND flash SSD는 overwrite를 직접 수행할 수 없기 때문에 갱신된 데이터는 새로운 physical page에 기록되고, 기존 page는 invalid 상태가 된다. 이후 Garbage Collection(GC)은 victim line의 valid page를 다른 위치로 복사한 후 block을 erase하여 공간을 회수한다. 이때 수명이 다른 데이터가 같은 line에 섞이면 Hot data의 반복 갱신으로 GC가 발생할 때 아직 유효한 Cold data까지 반복적으로 복사될 수 있다.

본 프로젝트의 목표는 다음과 같다.

- LBA 요청 빈도를 이용하여 Hot/Cold 데이터를 분류한다.
- erase 관련 feedback을 분류 신호로 활용한다.
- Hot/Cold line pool과 write pointer를 분리해 물리적으로 배치한다.
- Host/GC/NAND write를 계측하여 WAF를 비교한다.
- 분리 배치의 한계를 분석한 뒤 Hot/Cold 정보를 GC victim selection까지 확장한다.

```text
WAF = NAND page writes / Host page writes
    = 1 + GC page writes / Host page writes
```

---

# 2. Hot/Cold Classification Design

## 2.1 분류기 발전 과정

분류기는 단순한 rewrite interval에서 시작하여, erase feedback과 명시적인 logical window를 사용하는 방식으로 발전시켰다. 버전별 모든 실험을 나열하기보다, 최종 설계에 영향을 준 핵심 변화만 정리하면 다음과 같다.

**그림 1. Hot/Cold classifier의 핵심 발전 과정**

| 버전 | 핵심 아이디어 | 의의 | 다음 단계에서 보완한 점 |
|---|---|---|---|
| V1 | Host-write sequence 간격 | 구현이 단순하고 빈도 차이를 직접 반영 | 단일 interval만으로 장기적인 접근 특성을 표현하기 어려움 |
| V2 | 반복성·history 관리 방식 검토 | 일시적 rewrite보다 반복 접근을 보려는 중간 단계 | 누적 history의 유지 범위와 만료 기준을 더 명확히 할 필요 |
| V3 | Frequency + erase feedback | 빈도 경계 영역에서 erase 관련 신호 추가 | placement는 분리되지만 GC는 여전히 global policy |
| Final V4 | LPN별 logical window + actual erase event | frequency evidence의 유효 범위를 명확히 제한 | 최종 classifier로 사용 |

## 2.2 Final V4 classifier

최종 V4는 각 LPN마다 독립적인 logical window를 유지한다. Host의 4 KiB page write가 발생할 때마다 `host_write_seq`를 1 증가시키고, 해당 LPN의 현재 window에서 발생한 write 횟수(`writes_in_window`)를 센다.

이때 **시간 창은 wall-clock 시간이 아니라 Host page-write의 진행량을 기준으로 하는 logical window**이다.

| 항목 | 설정 |
|---|---:|
| Logical window | 4096 Host page writes |
| Low-frequency | `writes_in_window <= 1` → COLD |
| High-frequency | `writes_in_window >= 4` → HOT |
| Mid-frequency | 2~3 writes → erase-event feedback 사용 |
| Erase-event threshold | 1 |

```c
if (writes_in_window <= 1)
    state = COLD;
else if (writes_in_window >= 4)
    state = HOT;
else
    state = (erase_event_count >= 1) ? COLD : HOT;
```

erase-event feedback은 LPN의 현재 logical version이 GC relocation과 source block erase를 생존한 횟수를 의미한다. Host write로 새로운 version이 생성되면 다시 0부터 관찰한다. 따라서 단순한 전체 block erase 횟수와 달리 LPN별 lifetime 특성에 연결된 신호로 사용된다.

---

# 3. Hot/Cold Data Placement

분류 결과가 실제 NAND 배치에 영향을 주도록 non-FDP Blackbox FTL의 line 구조를 Hot/Cold 두 pool로 분리하고, 각 pool에 독립적인 write pointer를 두었다.

| I/O | Write pointer | Destination |
|---|---|---|
| Hot write | `wp_hot` | Hot line pool |
| Cold write | `wp_cold` | Cold line pool |
| GC relocation | 현재 LPN state에 따른 `wp_hot` / `wp_cold` | 분류 history는 갱신하지 않음 |

## 초기 Pool 구성

최종 V4 실험은 총 128 lines 중 **Hot 25 lines, Cold 103 lines(약 20:80)** 로 시작한다.

요청한 pool이 고갈되면 반대 pool의 free line을 borrowing하고 해당 line의 `data_class`를 실제 요청 class로 변경한다.

---

# 4. Placement-only 결과와 한계 발견

## 4.1 Hot/Cold 분리 배치의 효과

이전 V3 실험에서는 Hot/Cold를 분리하지 않은 Baseline의 WAF가 **7.990327**, frequency와 erase feedback을 이용한 V3 Hot/Cold FTL의 WAF가 **6.980748**으로 측정되어 약 **12.635% 감소**하였다.

즉, 수명이 다른 데이터를 서로 다른 line에 배치하는 것 자체가 GC overhead를 줄이는 효과가 있음을 확인하였다.

**그림 2. Baseline 대비 V3 Hot/Cold 분리 배치의 WAF 변화**

## 4.2 왜 개선폭은 제한적이었는가?

분류와 placement는 Hot/Cold를 구분했지만, 당시 GC victim selection은 기존 **Global Greedy**를 그대로 사용하였다. 즉 Host write는 데이터를 수명 특성에 맞게 분리했지만, 공간을 회수하는 GC는 Hot/Cold 정보를 직접 활용하지 않았다.

### 한계를 보여준 관찰

최종 V4의 Global Greedy control에서 line 하나는 **16,384 pages**인데 GC 1회당 평균 **14,094 pages**를 복사했다.

이는 victim line의 평균 invalid 비율이 약 **13.98%**이고, 반대로 약 **86.02%의 page가 여전히 valid인 상태에서 GC가 수행**됐다는 뜻이다.

따라서 단순히 데이터를 분리해 쓰는 것만으로는 충분하지 않고, Hot/Cold line의 서로 다른 invalidation 특성을 GC victim selection에도 반영해야 placement 효과를 실제 공간 회수 효율로 연결할 수 있다고 판단하였다.

---

# 5. Additional Optimization: Class-Aware GC

이 관찰을 바탕으로 최종 V4에서는 기존 classifier와 20:80 placement를 유지한 상태에서 GC victim policy만 class-aware 방식으로 확장하였다.

| Class | Candidate 조건 | Victim score | 설계 의도 |
|---|---|---|---|
| HOT | Normal: invalid >= 12.5% / Forced: `ipc > 0` | Greedy: `max(ipc)` | overwrite가 빠르므로 invalid가 많이 쌓인 line을 우선 회수 |
| COLD | Normal: invalid >= 30% / Forced: invalid >= 25% | `max(age × invalid ratio)` | 장수명 데이터를 너무 일찍 회수하지 않고 충분히 invalid가 쌓인 line 선택 |

## 5.1 Host-write logical age

`age`는 physical page의 마지막 program 시각이 아니라, 해당 line에 마지막 Host write가 기록된 이후 얼마나 많은 Host page writes가 진행되었는지를 나타낸다.

GC relocation은 Host access가 아니므로 `last_host_write_seq`를 갱신하지 않는다.

```c
age = (last_host_write_seq == 0)
      ? host_write_seq
      : host_write_seq - last_host_write_seq;

cold_score = age * ipc;
```

모든 line의 크기가 동일하므로 `age × ipc`의 순위는 `age × invalid_ratio`의 순위와 동일하다.

## 5.2 Pressure-aware class 선택

20:80처럼 pool 크기가 다른 경우 free line의 절대 개수를 비교하면 작은 Hot pool이 불리하다. 따라서 borrowing 이후 실제 line ownership을 기준으로 각 class의 사용 압력을 정규화하였다.

```text
Pressure = (Current total lines - Free lines) / Current total lines
```

Normal GC는 pressure가 높은 class의 normal selector를 우선 사용하고, 실패하면 반대 class의 normal selector를 사용한다.

Forced GC에서는 두 class의 forced selector가 모두 실패할 때에만 기존 Global Greedy를 emergency fallback으로 사용한다.

---

# 6. Implementation Summary

핵심 변경은 `hw/femu/bbssd/ftl.c`, `ftl.h`에 집중하였다. 전체 코드보다 과제와 직접 연결되는 세 가지 변경만 요약한다.

| 구현 영역 | 핵심 상태/함수 | 역할 |
|---|---|---|
| LPN classifier metadata | `window_start_seq`, `writes_in_window`, `erase_event_count`, `state` | LPN별 frequency와 erase-event 관리 |
| Hot/Cold placement | `wp_hot`, `wp_cold`, `line->data_class` | 분류 결과를 물리 line 배치로 연결 |
| Class-aware GC | `last_host_write_seq`, class selector, pressure selector | Hot/Cold 특성을 victim selection까지 연결 |

```c
/* Host write만 line age를 갱신 */
mark_page_valid(ssd, &ppa);
get_line(ssd, &ppa)->last_host_write_seq = ssd->host_write_seq;

/* GC relocation은 classifier/age를 갱신하지 않음 */
HOT  : candidate 중 max(ipc)
COLD : candidate 중 max(age * ipc)

/* preferred class는 normalized pool pressure로 결정 */
```

ClassGC 최종 구현은 다음 커밋을 기준으로 한다.

```text
c277fbcc99c016db265674cac7ce81c98addbdfc
```

Global control과의 비교에서는 classifier, pool ratio, borrowing, GC trigger, workload를 동일하게 유지하였다.

---

# 7. Experimental Setup

| 항목 | 설정 |
|---|---|
| FEMU mode | Blackbox |
| Raw NAND / Exposed | 8 GiB / 6 GiB |
| Over-Provisioning | 25% (raw capacity 기준) |
| NAND page | 4 KiB |
| Total lines | 128 |
| Initial Hot:Cold | 25:103 (약 20:80) |
| Preconditioning | 6 GiB sequential write 후 measurement reset |
| Final workload | fio randwrite, 5 GiB working set |
| Block size | 16 KiB |
| iodepth / numjobs | 128 / 32 |
| Distribution | Zipf 0.99 |
| Runtime | 3600 s |
| Seed | 20260824 |

## 7.1 최종 controlled comparison

| 시나리오 | 이름 | 구성 |
|---|---|---|
| Scenario 1 | V4 Global Greedy | V4 classifier + 20:80 placement + 기존 global greedy GC |
| Scenario 2 | V4 ClassGC | 동일 classifier/placement + Hot Greedy + Cold Cost-Benefit + pressure-aware class selection |

### 비교 해석 주의

V3의 12.64% 개선은 이전 placement-only 단계의 결과이며, 최종 V4와는 classifier/pool/reset 조건이 일부 다르다. 따라서 **V3 → ClassGC의 수치를 직접적인 feature ablation으로 해석하지 않는다.**

Class-aware GC의 효과는 동일한 V4 조건에서 Global Greedy와 ClassGC를 새로 실행한 controlled comparison으로 판단한다.

---

# 8. Results and Analysis

## 8.1 최종 WAF 비교

**그림 3. 동일 V4 조건에서 Global Greedy와 Class-aware GC의 WAF 비교**

V4 Global Greedy의 WAF는 **7.147977**, V4 ClassGC는 **3.060527**으로 측정되었다.

ClassGC는 동일한 classifier와 placement 조건에서 WAF를 **57.18% 감소**시켰다. Host write 한 page당 GC copy는 **6.148 pages에서 2.061 pages로 66.48% 감소**하였다.

| 지표 | V4 Global | V4 ClassGC | 변화/비고 |
|---|---:|---:|---|
| Host page writes | 133,084,356 | 320,201,272 | time-based workload이므로 절대량 직접 비교 주의 |
| GC page writes | 818,199,518 | 659,783,336 | -19.36% |
| WAF | 7.148 | 3.061 | -57.18% |
| GC count | 58,055 | 59,795 | +3.00% (절대값) |
| Avg GC copy | 14,094 | 11,034 | -21.71% |
| GC / 10^6 Host pages | 436.2 | 186.7 | -57.19% |
| IOPS | 9,240 | 약 22,200 | 참고 지표 |
| Avg latency | 443.24 ms | 184.22 ms | 참고 지표 |
| Counter invariant | PASS | PASS | 정상 |

## 8.2 왜 WAF가 크게 감소했는가?

WAF 감소는 단순히 GC 한 번의 비용만 줄어든 결과가 아니다. Host write당 GC 발생 빈도와 GC 1회당 valid-page copy가 동시에 감소하면서 GC amplification이 크게 낮아졌다.

**그림 4. GC 1회당 평균 valid-page copy**

**그림 5. Host write 기준 normalized GC 발생 빈도**

### 두 효과의 결합

```text
① GC 1회당 copy:
   14,094 → 11,034 pages
   (21.71% 감소)

② GC / 10^6 Host writes:
   436.2 → 186.7
   (57.19% 감소)

→ GC writes / Host writes:
  6.148 → 2.061
  (66.48% 감소)

→ WAF:
  7.148 → 3.061
```

## 8.3 Victim quality가 실제로 개선되었는가?

**그림 6. Global/ClassGC victim의 평균 invalid 비율**

Global Greedy victim의 평균 invalid 비율은 약 **13.98%**였다. 반면 ClassGC 전체 victim은 약 **32.65%**였고, 특히 Hot victim은 **76.55%**까지 높아졌다.

Hot data는 overwrite가 빠르게 누적되므로 `max(ipc)` Greedy가 invalid page가 많은 line을 효율적으로 선택했다.

Cold victim의 평균 invalid 비율은 **30.01%**로 설정한 normal threshold 30%와 거의 일치하였다. 이는 Cold line을 너무 일찍 회수하지 않고 invalid가 충분히 누적될 때까지 기다리도록 한 정책이 실제 victim selection에 반영되었음을 보여준다.

| Victim class | GC count | 비율 | Avg copy/GC | Avg invalid ratio |
|---|---:|---:|---:|---:|
| Hot victim | 3,396 | 5.68% | 3,841 | 76.55% |
| Cold victim | 56,399 | 94.32% | 11,467 | 30.01% |

**그림 7. 최종 WAF 감소의 인과관계**

## 8.4 Pressure, fallback, borrowing 관찰

| 항목 | 횟수/상태 | 해석 |
|---|---:|---|
| Opposite normal selection | 3,396 | preferred class에 normal victim이 없어 반대 class 선택 |
| Opposite forced selection | 0 | 발생하지 않음 |
| Global emergency fallback | 0 | Class selector만으로 progress 유지 |
| Foreground forced GC | 16 | 전체 GC 대비 매우 적음 |
| Borrowing | 18 | Hot 25/Cold 103 → Hot 7/Cold 121 |

ClassGC는 global emergency fallback 없이 정상적으로 진행되었다.

반면 Cold pool이 18회 고갈되어 Hot line을 borrowing했고, 최종 ownership은 **Hot 7 / Cold 121**로 변화하였다. 이는 초기 20:80이 고정 partition이 아니라 시작 비율이며, 해당 workload에서는 Cold 쪽 공간 수요가 더 컸음을 보여준다.

---

# 9. Discussion and Limitations

- 분리 배치의 효과와 ClassGC의 효과는 구분해서 해석해야 한다. V3의 약 12.64% 감소는 이전 placement-only 실험이고, 최종 ClassGC 57.18% 감소는 동일 V4 조건의 Global Greedy 대비 추가 GC 정책 효과이다.
- Final 실험은 seed `20260824`의 단일 1시간 run이다. 작은 차이에 대한 통계적 유의성이나 threshold/20:80 pool 비율의 최적성은 주장하지 않는다.
- ClassGC는 같은 1시간 동안 더 많은 Host writes를 처리했다. 따라서 absolute NAND writes나 block erase가 줄었다고 표현하지 않고, Host write당 GC copy와 normalized GC frequency를 기준으로 해석한다.
- IOPS와 latency도 개선되었지만 preconditioning 속도 차이가 관찰되어 host/KVM 환경 영향이 일부 섞였을 가능성이 있다. 따라서 정확한 성능 향상 배수는 반복 실험이 필요하다.
- Borrowing 후 Hot ownership이 7 lines까지 감소했으므로 workload에 따른 동적 pool 변화와 최적 pool ratio는 후속 분석 대상으로 남는다.

---

# 10. Conclusion

본 프로젝트는 FEMU Blackbox FTL에 Host-write frequency와 erase-event feedback 기반 Hot/Cold classifier를 구현하고, Hot/Cold 데이터를 서로 다른 line pool에 분리 배치하였다. 이전 placement-only 단계에서는 Baseline 대비 WAF가 약 12.64% 감소하여 lifetime separation의 효과를 확인하였다.

그러나 Global Greedy GC에서는 victim의 평균 invalid 비율이 약 13.98%에 머물러 valid-page copy가 여전히 컸다. 이에 Hot/Cold 특성을 victim selection까지 확장한 Class-aware GC를 적용하였다.

최종 controlled experiment에서 WAF는 **7.148 → 3.061**으로 **57.18% 감소**했고, GC 1회당 copy와 Host write당 GC 빈도도 함께 감소하였다.

> **최종 결론**  
> Hot/Cold 분류의 효과를 극대화하려면 데이터를 분리하여 기록하는 것뿐 아니라, 분류된 수명 특성을 GC 공간 회수 정책까지 일관되게 활용하는 것이 중요하다.

---

# References / Reproducibility

- FEMU Hot/Cold FTL repository: https://github.com/thecoldestm0ment/FEMU-hotcold
- Final ClassGC commit: `c277fbcc99c016db265674cac7ce81c98addbdfc`
- V3 historical results: `README_HOTCOLD.md` (Baseline/V1/V3 WAF and experiment configuration)
- Final raw log path: ________________________________
- Global final result directory: ________________________________
- ClassGC final result directory: ________________________________
