# FEMU Blackbox FTL Hot/Cold 분류기 단계별 실험 계획서

## 1. 실험 목적

본 실험의 목적은 FEMU Blackbox FTL에서 **Host hint 없이 LPN의 write history만으로 Hot/Cold 데이터를 구분**하고, 분류 방법을 단계적으로 개선했을 때 데이터 lifetime mixing과 Write Amplification이 어떻게 달라지는지 확인하는 것이다.

현재 Hot/Cold line pool, dual write pointer, Host write routing, GC relocation routing까지 구현된 상태이므로 이후에는 **placement 구조는 고정하고 Hot/Cold classifier만 단계적으로 변경**한다.

실험은 다음 세 단계로 진행한다.

| 버전       | 분류 방법                                        | 확인하려는 문제                                             |
| -------- | -------------------------------------------- | ---------------------------------------------------- |
| **V1**   | 마지막 rewrite interval 하나                      | 가장 단순한 Hot/Cold 분류만으로 효과가 있는가?                       |
| **V2**   | Frequency + rewrite interval + 반복 확인 + decay | 한 번의 rewrite만 보고 Hot으로 오판하는 문제를 줄일 수 있는가?            |
| **V2.5** | V2 + placement 시점 idle expiration            | 과거 Hot이었지만 현재 식은 데이터를 GC가 다시 Hot으로 옮기는 문제를 막을 수 있는가? |

---

# 2. 모든 버전에서 고정해야 할 조건

V1, V2, V2.5의 차이는 **Hot/Cold 판단 방식만**이어야 한다.

따라서 다음 조건은 동일하게 유지한다.

* SSD geometry / OP
* Hot/Cold line pool 비율
* GC threshold
* GC victim selection policy
* FIO workload
* block size
* Zipf parameter
* queue depth / job 수
* runtime
* preconditioning 방식
* random seed
* 통계 reset 시점

특히 **V2나 V2.5에서 GC policy까지 같이 변경하지 않는다.**

예를 들어

```text
V1
Global Greedy GC

V2
Hot Greedy + Cold Cost-Benefit GC
```

처럼 바꾸면 WAF가 개선되어도

```text
Classifier가 좋아져서?
GC policy가 좋아져서?
```

를 구분할 수 없다.

따라서 이번 실험에서는 **분류 정책만 변경한다.**

---

# 3. 공통 평가 지표

## 3.1 WAF — 가장 중요한 최종 지표

```text
WAF =
NAND Page Writes
───────────────
Host Page Writes
```

의미:

> Host가 1 page를 쓸 때 실제 NAND에서는 평균 몇 page가 write되었는가?

예:

```text
Host writes = 100
GC copy     = 50

NAND writes = 150

WAF = 1.5
```

Hot/Cold separation의 최종 목표는 WAF 감소이므로 **가장 중요한 결과 지표**다.

---

## 3.2 GC Page Writes

```text
GC Page Writes / Host Page Writes
```

Hot/Cold separation의 효과를 WAF보다 직접적으로 보여준다.

Hot/Cold가 잘 분리되면 같은 line의 page들이 비슷한 시기에 invalid되므로 GC할 때 살아 있는 valid page가 적어진다.

따라서 이상적인 결과는:

```text
Hot/Cold 분리 개선
        ↓
Victim line의 valid page 감소
        ↓
GC copy 감소
        ↓
NAND writes 감소
        ↓
WAF 감소
```

이다.

---

## 3.3 GC Count

몇 번 GC가 발생했는지를 본다.

단, **GC Count만으로 성능을 판단하면 안 된다.**

예를 들어:

```text
A
GC 100회 × 평균 5 page copy
= 500 copies

B
GC 70회 × 평균 20 page copy
= 1400 copies
```

라면 GC 횟수는 B가 적지만 WAF는 더 나쁠 수 있다.

따라서 반드시 `GC Count`와 `GC Page Writes`를 같이 본다.

---

## 3.4 Valid Pages Copied per GC

