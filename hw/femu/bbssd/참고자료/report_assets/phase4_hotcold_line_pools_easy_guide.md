# Phase 4: Hot/Cold Line Pool 쉽게 이해하기

## 1. 목표와 현재 경계

Phase 4는 non-FDP SSD의 free line을 Hot용과 Cold용으로 나눈 단계다.
```text
전체 free line
├── free_hot_line_list  → wp_hot이 사용
└── free_cold_line_list → wp_cold가 사용
```
한쪽 pool이 비면 반대쪽 line을 빌린다.
Phase 3은 pointer만 두 개였고, 두 pointer 모두 같은 global free list를 사용했다.
Phase 4부터 line의 Hot/Cold 소속도 따로 관리한다.
아직 LPN 상태와 pointer는 연결하지 않았다.
```c
/* 현재 host write와 GC write가 모두 사용하는 pointer */
struct write_pointer *wpp = &ssd->wp_cold;
```
따라서 아직 실제 Hot/Cold 배치나 WAF 개선을 판단하면 안 된다.
온도별 pointer 선택은 Phase 5의 책임이다.
## 2. Line class와 자료구조

GC 후 line을 어느 pool로 반환할지 기억하기 위해 `LineClass`를 추가했다.
```c
typedef enum LineClass {
    LINE_CLASS_NONE = 0,
    LINE_CLASS_COLD,
    LINE_CLASS_HOT,
} LineClass;
```
`NONE`은 아직 non-FDP pool에 배정되지 않은 상태다.
FDP line은 기존 동작을 유지하며 `NONE` 상태로 남는다.
각 line은 자신의 class를 `data_class`에 저장한다.
```c
typedef struct line {
    int id;
    int ipc;
    int vpc;
    QTAILQ_ENTRY(line) entry;
    size_t pos;
    LineClass data_class;
    FemuReclaimUnit *my_ru;
} line;
```
`data_class`는 line이 다음 상태를 이동해도 유지된다.
```text
free → active → full/victim → GC erase → free
```
그래야 GC가 끝난 line을 올바른 class pool로 돌려보낼 수 있다.
## 3. `line_mgmt`의 두 pool과 카운터

기존 global list는 FDP 때문에 삭제하지 않았다.
non-FDP용 list 두 개와 카운터 두 개만 추가했다.
```c
struct line_mgmt {
    struct line *lines;
    /* FDP용 기존 global list */
    QTAILQ_HEAD(free_line_list, line) free_line_list;
    /* non-FDP용 class별 list */
    union free_line_list free_hot_line_list;
    union free_line_list free_cold_line_list;
    pqueue_t *victim_line_pq;
    QTAILQ_HEAD(full_line_list, line) full_line_list;
    int tt_lines;
    int free_line_cnt;
    int free_hot_line_cnt;
    int free_cold_line_cnt;
    int victim_line_cnt;
    int full_line_cnt;
};
```
`free_line_cnt`는 두 non-FDP pool을 합친 전체 free line 수다.
가장 중요한 불변식은 다음과 같다.
```text
free_line_cnt = free_hot_line_cnt + free_cold_line_cnt
```
## 4. 최초 line 초기화와 pool 분리

`ssd_init_lines()`는 세 list와 카운터를 초기화한다.
모든 line은 우선 class 없이 기존 global list에 넣는다.
```c
QTAILQ_INIT(&lm->free_line_list);
QTAILQ_INIT(&lm->free_hot_line_list);
QTAILQ_INIT(&lm->free_cold_line_list);
lm->free_line_cnt = 0;
lm->free_hot_line_cnt = 0;
lm->free_cold_line_cnt = 0;
for (int i = 0; i < lm->tt_lines; i++) {
    line = &lm->lines[i];
    line->id = i;
    line->ipc = 0;
    line->vpc = 0;
    line->pos = 0;
    line->data_class = LINE_CLASS_NONE;
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
}
```
FDP 여부는 그 뒤 `ssd_init()`에서 확인한다.
Hot/Cold pool 분리는 non-FDP 분기에서만 호출한다.
```c
if (ssd->fdp_enabled) {
    /* 기존 FDP 초기화 */
} else {
    ssd_init_hotcold_line_pools(ssd);
}
```
이 순서 때문에 FDP는 기존 global `free_line_list`를 그대로 사용할 수 있다.
## 5. 초기 50:50 분배

