# Phase 3: 두 Write Pointer 쉽게 이해하기
## 1. 이 단계의 한 문장 목표
Phase 3는 non-FDP write pointer를 두 개로 나눈다.
이름은 `wp_hot`과 `wp_cold`다.
두 pointer는 서로 다른 active line을 가져야 한다.
공통 helper가 전달받은 pointer만 사용하게 만든다.
아직 LPN 온도에 따라 pointer를 선택하지 않는다.
즉, 이 단계는 배치 정책보다 구조를 먼저 준비한다.
## 2. Phase 2와 무엇이 다른가
Phase 2는 LPN의 상태만 계산했다.
그 상태는 `UNSEEN`, `COLD`, `HOT` 중 하나다.
하지만 Phase 2에서는 실제 PPA 위치가 달라지지 않았다.
Phase 3는 두 개의 독립적인 기록 위치를 만든다.
분류와 기록 위치를 연결하는 작업은 아직 하지 않는다.
그 연결은 이후 host write 배치 단계의 책임이다.
## 3. Phase 3에서 하지 않는 일
Hot free line list를 만들지 않는다.
Cold free line list도 만들지 않는다.
Line에 `data_class`를 붙이지 않는다.
LPN state에 따른 pointer selection을 하지 않는다.
GC migration을 Hot 또는 Cold로 보내지 않는다.
Victim priority queue를 분리하지 않는다.
GC threshold를 class별로 나누지 않는다.
## 4. 기존 구조
기존 non-FDP 구조에는 pointer가 하나였다.
```c
struct write_pointer wp;
```
`get_new_page()`는 항상 이 필드를 내부에서 선택했다.
`ssd_advance_write_pointer()`도 같은 필드를 내부에서 선택했다.
Pointer가 하나일 때는 문제가 없었다.
두 개가 되면 helper 내부의 고정 선택을 제거해야 한다.
## 5. 변경된 `struct ssd`
기존 `wp`를 다음 두 필드로 바꿨다.
```c
struct write_pointer wp_hot;
struct write_pointer wp_cold;
```
두 필드는 non-FDP용이다.
FDP의 RU별 `ssd_wptr`는 그대로 남아 있다.
이름이 다르므로 FDP pointer와 의미가 섞이지 않는다.
## 6. Write pointer가 저장하는 값
`struct write_pointer`는 다음 위치를 저장한다.
```c
struct write_pointer {
    struct line *curline;
    int ch;
    int lun;
    int pg;
    int blk;
    int pl;
};
```
`curline`은 현재 쓰고 있는 superblock이다.
나머지 필드는 그 line 안의 다음 PPA 좌표다.
## 7. Active line의 의미
Active line은 write pointer가 현재 사용 중인 line이다.
Free list에서 이미 제거된 상태다.
아직 full list에 들어가지 않았다.
Victim priority queue에도 들어가지 않았다.
따라서 active line은 GC victim으로 선택되면 안 된다.
두 pointer를 만들면 active line도 두 개가 된다.
## 8. 기존 global free list 유지
Phase 3는 기존 global free list를 그대로 사용한다.
```c
QTAILQ_HEAD(free_line_list, line) free_line_list;
```
Hot 전용 list와 Cold 전용 list는 아직 없다.
두 pointer 모두 이 list에서 line을 하나씩 받는다.
한 line을 꺼낼 때 aggregate count를 한 번 줄인다.
```c
lm->free_line_cnt--;
```
## 9. 공통 free line helper
Line을 꺼내는 책임은 기존 helper에 남겼다.
```c
static struct line *get_next_free_line(struct ssd *ssd)
```
이 함수는 list의 첫 line을 찾는다.
Line이 없으면 오류를 출력하고 `NULL`을 반환한다.
Line이 있으면 list에서 한 번 제거한다.
Free count도 이 함수에서만 한 번 감소한다.
## 10. 초기화 helper의 변경
초기화 helper는 pointer를 인자로 받는다.
```c
static void ssd_init_write_pointer(
    struct ssd *ssd,
    struct write_pointer *wpp)
```
Hot과 Cold용 함수를 따로 복사하지 않았다.
같은 함수에 서로 다른 pointer 주소를 전달한다.
이것이 중복 코드를 줄이는 핵심이다.
## 11. 초기화 helper의 line 할당
먼저 global free list에서 line을 하나 얻는다.
```c
struct line *curline = get_next_free_line(ssd);
```
Free line이 없으면 계속 진행할 수 없다.
```c
if (!curline) {
    abort();
}
```
잘못된 pointer로 page를 기록하는 것보다 즉시 중단하는 편이 안전하다.
## 12. 초기 좌표 설정
새 pointer는 line의 첫 page에서 시작한다.
```c
wpp->curline = curline;
wpp->ch = 0;
wpp->lun = 0;
wpp->pg = 0;
wpp->blk = curline->id;
wpp->pl = 0;
```
특히 `blk`를 0으로 고정하면 안 된다.
두 번째 pointer는 보통 line 1을 받기 때문이다.
반드시 `curline->id`를 사용해야 한다.
## 13. 초기화 순서
non-FDP 분기에서 두 pointer를 초기화한다.
```c
ssd_init_write_pointer(ssd, &ssd->wp_cold);
ssd_init_write_pointer(ssd, &ssd->wp_hot);
```
Cold를 먼저 초기화한 이유가 있다.
기존 단일 pointer처럼 첫 line 0에서 baseline write를 시작하기 위해서다.
Hot은 다음 line 1을 받는다.
## 14. 서로 다른 line이 되는 이유
Cold 초기화가 line 0을 free list에서 제거한다.
따라서 Hot 초기화가 같은 line 0을 다시 받을 수 없다.
Hot은 다음 free line을 받는다.
정상 초기값은 다음과 같다.
```text
wp_cold.curline->id = 0
wp_hot.curline->id = 1
```
List membership이 정확해야 이 관계가 유지된다.
## 15. 명시적인 검증 helper
List 동작만 믿지 않고 별도 검사를 추가했다.
```c
static void ssd_validate_write_pointers(struct ssd *ssd)
```
이 함수는 두 pointer가 모두 line을 가졌는지 확인한다.
두 `curline`이 같은지도 확인한다.
각 `blk`가 `curline->id`와 같은지도 확인한다.
오류가 있으면 release build에서도 중단한다.
## 16. 핵심 검증 코드
검사 조건은 다음과 같다.
```c
if (!hot->curline || !cold->curline ||
    hot->curline == cold->curline ||
    hot->blk != hot->curline->id ||
    cold->blk != cold->curline->id) {
    abort();
}
```
`ftl_assert`만 사용하지 않은 이유가 있다.
현재 release build에서는 `ftl_assert`가 사라질 수 있기 때문이다.
## 17. 초기화 후 로그
두 pointer를 만든 뒤 한 번만 로그를 출력한다.
```c
ftl_log("Write pointers: hot_line=%d "
        "cold_line=%d free_lines=%d\n", ...);
```
8 GiB geometry에는 line이 128개 있다.
두 line을 active pointer가 가져가면 126개가 남는다.
기대 로그는 다음과 같다.
```text
hot_line=1 cold_line=0 free_lines=126
```
## 18. `get_new_page()` 변경
기존 함수는 내부에서 `ssd->wp`를 골랐다.
Phase 3에서는 pointer를 직접 받는다.
```c
static struct ppa get_new_page(
    struct write_pointer *wpp)
```
이 함수는 분류 정책을 모른다.
전달받은 pointer의 좌표만 PPA로 복사한다.
그래서 Hot과 Cold에 같은 함수를 사용할 수 있다.
## 19. PPA 생성 코드
PPA는 다음처럼 만들어진다.
```c
ppa.g.ch = wpp->ch;
ppa.g.lun = wpp->lun;
ppa.g.pg = wpp->pg;
ppa.g.blk = wpp->blk;
ppa.g.pl = wpp->pl;
```
`get_new_page()`는 pointer를 이동시키지 않는다.
현재 좌표를 읽기만 한다.
Page valid 처리도 이 함수의 책임이 아니다.
## 20. Advance helper 변경
Pointer 이동 함수도 pointer를 인자로 받는다.
```c
static void ssd_advance_write_pointer(
    struct ssd *ssd,
    struct write_pointer *wpp)
```
함수 내부에서 `ssd->wp_hot`을 임의로 선택하지 않는다.
`ssd->wp_cold`도 임의로 선택하지 않는다.
호출자가 전달한 `wpp`만 이동한다.
## 21. Pointer 이동 순서
현재 순회 순서는 기존과 같다.
```text
channel
→ LUN
→ page
→ 다음 line
```
Channel 끝에서는 channel을 0으로 되돌리고 LUN을 올린다.
모든 LUN이 끝나면 page를 올린다.
Block의 모든 page가 끝나면 line이 완성된다.
Phase 3는 이 순서를 바꾸지 않았다.
## 22. 사용 완료 line 처리
Pointer가 line 끝에 도달하면 기존 정책을 유지한다.
모든 page가 valid이면 full list로 보낸다.
```c
QTAILQ_INSERT_TAIL(
    &lm->full_line_list, wpp->curline, entry);
```
Invalid page가 있으면 victim priority queue로 보낸다.
```c
pqueue_insert(lm->victim_line_pq, wpp->curline);
```
Hot/Cold class는 아직 판단하지 않는다.
## 23. 새 line으로 이동
기존 line을 container에 넣은 뒤 새 free line을 받는다.
```c
wpp->curline = get_next_free_line(ssd);
wpp->blk = wpp->curline->id;
```
Page, LUN, channel은 모두 0이어야 한다.
새 line도 page 0부터 기록한다.
교체가 끝나면 두 pointer 검증 helper를 다시 호출한다.
따라서 rotation 뒤에도 active line 중복을 확인한다.
## 24. 같은 pointer 사용 불변식
Page를 얻은 pointer와 이동시키는 pointer가 같아야 한다.
정상 흐름은 다음과 같다.
```c
ppa = get_new_page(wpp);
...
ssd_advance_write_pointer(ssd, wpp);
```
첫 줄에서 Cold를 사용하고 두 번째 줄에서 Hot을 이동시키면 안 된다.
그런 오류는 한 page를 쓰고 다른 위치를 건너뛰게 만든다.
Local 변수 `wpp`를 재사용해 실수를 줄였다.
## 25. Host write의 현재 선택
`ssd_write()` 시작에서 기본 pointer를 정한다.
```c
struct write_pointer *wpp = &ssd->wp_cold;
```
모든 host LPN은 현재 이 pointer를 사용한다.
```c
ppa = get_new_page(wpp);
...
ssd_advance_write_pointer(ssd, wpp);
```
Phase 2가 계산한 Hot 상태도 아직 placement에는 사용하지 않는다.
## 26. 왜 모든 host write가 Cold인가
Phase 3의 목표는 구조 분리다.
분류까지 동시에 연결하면 오류 원인을 구분하기 어렵다.
첫 write의 기본 상태가 Cold라는 Phase 2 의미와도 맞는다.
기존 write 흐름을 하나의 pointer에 유지할 수 있다.
Hot pointer는 다음 배치 단계 전까지 대기한다.
이 상태는 최종 성능 실험용이 아니라 기능 검증용이다.
## 27. GC write의 현재 선택
`gc_write_page()`도 local pointer를 사용한다.
```c
struct write_pointer *wpp = &ssd->wp_cold;
```
그다음 같은 pointer로 allocation과 advance를 한다.
```c
new_ppa = get_new_page(wpp);
...
ssd_advance_write_pointer(ssd, wpp);
```
GC는 아직 LPN state를 읽어 목적지를 고르지 않는다.
## 28. Free line count 변화
초기에는 모든 line이 global free list에 있다.
현재 geometry에서는 128개다.
Cold pointer가 하나를 꺼내면 127개다.
Hot pointer가 하나를 더 꺼내면 126개다.
각 할당에서 count는 정확히 한 번 감소한다.
Pointer가 사용 중인 line은 free count에 포함하지 않는다.
## 29. 두 번째 active line의 영향
Phase 3에서는 Hot line을 아직 쓰지 않는다.
따라서 line 하나가 예약된 채 대기한다.
이전 baseline보다 free line이 하나 적게 시작한다.
GC 시작 시점이 조금 달라질 수 있다.
그래서 Phase 3 결과를 최종 WAF 결과로 비교하면 안 된다.
Hot 배치가 연결된 뒤 동일 조건으로 최종 실험해야 한다.
## 30. Active line과 GC victim
Pointer 초기화는 line을 free list에서 제거한다.
그 line은 full list에도 없다.
Victim priority queue에도 없다.
따라서 victim selector가 active line을 꺼낼 수 없다.
이는 Hot pointer가 아직 쓰이지 않아도 마찬가지다.
Active Hot line을 free list에 다시 넣으면 안 된다.
## 31. Line 중복 위험
`struct line`에는 QTAILQ entry가 하나뿐이다.
같은 line을 여러 list에 동시에 넣으면 list가 깨진다.
현재는 global free list 하나만 사용한다.
Line을 pointer에 줄 때 반드시 list에서 제거한다.
Pointer rotation 후 완료 line은 정확히 한 container에 넣는다.
Phase 4에서 pool을 늘릴 때 이 규칙이 더 중요해진다.
## 32. Mapping 순서 보존
Phase 3는 overwrite 순서를 바꾸지 않았다.
```text
기존 PPA 확인
기존 page invalid
기존 rmap 제거
새 PPA 할당
새 maptbl/rmap 등록
새 page valid
pointer advance
```
달라진 것은 PPA helper가 `wpp`를 받는다는 점뿐이다.
## 33. FDP 경로 보존
두 pointer 초기화는 non-FDP `else` 분기에만 있다.
FDP에서는 실행되지 않는다.
FDP는 각 reclaim unit의 `ssd_wptr`를 계속 사용한다.
FDP용 `fdp_get_new_page()`도 그대로다.
RU rotation과 FDP GC policy도 그대로다.
공용 `get_next_free_line()`의 pop/count 의미도 유지했다.
## 34. 최소 정적 검증
기존 단일 pointer 참조가 남았는지 찾는다.
```bash
rg 'ssd->wp([^_]|$)' hw/femu/bbssd
```
정상이면 결과가 없어야 한다.
Helper 호출도 확인한다.
```bash
rg 'get_new_page|ssd_advance_write_pointer' \
  hw/femu/bbssd/ftl.c
```
각 쌍이 같은 `wpp`를 써야 한다.
## 35. 빌드 검증
다음 명령으로 빌드한다.
```bash
ninja -C build-femu qemu-system-x86_64
```
현재 구현은 이 빌드를 통과했다.
빌드 성공만으로 runtime line 관계가 증명되지는 않는다.
초기화 로그와 실제 write 테스트도 필요하다.
## 36. 초기화 로그 테스트
FEMU를 non-FDP Blackbox mode로 실행한다.
Host의 다른 터미널에서 로그를 확인한다.
```bash
grep 'Write pointers' build-femu/log
```
현재 geometry의 기대 결과는 다음과 같다.
```text
hot_line=1 cold_line=0 free_lines=126
```
두 line 번호가 같으면 즉시 실패다.
## 37. 작은 순차 write 테스트
작은 영역에 순차 write를 수행한다.
현재 모든 page는 `wp_cold`로 가야 한다.
Cold pointer 좌표만 write 수만큼 이동해야 한다.
Hot pointer는 초기 좌표에 있어야 한다.
GC가 없다면 WAF는 1이어야 한다.
Mapping과 reverse mapping도 일치해야 한다.
## 38. Pointer line rotation 테스트
Cold active line 하나를 채울 만큼 write한다.
기존 Cold line은 full 또는 victim container로 이동해야 한다.
Cold pointer는 새 global free line을 받아야 한다.
Hot pointer line과 새 Cold line은 달라야 한다.
새 Cold pointer의 page, LUN, channel은 0이어야 한다.
`blk`는 새 `curline->id`와 같아야 한다.
## 39. GC migration 테스트
Overwrite workload로 invalid page를 만든다.
GC가 발생할 때 valid page가 `wp_cold`로 이동해야 한다.
GC는 `host_page_writes`를 증가시키면 안 된다.
`gc_page_writes`와 `nand_page_writes`는 증가해야 한다.
새 PPA allocation과 pointer advance는 같은 `wpp`를 써야 한다.
Hot pointer는 아직 이동하지 않아야 한다.
## 40. 통계 확인
Guest에서 workload가 끝난 뒤 실행한다.
```bash
sudo nvme admin-passthru /dev/nvme0 \
  --opcode=0xef --cdw10=5
```
Host 로그에서 통계를 찾는다.
```bash
grep 'BBSSD-STATS' build-femu/log | tail -n 1
```
출력에는 `block_erases`도 포함된다.
## 41. 반드시 유지할 식
Page write 통계는 다음 식을 만족해야 한다.
```text
nand_page_writes
= host_page_writes + gc_page_writes
```
두 pointer를 만들었다고 이 식이 달라지지 않는다.
Pointer 개수와 NAND program 횟수는 다른 개념이다.
Hot pointer가 대기 중이어도 NAND write counter는 증가하지 않는다.
## 42. 핵심 불변식 정리
Hot과 Cold active line은 달라야 한다.
각 pointer의 `blk`는 `curline->id`와 같아야 한다.
Active line은 free/full/victim container에 없어야 한다.
Line 할당마다 free count는 한 번 감소해야 한다.
Page allocation과 advance는 같은 `wpp`를 사용해야 한다.
GC와 host 통계 의미는 그대로여야 한다.
FDP RU pointer 동작은 바뀌면 안 된다.
## 43. Phase 3 완료 판단
`struct ssd`에 두 pointer가 존재한다.
초기 active line이 서로 다르다.
공통 init helper가 pointer를 인자로 받는다.
`get_new_page()`가 pointer를 인자로 받는다.
Advance helper도 같은 pointer를 인자로 받는다.
Host와 GC가 같은 기본 pointer를 일관되게 사용한다.
Global free list와 기존 victim policy가 유지된다.
FEMU 빌드와 초기화 로그 검증이 통과한다.
이 조건을 확인한 뒤에만 line pool 분리로 넘어간다.
