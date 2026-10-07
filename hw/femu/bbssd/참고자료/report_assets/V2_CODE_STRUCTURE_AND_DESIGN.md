# FEMU BBSSD Hot/Cold V2 코드 구조와 설계 해설

## 1. 문서 범위

이 문서는 `hotcold/v2` 브랜치의 현재 코드와 correctness 결과를 기준으로 V2의 구조, 세부 동작, 설계 이유, 그리고 각 설계가 실험 지표에 어떤 영향을 주도록 의도되었는지를 설명한다.

- 문서 기준 브랜치: `hotcold/v2`
- 문서 기준 HEAD: `dc56f9b7b` (`v2: record classifier correctness validation`)
- V2 구현 커밋: `1efe61f24` (`ftl: implement V2 frequency classifier with lazy decay`)
- 설정 오류 전파 보정 커밋: `ffd7c8a70`
- V2 correctness 결과 커밋: `dc56f9b7b`
- 대상: non-FDP BBSSD 경로

V2의 300초 성능 실험은 아직 실행하지 않았다. 따라서 이 문서에서 말하는 “이점”은 코드가 목표로 하는 메커니즘과 지표상 기대 방향이며, 실제 WAF 개선을 확정하는 결과가 아니다. 현재 확인된 것은 분류, decay, TRIM, GC read-only 동작과 counter 불변식의 correctness이다.

---

## 2. V2를 한 문장으로 요약하면

V2는 각 LPN의 **누적 접근 횟수**, **짧은 간격의 반복 횟수**, **가장 최근 rewrite 간격**을 함께 사용하고 오래된 이력을 lazy decay로 약화하여, 일시적으로 다시 쓰인 데이터보다 지속적으로 자주 다시 쓰이는 데이터를 HOT으로 분류하는 FTL이다.

분류된 데이터는 V1에서 만든 두 개의 write pointer와 Hot/Cold line pool로 분리 배치한다.

```text
Host write
    │
    ├─ host_write_seq 증가
    ├─ 해당 LPN의 오래된 이력 lazy decay
    ├─ access/interval/short history 갱신
    ├─ HOT 또는 COLD 판정
    └─ HOT WP 또는 COLD WP로 기록

GC relocation
    │
    ├─ 저장된 HOT/COLD state만 읽음
    ├─ 분류 이력은 변경하지 않음
    └─ 같은 class의 WP로 valid page 이동

TRIM
    │
    ├─ mapping 해제
    └─ 해당 LPN의 분류 이력을 UNSEEN으로 초기화
```

핵심은 다음 세 가지 경계를 분리한 것이다.

1. **학습은 host write만 한다.**
2. **GC는 학습하지 않고 저장된 분류만 사용한다.**
3. **TRIM은 한 LPN의 논리적 수명이 끝났다고 보고 모든 이력을 지운다.**

이 경계를 지켜야 내부 데이터 이동인 GC가 “자주 쓰이는 데이터”로 잘못 학습되는 자기강화 오류를 막을 수 있다.

---

## 3. Base, V1, V2의 관계

V2는 무분리 baseline에 바로 frequency classifier만 붙인 구조가 아니다. `hotcold/v1`의 배치 구조를 그대로 상속하고 classifier를 확장했다.

| 단계 | 역할 |
|---|---|
| Corrected baseline | Hot/Cold 분리 없이 공통 WAF, GC 횟수, 평균 GC 복사량을 측정하는 기준점 |
| V1 | 최근 rewrite 간격 `T`를 중심으로 분류하고 Hot/Cold WP와 line pool을 사용 |
| V2 | V1 구조 위에 `A`, `C`, `D`, lazy decay를 추가하여 반복성과 오래된 이력을 함께 고려 |

V2에서도 다음 V1 구조는 그대로 유지된다.

- Hot/Cold 두 개의 active write pointer
- 초기 50:50 Hot/Cold free-line pool
- 요청 pool이 비었을 때 반대 pool에서 line을 빌리는 정책
- GC 후 victim line을 기존 class pool로 반환하는 정책
- valid GC page를 현재 LPN state에 맞는 write pointer로 이동하는 정책
- 공통 WAF, GC, pool, 전이 통계

즉 V1과 V2를 비교할 때 geometry, GC 정책, line pool 구조는 같고 classifier의 의사결정 방식이 주된 차이가 된다. 이 점이 V2 classifier의 효과를 비교적 독립적으로 해석할 수 있게 한다.

---

## 4. 파일별 코드 구조

### 4.1 `hw/femu/bbssd/ftl.h`

V2의 상태와 장기 보관 데이터를 정의한다.

- `LpnState`: `UNSEEN`, `COLD`, `HOT`
- `LineClass`: line이 속한 `COLD` 또는 `HOT` pool
- `LpnMeta`: LPN별 분류 이력
- `struct ssd`: 런타임 설정값, 전체 LPN metadata 포인터, host sequence, 측정 counter

### 4.2 `hw/femu/bbssd/ftl.c`

실제 동작이 대부분 구현되어 있다.