첫 구현은 검증하기 쉬운 50:50 비율이다.
Line 수가 홀수면 Cold pool이 하나 더 많다.
```c
static void ssd_init_hotcold_line_pools(struct ssd *ssd)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;
    int cold_target = (lm->tt_lines + 1) / 2;
    while ((line = QTAILQ_FIRST(&lm->free_line_list)) != NULL) {
        QTAILQ_REMOVE(&lm->free_line_list, line, entry);
        if (lm->free_cold_line_cnt < cold_target) {
            line->data_class = LINE_CLASS_COLD;
            QTAILQ_INSERT_TAIL(&lm->free_cold_line_list, line, entry);
            lm->free_cold_line_cnt++;
        } else {
            line->data_class = LINE_CLASS_HOT;
            QTAILQ_INSERT_TAIL(&lm->free_hot_line_list, line, entry);
            lm->free_hot_line_cnt++;
        }
    }
    ssd_validate_free_line_counts(ssd);
}
```
이때 `free_line_cnt`는 바꾸지 않는다.
Line을 사용한 것이 아니라 global list에서 class list로 옮겼기 때문이다.
50:50은 최적값이 아니라 첫 동작 검증을 위한 초기값이다.
## 6. 카운터 불변식 검사

초기 분배, line 할당, GC 반환 뒤마다 다음 helper가 호출된다.
```c
static void ssd_validate_free_line_counts(struct ssd *ssd)
{
    struct line_mgmt *lm = &ssd->lm;
    if (lm->free_line_cnt < 0 || lm->free_hot_line_cnt < 0 ||
        lm->free_cold_line_cnt < 0 ||
        lm->free_line_cnt !=
        lm->free_hot_line_cnt + lm->free_cold_line_cnt) {
        ftl_err("invalid Hot/Cold free line counts: ...");
        abort();
    }
}
```
음수이거나 전체와 부분 합이 다르면 즉시 중단한다.
잘못된 count로 계속 실행하면 line 중복 할당이나 잘못된 GC로 이어질 수 있다.
## 7. 공통 line 제거 helper

Hot용과 Cold용 코드를 복사하지 않고 list와 count를 인자로 받는다.
```c
static struct line *pop_free_line(union free_line_list *list, int *count)
{
    struct line *line = QTAILQ_FIRST(list);
    if (!line) {
        return NULL;
    }
    if (*count <= 0) {
        abort();
    }
    QTAILQ_REMOVE(list, line, entry);
    (*count)--;
    return line;
}
```
List가 비면 `NULL`을 반환한다.
Line이 있으면 list 제거와 해당 class count 감소를 한곳에서 처리한다.
## 8. Class 선택과 borrowing

Class별 allocator는 필요한 class를 인자로 받는다.
```c
static struct line *get_next_free_line_by_class(struct ssd *ssd,
                                                LineClass data_class)
```
Hot 요청은 Hot을 우선하고 Cold를 fallback으로 둔다.
Cold 요청은 반대로 설정한다.
```c
if (data_class == LINE_CLASS_HOT) {
    requested = &lm->free_hot_line_list;
    requested_cnt = &lm->free_hot_line_cnt;
    fallback = &lm->free_cold_line_list;
    fallback_cnt = &lm->free_cold_line_cnt;
} else if (data_class == LINE_CLASS_COLD) {
    requested = &lm->free_cold_line_list;
    requested_cnt = &lm->free_cold_line_cnt;
    fallback = &lm->free_hot_line_list;
    fallback_cnt = &lm->free_hot_line_cnt;
}
```
요청 pool이 비면 반대 pool에서 빌린다.
```c
line = pop_free_line(requested, requested_cnt);
if (!line) {
    line = pop_free_line(fallback, fallback_cnt);
    borrowed = true;
}
if (!line) {
    return NULL;
}
line->data_class = data_class;
lm->free_line_cnt--;
ssd_validate_free_line_counts(ssd);
```
빌린 line은 요청 class로 재분류된다.
예를 들어 Cold가 Hot line을 빌리면 그 line은 이후 Cold pool로 돌아간다.
한 번의 할당에서 class count와 전체 count는 각각 정확히 한 번 감소한다.
두 pool이 모두 비었을 때만 할당이 실패한다.
## 9. 두 write pointer와 다음 active line

초기화할 때 각 pointer가 필요한 class를 명시한다.
```c
ssd_init_write_pointer(ssd, &ssd->wp_cold, LINE_CLASS_COLD);
ssd_init_write_pointer(ssd, &ssd->wp_hot, LINE_CLASS_HOT);
ssd_validate_write_pointers(ssd);
```
검증 조건은 다음과 같다.
```text
hot->curline != cold->curline
hot->curline->data_class == LINE_CLASS_HOT
cold->curline->data_class == LINE_CLASS_COLD
각 wpp->blk == wpp->curline->id
```
Active line을 다 쓰면 `ssd_advance_write_pointer()`가 현재 class를 기억한다.
기존 line은 기존 정책대로 full list나 victim queue로 이동한다.
새 active line은 같은 class에서 할당한다.
```c
LineClass data_class = wpp->curline->data_class;
if (wpp->curline->vpc == spp->pgs_per_line) {
    QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
    lm->full_line_cnt++;
} else {
    pqueue_insert(lm->victim_line_pq, wpp->curline);
    lm->victim_line_cnt++;
}
wpp->curline = get_next_free_line_by_class(ssd, data_class);
wpp->blk = wpp->curline->id;
ssd_validate_write_pointers(ssd);
```
`get_new_page()`와 advance는 계속 같은 `wpp`를 사용한다.
## 10. GC 후 line 반환