가능하면 추가하는 것을 추천한다.

```text
Average GC Copy
=
GC Page Writes / GC Count
```

이 값은 **line 내부 lifetime mixing이 얼마나 발생했는지**를 보여주는 좋은 지표다.

값이 작다면:

> GC 시점에 victim line의 데이터가 대부분 함께 invalid되어 있었다.

라고 해석할 수 있다.

즉 classifier 개선 효과를 설명하기 가장 좋은 내부 지표 중 하나다.

---

## 3.5 Hot Write Ratio

```text
Hot으로 분류된 Host Page Writes
──────────────────────────
전체 Host Page Writes
```

이 지표가 매우 중요하다.

왜냐하면 threshold를 바꿨을 때 실제로 classifier가 어떻게 반응했는지 확인할 수 있기 때문이다.

예:

```text
T = 64
Hot Write Ratio = 5%

T = 1024
Hot Write Ratio = 35%

T = 4096
Hot Write Ratio = 72%
```

이렇게 나오면 threshold가 커질수록 Hot 판정이 느슨해진다는 것을 실제 실험으로 확인할 수 있다.

---

## 3.6 Hot ↔ Cold 전이 횟수

V2부터 추가하는 것을 추천한다.

```text
Cold → Hot promotion count
Hot → Cold demotion count
```

이 값은 classifier가 얼마나 안정적인지 보여준다.

너무 많다면:

```text
HOT
COLD
HOT
COLD
HOT
...
```

처럼 classification이 흔들리고 있을 가능성이 있다.

---

# 4. V1 — Latest Rewrite Interval Baseline

## 4.1 목적

가장 단순한 질문부터 확인한다.

> 최근에 빠르게 overwrite된 LPN을 Hot이라고 보는 것만으로 WAF를 줄일 수 있는가?

현재 V1은 다음과 같다.

```text
첫 write
→ Cold

rewrite 발생

update_interval
= current_host_write_seq
- last_write_seq

interval <= T
→ Hot

interval > T
→ Cold
```

현재 초기값:

```text
T = 1024 page writes
```

4 KiB page 기준 약 4 MiB의 logical write distance이다.

---

# 4.2 V1에서 반드시 확인할 것

### ① 실제 Hot/Cold routing이 발생하는가?

확인:

```text
Hot Host Writes
Cold Host Writes
```

둘 다 존재해야 한다.

만약:

```text
Hot = 0%
```

이면 threshold가 지나치게 작거나 classifier가 잘못 동작하는 것이다.

반대로:

```text
Hot = 95%
```

이면 사실상 Hot/Cold separation이 없는 것과 비슷해질 수 있다.

---

### ② Hot line과 Cold line이 실제 분리되는가?

확인:

```text
wp_hot.curline != wp_cold.curline
```

그리고 Hot write가 Cold line으로 들어가거나 반대 상황이 없는지 확인한다.

---

### ③ GC relocation도 동일한 class로 이동하는가?

Host write만 분리해도 GC가 모든 valid page를 하나의 write pointer로 다시 쓰면 lifetime mixing이 재발한다.

따라서:

```text
GC source LPN
    ↓
Hot/Cold 조회
    ↓
wp_hot / wp_cold
```

가 정상 동작해야 한다.

---

# 4.3 V1 Threshold 실험

V1에서는 `T` 하나만 바꿔본다.

예:

```text
T = 64
T = 256
T = 1024
T = 4096
```

4 KiB 기준:

|    T | Host write distance |
| ---: | ------------------: |
|   64 |             256 KiB |
|  256 |               1 MiB |
| 1024 |               4 MiB |
| 4096 |              16 MiB |

### T를 작게 하면

```text
HOT 판정 엄격
↓
정말 자주 rewrite되는 LPN만 Hot
↓
Hot Write Ratio 감소
```

장점:

* false Hot 감소

단점:

* 실제 Hot을 Cold로 놓칠 가능성 증가

즉 **precision을 높이고 recall을 낮추는 방향**이라고 생각하면 쉽다.

