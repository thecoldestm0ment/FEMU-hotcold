전체 작업 순서
단계	내용	성격	우선도
0	현재 동작 버전 보존	안전장치	최우선
1	실제 시간 window 기반 LBA frequency 구현	요구사항 보정	최우선
2	Block erase count 의미 명확화	요구사항 보정	최우선
3	코드 이름/주석/Phase/V3 흔적 정리	리팩토링	높음
4	Counter 다이어트 + saturation 방어 제거	리팩토링	높음
5	Borrowing minimum reserve	동작 개선	높음
6	Pool ratio를 runtime parameter화	실험 준비	높음
7	20:80 / 30:70 / 50:50 sweep	실험	높음
8	Erase signal coverage 개선	알고리즘 실험	중간
9	Class-aware GC	별도 확장	마지막

나는 1~5까지 먼저 끝내고 correctness test를 돌린 다음, 6~9는 성능 실험으로 넘어가는 걸 추천해.

0. 제일 먼저 현재 버전 얼리기

코드 수정 전에 현재 동작 코드를 반드시 보존해.

현재 코드는 WAF accounting, Hot/Cold pool, classifier, GC routing까지 이미 작동하는 상태다. 특히 NAND = Host + GC 같은 invariant도 있어서 이걸 기준점으로 삼을 수 있다.

예를 들어 브랜치를 이렇게 가져가면 좋다.

hotcold/current-result
        ↓
hotcold/requirement-fix
        ↓
hotcold/refactor
        ↓
hotcold/pool-reserve
        ↓
hotcold/pool-sweep
        ↓
hotcold/erase-tuning
        ↓
hotcold/class-aware-gc

중요: 리팩토링과 성능 개선을 같은 commit에 섞지 않는 게 좋다.

교수님한테도 나중에

“이 commit은 behavior-preserving refactor이고, 다음 commit부터 정책을 변경했다.”

라고 설명할 수 있어.

1. 「일정 시간 동안 LBA 요청 빈도」 — 이건 수정하는 걸 추천

현재는:

update_interval =
    host_write_seq - last_write_seq;

방식이다.

즉 1024라는 값은

1024개의 Host page write가 사이에 있었다

는 의미이지

100ms 동안 몇 번 접근했다

는 의미가 아니다.

따라서 과제 문구를 확실히 충족시키고 싶다면 진짜 time window를 넣는 게 좋다.

추천 구현

LPN metadata를 예를 들어 이렇게 바꾼다.

typedef struct LpnMeta {
    uint64_t window_start_ns;
    uint32_t writes_in_window;

    uint64_t last_write_seq;
    uint64_t rewrite_interval;

    uint32_t erase_event_count;

    LpnState state;
} LpnMeta;

Host write가 들어오면 이미 NvmeRequest에 req->stime이 있으니까 이 시간을 사용하면 된다.

개념적으로:

now = req->stime;

if (now - meta->window_start_ns >= frequency_window_ns) {
    classify using meta->writes_in_window;
    meta->window_start_ns = now;
    meta->writes_in_window = 0;
}

meta->writes_in_window++;

이렇게 하면 보고서에 진짜로

“일정 시간 window 동안 각 LPN의 Host write 횟수를 측정하였다.”

라고 쓸 수 있다.

중요한 점

QEMU의 실제 실행 wall-clock을 직접 부르기보다는 req->stime을 쓰는 쪽을 추천해.

왜냐하면 우리가 측정하고 싶은 건 Host I/O request timing이지, FEMU 프로세스가 CPU scheduling 때문에 얼마나 늦게 실행됐는지가 아니기 때문이다.

Frequency classifier는 너무 복잡하게 만들지 마

예를 들면:

writes_in_window >= HOT_FREQ_THRESHOLD
        → HOT

writes_in_window <= COLD_FREQ_THRESHOLD
        → COLD

그 사이
        → erase signal 사용

이 정도면 과제 요구사항과도 딱 맞는다.

예:

1 second window

writes >= 3 → HOT
writes == 1 → COLD
writes == 2 → erase count로 결정

단, 1초 / 3회 같은 숫자를 바로 최종값으로 박지는 말고 tuning run으로 정해야 한다.

2. 「SSD 내부 블록 지우는 횟수」 — 새로 완전히 만들 필요는 없음

이 부분은 생각보다 덜 오래 걸린다.

현재 코드에 이미:

struct nand_block {
    ...
    int erase_cnt;
};

가 있고, SSD 전체 측정값도:

uint64_t block_erases;

가 있다.

그리고 실제 erase에서 증가한다.

현재 erase_survival_count 역시 실제 source block erase 시점에서 증가한다.

그러니까 erase event 자체를 fake하게 추정하고 있는 건 아니다.

문제는 이름과 의미가 좀 돌아가 있다는 것이다.

내가 추천하는 수정

erase_survival_count를 과제 요구사항에 더 직접 대응하도록 이름부터 바꾸는 게 좋다.

예:

uint32_t erase_event_count;

혹은 더 명확하게:

uint32_t lpn_erase_event_count;

그리고 주석:

/* Current logical version이 valid 상태로 경험한
 * source-block erase event 수 */

분류에서는:

최근 time window의 LBA frequency
+
LPN과 연관된 actual block erase event count

를 사용한다고 설명한다.

이러면 교수님이

“블록 지우는 횟수 어디서 쓰나요?”

라고 하면 정확히:

mark_block_free()
    ↓
실제 block erase
    ↓
blk->erase_cnt++
ssd->block_erases++
LPN의 erase_event_count++
    ↓
classifier

라고 보여줄 수 있다.

이 흐름이 훨씬 강하다.

굳이 global block_erases를 classifier에 넣지는 마

예를 들어

if (ssd->block_erases > 10000)
    ...

처럼 만드는 건 비추천이다.

모든 LPN이 같은 값을 보게 되니까 Hot/Cold 구분력이 거의 없다.

현재처럼

실제 block erase event를 LPN current version과 연결

하는 아이디어가 더 낫다.

즉 설계 자체를 버릴 필요는 없고, 의미/이름을 정리하면 된다.

이 두 요구사항 수정 오래 걸리냐?

코드 작성 자체는 그렇게 크지 않다.

현재 구조가 이미 classifier가 한 함수에 모여 있어서 수정 위치가 명확하다. 지금 runtime parameter loader도 이미 존재한다.

대략 작업량 감각은:

작업	난이도
time-window metadata 추가	★★☆☆☆
req->stime 전달	★★☆☆☆
frequency counter 구현	★★☆☆☆
erase signal rename/정리	★☆☆☆☆
classifier 수정	★★☆☆☆
correctness test	★★★☆☆
3600초 성능 재실험	시간 대부분 차지

구현보다 실험이 오래 걸린다.

코딩+간단 검증은 몇 시간 안쪽 규모인데, 최종 configuration마다 workload가 1시간이므로 제대로 비교하려면 실험 시간이 훨씬 길어진다.

3. 이름부터 정리하기

이건 requirement fix 직후에 한다.

현재:

HOT_REWRITE_WINDOW_DEFAULT
HOT_BOUNDARY_WINDOW_DEFAULT

hot_rewrite_window
hot_boundary_window

인데 실제론 시간 window가 아니었다.

진짜 time window를 추가하면 더 헷갈린다.

추천 이름

기존 rewrite interval 관련:

FAST_REWRITE_THRESHOLD_DEFAULT
SLOW_REWRITE_THRESHOLD_DEFAULT

fast_rewrite_threshold
slow_rewrite_threshold

진짜 frequency window:

FREQUENCY_WINDOW_NS_DEFAULT
HOT_WRITES_PER_WINDOW_DEFAULT

frequency_window_ns
hot_writes_per_window

Erase:

ERASE_EVENT_THRESHOLD_DEFAULT
erase_event_threshold

LPN:

last_write_seq
rewrite_interval

window_start_ns
writes_in_window

erase_event_count

이름만 봐도 의미가 거의 다 드러난다.

4. Phase 1, Phase 3, V3 흔적 제거

이건 무조건 정리해도 된다. 동작에 영향 없다.

현재 header에 실제로:

/* non-FDP Phase 3 ... */
struct write_pointer wp_hot;
struct write_pointer wp_cold;

/* non-FDP Phase 1 ... */
uint64_t host_page_writes;
...

그리고

/* non-FDP V3 ... */

가 남아 있다.

이건 다음처럼 바꾼다.

/* Hot/Cold placement */
struct write_pointer wp_hot;
struct write_pointer wp_cold;

/* WAF and GC statistics */
...

/* LPN classification metadata */
LpnMeta *lpn_meta;

출력도:

BBSSD-STATS version=V3

보다는

BBSSD-HOTCOLD-STATS

정도로 하면 더 자연스럽다.

5. Defensive counter 다이어트

현재 counter가 꽤 많다.