GC가 valid page 복사와 block erase를 마치면 `mark_line_free()`가 호출된다.
```c
line->ipc = 0;
line->vpc = 0;
if (line->data_class == LINE_CLASS_HOT) {
    QTAILQ_INSERT_TAIL(&lm->free_hot_line_list, line, entry);
    lm->free_hot_line_cnt++;
} else if (line->data_class == LINE_CLASS_COLD) {
    QTAILQ_INSERT_TAIL(&lm->free_cold_line_list, line, entry);
    lm->free_cold_line_cnt++;
} else {
    abort();
}
lm->free_line_cnt++;
ssd_validate_free_line_counts(ssd);
```
Hot line은 Hot pool로, Cold line은 Cold pool로 돌아간다.
한 번의 반환에서 class count와 전체 count는 각각 정확히 한 번 증가한다.
## 11. 기존 GC와 FDP를 보존한 부분

Victim 선택은 기존 global priority queue를 그대로 사용한다.
GC threshold도 class별 count가 아닌 전체 `free_line_cnt`를 사용한다.
```c
lm->victim_line_pq
ssd->lm.free_line_cnt
```
FDP reclaim unit은 기존 global allocator를 계속 호출한다.
```c
femu_ru->lines[i] = get_next_free_line(ssd);
```
Hot/Cold pool 초기화는 non-FDP 분기에서만 호출된다.
따라서 FDP RU, FDP pointer, mapping/rmap, TRIM, NAND timing 경로는 바뀌지 않았다.
## 12. 128-line 동작 예시

현재 geometry가 128 line이면 다음처럼 나뉜다.
```text
Cold pool: line 0~63   = 64개
Hot pool : line 64~127 = 64개
```
Cold pointer가 line 0, Hot pointer가 line 64를 가져간 뒤 상태는 다음과 같다.
```text
전체 free = 126
Cold free = 63
Hot free  = 63
126 = 63 + 63
```
예상 초기 로그는 다음과 같다.
```text
Write pointers: hot_line=64 cold_line=0 free=126 hot_free=63 cold_free=63
```
Cold line 하나의 상태 변화는 다음과 같다.
```text
Cold free pool
→ wp_cold active line
→ full list 또는 victim queue
→ GC copy와 erase
→ Cold free pool
```
Borrowing한 line도 재분류된 class를 따라 같은 순서로 이동한다.
## 13. 불변식과 현재 위험

```text
1. total free = hot free + cold free
2. 모든 free count는 0 이상
3. 한 line은 동시에 두 free list에 존재하지 않음
4. active line은 free list나 victim queue에 존재하지 않음
5. wp_hot과 wp_cold는 같은 line을 가리키지 않음
6. 할당 시 total/class count가 각각 한 번 감소
7. GC 반환 시 total/class count가 각각 한 번 증가
8. FDP는 기존 global free list를 사용
```
현재 host와 GC가 모두 `wp_cold`를 사용하므로 Cold pool이 먼저 줄어드는 것이 정상이다.
Cold pool이 비면 Hot pool에서 borrowing하여 일부 line이 Cold로 바뀐다.
이것은 오류가 아니라 고정 분할로 공간이 남는 것을 막는 동작이다.
## 14. 검증 방법과 다음 단계

Host에서 diff와 빌드를 확인한다.
```bash
cd /root/workspace/FEMU
git diff --check
ninja -C build-femu qemu-system-x86_64
```
non-FDP Blackbox 실행 후 초기 로그를 확인한다.
```bash
grep 'Write pointers:' build-femu/log
```
128-line 설정의 정상 조건은 다음과 같다.
```text
hot_line != cold_line
free == hot_free + cold_free
126 == 63 + 63
```
짧은 write workload에서도 assertion이나 abort가 없어야 한다.
FDP 모드에서는 non-FDP pointer 초기화 로그가 나오지 않아야 한다.
이 검증 뒤 Phase 5에서만 LPN 상태와 pointer를 연결한다.
```text
LPN_STATE_HOT → wp_hot
그 외         → wp_cold
```