### T를 크게 하면

```text
HOT 판정 느슨
↓
많은 LPN이 Hot
↓
Hot Write Ratio 증가
```

장점:

* Hot을 놓칠 가능성 감소

단점:

* Warm/Cold까지 Hot으로 오분류 가능

따라서 이 실험은:

> **Hot과 Cold의 경계를 어디에 둘 때 lifetime separation이 가장 잘 되는가?**

를 확인하는 실험이다.

---

# 5. V2 — Frequency + Recency + Repeated Confirmation

## 5.1 V2를 만드는 이유

V1의 가장 큰 문제는 **한 번의 빠른 rewrite만으로 Hot이 된다는 것**이다.

예:

```text
A write

↓ 100 writes

A rewrite

→ HOT
```

그 뒤 다시 거의 쓰이지 않는 데이터라도 순간적으로 Hot이 된다.

V2에서는 Hot 승격 조건을 조금 더 보수적으로 만든다.

---

# 5.2 V2 기본 조건

초기 구조는 다음과 같이 잡는다.

```text
access_count >= A

AND

update_interval <= T

AND

short_interval_count >= C
```

초기값은 다음 정도로 시작한다.

```text
A = 3
C = 2
```

`T`는 **처음 V1과 비교할 때는 1024를 그대로 사용한다.**

이게 중요하다.

V1:

```text
T = 1024
```

V2:

```text
A = 3
T = 1024
C = 2
```

로 비교해야

> V2가 좋아진 이유가 frequency와 repeated confirmation 때문인지

볼 수 있다.

처음부터 V2의 `T`까지 64로 바꾸면

```text
알고리즘 때문인지?
threshold 때문인지?
```

알 수 없어진다.

---

# 5.3 V2에서 추가할 metadata

LPN마다 다음 값이 필요하다.

```text
state

access_count

last_write_seq

short_interval_count
```

의미:

### `access_count`

최근 history에서 해당 LPN이 얼마나 자주 쓰였는가.

### `last_write_seq`

마지막 write가 언제 발생했는가.

### `short_interval_count`

빠른 rewrite가 우연히 한 번 발생한 것이 아니라 **반복되는 패턴인지** 확인한다.

---

# 5.4 V2 Hot 승격

예:

```text
A = 3
T = 1024
C = 2
```

라면:

```text
첫 write
access = 1
COLD

↓ 빠른 rewrite

access = 2
short = 1
COLD

↓ 빠른 rewrite

access = 3
short = 2

→ HOT
```

즉 V1보다 훨씬 보수적으로 Hot이 된다.

---

# 5.5 History Decay

누적 `access_count`를 영원히 유지하면 문제가 생긴다.

예:

```text
과거
A가 100회 write

현재
A는 거의 write되지 않음

access_count = 100
```

이면 과거 정보가 영원히 영향을 준다.

따라서 일정 host write가 지나면:

```text
access_count >>= 1
short_interval_count >>= 1
```

과 같이 오래된 history의 영향력을 감소시킨다.

초기 구현에서는 단순하게 periodic decay를 사용해도 된다.

---

# 5.6 V2에서 수정할 위치

## ① LPN metadata

`struct ssd` 또는 Hot/Cold metadata 영역에:

```text
lpn_access_count[]
lpn_last_write_seq[]
lpn_short_interval_count[]
lpn_state[]
last_decay_seq
```

를 관리한다.

---

## ② 초기화

SSD 초기화 시 모든 LPN:

```text
state = COLD
access_count = 0
last_write_seq = 0
short_interval_count = 0
```

으로 초기화한다.

---

## ③ `ssd_write()`

Host write가 발생했을 때만 classification history를 학습한다.

순서:

```text
host_write_seq 증가

↓
필요하면 decay

↓
LPN write history 업데이트

↓
Hot/Cold 판단

↓
해당 write pointer 선택
```

---

## ④ `gc_write_page()`

여기서는 **access_count를 증가시키면 안 된다.**