| 함수 | 역할 |
|---|---|
| `ssd_load_positive_u64()` | T, D 양의 정수 환경변수 검증 |
| `ssd_load_positive_u32()` | A, C 양의 정수와 `UINT32_MAX` 범위 검증 |
| `ssd_load_bool()` | decay boolean 값 검증 |
| `ssd_init_lpn_metadata()` | 전체 LPN metadata 할당과 V2 설정 로드 |
| `ssd_apply_lpn_decay()` | 해당 LPN에 경과 epoch만큼 lazy decay 적용 |
| `ssd_update_lpn_temperature()` | host write 한 번을 반영해 HOT/COLD 재판정 |
| `ssd_select_write_pointer()` | 저장된 state에 맞는 Hot/Cold WP 반환 |
| `ssd_reset_lpn_metadata()` | TRIM 대상 LPN의 history 초기화 |
| `ssd_write()` | host write 경로에서 학습, 배치, page-write 계측 |
| `gc_write_page()` | GC 이동 시 state만 읽어 재배치하고 GC write 계측 |
| `do_gc()` | victim line 선택, valid page 복사, block erase, line 반환 |
| `ssd_reset_stats()` | 측정 counter만 초기화 |
| `ssd_print_stats()` | 설정값, 결과 지표, counter 불변식 출력 |

### 4.3 `hw/femu/bbssd/bb.c`

`FEMU_RESET_ACCT` admin command를 실험 구간 경계로 사용한다.

```text
현재까지 누적된 BBSSD-STATS 출력
                ↓
측정 counter 초기화
```

중요한 점은 이 명령이 FTL mapping, LPN history, 현재 write pointer, line 상태를 초기화하지 않는다는 것이다. preconditioning으로 만든 실제 SSD 상태는 유지하면서 이후 workload의 통계만 0부터 잴 수 있다.

### 4.4 `hw/femu/scripts/run-blackbox.sh`

다음 다섯 개의 V2 환경변수를 `sudo` 경계 너머 QEMU 프로세스에 명시적으로 전달한다.

- `FEMU_HOT_REWRITE_WINDOW`
- `FEMU_HOT_ACCESS_THRESHOLD`
- `FEMU_HOT_CONFIRM_THRESHOLD`
- `FEMU_HOT_DECAY_WINDOW`
- `FEMU_HOT_DECAY`

임의의 host 환경 전체를 전달하지 않고 실험에 필요한 값만 전달하므로, 실제 적용된 설정을 통제하기 쉽다.

### 4.5 correctness 결과

`experiment_results/ftl_hotcold/v2/20260819_135900_KST/`에는 다음 검증 근거가 있다.

- `manifest.md`: 실행 조건과 판정 요약
- `analysis.md`: correctness 해석
- `correctness/classification_trace.txt`: 승격, decay, 재승격, TRIM trace
- `correctness/gc_trace.txt`: GC 전후 metadata 보존 trace
- `correctness/stats.txt`: 단계별 통계와 불변식
- `correctness/config_validation.txt`: 잘못된 환경변수의 fail-fast 결과

---

## 5. 핵심 자료구조

### 5.1 LPN 상태

```c
typedef enum LpnState {
    LPN_STATE_UNSEEN = 0,
    LPN_STATE_COLD,
    LPN_STATE_HOT,
} LpnState;
```

`UNSEEN`을 0으로 둔 이유는 `g_new0()`로 metadata 배열을 할당했을 때 별도 전체 초기화 loop 없이 모든 LPN이 자연스럽게 미관찰 상태가 되게 하기 위해서다. TRIM도 구조체를 0으로 지우는 것만으로 같은 초기 상태를 복원할 수 있다.

### 5.2 `LpnMeta`

```c
typedef struct LpnMeta {
    uint64_t last_write_seq;
    uint64_t update_interval;
    uint64_t last_decay_epoch;
    uint32_t access_count;
    uint32_t short_interval_count;
    LpnState state;
} LpnMeta;
```

각 필드의 의미는 다음과 같다.

| 필드 | 의미 | 필요한 이유 |
|---|---|---|
| `last_write_seq` | 이 LPN에 대한 직전 host write 당시의 전역 sequence | 현재 rewrite 간격 계산 |
| `update_interval` | 현재 write와 직전 write 사이의 host page-write 수 | 최근성이 T 안에 있는지 판단하고 trace로 확인 |
| `last_decay_epoch` | 이 LPN에 decay를 마지막으로 반영한 epoch | 전체 배열을 훑지 않고 밀린 decay 계산 |
| `access_count` | decay가 반영된 host write 누적 빈도 | 한두 번의 우연한 rewrite를 HOT에서 제외 |
| `short_interval_count` | 최근 간격이 T 이하였던 rewrite의 누적 횟수 | 빠른 rewrite가 반복되었는지 확인 |
| `state` | 현재 `UNSEEN/COLD/HOT` | host와 GC placement가 함께 소비하는 결과 |

현재 빌드에서 `sizeof(LpnMeta)`는 정렬 padding을 포함해 40 byte다. correctness 환경의 `2,097,152` LPN에는 약 80 MiB가 필요하다.

```text
2,097,152 entries × 40 bytes = 83,886,080 bytes = 80 MiB
```

이 메모리는 QEMU host 메모리에 존재하는 모델 metadata다. V2가 V1보다 정교한 이력을 유지하는 대신 지불하는 명확한 공간 비용이다.

### 5.3 `struct ssd`의 V2 전역 상태

```c
LpnMeta *lpn_meta;
uint64_t host_write_seq;
uint64_t hot_rewrite_window;
uint64_t hot_decay_window;
uint32_t hot_access_threshold;
uint32_t hot_confirmation_threshold;
bool hot_decay_enabled;
```

`host_write_seq`는 시간 단위가 아니라 **host page write의 논리적 순서**다. 요청 하나가 여러 LPN을 포함하면 LPN을 처리할 때마다 1씩 증가한다. 따라서 T와 D도 초나 밀리초가 아니라 host page-write 거리로 해석해야 한다.

