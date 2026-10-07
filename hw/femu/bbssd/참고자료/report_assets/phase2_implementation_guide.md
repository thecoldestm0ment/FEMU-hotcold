# FEMU Blackbox SSD Hot/Cold FTL: Phase 2 구현 가이드

## 1. 이 문서의 목표

이 문서는 코드를 바로 복사해 완성하는 답안이 아니라, 다음 자료구조를 직접
설계하고 단계적으로 구현하기 위한 작업 순서를 설명한다.

- LPN의 Hot/Cold 상태와 접근 이력
- line의 Hot/Cold class와 생명주기 상태
- Hot/Cold free line pool
- Hot/Cold write pointer

Phase 1에서 추가한 WAF counter는 그대로 유지한다. Phase 2의 결과가 맞는지는
최종 성능보다 먼저 기존 FTL 불변식과 Phase 1 WAF 관계를 이용해 검증한다.

이 문서의 첫 구현 범위는 **non-FDP Blackbox FTL**이다. FDP는
`struct line`, `struct write_pointer`, `get_next_free_line()`을 공유하므로
자료형에 필드가 추가되는 것 자체는 허용하되, FDP의 RU allocation과 GC
동작은 바꾸지 않는다.

---

## 2. 먼저 고정할 용어

Hot/Cold를 구현할 때 가장 먼저 피해야 할 혼동은 다음 세 가지를 모두
`state`라고 부르는 것이다.

| 개념 | 질문 | 권장 타입 |
|---|---|---|
| LPN 상태 | 이 논리 page는 최근 host write 패턴상 Hot인가? | `LpnState` |
| line class | 이 line은 어떤 종류의 page를 모으는가? | `LineClass` |
| line 상태 | 이 line은 free/open/full/victim/GC 중 어디에 있는가? | `LineState` |

예를 들어 `LINE_CLASS_HOT`인 line도 생명주기 중에는
`LINE_STATE_OPEN`, `LINE_STATE_FULL`, `LINE_STATE_VICTIM`,
`LINE_STATE_GC`, `LINE_STATE_FREE`를 차례로 가질 수 있다.

따라서 `line->state == HOT`처럼 온도와 생명주기를 한 enum에 섞지 않는다.

---

## 3. `typedef enum`인가, 그냥 `enum`인가?

### 결론

둘 다 C 문법상 맞지만, 이번 상태값에는 **이름 있는 enum tag와 typedef를
같이 선언하는 방식**을 권장한다.

```c
typedef enum LpnState {
    LPN_STATE_UNSEEN = 0,
    LPN_STATE_COLD,
    LPN_STATE_HOT,
} LpnState;
```

이 방식을 쓰면 다음 두 이름이 모두 생긴다.

- `enum LpnState`: enum tag
- `LpnState`: typedef 이름

구조체에서는 짧게 `LpnState state;`라고 쓸 수 있고, 디버거에서도 enum
tag가 남는다. FEMU 주변 코드의 `NandMediaOp`, `FlashType` 같은 타입과도
형태가 잘 맞는다.

다음과 같은 익명 enum은 이번 목적에는 사용하지 않는 편이 좋다.

```c
enum {
    LPN_STATE_COLD,
    LPN_STATE_HOT,
};
```

익명 enum은 정수 상수를 만드는 데는 충분하지만, `LpnState`라는 별도 타입이
생기지 않는다. 현재 `ftl.h`의 `NAND_READ`, `USER_IO`, `PG_FREE`처럼 단순
상수 묶음에는 적합하지만, 구조체 필드의 의미를 구분하려는 Phase 2에는
부족하다.

`typedef enum { ... } LpnState;`도 동작하지만 tag가 없으므로, 이번에는
`typedef enum LpnState { ... } LpnState;` 형태로 통일한다.

---

## 4. 권장 타입 설계

아래 코드는 최종 정답이 아니라 `ftl.h`에 추가할 타입의 설계 골격이다.
값의 이름과 전이 의미를 먼저 이해한 뒤 직접 옮긴다.

### 4.1 LPN 상태

```c
typedef enum LpnState {
    LPN_STATE_UNSEEN = 0,
    LPN_STATE_COLD,
    LPN_STATE_HOT,
} LpnState;
```

각 값의 의미는 다음처럼 고정한다.

- `UNSEEN`: 아직 host write 이력이 없는 LPN
- `COLD`: 첫 write이거나, 직전 write와의 거리가 Hot 기준보다 긴 LPN
- `HOT`: 최근에 반복해서 갱신된 LPN

첫 write부터 Hot으로 분류하면 순차 write도 전부 Hot이 될 수 있다. 첫
write는 Cold, 이후 rewrite 간격으로 Hot 여부를 판단하는 것이 안전한
baseline이다.

### 4.2 line class

```c
typedef enum LineClass {
    LINE_CLASS_NONE = 0,
    LINE_CLASS_COLD,
    LINE_CLASS_HOT,
} LineClass;
```

`NONE`이 필요한 이유는 다음과 같다.