GC copy는 Host workload가 아니기 때문이다.

따라서:

```text
GC
↓
LPN state 조회만 함
↓
Hot / Cold placement
```

으로 유지한다.

---

## ⑤ TRIM

TRIM된 LPN은 논리 데이터 lifetime이 끝난 것이므로 metadata도 초기화하는 것이 자연스럽다.

```text
state = COLD
access_count = 0
short_interval_count = 0
last_write_seq = 0
```

그러면 TRIM 이후 새로운 write는 다시 first write처럼 처리할 수 있다.

---

# 5.7 V2 파라미터별 의미

## `T` — Short Update Interval

```text
update_interval <= T
```

T를 줄이면:

> 정말 빠르게 rewrite되는 데이터만 Hot 후보로 인정

T를 늘리면:

> 더 긴 interval의 데이터까지 Hot 후보로 인정

즉 **Hot lifetime 경계 실험**이다.

---

## `A` — Access Threshold

```text
access_count >= A
```

A를 늘리면:

> 충분히 자주 관찰된 데이터만 Hot으로 인정

예:

```text
A = 2
A = 3
A = 5
```

이 실험은:

> **몇 번 정도 반복되어야 이 LPN의 write pattern을 믿을 수 있는가?**

를 확인한다.

---

## `C` — Confirmation Count

```text
short_interval_count >= C
```

C가 작으면 빠르게 Hot으로 승격한다.

C가 크면 여러 번 반복되는 패턴을 확인한 뒤 승격한다.

따라서 이 실험은:

> **빠른 적응과 잘못된 Hot 판정 사이의 trade-off**

를 본다.

---

## `D` — Decay Window

D가 작으면:

```text
과거 history 빨리 사라짐
→ workload 변화에 빠르게 적응
```

하지만 너무 작으면 실제 Hot data의 history도 빨리 사라진다.

D가 크면:

```text
classification 안정성 증가
```

하지만 workload가 변했을 때 stale history가 오래 남는다.

즉:

> **classifier가 workload 변화에 얼마나 빠르게 적응할 것인가**

를 결정한다.

---

# 6. V2.5 — Placement-Time Idle Expiration

## 6.1 추가하는 이유

V2에도 한 가지 문제가 남는다.

예:

```text
A가 반복적으로 빠르게 rewrite됨
→ HOT

그 이후 100,000 page write 동안
A에는 write가 없음
```

periodic decay 때문에:

```text
access_count → 0
short_interval_count → 0
```

가 되더라도 저장된:

```text
state = HOT
```

은 그대로 남아 있을 수 있다.

그리고 그 사이 GC가 A를 이동하면:

```text
state == HOT
↓
Hot line으로 relocation
```

할 수 있다.

즉 **과거에는 Hot이었지만 현재는 Cold에 가까운 stale-Hot data**가 Hot line에 섞인다.

---

# 6.2 V2.5 핵심 아이디어

저장된 state와 실제 placement state를 분리한다.

```text
Historical State

HOT / COLD
     ↓
현재 idle age 확인
     ↓
Effective State

HOT / COLD
```

계산:

```text
idle_age
=
current_host_write_seq
-
last_write_seq
```

그리고:

```text
historical_state == HOT

AND

idle_age <= E

→ Effective HOT
```

그렇지 않으면:

```text
Effective COLD
```

로 placement한다.

---

# 6.3 중요한 설계 원칙

GC가 이 검사를 수행하더라도 **LPN의 학습 history 자체를 변경하지 않는다.**

즉 GC에서:

```text
state = COLD
```

로 강제로 바꾸는 것이 아니라,

```text
historical state = HOT

하지만 지금 placement만 COLD
```

로 판단한다.

이렇게 해야:

```text
Host workload observation
```

과

```text
GC placement decision
```

의 역할이 분리된다.

---

# 6.4 V2.5에서 수정할 위치

V2의 구조는 그대로 둔다.

새로운 helper 개념만 추가한다.

예:

```text
is_lpn_hot_for_placement()
```