---

## 6. 런타임 파라미터

| 기호 | 환경변수 | 기본값 | 의미 |
|---|---|---:|---|
| T | `FEMU_HOT_REWRITE_WINDOW` | 1024 | 가장 최근 rewrite가 짧다고 인정할 최대 host page-write 간격 |
| A | `FEMU_HOT_ACCESS_THRESHOLD` | 3 | HOT이 되기 위한 최소 `access_count` |
| C | `FEMU_HOT_CONFIRM_THRESHOLD` | 2 | HOT이 되기 위한 최소 `short_interval_count` |
| D | `FEMU_HOT_DECAY_WINDOW` | 65536 | decay epoch 하나의 host page-write 길이 |
| decay | `FEMU_HOT_DECAY` | ON | lazy decay 사용 여부 |

4 KiB page 기준으로 보면 기본 T=1024는 약 4 MiB의 논리적 write 거리이고, D=65536은 약 256 MiB의 논리적 write 거리다. 이 값은 wall-clock 시간이 아니므로 같은 D라도 workload write rate와 locality에 따라 체감되는 시간은 다르다.

### 6.1 각 파라미터의 역할

**T: 최신 write가 충분히 가까운가**

T가 크면 HOT 판정의 최근성 조건이 느슨해지고, 작으면 매우 빠른 rewrite만 인정한다.

**A: 충분히 여러 번 관찰했는가**

첫 write나 한 번의 우연한 rewrite만으로 HOT이 되는 것을 막는다. 기본 A=3에서는 최소 세 번의 host write 관찰이 필요하다.

**C: 짧은 rewrite가 반복되었는가**

단순 접근 횟수만 많은 데이터가 아니라 T 이하의 rewrite를 반복한 데이터인지 확인한다. 기본 C=2에서는 짧은 rewrite가 최소 두 번 필요하다.

**D: 오래된 확신을 얼마나 빨리 잊을 것인가**

D가 작으면 workload 변화에 빨리 적응하지만 분류가 흔들릴 수 있다. D가 크면 안정적이지만 과거에 HOT이었던 데이터가 오래 남을 수 있다.

### 6.2 fail-fast 검증

T, A, C, D는 양의 정수만 허용한다. 0, 음수 표현, 숫자 뒤의 불필요한 문자, 범위 초과는 QEMU 시작 단계에서 실패한다. A와 C는 `uint32_t`이므로 `UINT32_MAX`도 검사한다.

decay boolean은 다음 값만 허용한다.

- 참: `1`, `on`, `true`
- 거짓: `0`, `off`, `false`

예를 들어 `FEMU_HOT_DECAY=maybe`는 자동 보정하지 않고 종료한다. 잘못된 설정으로 밤새 실험한 뒤 결과를 폐기하는 상황을 앞에서 막기 위한 설계다. launcher의 `pipefail` 보정은 QEMU 출력이 `tee`를 지나더라도 이 비정상 종료 코드가 상위 실행기에 전달되게 한다.

---

## 7. 초기화 순서

non-FDP 초기화 흐름은 다음과 같다.

```text
ssd_init()
  ├─ ssd_reset_stats()
  ├─ geometry / NAND / mapping / reverse mapping 초기화
  ├─ 전체 line 초기화
  ├─ fdp_enabled 판정
  ├─ ssd_init_lpn_metadata()
  │    ├─ LpnMeta[tt_pgs] zero allocation
  │    └─ T/A/C/D/decay 환경변수 읽기 및 검증
  ├─ free line을 Cold/Hot 50:50 pool로 분배
  ├─ Cold write pointer 초기화
  ├─ Hot write pointer 초기화
  └─ FTL worker thread 시작
```

FDP 경로에서는 V2 metadata를 할당하지 않고 기존 FDP 초기화로 빠진다. 즉 V2 classifier가 FDP 동작 의미를 변경하지 않도록 non-FDP에만 한정되어 있다.

---

## 8. Host write 분류 알고리즘

### 8.1 실제 처리 순서

`ssd_write()`는 요청의 LBA 범위를 LPN 범위로 바꾼 뒤 각 LPN에 대해 다음 순서로 처리한다.

1. free line이 긴급 임계값 이하라면 foreground GC를 먼저 수행한다.
2. `ssd_update_lpn_temperature()`로 해당 LPN의 분류 이력을 갱신한다.
3. 기존 mapping이 있으면 이전 PPA를 invalid로 만들고 reverse mapping을 해제한다.
4. 갱신된 `state`로 `wp_hot` 또는 `wp_cold`를 고른다.
5. 선택된 pointer에서 새 PPA를 얻는다.
6. LPN→PPA와 PPA→LPN mapping을 갱신한다.
7. 새 page를 valid로 표시한다.
8. write pointer를 다음 page로 이동한다.
9. NAND write timing을 반영한다.
10. `host_page_writes`와 `nand_page_writes`를 각각 1 증가시킨다.

분류를 old page invalidation보다 먼저 하는 것은 현재 host write 자체를 반영한 최신 state로 새 page의 배치 위치를 고르기 위해서다.

### 8.2 HOT 판정식

코드의 최종 조건은 다음과 같다.

```text
HOT =
    access_count >= A
    AND 현재 write가 rewrite임
    AND update_interval <= T
    AND short_interval_count >= C
```

하나라도 만족하지 않으면 현재 state는 COLD다.

이를 의사코드로 쓰면 다음과 같다.