- `g_malloc0()` 직후 아직 class가 정해지지 않은 상태를 표현한다.
- FDP가 사용하는 global free line은 Hot/Cold class를 갖지 않아도 된다.
- 초기화 누락을 `COLD == 0`으로 조용히 오인하지 않고 assertion으로 찾을 수
  있다.

LPN 상태와 line class는 값이 비슷해도 타입을 분리한다. LPN은 `UNSEEN`이
가능하지만 line에는 `UNSEEN`이라는 의미가 없기 때문이다.

### 4.3 line 생명주기 상태

```c
typedef enum LineState {
    LINE_STATE_UNINITIALIZED = 0,
    LINE_STATE_FREE,
    LINE_STATE_OPEN,
    LINE_STATE_FULL,
    LINE_STATE_VICTIM,
    LINE_STATE_GC,
} LineState;
```

여기서 `OPEN`은 write pointer가 현재 사용 중인 active line을 뜻한다.
`GC`는 victim priority queue에서 빠진 뒤 erase가 끝나기 전까지의
in-flight 상태다.

`UNINITIALIZED`를 0으로 두면 `g_malloc0()`에 의해 잘못 초기화된 line을
정상 free line으로 오인하지 않는다. `ssd_init_lines()`가 모든 line을
명시적으로 `FREE`로 바꿔야 한다.

---

## 5. LPN metadata 설계

### 5.1 최소 필드

권장 시작점은 다음 세 필드다.

```c
typedef struct LpnMeta {
    uint64_t last_write_seq;
    uint32_t write_count;
    LpnState state;
} LpnMeta;
```

- `last_write_seq`: 이 LPN에 마지막 host write가 발생한 논리 시각
- `write_count`: 첫 write와 rewrite를 구분하고 관찰 횟수를 확인하는 값
- `state`: 현재 분류 결과

실제 시간(ns)을 사용하지 않고 host page write마다 증가하는 sequence를
사용하면 NAND latency 설정이나 VM 실행 속도에 분류 결과가 흔들리지 않는다.

`write_count`는 정책 입력으로 꼭 사용하지 않아도 된다. 초기 디버깅에서
`UNSEEN`, 첫 write, rewrite가 예상대로 구분되는지 확인하는 데 유용하다.
`uint32_t`를 계속 증가시킨다면 `UINT32_MAX`에서 포화시키고 wrap시키지 않는
편이 좋다.

필드 순서를 위처럼 두면 일반적인 환경에서 구조체 크기는 16바이트가 될
가능성이 높다. 반드시 `sizeof(LpnMeta)`를 확인한다. 기본 geometry의
`tt_pgs`는 약 419만 개이므로 16바이트 metadata는 약 64MiB를 추가로
사용한다.

### 5.2 `struct ssd`에 추가할 소유 상태

```c
LpnMeta *lpn_meta;
uint64_t host_write_seq;
uint64_t hot_rewrite_window;
```

이름은 예시다. 의미는 다음과 같이 정한다.

- `lpn_meta[lpn]`: LPN별 metadata
- `host_write_seq`: host가 기록한 page 수 기준의 전역 sequence
- `hot_rewrite_window`: 직전 write와 현재 write의 sequence 차이가 이 값
  이하이면 Hot으로 볼 기준

threshold를 처음부터 하드코딩해도 구조 검증은 가능하지만, 실험값을 바꿀
예정이라면 나중에 QOM property 또는 `ssdparams`로 옮길 수 있게 이름 있는
필드로 둔다. Phase 2의 첫 commit에서 QOM property까지 동시에 추가할
필요는 없다.

### 5.3 allocation과 초기화

metadata 개수는 현재 mapping table과 같은 `ssd->sp.tt_pgs`로 시작한다.

```text
ssd_init_params()
    ↓
tt_pgs 확정
    ↓
fdp_enabled 확정
    ↓
non-FDP일 때만 lpn_meta[tt_pgs] 할당 및 초기화
```

각 entry의 초기값은 다음과 같다.

- `last_write_seq = 0`
- `write_count = 0`
- `state = LPN_STATE_UNSEEN`
- `host_write_seq = 0`

첫 host page write에서 sequence를 **먼저 1로 증가**시키면
`last_write_seq == 0`을 미관찰 sentinel로 안전하게 사용할 수 있다.

FDP 실험에서 사용하지 않을 metadata를 무조건 약 64MiB 할당하지 않도록
non-FDP 조건을 둔다. 이후 teardown 경로를 추가한다면 `lpn_meta`도
`g_free()` 대상에 포함해야 한다.

### 5.4 상태 갱신 순서

한 LPN의 host write가 들어왔을 때의 권장 순서는 다음과 같다.

```text
host_write_seq 증가
    ↓
write_count == 0 ?
    ├─ yes: COLD
    └─ no : host_write_seq - last_write_seq 계산
              ├─ window 이하: HOT
              └─ window 초과: COLD
    ↓
last_write_seq 갱신
write_count 증가 또는 포화
```

중요한 규칙:

- metadata 갱신은 `ssd_write()`의 host write에 대해서만 수행한다.
- GC migration은 host access가 아니므로 sequence와 write count를
  증가시키지 않는다.
- GC는 해당 LPN의 현재 `state`를 읽어 destination pointer를 고를 수 있다.
- read를 온도 정책에 포함할지는 별도 정책이다. 첫 구현에서는 write
  temperature만 다룬다.
- TRIM 시 metadata를 초기화할지 유지할지는 명시적으로 결정해야 한다.

첫 구현에서는 TRIM을 logical data lifetime의 끝으로 보고 해당 LPN
metadata를 `UNSEEN`으로 초기화하는 정책을 권장한다. 주소 자체의 장기
hotness를 연구하려는 경우에는 유지할 수 있지만, 두 실험의 의미는 다르다.

---

## 6. `struct line`에 추가할 필드

현재 `struct line`은 `id`, `ipc`, `vpc`, list entry, victim queue 위치,
FDP의 `my_ru`를 갖는다. 여기에 다음 두 필드를 추가하는 방향을 권장한다.

```c
LineClass data_class;
LineState state;
```

필드 이름을 단순히 `class`로 해도 C에서는 가능하지만, `class`는 C++ 예약어이고
검색할 때 의미도 넓다. `data_class` 또는 `line_class`가 더 명확하다.

`data_class`의 의미는 “이 line에 현재 어떤 종류의 page를 모으는가”다.
line이 erase되어 free pool로 돌아가도 우선 class를 유지한다. 다른 pool에서
빌려 온 line을 활성화할 때는 요청받은 class로 바꾸고, 이후 그 class pool로
돌아가게 하면 pool 크기가 workload에 따라 서서히 적응한다.

`state`는 실제 container membership과 항상 일치해야 한다. 필드만 추가하고
전이 지점에서 갱신하지 않으면 두 번째 진실 공급원만 생기므로, 아래 전이
표를 구현할 준비가 되었을 때 추가한다.

### line 상태 전이

| 발생 지점 | 이전 상태 | 다음 상태 | container |
|---|---|---|---|
| `ssd_init_lines()` | `UNINITIALIZED` | `FREE` | global free list |
| non-FDP pool 분배 | `FREE` | `FREE` | Hot 또는 Cold free pool |
| free line 할당 | `FREE` | `OPEN` | 어떤 list/pq에도 없음 |
| active line 완주, invalid 없음 | `OPEN` | `FULL` | full list |
| active line 완주, invalid 있음 | `OPEN` | `VICTIM` | victim pq |
| full line의 page overwrite/TRIM | `FULL` | `VICTIM` | full list에서 victim pq |
| `select_victim_line()` pop | `VICTIM` | `GC` | 어떤 list/pq에도 없음 |
| erase 후 `mark_line_free()` | `GC` | `FREE` | 해당 class free pool |

FDP path는 이 상태 필드를 Phase 2의 판단에 사용하지 않는다. FDP line은
`my_ru`와 RU queue가 별도 생명주기를 관리하기 때문이다.

---

## 7. `line_mgmt`에 두 free pool 추가

### 7.1 자료구조 골격

기존 global free list는 FDP 호환을 위해 바로 삭제하지 않는다.

```c
QTAILQ_HEAD(free_line_list, line) free_line_list;
QTAILQ_HEAD(free_hot_line_list, line) free_hot_line_list;
QTAILQ_HEAD(free_cold_line_list, line) free_cold_line_list;

int free_line_cnt;
int free_hot_line_cnt;
int free_cold_line_cnt;
```

`free_line_cnt`는 전체 free line 수를 뜻하는 aggregate count로 유지한다.
기존 `should_gc()`와 `should_gc_high()`가 이 값을 사용하기 때문이다.

non-FDP에서는 항상 다음 관계가 성립해야 한다.

```text
free_line_cnt == free_hot_line_cnt + free_cold_line_cnt
```

FDP에서는 기존 global free list와 `free_line_cnt`를 사용하고 Hot/Cold
count는 0으로 유지한다.

### 7.2 QTAILQ entry 제약

`struct line`에는 `QTAILQ_ENTRY(line) entry`가 하나뿐이다. 따라서 한 line을
global, Hot, Cold list 중 두 곳에 동시에 넣을 수 없다.

올바른 이동은 항상 다음 순서다.

```text
기존 list에서 REMOVE
    ↓
필요한 count 갱신
    ↓
새 list에 INSERT
```

같은 line을 global free list에 남겨 둔 채 Hot pool에도 insert하면 list
포인터가 깨진다.

### 7.3 FDP와 안전하게 초기화하는 순서

현재 `ssd_init_lines()`는 `ssd->fdp_enabled`를 정하기 전에 모든 line을
global `free_line_list`에 넣는다. 이 흐름을 크게 뒤집기보다 다음처럼
non-FDP 후처리 단계를 두는 것이 안전하다.