이 함수가:

```text
state 확인

+
current_seq - last_write_seq 확인
```

을 수행한다.

### Host write path

V2 classification update 후 effective state를 조회한다.

Host write가 발생했기 때문에 `last_write_seq`가 갱신되므로 최근 Hot data는 자연스럽게 Hot으로 들어간다.

### GC path

V2.5의 효과가 가장 크게 나타나는 위치다.

```text
GC victim valid page
↓
rmap → LPN
↓
historical state 확인
↓
idle age 계산
↓
effective Hot / Cold
↓
새 line으로 relocation
```

---

# 6.5 Expiration Window `E`

처음에는:

```text
E = 4 × T
```

정도로 두는 것이 비교하기 쉽다.

예:

```text
T = 1024

E = 4096
```

그리고 이후:

```text
E = 2T
E = 4T
E = 8T
```

를 비교한다.

### E가 작으면

Hot data가 빨리 Cold 취급된다.

장점:

* stale-Hot 감소

단점:

* 잠시 idle한 Hot data까지 Cold로 잘못 보낼 수 있음

### E가 크면

Hot state를 오래 신뢰한다.

장점:

* Hot data의 일시적인 idle에 덜 민감

단점:

* stale-Hot이 Hot line에 오래 남음

즉 E 실험은:

> **“과거 Hot이라는 정보를 얼마나 오래 신뢰할 것인가?”**

를 확인하는 실험이다.

---

# 7. V2.5에서 반드시 추가할 지표

## Stale-Hot GC Relocation Count

다음 경우를 센다.

```text
historical state = HOT

하지만

idle_age > E

따라서 GC에서 Cold placement
```

예:

```text
stale_hot_gc_demotions++
```

이 지표가 중요한 이유는 V2.5가 실제로 무엇을 바꿨는지 직접 보여주기 때문이다.

예:

```text
V2.5

GC relocated pages = 1,000,000

그중 stale-Hot → Cold
= 180,000 pages
```

라면:

> V2에서는 Hot으로 다시 들어갔을 18%의 GC page를 V2.5가 Cold로 보냈다.

라고 설명할 수 있다.

그 결과:

```text
GC copy ↓
WAF ↓
```

까지 이어지면 설계 효과를 매우 명확하게 설명할 수 있다.

---

# 8. 추천 실험 순서

## Experiment 0 — Correctness

성능을 보기 전에 동작을 확인한다.

확인:

```text
Hot / Cold 모두 발생

wp_hot ≠ wp_cold

mapping / rmap 정상

GC 후 mapping 정상

Hot/Cold free-line count 정상

Host write와 GC write counter 분리
```

---

# Experiment 1 — V1 Baseline

현재 구현 그대로:

```text
T = 1024
```

측정:

```text
WAF
GC Writes
GC Count
Average GC Copy
Hot Write Ratio
```

먼저 이 결과를 기준점으로 저장한다.

---

# Experiment 2 — V1 Threshold Sensitivity

```text
T = 64
256
1024
4096
```

비교:

```text
T
↓
Hot Write Ratio
↓
GC Copy
↓
WAF
```

목적:

> Hot 판정 범위를 넓히거나 좁히는 것이 실제 placement에 어떻게 영향을 주는가?

---

# Experiment 3 — V2 Algorithm Comparison

우선 V1과 같은:

```text
T = 1024
```

를 유지한다.

그리고:

```text
A = 3
C = 2
Decay = ON
```

만 추가한다.

비교:

```text
V1
latest interval

vs

V2
frequency
+ interval
+ repeated confirmation
+ decay
```

여기서는 **알고리즘 자체의 차이**를 본다.

---

# Experiment 4 — V2 Parameter Sensitivity

알고리즘이 정상 동작하는 것이 확인된 후 하나씩 바꾼다.

예:

```text
T
64 / 256 / 1024

A
2 / 3 / 5

C
1 / 2 / 3
```

처음부터 모든 조합을 돌리기보다는 **One-Factor-at-a-Time** 방식으로 시작한다.