```text
host_write_seq = saturating_increment(host_write_seq)
current_epoch = (host_write_seq - 1) / D
apply_lazy_decay(meta, current_epoch)

if meta.state == UNSEEN:
    is_rewrite = false
    interval = 0
else:
    is_rewrite = true
    interval = host_write_seq - meta.last_write_seq

is_short = is_rewrite AND interval <= T

meta.access_count = saturating_increment(meta.access_count)
if is_short:
    meta.short_interval_count =
        saturating_increment(meta.short_interval_count)

if access_count >= A AND is_short AND short_interval_count >= C:
    meta.state = HOT
else:
    meta.state = COLD

record state transition
meta.last_write_seq = host_write_seq
```

### 8.3 기본값에서 같은 LPN을 빠르게 세 번 쓴 예

T=1024, A=3, C=2일 때 다른 host write가 거의 끼지 않고 같은 LPN을 세 번 쓰면 다음과 같다.

| write | access | short | interval | 결과 | 이유 |
|---:|---:|---:|---:|---|---|
| 1회 | 1 | 0 | 0 | COLD | 첫 관찰이라 rewrite가 아님 |
| 2회 | 2 | 1 | 1 | COLD | A=3, C=2에 아직 미달 |
| 3회 | 3 | 2 | 1 | HOT | A, T, C를 모두 충족 |

correctness trace에서도 정확히 `COLD(1,0) → COLD(2,1) → HOT(3,2)`가 관찰되었다.

### 8.4 긴 rewrite가 들어오면

현재 interval이 T보다 크면 `is_short_rewrite=false`이므로, 과거 access/short count가 임계값보다 크더라도 이번 판정은 COLD가 된다.

다만 긴 rewrite가 `short_interval_count`를 즉시 0으로 만들지는 않는다. 과거 이력은 decay가 점진적으로 약화한다. 이 설계는 다음 두 성질을 갖는다.

- 최신 interval이 길면 즉시 HOT placement를 막는다.
- 과거 반복 이력을 한 번에 모두 버리지 않아 다시 짧은 rewrite가 시작될 때 재학습 속도를 보존한다.

따라서 `short_interval_count`는 “연속 streak”가 아니라 decay된 짧은 rewrite의 누적 증거다.

### 8.5 포화 증가를 쓰는 이유

`host_write_seq`는 `UINT64_MAX`, access/short counter는 `UINT32_MAX`에서 멈춘다. overflow로 0으로 돌아가면 매우 오래 쓰인 HOT LPN이 갑자기 cold history처럼 보일 수 있다. 포화 증가는 장기 실험에서도 상태 의미가 역전되지 않게 한다.

---

## 9. Lazy decay

### 9.1 epoch 계산

현재 epoch는 다음과 같다.

```text
current_epoch = (host_write_seq - 1) / D
```

따라서 sequence `1..D`는 epoch 0에 속하고, 그 다음 host page write부터 epoch 1이다.

### 9.2 decay 계산

해당 LPN의 마지막 반영 epoch와 현재 epoch 차이를 구한다.

```text
elapsed = current_epoch - last_decay_epoch

access_count         >>= elapsed
short_interval_count >>= elapsed
```

한 epoch가 지날 때마다 두 counter가 대략 절반이 된다. 여러 epoch가 지났다면 한 번에 그만큼 right shift한다. 두 counter가 32-bit이므로 `elapsed >= 32`면 shift하지 않고 안전하게 0으로 만든다. C에서 bit width 이상의 shift는 정의되지 않은 동작이 될 수 있기 때문이다.

실제로 decay가 적용되면 `decay_application_count`가 1 증가한다. 이 counter도 `UINT64_MAX`에서 포화한다.

### 9.3 왜 전체 배열을 주기적으로 훑지 않는가

2,097,152개 LPN을 D write마다 모두 순회하면 classifier 유지 자체가 큰 CPU 및 memory bandwidth 부하가 된다. 그러면 FTL 정책이 아니라 background scan 비용이 성능 결과에 섞일 수 있다.

V2는 어떤 LPN에 host write가 도착했을 때 그 LPN에 밀린 epoch를 한 번에 적용한다.

```text
전역 sweep 방식: O(전체 LPN 수) per decay cycle
V2 lazy 방식:    O(1) per touched LPN
```

이 방식은 모델을 단순하고 결정적으로 유지하며, 대규모 metadata 배열을 반복 스캔하지 않는 장점이 있다.

### 9.4 correctness의 D=8 예

correctness 실행은 기본 D=65536 대신 D=8을 사용해 짧은 시간 안에 decay를 강제로 관찰했다.

```text
HOT: access=3, short=2
두 epoch 경과
3 >> 2 = 0
2 >> 2 = 0
현재 write 반영
COLD: access=1, short=1
```

그 뒤 두 번의 빠른 rewrite로 `COLD access=2/short=2`, `HOT access=3/short=3`이 되어 재승격도 확인했다.

### 9.5 lazy decay의 의도된 한계

decay는 해당 LPN에 다음 host write가 올 때만 적용된다. 오랫동안 쓰이지 않은 HOT LPN은 metadata상 HOT으로 남을 수 있다. 그동안 GC가 이 page를 이동하면 GC는 저장된 HOT state를 읽어 Hot WP로 보낸다.

이것은 V2의 알려진 한계다. V2.5에서 GC placement 시점에도 경과 epoch를 확인하는 **placement-time expiration**으로 다룰 대상이다.

---