```text
ssd_init_lines()
    모든 line → global free_line_list
    ↓
ssd->fdp_enabled 확정
    ├─ FDP: 기존 global list를 그대로 RU 초기화에 사용
    └─ non-FDP:
         ssd_init_hotcold_pools()
             global list의 각 line을 REMOVE
             Hot/Cold class 결정
             해당 class list에 INSERT
         두 write pointer 초기화
```

pool 분배는 free line의 container만 바꾸므로 aggregate
`free_line_cnt`는 변하지 않는다. Hot/Cold class count만 새로 증가시킨다.

### 7.4 초기 분할 정책

첫 구현은 line ID 기준으로 deterministic한 50:50 분할이면 충분하다.

- 전체 line 수가 홀수이면 한 pool에 하나를 더 준다.
- 두 write pointer가 각각 하나를 가져갈 수 있도록 각 pool에 최소 한 line이
  있어야 한다.
- 현재 기본 geometry에서는 line이 충분하지만, 작은 test geometry에서는
  `tt_lines >= 2`를 확인한다.

50:50은 최적 정책이 아니라 구조 검증용 초기값이다. Hot/Cold 비율과 workload
비율이 다를 수 있으므로 pool 고갈 정책이 반드시 필요하다.

### 7.5 pool 고갈과 borrowing

한 pool만 비었는데 aggregate `free_line_cnt`가 충분하면 기존
`should_gc()`는 GC를 시작하지 않는다. 따라서 다음 중 하나가 필요하다.

1. 다른 class pool에서 line을 빌린다.
2. class별 GC threshold와 victim 선택을 추가한다.

Phase 2에서는 1번을 권장한다. 요청한 pool에서 먼저 pop하고, 비었으면 다른
pool에서 pop한 뒤 line을 요청 class로 재분류한다.

```text
requested pool pop 시도
    ├─ 성공: 그대로 사용
    └─ 실패: other pool pop
                 ↓
             data_class를 requested class로 변경
```

이 방식은 global victim policy와 aggregate GC threshold를 우선 보존하면서
liveness를 확보한다. borrowing 횟수를 counter로 두면 초기 pool 비율이
workload와 얼마나 다른지도 알 수 있다.

free line을 꺼낼 때는 다음 값을 한 함수에서 함께 변경한다.

- 해당 class count `-1`
- aggregate `free_line_cnt -1`
- `line->state = LINE_STATE_OPEN`
- 필요하면 `line->data_class` 재분류

GC 후 line을 반환할 때는 정확히 반대로 처리한다.

---

## 8. `struct ssd`에 두 write pointer 추가

### 8.1 권장 필드

non-FDP의 기존 `struct write_pointer wp`를 최종적으로 다음 둘로 교체한다.

```c
struct write_pointer hot_wp;
struct write_pointer cold_wp;
```

`struct write_pointer` 자체에는 Hot/Cold 필드를 넣지 않는 편이 좋다. 이
자료형은 FDP RU의 `ssd_wptr`도 사용하기 때문이다. pointer의 class는
non-FDP 소유자인 `struct ssd`의 필드 이름과 현재 `curline->data_class`로
알 수 있다.

기존 `wp`를 남긴 채 세 pointer를 장기간 같이 사용하면 일부 함수는 `wp`,
다른 함수는 `hot_wp`를 이동하는 문제가 생기기 쉽다. 아래 helper
parameterization을 먼저 끝낸 뒤 call site를 한 번에 두 pointer로
전환한다.

### 8.2 초기화 helper

기존 `ssd_init_write_pointer()`는 `&ssd->wp`를 내부에서 고정해서 선택하고
첫 line의 block을 0으로 가정한다. 두 pointer용 helper는 적어도 다음 입력을
받아야 한다.

```c
ssd_init_write_pointer(ssd, wpp, requested_class);
```

초기화 절차:

1. requested class pool에서 line을 하나 얻는다.
2. `wpp->curline`에 저장한다.
3. `ch`, `lun`, `pg`, `pl`을 0으로 초기화한다.
4. `wpp->blk = wpp->curline->id`로 설정한다.
5. `curline->state == OPEN`인지 확인한다.
6. `curline->data_class == requested_class`인지 확인한다.

두 번째 pointer에 `blk = 0`을 복사하면 첫 pointer가 line 0을 가져간 뒤에도
두 번째 pointer가 같은 물리 block을 가리킬 수 있다. `blk`는 반드시 실제
할당된 `curline->id`에서 가져온다.

두 pointer의 `curline`은 서로 달라야 한다.

### 8.3 allocation/advance helper parameterization

현재 non-FDP 함수는 내부에서 항상 `&ssd->wp`를 선택한다.

- `get_new_page()`
- `ssd_advance_write_pointer()`

두 pointer를 안전하게 사용하려면 호출자가 같은 pointer를 두 함수에
전달해야 한다.

권장 책임 분리는 다음과 같다.

```c
get_new_page(ssd, wpp);
ssd_advance_write_pointer(ssd, wpp, requested_class);
```

한 page write의 핵심 불변식:

```text
PPA를 만든 pointer == 사용 후 advance한 pointer
```

`get_new_page()`는 분류 정책을 몰라도 된다. 전달받은 좌표로 PPA를 만드는
역할만 유지한다.