host_page_writes
nand_page_writes
gc_page_writes
block_erases
gc_count

host_hot_writes
host_cold_writes

host_cold_first_writes
host_hot_fast_writes
host_hot_boundary_writes
host_cold_survival_writes
host_cold_slow_writes

boundary_survival_zero
boundary_survival_one
boundary_survival_two
boundary_survival_three_plus

gc_hot_writes
gc_cold_writes

cold_to_hot_count
hot_to_cold_count

hot_pool_empty_count
cold_pool_empty_count
borrow_count
emergency_gc_count
최종 제출 코드에서 반드시 유지할 것
host_page_writes
nand_page_writes
gc_page_writes
block_erases
gc_count

host_hot_writes
host_cold_writes

erase_assisted_decisions

borrow_count
emergency_gc_count

이 정도면 충분하다.

실험할 때만 있으면 좋은 것
gc_hot_writes / gc_cold_writes
cold_to_hot / hot_to_cold
boundary histogram
pool empty count

이건 아예 삭제보다는:

#ifdef HOTCOLD_DEBUG_STATS
...
#endif

안으로 넣는 걸 추천한다.

그러면 필요할 땐 다시 켤 수 있다.

더 예쁘게 하려면 Stats struct로 묶어

지금 struct ssd가 너무 길어진다.

추천:

typedef struct HotColdStats {
    uint64_t host_writes;
    uint64_t nand_writes;
    uint64_t gc_writes;
    uint64_t block_erases;
    uint64_t gc_count;

    uint64_t hot_writes;
    uint64_t cold_writes;

    uint64_t erase_assisted_decisions;
    uint64_t borrow_count;
    uint64_t emergency_gc_count;
} HotColdStats;

그리고:

struct ssd {
    ...
    HotColdStats stats;
};

이게 훨씬 깔끔하다.

단, 이건 behavior-preserving refactor commit으로 따로 하자.

6. Saturation 방어 제거

현재 코드에:

if (ssd->host_write_seq != UINT64_MAX)
    ssd->host_write_seq++;
if (meta->write_count != UINT32_MAX)
    meta->write_count++;

같은 부분이 있다.

이건 최종 과제에서는 거의 필요 없다.

특히 write_count는 더 좋은 방법이 있다.

현재:

write_count == 0

으로 first write인지 확인하는데,

이미:

LPN_STATE_UNSEEN

이 있다.

그러니까:

if (meta->state == LPN_STATE_UNSEEN) {
    ...
}

으로 하면 된다.

그럼 아예:

uint32_t write_count;

를 삭제할 수도 있다.

이건 좋은 리팩토링이다.

7. Borrowing minimum reserve

이건 꼭 추가하는 게 좋다.

현재 로직은 requested pool이 비면 바로 fallback에서 꺼낸다.

즉:

Cold free = 0
Hot free  = 1

Cold 요청
→ Hot 마지막 1개를 가져감
→ Hot free = 0

도 가능하다.

추천 정책:

requested pool 존재
→ 그대로 allocation

requested pool 없음
AND fallback free > reserve
→ borrowing

fallback free <= reserve
→ borrowing 하지 않음
→ GC

GC 후 다시 allocation
구현 구조

상수 박는 것보다는 runtime parameter를 추천한다.

#define POOL_MIN_RESERVE_DEFAULT 2

또는:

ssd->pool_min_reserve

그리고:

if (!line && *fallback_cnt > ssd->pool_min_reserve) {
    line = pop_free_line(fallback, fallback_cnt);
    borrowed = true;
}

없으면:

if (!line) {
    do_gc(ssd, true);
    ...
}
그런데 여기서 중요한 점

현재 get_next_free_line_by_class()에서 바로 do_gc()를 호출하게 만들면 GC → GC relocation → write pointer allocation → 다시 GC 같은 호출 관계가 꼬일 가능성을 확인해야 한다.

그래서 더 깔끔한 구조는:

allocator
→ "NO_FREE_LINE" 반환

caller
→ GC 실행
→ allocator 재시도

다.

즉 allocation helper에 GC 정책까지 넣지 않는 게 좋다.

8. Pool ratio sweep 전에 ratio를 코드 상수로 박지 마

현재:

cold_target = (lm->tt_lines + 1) / 2;

라서 50:50이다.

이걸 매번 소스 수정해서

20:80
30:70
50:50

돌리면 결과 관리가 귀찮아진다.

runtime parameter로 바꿔

예:

FEMU_HOT_POOL_PERCENT=20

코드:

ssd->hot_pool_percent =
    load_percentage("FEMU_HOT_POOL_PERCENT", 50);

hot_target =
    lm->tt_lines * ssd->hot_pool_percent / 100;

cold_target =
    lm->tt_lines - hot_target;

그러면 같은 binary로:

FEMU_HOT_POOL_PERCENT=20 ./run-blackbox.sh
FEMU_HOT_POOL_PERCENT=30 ./run-blackbox.sh
FEMU_HOT_POOL_PERCENT=50 ./run-blackbox.sh

할 수 있다.

실험 신뢰도도 더 높아진다.

9. Pool ratio sweep

여기부터가 성능 실험이다.

처음에는:

Hot:Cold	목적
20:80	현재 Hot ratio ≈15% 근처
30:70	약간 여유 있는 Hot pool
50:50	기존 V3 기준

정도면 충분하다.

나는 지금 단계에서 10:90까지는 굳이 안 한다.

Hot ratio가 15%라고 해서 Hot pool을 정확히 15%로 주는 게 최적이라는 보장은 없기 때문이다.

Hot data는 GC turnover도 빠르니까 write ratio보다 Hot physical capacity가 더 필요할 수도 있다.

측정값은:

WAF
Average GC copy
GC / million Host writes
IOPS
borrow_count
Hot/Cold pool exhaustion

정도면 충분.

10. Erase signal 0.133% 문제는 그 다음

이건 지금 바로 classifier부터 뜯지 마.

먼저 진짜 time-frequency + erase requirement를 만든 상태에서 다시 coverage를 봐야 한다.

classifier 자체가 바뀌니까 기존 0.133%는 더 이상 유지된다는 보장이 없다.

새 classifier에서 먼저:

Frequency HOT
Frequency COLD
Boundary
Boundary + erase → HOT/COLD

의 분포를 본다.

그 후에도 erase signal이 0.1% 수준이면 그때 threshold를 조정한다.

여기서 tuning seed 분리

이건 꼭 유지하자.

Tuning
seed = 20260823

Final evaluation
seed = 20260824

현재 했던 방식이 좋다.

Tuning 결과를 보고 parameter를 정한 뒤 final seed 결과는 한 번만 평가한다.

11. Class-aware GC는 진짜 마지막

이건 지금 바로 넣지 않는 걸 강하게 추천한다.

왜냐하면 현재 네 실험의 장점은:

기존 GC 그대로
+
Hot/Cold placement만 변경

이라서 인과관계가 깨끗하다는 점이다.

Class-aware GC를 넣으면:

Classifier
+
Placement
+
Pool ratio
+
GC victim policy

가 한꺼번에 바뀐다.

그때는 반드시 별도 configuration으로:

Baseline
Hot/Cold placement
Hot/Cold placement + reserve
Hot/Cold placement + tuned pool ratio
Hot/Cold placement + class-aware GC

순으로 비교해야 한다.

내가 추천하는 최종 작업 단위

이 순서 그대로 가자.

STEP 1
과제 요구사항 literal compliance
- 실제 time-window LBA frequency
- block erase event signal 명확화

STEP 2
behavior-preserving refactor
- 이름 변경
- Phase/V3 제거
- 자명한 주석 제거
- saturation guard 제거
- counter 정리

STEP 3
pool safety
- minimum reserve
- borrowing/GC 재시도 구조

STEP 4
pool experiment infrastructure
- FEMU_HOT_POOL_PERCENT runtime parameter

STEP 5
pool ratio sweep
- 20:80
- 30:70
- 50:50

STEP 6
classifier coverage 분석
- frequency 분포
- erase decision coverage

STEP 7
erase threshold/window tuning
- tuning seed만 사용

STEP 8
최종 3600초 evaluation

STEP 9 (선택)
class-aware GC

그리고 STEP 1 끝날 때 기존 WAF와 바로 비교하지 말고 correctness부터 보자. FIRST/HOT/COLD/erase branch, time-window reset, erase count 증가, GC가 Host frequency counter를 건드리지 않는지까지 확인한 뒤 다음 단계로 가는 게 맞다.

다음에는 바로 STEP 1 — 현재 ftl.c/ftl.h 기준으로 “진짜 시간 window + 실제 block erase signal”을 어떻게 설계할지 함수/필드 단위 수정안부터 잡으면 된다. 여기서는 코드량을 최대한 적게 유지하는 방향으로 짜는 게 좋다.