## 10. Hot/Cold placement 구조

### 10.1 두 개의 write pointer

```text
state=HOT  ──> wp_hot  ──> HOT active line
state=COLD ──> wp_cold ──> COLD active line
```

Hot과 Cold가 같은 active line을 공유하면 classifier가 맞아도 물리적으로 다시 섞인다. 그래서 두 pointer는 서로 다른 active line을 반드시 소유하며 `ssd_validate_write_pointers()`가 다음 오류를 즉시 중단한다.

- pointer가 line을 가지지 않음
- 두 pointer가 같은 line을 공유함
- pointer와 line class가 맞지 않음
- pointer의 block id와 current line id가 맞지 않음

### 10.2 50:50 free-line pool

초기 non-FDP free line을 Cold와 Hot pool에 절반씩 나눈다. 홀수면 Cold 쪽이 하나 더 가진다.

한 write pointer가 line 끝에 도달하면 기존 line을 full/victim 구조에 넣고 같은 class pool에서 다음 line을 받는다. 요청 pool이 비면 반대 pool에서 빌리고, 빌린 line의 class를 요청 class로 변경한다.

이 borrowing은 가용 공간 고갈을 피하지만 분류와 실제 pool 수요가 맞지 않는다는 신호이기도 하다. 그래서 `hot_pool_empty_count`, `cold_pool_empty_count`, `borrow_count`를 따로 측정한다.

### 10.3 왜 line 단위로 분리하는가

이 FTL의 GC 단위는 여러 channel/LUN에서 같은 block 번호를 묶은 line이다. page 단위로 HOT/COLD label만 붙이고 같은 line에 섞어 쓰면 victim line에 오래 사는 Cold page와 빨리 invalid되는 Hot page가 같이 남는다.

Hot/Cold를 서로 다른 line에 모으면 다음을 기대한다.

- Hot line: rewrite가 빨라 여러 page가 비슷한 시기에 invalid됨
- Cold line: 오래 유효한 page가 모여 GC victim으로 덜 선택되거나 안정적으로 유지됨
- victim line의 valid page 수 감소
- GC copy 감소
- NAND write와 WAF 감소

이 인과관계에서 classifier는 수단이고 `average_gc_copy`와 WAF가 결과 지표다.

---

## 11. GC 경로

### 11.1 전체 흐름

```text
select_victim_line()
       ↓ 성공 시 gc_count +1
victim line의 channel × LUN block 순회
       ↓
각 block에서 valid page만 gc_write_page()
       ↓
각 physical block erase, block_erases +1
       ↓
victim line을 원래 class의 free pool로 반환
```

일반 GC는 invalid page가 line page 수의 1/8 미만이면 복사 비용 때문에 victim을 건너뛴다. foreground GC는 공간 압력이 높을 때 `force=true`로 실행하며 `emergency_gc_count`도 증가한다.

`gc_count`는 victim 선택이 성공한 line GC마다 정확히 1 증가한다. `block_erases`는 실제 block을 free로 만들 때마다 증가한다. 한 line은 channel×LUN의 block 묶음이므로 일반적으로 한 번의 line GC에 여러 block erase가 대응한다.

### 11.2 GC가 classifier를 갱신하지 않는 이유

`gc_write_page()`는 `ssd_update_lpn_temperature()`나 `ssd_apply_lpn_decay()`를 호출하지 않는다. 다음 값은 그대로 보존된다.

- `state`
- `access_count`
- `short_interval_count`
- `last_write_seq`
- `last_decay_epoch`

GC는 host workload가 아니라 FTL 내부 공간 회수다. GC copy까지 access로 세면 이런 잘못된 순환이 생긴다.

```text
GC로 page 이동
  → access_count 증가
  → HOT 판정 강화
  → 다시 Hot 영역 배치
  → 내부 이동이 자기 자신의 분류 근거가 됨
```

이를 막아 classifier가 오직 host의 실제 rewrite locality를 표현하게 했다.

### 11.3 GC placement는 저장된 state를 소비한다

GC가 valid page를 옮길 때 현재 `state`가 HOT이면 `wp_hot`, COLD이면 `wp_cold`를 사용한다. 이는 host write에서 만든 lifetime 분리를 relocation 이후에도 유지하기 위한 것이다.

correctness workload에서는 LPN 0이 다섯 번 GC 이동되었고 다음 metadata가 모두 동일했다.

```text
state=COLD
access=1
short=0
last_seq=2566169
decay_epoch=320771
```

즉 GC가 분류 이력을 학습하거나 decay하지 않는다는 것이 trace로 확인되었다.

---

## 12. TRIM 경로

TRIM은 단순히 현재 mapping만 invalid로 만드는 것이 아니라 해당 LPN의 논리적 객체 수명이 끝났다는 뜻으로 처리한다.

각 대상 LPN에 대해 먼저 `ssd_reset_lpn_metadata()`가 `LpnMeta` 전체를 0으로 만든다.

```text
state = UNSEEN
last_write_seq = 0
update_interval = 0
last_decay_epoch = 0
access_count = 0
short_interval_count = 0
```

그 뒤 기존 page를 invalid로 하고 LPN→PPA, PPA→LPN mapping을 해제한다. 이미 mapping이 없는 LPN이어도 metadata reset은 먼저 수행된다.

따라서 TRIM 뒤 같은 LPN 주소에 새 데이터가 기록되어도 이전 데이터의 HOT history를 상속하지 않는다. correctness trace에서 TRIM 다음 첫 write는 `COLD access=1 short=0 interval=0`이었다.