`ssd_advance_write_pointer()`는 line을 끝까지 사용했을 때 다음을 수행한다.

1. 현재 line을 `FULL` 또는 `VICTIM` 상태/container로 이동한다.
2. requested class의 다음 free line을 얻는다.
3. `wpp->curline`과 `wpp->blk`를 새 line에 맞춘다.
4. 새 line이 `OPEN`이고 class가 맞는지 확인한다.

FDP 전용 `fdp_get_new_page()`와 `fdp_advance_ru_pointer()`는 이 작업에서
수정하지 않는다.

---

## 9. Host write와 GC가 pointer를 선택하는 방법

### 9.1 선택 helper

분류와 pointer 선택을 한 곳에 모으면 call site에서 Hot/Cold 조건문이
중복되지 않는다.

```text
LPN_STATE_HOT  → &ssd->hot_wp,  LINE_CLASS_HOT
LPN_STATE_COLD → &ssd->cold_wp, LINE_CLASS_COLD
LPN_STATE_UNSEEN → host write에서 먼저 COLD로 갱신한 뒤 선택
```

예상 helper 책임:

- `update_lpn_state(ssd, lpn)`: host write 이력과 상태만 갱신
- `get_lpn_state(ssd, lpn)`: 현재 상태만 조회
- `select_write_pointer(ssd, state)`: pointer만 선택
- `lpn_state_to_line_class(state)`: 서로 다른 enum 간 명시적 변환

서로 다른 enum 값을 `(LineClass)state`처럼 cast하지 않는다. 현재 숫자가
우연히 같아도 enum 값 추가 순서가 바뀌면 잘못된 class가 선택된다.

### 9.2 `ssd_write()`의 순서

기존 overwrite/mapping 순서를 최대한 유지하면서 다음 단계만 삽입한다.

```text
LPN host-write metadata 갱신
    ↓
LPN state로 wpp/class 선택
    ↓
old PPA가 있으면 invalid 처리 및 old rmap 제거
    ↓
선택한 wpp에서 new PPA 생성
    ↓
new maptbl/rmap 등록
    ↓
new page valid 처리
    ↓
같은 wpp advance
    ↓
USER_IO NAND timing 및 Phase 1 counter
```

metadata를 old PPA invalid 처리 뒤에 갱신해도 단일 FTL thread에서는 큰
차이가 없지만, “이번 host write를 어떤 class로 기록했는가”를 먼저 결정하면
allocation 흐름을 읽기 쉽다.

### 9.3 `gc_write_page()`의 순서

GC는 old PPA의 reverse map에서 LPN을 복원한 뒤 기존 metadata를 조회한다.

```text
old PPA → rmap → LPN
    ↓
lpn_meta[lpn].state 조회
    ↓
wpp/class 선택
    ↓
선택한 wpp에서 destination PPA 생성
    ↓
maptbl/rmap 갱신, page valid
    ↓
같은 wpp advance
    ↓
GC_IO timing 및 Phase 1 GC counter
```

GC migration은 다음 작업을 하지 않는다.

- `host_write_seq` 증가
- `write_count` 증가
- `last_write_seq` 변경
- Hot/Cold 재분류
- `host_page_writes` 증가

GC가 data를 이동했다는 이유로 Hot으로 승격하면 GC 자체가 workload의
temperature를 오염시킨다.

---

## 10. 구현 순서: 한 번에 모두 바꾸지 않기

각 checkpoint에서 빌드와 작은 검증을 끝낸 뒤 다음 단계로 간다.

### Checkpoint A: enum과 metadata 타입

수정 범위:

- `ftl.h`: `LpnState`, `LineClass`, `LineState`, `LpnMeta`
- `struct ssd`: `lpn_meta`, `host_write_seq`, threshold

아직 allocation path는 바꾸지 않는다.

확인:

- 기존 non-FDP/FDP 빌드 성공
- `sizeof(LpnMeta)` 확인
- 새 enum의 0 값이 의도한 uninitialized 상태인지 확인

### Checkpoint B: LPN metadata 초기화와 갱신

수정 범위:

- non-FDP metadata init helper
- `ssd_init()`의 호출 위치
- `ssd_write()`의 host-only metadata 갱신
- 필요하면 `ssd_trim()`의 reset

아직 기존 `ssd->wp` 하나를 사용한다.

확인:

- 순차 write의 첫 write는 Cold
- 같은 LPN의 짧은 간격 rewrite는 Hot
- 긴 간격 뒤 rewrite는 Cold로 내려갈 수 있음
- GC 전후 metadata가 변하지 않음
- Phase 1 WAF 값이 이전 baseline과 같음

### Checkpoint C: line class/state 추적

수정 범위:

- `struct line`에 `data_class`, `state`
- 상태 전이 지점에 assignment와 assertion

아직 free pool은 하나로 유지해도 된다. 이 단계의 목적은 현재 암묵적인 line
생명주기를 명시적으로 검증하는 것이다.

확인:

- active line은 `OPEN`
- full list의 모든 line은 `FULL`
- victim pq의 모든 line은 `VICTIM`
- GC pop 직후 `GC`, 반환 후 `FREE`

### Checkpoint D: Hot/Cold free pool

수정 범위:

- `line_mgmt`의 두 list와 두 count
- non-FDP pool 분배 helper
- class-aware pop/return helper
- borrowing 정책

아직 pointer를 하나만 사용한다면 이 checkpoint를 짧은 중간 commit으로
유지하고 바로 다음 단계로 넘어간다. 하나의 pointer가 두 pool을 임의로
사용하는 상태를 최종 동작으로 남기지 않는다.

확인:

- global list와 class list에 중복 membership 없음
- aggregate/class count 관계 유지
- Hot/Cold pool 모두 최소 한 line 보유
- requested pool 고갈 시 borrowing이 count를 깨지 않음

### Checkpoint E: 두 write pointer

수정 범위:

- `struct ssd`의 `hot_wp`, `cold_wp`
- pointer를 인자로 받는 init/get/advance helper
- `ssd_write()`의 state 기반 선택
- `gc_write_page()`의 state 기반 선택

확인:

- 두 `curline`이 서로 다름
- Hot PPA의 line class는 Hot
- Cold PPA의 line class는 Cold
- PPA를 생성한 pointer만 advance됨
- 두 active line을 고려한 전체 line count가 맞음

### Checkpoint F: 실험용 관찰과 정리

per-page 상시 로그 대신 다음 aggregate counter를 고려한다.

- Hot으로 분류된 host page write 수
- Cold로 분류된 host page write 수
- Hot/Cold GC migration page 수
- Hot→Cold, Cold→Hot 전이 수
- Hot pool/Cold pool borrowing 수
- 현재 두 free pool count

Phase 1 관계도 계속 확인한다.

```text
nand_page_writes == host_page_writes + gc_page_writes
```

---

## 11. 함수별 수정 지도

| 위치 | Phase 2에서 할 일 | 하지 말아야 할 일 |
|---|---|---|
| `ftl.h` enum/type | 세 상태 domain과 LPN metadata 선언 | Hot/Cold와 line lifecycle을 한 enum에 합치기 |
| `struct line` | `data_class`, `state` 추가 | `my_ru` 재사용 또는 삭제 |
| `struct line_mgmt` | class free list/count 추가 | global free list를 즉시 삭제 |
| `struct ssd` | LPN metadata, sequence, 두 WP 추가 | FDP RU pointer를 두 WP로 교체 |
| `ssd_init_lines()` | 모든 line의 기본 state/class 명시 | FDP 여부를 모른 채 class pool에 바로 삽입 |
| 새 pool init helper | non-FDP에서 global list를 두 pool로 이동 | 한 line을 두 list에 동시 삽입 |
| `get_next_free_line()` | FDP용 기존 의미 보존 | 무조건 Hot/Cold pool을 사용하게 변경 |
| 새 class pop helper | non-FDP pool pop, count, borrowing 담당 | call site마다 count를 따로 수정 |
| `ssd_init_write_pointer()` | `wpp`와 class를 인자로 받도록 일반화 | `blk = 0` 가정 |
| `get_new_page()` | 전달받은 `wpp` 좌표로 PPA 생성 | 내부에서 LPN 분류 |
| `ssd_advance_write_pointer()` | 같은 `wpp` 이동, 다음 class line 할당 | 내부에서 항상 `ssd->hot_wp` 선택 |
| `ssd_write()` | host metadata 갱신 후 pointer 선택 | GC용 metadata 갱신 |
| `gc_write_page()` | metadata 조회 후 pointer 선택 | host sequence/write count 증가 |
| `mark_page_invalid()` | FULL→VICTIM 상태 전이 동기화 | page 온도 재분류 |
| `select_victim_line()` | pop한 line을 `GC`로 변경 | 첫 단계부터 class별 victim pq 추가 |
| `mark_line_free()` | line class에 맞는 pool로 반환 | 항상 기존 global free list로 반환 |
| `should_gc*()` | 우선 aggregate free count 유지 | class GC 정책을 동시에 도입 |
| FDP 함수들 | smoke test만 수행 | RU allocation/rotation 정책 수정 |

---

## 12. 반드시 지킬 불변식

### 12.1 LPN metadata

1. `lpn < tt_pgs`인 경우에만 `lpn_meta[lpn]`에 접근한다.
2. `UNSEEN`이면 `write_count == 0`, `last_write_seq == 0`이다.
3. 관찰된 LPN의 `last_write_seq <= host_write_seq`다.
4. sequence는 host page write당 한 번만 증가한다.
5. GC와 read는 host write sequence를 증가시키지 않는다.

TRIM에서 history를 유지하는 정책을 선택한다면 2번은 TRIM 후 unmapped
LPN에는 적용하지 않는다. 그래서 TRIM 정책을 문서와 코드에서 명시해야 한다.

### 12.2 line/container