예:

```text
A=3, C=2 고정
→ T 실험

가장 적절한 T 고정
→ A 실험

T, A 고정
→ C 실험
```

그래야 각 값이 어떤 역할을 하는지 이해하기 쉽다.

---

# Experiment 5 — V2.5

V2에서 선택한 동일한:

```text
T
A
C
Decay
```

를 그대로 사용한다.

유일하게:

```text
Idle Expiration
```

만 추가한다.

초기:

```text
E = 4T
```

비교:

```text
V2
vs
V2.5
```

중점 지표:

```text
Stale-Hot GC Demotions
Average GC Copy
GC Writes
WAF
```

---

# Experiment 6 — Expiration Sensitivity

```text
E = 2T
4T
8T
```

를 비교한다.

확인:

```text
E ↓
→ stale-hot demotion ↑

하지만
너무 많은 실제 Hot까지 Cold로 보내지는 않는가?
```

를 본다.

---

# 9. 결과 해석 예시

## Case A

```text
V1 WAF = 2.4
V2 WAF = 2.0

GC copy도 감소
Hot Write Ratio도 적당히 감소
```

해석:

> 단순한 한 번의 short rewrite 대신 반복적인 write pattern을 확인함으로써 false-Hot placement가 감소했고, lifetime mixing 감소로 GC copy가 줄어든 것으로 볼 수 있다.

---

## Case B

```text
V2 WAF = 2.0
V2.5 WAF = 1.8

stale_hot_gc_demotions 많음
GC copy 감소
```

해석:

> V2에서는 과거 Hot 상태가 GC relocation 시 그대로 사용됐지만, V2.5에서는 오랫동안 rewrite되지 않은 Hot LPN을 Cold placement하여 stale-Hot mixing을 감소시켰다.

---

## Case C

```text
V2.5에서 E를 너무 작게 설정

Hot Write Ratio 급감
WAF 오히려 증가
```

해석:

> stale-Hot은 줄었지만 일시적으로 idle한 실제 Hot data까지 Cold line으로 배치되어 Hot/Cold lifetime mixing이 다시 증가한 것으로 볼 수 있다.

즉 threshold는 작을수록 좋은 것이 아니다.

---

# 10. 이번 실험에서 가장 중요한 그래프

최종 보고서에서는 다음 네 가지가 가장 중요하다.

### ① Version별 WAF

```text
Baseline FTL
V1
V2
V2.5
```

최종 효과.

### ② Version별 GC Page Writes

WAF가 왜 변했는지 보여준다.

### ③ Average Valid Page Copies per GC

Hot/Cold lifetime separation이 실제 GC 내부에서 개선됐는지 보여준다.

### ④ Parameter vs Hot Write Ratio / WAF

예:

```text
X축 = T
왼쪽 Y축 = WAF
오른쪽 Y축 = Hot Write Ratio
```

이 그래프를 보면:

> threshold 변화 → classifier 행동 변화 → WAF 변화

라는 인과관계를 설명할 수 있다.

---

# 11. 추가로 기록하면 좋은 Debug / Analysis Counter

```text
host_hot_writes
host_cold_writes

cold_to_hot_count
hot_to_cold_count

gc_hot_writes
gc_cold_writes

gc_count
gc_page_writes

stale_hot_gc_demotions

hot_pool_empty_count
cold_pool_empty_count

borrow_count
emergency_gc_count
```

특히 pool 고갈 관련 counter가 중요하다.

만약 V2의 WAF가 나빠졌는데:

```text
Hot pool empty 급증
Emergency GC 급증
```

했다면 classifier 자체보다 **pool 비율과 분류 비율이 맞지 않는 문제**일 수 있기 때문이다.

---

# 12. 지표별로 답할 수 있는 질문