이 설계가 없으면 주소 재사용만으로 새 데이터가 HOT으로 오분류될 수 있다.

---

## 13. 통계 reset의 정확한 의미

`ssd_reset_stats()`는 다음 측정 counter를 0으로 만든다.

- host/nand/gc page writes
- block erases, GC count
- host/gc Hot/Cold writes
- Cold→Hot, Hot→Cold 전이
- pool empty, borrowing, emergency GC
- decay 적용 횟수

그러나 다음 상태는 유지한다.

- LPN→PPA와 PPA→LPN mapping
- valid/invalid/free page 상태
- active Hot/Cold write pointer
- line pool과 victim/full line 상태
- 모든 LPN의 classification history와 state
- `host_write_seq`
- T/A/C/D/decay 설정

이 차이가 중요하다. preconditioning 뒤 stats reset을 호출하면 SSD는 이미 채워지고 GC 가능한 실제 상태를 유지하지만 측정 workload의 counter만 새로 시작한다.

반대로 classifier cold-start 자체를 비교하려면 단순 stats reset으로는 부족하고 fresh FEMU device가 필요하다.

---

## 14. 지표 정의와 해석

### 14.1 공통 page-write 불변식

```text
nand_page_writes = host_page_writes + gc_page_writes
```

현재 모델에서 계측하는 NAND program은 host write 또는 GC copy이므로 이 식이 맞아야 한다. 출력의 `counter_invariant=PASS`는 이 관계를 검사한다.

FAIL이면 성능 차이를 해석하기 전에 누락되거나 중복된 counter부터 찾아야 한다.

### 14.2 WAF

```text
WAF = nand_page_writes / host_page_writes
    = 1 + gc_page_writes / host_page_writes
```

Host writes가 같다면 WAF를 낮추는 직접적인 방법은 GC page writes를 줄이는 것이다. Hot/Cold 분리는 같은 lifetime의 page를 모아 victim의 valid page를 줄이고, 결과적으로 GC copy를 줄이려는 설계다.

WAF는 최종 효과를 보는 핵심 지표지만, 왜 값이 변했는지는 WAF만으로 알 수 없다. 반드시 평균 GC 복사량, Hot 비율, pool borrowing과 같이 봐야 한다.

### 14.3 `average_gc_copy`

```text
average_gc_copy = gc_page_writes / gc_count
```

한 victim line을 회수할 때 평균 몇 개의 valid page를 옮겼는지를 뜻한다. lifetime mixing이 줄면 같은 line의 page들이 비슷한 시기에 invalid되어 이 값이 낮아질 가능성이 있다.

classifier 구조와 가장 직접적으로 연결되는 내부 지표다.

- 값 감소: victim에 남은 valid page가 줄었을 가능성
- 값 유지 + WAF 감소: GC 빈도나 host write 수 차이도 확인 필요
- 값 증가: Hot/Cold 분리가 오히려 valid page를 많이 남기거나 pool 정책이 방해했을 가능성

### 14.4 `gc_count`

victim 선택에 성공한 line GC 횟수다. 횟수만 낮다고 좋은 것은 아니다.

```text
GC 100회 × 평균 5 page copy  = 500 copies
GC  70회 × 평균 20 page copy = 1400 copies
```

따라서 `gc_count`는 반드시 `average_gc_copy` 및 `gc_page_writes`와 함께 본다.

### 14.5 `block_erases`

실제 physical block erase 횟수다. line GC 한 번은 각 channel/LUN의 해당 block을 erase하므로 `gc_count`와 1:1이 아니다.

이 값은 erase 활동량과 wear 관점의 근거지만, 한 번의 고정 길이 성능 실험에서 단독으로 classifier 우열을 말하기에는 부족하다. geometry가 동일한 실행끼리 비교해야 한다.

### 14.6 `hot_write_ratio`

```text
hot_write_ratio = host_hot_writes / host_page_writes
```

classifier가 실제 workload를 얼마나 선택적으로 HOT으로 판정했는지를 보여준다.

- 지나치게 낮음: A/C가 너무 크거나 T/D가 너무 작아 실제 hot set을 놓칠 수 있음
- 지나치게 높음: 거의 모든 데이터가 HOT이 되어 분리 의미가 약해질 수 있음
- 적정 범위: workload의 반복 영역을 잡으면서 Cold 데이터와 물리적으로 분리

비율이 높을수록 무조건 좋은 것이 아니다. WAF와 평균 GC 복사량이 함께 좋아지는 범위를 찾아야 한다.

### 14.7 상태 전이 횟수

- `cold_to_hot_count`: promotion 횟수
- `hot_to_cold_count`: 최신 긴 interval 또는 decay로 인한 demotion 횟수

전이가 너무 많으면 classifier가 경계에서 흔들리는지 의심할 수 있다. 반대로 거의 없으면 너무 보수적이거나 decay가 사실상 작동하지 않을 수 있다.

다만 workload가 실제로 phase 변화가 많은 경우 전이가 많은 것이 정상일 수도 있다. 전이 횟수는 host write 수로 정규화하고 workload 특성과 함께 해석해야 한다.

### 14.8 GC Hot/Cold writes

- `gc_hot_writes`
- `gc_cold_writes`

GC relocation traffic이 어느 write pointer로 갔는지 보여준다. Host Hot 비율과 함께 보면 분류된 page가 GC 때도 같은 class에 유지되는지, 특정 pool에 relocation이 집중되는지 알 수 있다.