1. 한 line은 한 QTAILQ에만 들어갈 수 있다.
2. `FREE` line만 free pool에 있다.
3. `OPEN` line은 free/full/victim container 어디에도 없다.
4. `FULL` line만 full list에 있다.
5. `VICTIM` line만 victim pq에 있고 `pos != 0`이다.
6. `GC` line은 victim pq에서 제거되어 `pos == 0`이다.
7. `hot_wp.curline != cold_wp.curline`이다.
8. `wpp->blk == wpp->curline->id`다.
9. `hot_wp.curline->data_class == LINE_CLASS_HOT`이다.
10. `cold_wp.curline->data_class == LINE_CLASS_COLD`이다.

### 12.3 count

non-FDP 안정 상태에서:

```text
free_line_cnt = free_hot_line_cnt + free_cold_line_cnt
```

두 pointer가 모두 정상이고 GC가 실행 중이 아닐 때:

```text
tt_lines =
    free_line_cnt
  + victim_line_cnt
  + full_line_cnt
  + 2 active lines
```

GC가 victim을 pop한 뒤 반환하기 전에는 여기에 GC in-flight line 하나를
추가한다. GC migration 중 active pointer가 새 line을 얻을 수 있으므로
단순히 “GC 한 번이면 free count가 항상 1 증가한다”고 가정하지 않는다.

### 12.4 mapping과 통계

1. 최신 `maptbl[lpn]`의 page는 valid다.
2. 최신 PPA의 `rmap`은 해당 LPN이다.
3. 새 PPA의 line class는 선택한 LPN state와 맞는다.
4. Host page program은 `host_page_writes`와 `nand_page_writes`를 각각
   한 번 증가시킨다.
5. GC page migration은 `gc_page_writes`와 `nand_page_writes`를 각각
   한 번 증가시킨다.
6. GC migration은 `host_page_writes`를 증가시키지 않는다.

---

## 13. 주요 위험 요소

### 13.1 기존 helper가 FDP와 공유됨

`get_next_free_line()`은 non-FDP pointer뿐 아니라
`femu_fdp_init_ssd_reclaim_unit()`도 호출한다. 기존 함수를 class 전용으로
바꾸지 말고, non-FDP용 class helper를 새로 두는 것이 안전하다.

### 13.2 `ssd_init_lines()` 시점에는 FDP 여부가 아직 확정되지 않음

Hot/Cold pool은 `fdp_enabled == false`가 확인된 뒤 별도 단계에서 만든다.
그렇지 않으면 FDP RU 초기화가 사용할 global free list가 비게 된다.

### 13.3 pointer 선택 불일치

다음과 같은 실수가 가장 치명적이다.

```text
hot_wp에서 PPA 생성
    ↓
cold_wp advance
```

allocation과 advance 사이에 mapping/page 상태 변경 코드가 있어 눈으로 찾기
어렵다. 한 iteration 시작에서 `wpp` 지역 변수를 한 번 선택하고 끝까지 같은
포인터를 사용한다.

### 13.4 line class와 LPN의 현재 state가 항상 같지는 않음

Hot line에 기록된 LPN이 나중에 Cold로 내려갈 수 있다. 이미 기록된 page를
즉시 옮기지 않는다. 다음 host overwrite나 GC migration 때 현재 state에 맞는
destination으로 이동한다.

따라서 “Hot LPN의 현재 PPA는 항상 Hot line”이라는 검사는 state 전이 직후에는
성립하지 않을 수 있다. 대신 “새로 쓴 destination의 line class가 write
시점의 state와 일치한다”를 검사한다.

### 13.5 pool 고갈과 GC liveness

aggregate free count만 충분하고 요청 class pool이 빈 상황을 처리하지 않으면
GC threshold에 도달하기 전에 write가 실패한다. 초기에는 borrowing으로
해결하고, class별 GC는 별도 실험 단계로 미룬다.

### 13.6 enum 0 값과 `g_malloc0()`

0을 정상 `COLD`나 `FREE`로 두면 초기화 누락이 정상 상태처럼 보인다.
`UNSEEN`, `NONE`, `UNINITIALIZED`를 0으로 두고 init 함수가 정상 값으로
바꾸도록 한다.

### 13.7 assertion이 release build에서 사라짐

현재 `ftl_assert`는 `FEMU_DEBUG_FTL`이 없으면 빈 매크로다. assertion을
추가한 것만으로 검증이 끝났다고 보면 안 된다. debug build에서 실제로
활성화되었는지 확인하고, 필수 오류 처리는 assertion에만 의존하지 않는다.

### 13.8 per-page 로그의 성능 왜곡

FTL worker는 단일 thread다. 모든 write에서 LPN/state/PPA를 출력하면 I/O
특성을 크게 바꾼다. 짧은 기능 테스트에서만 제한 로그를 켜고, 실험에서는
aggregate counter와 구간 종료 snapshot을 사용한다.

---

## 14. 최소 테스트 계획

### 테스트 A: 작은 순차 write

목적:

- 첫 write가 Cold인지 확인
- Cold pointer만 이동하는지 확인

기대:

- 모든 처음 본 LPN은 `COLD`
- `cold_wp`만 기록한 page 수만큼 이동
- `hot_wp` 좌표는 초기값 유지
- GC가 없으면 WAF는 1.0

### 테스트 B: 같은 LPN 반복 overwrite

목적:

- Cold→Hot 전이와 Hot pointer 사용 확인

기대:

- 첫 write는 Cold
- rewrite distance가 threshold 이하이면 Hot
- 전이 이후 새 PPA의 line은 Hot class
- old Cold page는 정상적으로 invalid 처리

### 테스트 C: 간격을 둔 rewrite

목적:

- Hot→Cold 전이가 가능한지 확인

방법:

1. 특정 LPN을 짧은 간격으로 반복해 Hot으로 만든다.
2. 다른 LPN을 threshold보다 많이 기록한다.
3. 원래 LPN을 다시 기록한다.

기대:

- 다시 Cold로 분류
- 새 destination은 Cold line

### 테스트 D: pool borrowing

목적:

- 한 class 편향 workload에서도 allocation이 멈추지 않는지 확인

기대:

- requested pool이 비면 other pool count가 정확히 감소
- 빌린 line은 requested class로 바뀜
- aggregate count 관계 유지
- 같은 line이 두 free list에 나타나지 않음

### 테스트 E: GC migration

목적:

- GC가 metadata를 변경하지 않고 현재 state에 맞게 이동하는지 확인

기대:

- migration 전후 `host_write_seq`, `write_count`, `last_write_seq` 불변
- destination line class는 현재 LPN state와 일치
- `gc_page_writes`는 증가
- `host_page_writes`는 GC 때문에 증가하지 않음
- WAF 관계 유지

### 테스트 F: FDP smoke test

목적:

- 공용 구조체 변경이 FDP를 깨지 않았는지 확인

기대:

- FDP는 기존 global free list로 RU를 초기화
- `lpn_meta`가 NULL이거나 FDP에서 사용되지 않음
- RU별 `ssd_wptr`, `my_ru`, FDP GC가 기존대로 동작
- Hot/Cold pool count가 FDP bookkeeping에 섞이지 않음

---

## 15. 구현 전 스스로 답할 질문

코드를 쓰기 전에 아래 질문에 답을 적어 보면 각 필드와 helper의 책임이
명확해진다.

1. 첫 write는 왜 Hot이 아니라 Cold인가?
2. 실제 ns 대신 write sequence를 쓰면 실험 재현성이 왜 좋아지는가?
3. GC migration이 LPN write count를 올리면 어떤 feedback loop가 생기는가?
4. `LpnState`와 `LineClass`를 같은 enum으로 쓰지 않는 이유는 무엇인가?
5. active line은 왜 free/full/victim container 어디에도 없는가?
6. QTAILQ entry 하나로 두 free list를 어떻게 안전하게 운영하는가?
7. `free_line_cnt`를 없애지 않고 aggregate로 유지하는 이유는 무엇인가?
8. pool 하나만 비었을 때 기존 `should_gc()`가 왜 문제를 발견하지 못하는가?
9. 두 write pointer 초기화에서 `blk = 0`을 쓰면 왜 같은 line을 가리킬 수
   있는가?
10. `get_new_page()`와 `ssd_advance_write_pointer()`가 같은 `wpp`를
    사용했다는 것을 어떻게 검증할 것인가?
11. Hot LPN이 Cold가 된 순간 기존 물리 page를 즉시 옮겨야 하는가?
12. FDP에서 global free list를 계속 보존해야 하는 이유는 무엇인가?

---

## 16. Phase 2 완료 기준

다음 조건을 모두 만족한 뒤에 class별 victim 선택이나 adaptive threshold 같은
다음 정책으로 넘어간다.

- LPN metadata가 non-FDP에서만 정상 할당·초기화된다.
- Host write만 LPN history를 갱신한다.
- LPN이 Cold↔Hot으로 예상대로 전이한다.
- line의 class와 lifecycle state가 분리되어 있다.
- Hot/Cold free pool의 실제 membership과 count가 일치한다.
- 두 write pointer가 서로 다른 active line을 가진다.
- 한 page의 allocation과 advance가 항상 같은 pointer를 사용한다.
- Host write와 GC migration 모두 현재 LPN state에 맞는 destination을
  선택한다.
- pool 편향이 있어도 borrowing으로 write가 계속 진행된다.
- 기존 mapping 불변식과 Phase 1 WAF 관계가 유지된다.
- FDP smoke test가 기존 동작을 보존한다.

이 기준을 만족하기 전에는 다음을 한꺼번에 추가하지 않는다.

- class별 victim priority queue
- class별 GC threshold
- 동적 pool 비율 최적화
- read temperature
- decay가 여러 단계인 Warm/Hot 정책
- FDP RU의 Hot/Cold 분리

먼저 구조와 불변식을 안정시킨 뒤 정책을 추가해야, WAF 변화가 분류 정책 때문인지
자료구조 오류 때문인지 구분할 수 있다.