| 지표                       | 이 값으로 답하는 질문                    |
| ------------------------ | ------------------------------- |
| WAF                      | 최종적으로 NAND write가 줄었는가?         |
| GC Page Writes           | 불필요한 valid-page copy가 줄었는가?     |
| GC Count                 | GC 자체가 얼마나 자주 발생했는가?            |
| Copy / GC                | 한 번 GC할 때 line이 얼마나 깨끗해졌는가?     |
| Hot Write Ratio          | classifier가 얼마나 공격적으로 Hot을 잡는가? |
| C→H / H→C                | classifier가 안정적인가, 흔들리는가?       |
| Stale-Hot Demotion       | V2.5 expiration이 실제로 작동했는가?     |
| Pool Empty Count         | classifier와 pool 크기가 맞지 않는가?    |
| Erase Count Distribution | 특정 physical block에 wear가 집중되는가? |

`erase_count`는 Hot/Cold classifier 정확도를 보여주는 지표라기보다 **wear distribution을 확인하는 보조 지표**로 해석한다.

---

# 13. 전체 실험 흐름

```text
[Baseline FTL]
Hot/Cold 없음
       │
       ▼
[V1]
Latest Rewrite Interval
       │
       │ 문제:
       │ 한 번의 빠른 rewrite로도 Hot
       ▼
[V2]
Frequency
+ Rewrite Interval
+ Repeated Confirmation
+ History Decay
       │
       │ 문제:
       │ 과거 Hot state가 GC까지 남을 수 있음
       ▼
[V2.5]
V2
+ Placement-Time Idle Expiration
       │
       ▼
Stale-Hot 감소
       │
       ▼
Lifetime Mixing 감소?
       │
       ▼
GC Valid Copy 감소?
       │
       ▼
WAF 감소?
```

이 인과관계를 실험으로 하나씩 확인하는 것이 본 프로젝트의 핵심이다.

---

# 14. 구현 및 실험 추천 순서

현재 Phase 6의 V1이 완성되어 있으므로 바로 V2로 넘어가기 전에 먼저 다음 순서로 진행한다.

**1단계 — V1 correctness 확인**

Hot/Cold write 수, GC routing, mapping consistency를 검증한다.

**2단계 — V1 기준 결과 확보**

현재 `T=1024` 결과를 저장한다.

**3단계 — V1 threshold sweep**

64 / 256 / 1024 / 4096를 비교한다.

**4단계 — V2 구현**

기존 line pool이나 GC는 건드리지 않고 LPN classifier metadata와 host-write update 부분만 확장한다.

**5단계 — V1과 동일한 T로 V2 비교**

먼저 classifier 구조 자체의 효과를 확인한다.

**6단계 — V2 parameter sensitivity**

T → A → C 순으로 하나씩 변화시킨다.

**7단계 — V2.5 구현**

기존 V2 학습 로직은 그대로 두고 placement-time effective classification만 추가한다.

**8단계 — V2 vs V2.5 비교**

특히 `stale_hot_gc_demotions → GC copy → WAF` 관계를 확인한다.

---

## 최종 연구 질문

이 실험을 모두 끝내면 다음 세 질문에 답할 수 있어야 한다.

**RQ1.** 단순한 latest rewrite interval만으로도 Hot/Cold separation을 통해 WAF를 줄일 수 있는가?

**RQ2.** Frequency와 반복적인 short rewrite를 함께 고려하면 단순 interval classifier보다 lifetime separation이 개선되는가?

**RQ3.** GC relocation 시 stale-Hot 데이터를 현재 idle age로 재평가하면 valid-page copy와 WAF를 추가로 줄일 수 있는가?

이 세 질문을 중심으로 결과를 정리하면 단순 구현 결과가 아니라 **classifier가 왜 단계적으로 개선되었는지를 설명할 수 있는 실험 보고서**가 된다.

## 명령어 가이드
빌드 하는법
```bash
modprobe kvm
modprobe kvm_intel

lsmod | grep kvm
ls -l /dev/kvm

cd /root/workspace/FEMU/build-femu
./run-blackbox.sh
```

Guest 부팅 후 로그인:  (id: femu, pw: femu)