V2 lazy decay의 stale-HOT 현상을 연구할 때도 유용하다. host write가 없는 오래된 HOT page가 GC에서 계속 Hot으로 이동하면 `gc_hot_writes`에 포함된다.

### 14.9 decay 적용 횟수

`decay_application_count`는 실제로 경과 epoch가 있어 counter shift를 적용한 LPN update 횟수다.

- 0에 가까움: D가 workload 길이에 비해 너무 크거나 동일 epoch 내 접근이 대부분
- 매우 큼: D가 작아 대부분의 재접근에서 decay 발생
- HOT 비율과 같이 변화: D가 classifier selectivity에 실제 영향을 주고 있다는 근거

이 값 자체는 성능 목표가 아니다. decay가 설정만 되어 있고 실제로는 발동하지 않은 실험을 구별하는 진단 지표다.

### 14.10 pool 및 emergency 지표

| 지표 | 의미 | 해석 |
|---|---|---|
| `hot_pool_empty_count` | Hot WP가 새 Hot line을 구할 때 Hot pool이 빈 횟수 | Hot 수요가 초기 50%보다 컸을 가능성 |
| `cold_pool_empty_count` | Cold WP가 새 Cold line을 구할 때 Cold pool이 빈 횟수 | Cold 수요가 초기 50%보다 컸을 가능성 |
| `borrow_count` | 반대 class pool에서 line을 빌린 횟수 | 고정 50:50 분배와 실제 분류 비율의 불일치 |
| `emergency_gc_count` | host write 전 foreground forced GC 횟수 | 심한 공간 압력 또는 pool/GC 정책 문제 신호 |

borrow가 많으면 classifier가 좋아도 pool 경계가 실제 배치를 방해하거나 class 비율이 바뀌고 있을 수 있다. 따라서 WAF 차이를 classifier 효과로만 단정하지 않도록 하는 보조 지표다.

---

## 15. 설계 선택과 기대되는 지표상 이점

| 설계 선택 | 해결하려는 문제 | 직접 관찰할 지표 | 기대 방향 |
|---|---|---|---|
| A 최소 접근 횟수 | 한두 번 접근한 page의 성급한 HOT 판정 | Hot ratio, promotion count | false HOT 감소 |
| C 짧은 rewrite 확인 | 접근은 많지만 빠른 rewrite가 반복되지 않은 page 구별 | Hot ratio, 전이 횟수 | 반복 hot set 선택성 향상 |
| 최신 interval ≤ T 필수 | 오래전에 hot이었던 page의 즉시 재승격 방지 | Hot→Cold, Hot ratio | 오래된 HOT placement 억제 |
| D 기반 decay | 영구히 남는 과거 빈도 | decay count, Hot→Cold | workload phase 변화 적응 |
| lazy per-LPN decay | 전체 metadata scan 비용 | host 성능, decay count | 정책 자체의 유지비 감소 |
| 포화 counter | 장기 실행 overflow | invariant, 장기 trace | 상태 역전 방지 |
| Host-only 학습 | GC 자기강화 오분류 | GC trace, host/GC Hot writes | 분류 의미 보존 |
| GC state-preserving placement | relocation 후 lifetime 재혼합 | average GC copy, WAF | 분리 효과 지속 |
| TRIM history reset | 주소 재사용 시 이전 객체 history 오염 | TRIM trace, Hot ratio | 새 객체 false HOT 방지 |
| dual WP + class pool | 분류 결과가 같은 line에 다시 섞임 | avg GC copy, GC writes, WAF | victim valid page 감소 |
| runtime 설정 + stats 출력 | 다른 binary/config 혼동 | stats header | 재현성과 OFAT 비교 향상 |
| fail-fast 설정 검증 | 무효 설정으로 장시간 실험 | exit code, config validation | 실패 비용 감소 |

가장 이상적인 V2 결과 패턴은 다음과 같다.

```text
counter_invariant = PASS
        +
Hot ratio가 0%나 100%에 치우치지 않음
        +
pool borrowing과 emergency GC가 통제됨
        +
average_gc_copy 감소
        ↓
gc_page_writes 감소
        ↓
WAF 감소
```

반대로 WAF만 낮고 measurement 시간, host writes, fresh-device 조건이 다르다면 classifier 이점으로 결론 내릴 수 없다.

---

## 16. 현재까지 확인된 결과

### 16.1 correctness에서 확인된 것

V2 correctness는 기본 classifier 값 T=1024, A=3, C=2를 사용하고 decay만 D=8로 줄여 다음 항목을 검증했다.

- 빌드와 shell syntax PASS
- `COLD → COLD → HOT` 승격 PASS
- 두 decay epoch 후 `HOT → COLD` PASS
- 빠른 rewrite 재개 후 HOT 재승격 PASS
- TRIM 뒤 첫 write COLD PASS
- LPN 0의 다섯 번 GC relocation에서 metadata 동일 PASS
- 모든 단계 `NAND = Host + GC` PASS
- 여섯 가지 잘못된 설정이 Guest boot 전에 비정상 종료 PASS

### 16.2 비교 참고값

동일 프로젝트의 기존 공식 결과는 다음과 같다.

| 버전 | WAF | GC count | Average GC copy | Host Hot ratio |
|---|---:|---:|---:|---:|
| Corrected baseline | 7.949892 | 4814 | 14334.293103 | 해당 없음 |
| V1 T=1024 | 7.297968 | 4766 | 14151.711918 | 0.095534 |
| V2 기본값 | 아직 미실행 | 아직 미실행 | 아직 미실행 | 아직 미실행 |

V2 correctness의 30초 GC 유도 구간은 D=8을 사용했고 같은 device의 누적 상태에서 실행했다. 그 WAF 값은 성능 비교에 사용하면 안 된다.

V2의 지표 이점을 주장하려면 fresh FEMU device에서 기본 `T=1024, A=3, C=2, D=65536, decay=ON`으로 300초 본 실험을 실행하고 corrected baseline 및 V1 T=1024와 비교해야 한다.

---

## 17. V2 결과를 읽는 권장 순서

1. 출력의 `version=V2`와 T/A/C/D/decay가 의도한 값인지 확인한다.
2. fio 조건, seed, 실행 시간, geometry, OP, GC threshold, fresh-device 조건이 비교군과 같은지 확인한다.
3. `counter_invariant=PASS`를 확인한다.
4. `host_page_writes`와 실제 측정 시간이 비교 가능한지 확인한다.
5. `hot_write_ratio`로 classifier가 얼마나 선택적으로 작동했는지 본다.
6. `cold_to_hot_count`, `hot_to_cold_count`, `decay_application_count`로 안정성과 decay 발동 여부를 본다.
7. `hot_pool_empty_count`, `cold_pool_empty_count`, `borrow_count`, `emergency_gc_count`로 pool/공간 압력의 교란을 확인한다.
8. `average_gc_copy`가 줄었는지 본다.
9. `gc_count`와 `gc_page_writes`를 함께 본다.
10. 최종적으로 WAF가 줄었는지 판단한다.

이 순서를 쓰면 “WAF가 변했다”에서 끝나지 않고 classifier가 실제로 어떤 경로를 통해 결과를 만들었는지 설명할 수 있다.

---

## 18. 한계와 V2.5로 넘길 문제

### 18.1 placement-time expiration 부재

idle HOT LPN은 다음 host write 전까지 decay되지 않는다. GC는 그 사이 저장된 HOT state를 그대로 사용한다. V2.5의 가장 직접적인 개선 대상이다.

### 18.2 논리적 write 거리와 실제 시간의 차이

T와 D는 host page-write 수 기준이다. write rate가 다른 workload끼리는 같은 설정도 실제 시간 의미가 달라진다. 성능 비교에서는 workload와 실행 조건을 반드시 고정해야 한다.

### 18.3 고정 50:50 pool

실제 Hot 비율이 10%여도 초기 pool은 50:50이다. borrowing으로 동작은 계속되지만 pool 불일치가 WAF에 영향을 줄 수 있다. 이는 classifier와 독립된 후속 정책 연구 대상이다.

### 18.4 coarse decay

right shift는 빠르고 결정적이지만 정수 기반의 거친 지수 감쇠다. 작은 counter는 한두 epoch만 지나도 0이 될 수 있다. D를 너무 작게 잡으면 hot set을 과도하게 잊을 수 있다.

### 18.5 metadata 비용

현재 geometry에서 약 80 MiB가 필요하다. 실제 SSD controller 관점에서는 field packing, counter bit width 축소, 압축 또는 선택적 metadata 보관을 검토할 수 있다.

### 18.6 성능 결론 부재

correctness PASS는 구현이 의도대로 동작한다는 뜻이지 WAF가 baseline/V1보다 좋아졌다는 뜻이 아니다. 본 실험 전에는 V2의 성능 우위를 주장하면 안 된다.

---

## 19. 권장 코드 읽기 순서

V2 코드를 처음 읽을 때는 다음 순서가 가장 이해하기 쉽다.

1. `ftl.h`의 `LpnState`, `LpnMeta`, `struct ssd`
2. `ssd_init_lpn_metadata()`의 기본값, 환경변수, allocation
3. `ssd_apply_lpn_decay()`의 epoch와 shift
4. `ssd_update_lpn_temperature()`의 실제 판정 순서
5. `ssd_select_write_pointer()`
6. `ssd_write()`의 host write placement
7. `gc_write_page()`의 read-only history 경계
8. `ssd_trim()`과 `ssd_reset_lpn_metadata()`
9. `ssd_init_hotcold_line_pools()`와 `ssd_advance_write_pointer()`
10. `select_victim_line()`, `do_gc()`, `mark_line_free()`
11. `ssd_print_stats()`와 `bb.c`의 `FEMU_RESET_ACCT`
12. correctness trace와 실제 code path 대조

---

## 20. 결론

V2의 핵심 목적은 HOT 판정을 단순한 한 번의 짧은 rewrite에서 **빈도 + 반복 확인 + 최신성 + 망각**의 조합으로 바꾸는 것이다.

이 구조의 성공 여부는 HOT으로 많이 분류했는지가 아니라 다음 인과관계로 판단해야 한다.

```text
더 정확하고 적응적인 LPN 분류
        ↓
Hot/Cold line의 lifetime mixing 감소
        ↓
victim line의 valid page 감소
        ↓
average_gc_copy 및 gc_page_writes 감소
        ↓
WAF 감소
```

동시에 transition, decay, pool borrowing 지표를 봐야 개선 원인이 classifier인지, pool 비율인지, workload 변동인지 구분할 수 있다.

현재 V2는 이 인과관계를 실험할 수 있는 코드와 correctness 검증까지 준비된 상태다. 다음 단계는 기본값으로 fresh-device 300초 본 실험을 한 번 실행해 corrected baseline과 V1 T=1024를 같은 조건에서 비교하는 것이다.
