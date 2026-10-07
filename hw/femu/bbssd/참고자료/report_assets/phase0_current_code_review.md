# FEMU Blackbox SSD Hot/Cold FTL: Phase 0 현재 코드 분석

## 문서 범위

이 문서는 현재 작업 트리의 다음 파일을 기준으로 작성했다.

- `hw/femu/bbssd/ftl.c`
- `hw/femu/bbssd/ftl.h`

호출 관계를 정확히 확인하기 위해 다음 보조 파일의 accessor, 초기화, timing
bridge도 함께 확인했다.

- `hw/femu/bbssd/ftl-internal.h`
- `hw/femu/bbssd/ftl-geom.c`
- `hw/femu/bbssd/ftl-map.c`
- `hw/femu/bbssd/ftl-media.c`
- `hw/femu/bbssd/bb.c`의 기존 accounting control

줄 번호는 이 문서를 작성한 시점의 현재 작업 트리를 기준으로 한다. 이 단계에서는
Hot/Cold 코드, 통계 코드, write pointer, line 관리 코드를 수정하지 않았다.

## 먼저 알아둘 핵심 결론

1. non-FDP write와 GC는 모두 `ssd->wp`라는 하나의 write pointer를 공유한다.
   Host write와 GC migration이 서로 다른 함수에서 실행되더라도 다음 물리 page는
   같은 frontier에서 할당된다.
2. non-FDP의 공간 관리 단위는 line이다. 한 line은 현재 geometry에서 모든
   channel/LUN의 같은 block 번호를 묶은 superblock이다.
3. active line은 free list, victim priority queue, full list 어디에도 들어 있지
   않다. line을 끝까지 쓴 순간에만 victim 또는 full 상태가 된다.
4. FDP는 단순히 write 함수만 다른 것이 아니다. RU별 write pointer,
   `ru_mgmt`, FDP 전용 page 상태 갱신, FDP 전용 GC를 사용한다.
5. `get_next_free_line()`과 `struct write_pointer`, `struct line`은 non-FDP와
   FDP가 공유한다. 이 세 지점은 Hot/Cold 변경 시 가장 주의해야 하는 경계다.
6. 현재 non-FDP에는 host page write, NAND page write, GC page write를
   구분하는 baseline 카운터가 없다. 따라서 다음 구현 단계는 Hot/Cold 분류가
   아니라 Phase 1 baseline 통계 추가가 적절하다.

---

# 1. 현재 코드 상태

## 1.1 초기화와 FDP/non-FDP 분기

`ssd_init()`은 다음 순서로 공통 상태를 만든다
(`ftl.c:328-380`).

1. `ssd_init_params()`가 NAND geometry와 GC threshold를 계산한다.
2. channel/LUN/plane/block/page 배열을 만든다.
3. `bb_nand_media_init()`으로 현재 NAND timing layer를 연결한다.
4. `ssd_init_maptbl()`과 `ssd_init_rmap()`으로 양방향 mapping을 초기화한다.
5. `ssd_init_lines()`가 모든 line을 하나의 `free_line_list`에 넣는다.
6. 그 뒤에야 `ssd->fdp_enabled`를 설정한다.
7. FDP이면 RG/RU/RUH를 초기화하고, non-FDP이면
   `ssd_init_write_pointer()`로 `ssd->wp` 하나를 초기화한다.
8. 마지막으로 `ftl_thread()`를 시작한다.

중요한 점은 `ssd_init_lines()`가 FDP 여부를 결정하기 전에 실행된다는 것이다.
따라서 이 함수를 곧바로 Hot/Cold 전용 free pool 초기화 함수로 바꾸면 FDP RU
초기화도 영향을 받는다.

non-FDP에서는 `ssd_init_write_pointer()`가 free list의 첫 line을 제거하면서
`free_line_cnt`를 한 번 감소시킨다(`ftl.c:213-231`). 이 line은 곧바로
active line이 되며 어떤 list나 priority queue에도 들어 있지 않다.

현재 함수는 첫 line의 ID가 0이라는 초기 순서에 의존해 `wpp->blk = 0`으로
설정한다. 이후 line 교체 시에는 `wpp->blk = wpp->curline->id`를 사용한다.
두 write pointer를 도입할 때 두 번째 pointer에도 `blk = 0`을 복사하면 실제로
할당받은 line과 PPA의 block 번호가 달라질 수 있다. 최종적으로는 항상
`curline->id`에서 `blk`를 설정해야 한다.

## 1.2 현재 non-FDP Blackbox write path

### 함수 호출 순서

호스트 write 요청 한 건의 바깥쪽 호출 흐름은 다음과 같다.

1. NVMe poller가 요청을 `to_ftl` ring에 넣는다.
2. `ftl_thread()`가 `femu_ring_dequeue()`로 요청을 꺼낸다
   (`ftl.c:2672-2683`).
3. 기존 validation 결과가 `NVME_SUCCESS`인 경우에만 opcode handler를
   실행한다.
4. opcode가 `NVME_CMD_WRITE`이고 `ssd->fdp_enabled == false`이면
   `ssd_write(ssd, req)`를 호출한다(`ftl.c:2693-2701`).
5. `ssd_write()`가 반환한 최대 page latency를 `req->reqlat`와
   `req->expire_time`에 반영한다.
6. 요청을 `to_poller` ring으로 돌려보낸다.
7. 요청 반환 뒤 `should_gc()`가 참이면 background `do_gc(ssd, false)`를
   한 번 시도한다(`ftl.c:2729-2748`).

따라서 background GC 검사는 write 뒤에만 실행되는 것이 아니라 FTL thread가
처리한 read, write, DSM 요청 뒤에도 실행될 수 있다.

### `ssd_write()` 내부 순서

`ssd_write()`의 실제 순서는 다음과 같다(`ftl.c:864-928`).

1. 요청의 LBA/sector 길이를 `start_lpn`과 `end_lpn`으로 변환한다.
2. `end_lpn >= tt_pgs`이면 요청을 `NVME_LBA_RANGE`로 실패시킨다.
3. host page를 쓰기 전에 `should_gc_high()`를 확인한다.
4. high threshold 이하인 동안 `do_gc(ssd, true)`를 반복한다.
   victim을 찾지 못해 `do_gc()`가 `-1`을 반환하면 반복을 끝낸다.
5. 요청 범위의 각 LPN에 대해 다음 작업을 수행한다.

각 LPN의 out-of-place update 순서는 아래와 같다.

1. `get_maptbl_ent(ssd, lpn)`으로 기존 PPA를 읽는다.
2. 기존 PPA가 mapped 상태이면:
   - `mark_page_invalid()`로 기존 page를 `PG_INVALID`로 바꾼다.
   - block과 line의 `vpc`를 감소시키고 `ipc`를 증가시킨다.
   - full line이었다면 full list에서 제거해 victim priority queue로 옮긴다.
   - 이미 victim queue에 있었다면 변경된 `vpc`에 맞춰 priority를 갱신한다.
   - `set_rmap_ent(ssd, INVALID_LPN, old_ppa)`로 기존 reverse mapping을
     지운다.
3. `get_new_page(ssd)`가 현재 `ssd->wp` 좌표로 새 PPA를 만든다.
4. `set_maptbl_ent()`로 `LPN -> new PPA`를 등록한다.
5. `set_rmap_ent()`로 `new PPA -> LPN`을 등록한다.
6. `mark_page_valid()`로 새 page를 `PG_FREE -> PG_VALID`로 바꾸고 block/line
   `vpc`를 증가시킨다.
7. 기존 data-remanence 실험 instrumentation을 실행한다.
8. `ssd_advance_write_pointer()`로 같은 `ssd->wp`를 다음 page로 이동한다.
9. `NAND_WRITE`, `USER_IO`, `req->stime`으로 `ssd_advance_status()`를
   호출한다.
10. 요청에 포함된 모든 LPN 중 가장 큰 program latency를 반환한다.

이 순서는 사용자가 제시한 overwrite 순서와 일치한다.

`기존 PPA 확인 -> 기존 page invalid -> 기존 rmap 제거 -> 새 PPA 할당 ->
새 maptbl/rmap 등록 -> 새 page valid`

다만 기존 `maptbl[lpn]`은 중간에 `UNMAPPED_PPA`로 바뀌지 않고 새 PPA 등록
시점에 바로 덮어쓴다. 현재 FTL 상태 변경은 단일 `ftl_thread()`에서 순차적으로
실행되므로 이 짧은 중간 상태를 다른 FTL write가 관찰하지 않는다.

### write pointer의 이동과 line 상태 변화

`get_new_page()`와 `ssd_advance_write_pointer()`는 둘 다 내부에서
`&ssd->wp`를 직접 선택한다(`ftl.c:256-323`). 현재는 항상 같은 pointer를
사용하므로 allocation/advance 불일치는 없다.

pointer는 다음 순서로 이동한다.

`channel -> LUN -> page`

모든 channel과 LUN의 현재 page offset을 사용한 뒤 page offset을 하나
증가시킨다. `pg == pgs_per_blk`가 되면 현재 line을 모두 사용한 것이다.

- `vpc == pgs_per_line`, `ipc == 0`이면 full list로 이동한다.
- 일부 page가 invalid이면 victim priority queue로 이동한다.
- 그 뒤 `get_next_free_line()`으로 다음 line을 할당하고
  `free_line_cnt`를 한 번 감소시킨다.

현재 plane은 `pl == 0`으로 고정되어 있으며 관련 코드에도 multi-plane TODO가
남아 있다. Hot/Cold 설계에서 이 geometry 가정을 별도로 확장하면 작업 범위가
커지므로 첫 구현에서는 현재 가정을 유지해야 한다.

### NAND timing 경로

Host program latency는 다음 경로로 계산된다.

`ssd_write()`  
→ `ssd_advance_status(ssd, ppa, USER_IO/NAND_WRITE)`  
→ `ftl-media.c`의 `ssd_advance_status()`  
→ `nand_media_op()`

현재 media layer는 per-LUN availability time을 갱신한다. Hot/Cold allocation은
PPA 선택만 바꾸고 이 호출 경로와 `nand_cmd`의 `USER_IO/GC_IO` 구분은
보존해야 한다.

## 1.3 현재 non-FDP GC path

### GC가 시작되는 두 경로

Foreground GC:

`ftl_thread()`  
→ `ssd_write()`  
→ `should_gc_high()`  
→ `do_gc(ssd, true)` 반복

`free_line_cnt <= gc_thres_lines_high`일 때 host page를 할당하기 전에
실행된다. `force == true`이므로 victim의 invalid 비율이 낮아도 선택할 수
있다.

Background GC:

`ftl_thread()`의 host 요청 처리 및 poller 반환  
→ `should_gc()`  
→ `do_gc(ssd, false)` 한 번

`free_line_cnt <= gc_thres_lines`일 때 실행된다. 현재 request latency에
background GC 시간을 직접 더하지는 않지만, GC가 갱신한 LUN availability
time은 뒤의 요청 latency에 영향을 줄 수 있다.

### `do_gc()` 내부 호출 순서

`do_gc()`의 실제 순서는 다음과 같다(`ftl.c:767-810`).

1. `select_victim_line(ssd, force)`
   - `victim_line_pq`의 top을 확인한다.
   - queue priority는 line의 `vpc`이므로 valid page가 적은 line을 우선한다.
   - background GC는 `ipc < pgs_per_line / 8`이면 이번 GC를 건너뛴다.
   - 선택하면 priority queue에서 제거하고 `victim_line_cnt`를 감소시킨다.
2. victim line의 ID를 `ppa.blk`에 넣는다.
3. 모든 channel과 LUN의 같은 block 번호를 순회한다.
4. 각 block마다 `clean_one_block()`을 호출한다.
5. `clean_one_block()`은 block의 모든 page를 순회한다.
6. `PG_VALID` page마다:
   - `gc_read_page()`를 호출한다.
   - `get_rmap_ent(old_ppa)`로 LPN을 찾는다.
   - `gc_write_page()`를 호출해 valid data를 이동한다.
7. `gc_write_page()`는:
   - `get_new_page(ssd)`로 `ssd->wp`에서 destination PPA를 얻는다.
   - 새 `maptbl`과 새 `rmap`을 등록한다.
   - destination page를 valid로 만든다.
   - 같은 `ssd->wp`를 advance한다.
   - GC delay가 켜져 있으면 `GC_IO/NAND_WRITE` timing을 진행한다.
   - destination LUN의 `gc_endtime`을 갱신한다.
8. 한 victim block의 valid page를 모두 옮긴 뒤 `mark_block_free()`를
   호출한다.
   - block의 모든 page 상태를 `PG_FREE`로 만든다.
   - block `ipc/vpc`를 0으로 만든다.
   - `erase_cnt`를 증가시킨다.
9. GC delay가 켜져 있으면 `GC_IO/NAND_ERASE` timing을 진행한다.
10. victim line에 속한 모든 block을 처리한 뒤 `mark_line_free()`를 한 번
    호출한다.
    - line `ipc/vpc`를 0으로 만든다.
    - free list에 line을 넣는다.
    - `free_line_cnt`를 한 번 증가시킨다.

GC migration은 현재도 host write 함수인 `ssd_write()`를 재사용하지 않는다.
따라서 이후 LPN temperature 갱신과 host counter를 `ssd_write()`에만 두면
GC migration을 host access로 잘못 세는 것을 피할 수 있다.

### GC 중 mapping/rmap의 현재 동작

`gc_write_page()`는 source PPA의 rmap을 읽어 LPN을 찾은 뒤 destination의
`maptbl/rmap`을 갱신한다. 하지만 source page를 개별적으로 invalid 처리하거나
source rmap을 `INVALID_LPN`으로 지우지는 않는다. source block 전체는
`clean_one_block()`이 끝난 뒤 `mark_block_free()`에서 한꺼번에 free가 된다.

이 때문에 현재 코드에서 다음 사실을 구분해야 한다.

- `PG_VALID`인 victim page의 rmap은 GC copy 시 LPN 복원에 사용된다.
- GC copy 직후 `maptbl[lpn]`은 destination PPA를 가리킨다.
- erase 전까지 source page status와 source rmap은 잠시 남아 있다.
- `mark_block_free()`는 page status를 free로 만들지만 source rmap 배열
  entry 자체는 지우지 않는다.
- 이후 그 free PPA가 다시 쓰이면 새 rmap 값으로 덮어쓴다.

따라서 “모든 물리 PPA의 rmap은 항상 maptbl과 완전한 역관계”라는 강한
검사는 현재 baseline에는 맞지 않는다. 현재 코드에 맞는 안전한 검사는 아래와
같이 유효 page에 범위를 한정해야 한다.

- mapped LPN이 가리키는 최신 PPA는 `PG_VALID`여야 한다.
- 그 최신 PPA의 rmap은 해당 LPN이어야 한다.
- GC source를 제외한 `PG_VALID` page의 `maptbl[rmap[ppa]]`는 자기 자신이어야
  한다.

source rmap을 언제 지울지는 별도 mapping 정책 변경이다. Phase 1 통계 단계에서
baseline 동작과 함께 바꾸면 결과 원인을 분리할 수 없으므로 지금은 수정하지
않는 것이 좋다. Hot/Cold GC를 연결할 때도 source rmap은 반드시 LPN을 읽은
뒤에만 정리할 수 있다.

## 1.4 현재 line 상태 전이

non-FDP line의 정상 상태 전이는 다음과 같다.

`free list`  
→ write pointer가 할당하면 `active`  
→ line을 끝까지 쓰면 `full list` 또는 `victim priority queue`  
→ full line의 page가 overwrite/TRIM되면 `victim priority queue`  
→ GC가 선택하면 `GC in-flight`  
→ erase 완료 후 `free list`

현재 `struct line`에는 명시적인 state enum이 없다. 실제 상태는 다음 정보의
조합으로 암묵적으로 표현된다.

- QTAILQ `entry`
- victim priority queue의 `pos`
- `free_line_cnt`, `victim_line_cnt`, `full_line_cnt`
- `ssd->wp.curline`

정상적인 안정 상태에서는 다음 관계가 성립한다.

`tt_lines = free_line_cnt + victim_line_cnt + full_line_cnt + 1 active line`

`select_victim_line()`이 victim을 pop한 뒤 `mark_line_free()` 전까지는
GC in-flight line 하나를 추가해야 한다.

두 write pointer를 도입한 뒤에는 정상 안정 상태 관계가 다음처럼 바뀐다.

`tt_lines = free + victim + full + 2 active lines`

단, 두 pointer가 모두 유효하고 서로 다른 line을 갖는 정상 운용 시의 관계다.
초기화 실패나 pool 고갈 상태는 별도로 표현해야 한다.

---

# 2. 이번 단계의 목표

현재 요청은 Phase 0 분석이다. 이 단계의 목표는 다음 네 가지를 고정하는 것이다.

1. host write와 GC migration의 실제 allocation/mapping 순서를 이해한다.
2. line이 free, active, victim, full, GC in-flight 사이에서 이동하는 위치를
   확인한다.
3. FDP와 non-FDP가 공유하는 자료형과 완전히 분리된 실행 경로를 구분한다.
4. Hot/Cold 정책 전의 측정 가능한 baseline을 만들기 위한 다음 작업을 정한다.

이번 단계에서 아직 구현하지 말아야 할 것은 다음과 같다.

- LPN별 temperature metadata
- Hot/Cold threshold 또는 decay
- `wp_hot`, `wp_cold`
- class별 free line pool
- GC migration의 temperature routing
- class별 victim policy
- FDP 함수와 RU 자료구조 변경

---

# 3. 주요 자료구조의 역할

## 3.1 `struct ssd`

위치: `ftl.h:300-327`

`struct ssd`는 한 Blackbox SSD 인스턴스의 FTL 전체 상태를 소유한다.

- `sp`: geometry, latency, GC threshold
- `media`: NAND media timing handle
- `ch`: channel부터 physical page까지 이어지는 NAND 상태 배열의 시작점
- `maptbl`: LPN에서 최신 PPA로 가는 forward mapping
- `rmap`: physical page index에서 LPN으로 가는 reverse mapping
- `wp`: 현재 non-FDP의 단일 write frontier
- `lm`: line 배열과 free/victim/full 관리 상태
- ring/thread 필드: NVMe poller와 FTL worker 사이 요청 전달
- FDP 필드: RG/RU/RUH와 FDP enable/debug 상태
- `n`: 상위 FEMU controller

Hot/Cold metadata, 두 write pointer, baseline stats는 전역 변수보다 이 구조체
안에 두는 것이 맞다. 그래야 SSD 인스턴스별 상태가 분리되고 초기화와 실험
reset 범위가 명확해진다.

## 3.2 `struct line_mgmt`

위치: `ftl.h:190-201`

non-FDP line의 소유권과 상태별 container를 관리한다.

- `lines`: line ID로 직접 접근하는 전체 line 배열
- `free_line_list`: 아직 active pointer에 할당되지 않은 free line
- `victim_line_pq`: invalid page가 있어 GC할 수 있는 닫힌 line
- `full_line_list`: 닫혔지만 모든 page가 valid인 line
- `tt_lines`: 전체 line 수
- 세 count: 각 container의 논리적 원소 수

victim queue는 valid page 수가 작은 line을 우선하기 위해 `line->vpc`를
priority로 사용한다. `line->pos`는 priority queue 안의 위치이며, 0은 queue에
없다는 표지로 사용된다.

Hot/Cold class별 free list를 추가할 때 가장 위험한 점은 count와 실제 list
원소 수가 어긋나는 것이다. `total free = hot free + cold free`는 count만
맞춰서는 부족하고 실제 list membership도 중복 없이 일치해야 한다.

## 3.3 `struct line`

위치: `ftl.h:169-178`

현재 line ID는 같은 block 번호와 같다. `get_line()`도 PPA의 `blk`를 그대로
`ssd->lm.lines[blk]`의 index로 사용한다.

- `id`: line ID이자 각 channel/LUN에서 선택할 block 번호
- `ipc`: line 전체의 invalid page 수
- `vpc`: line 전체의 valid page 수
- `entry`: free list 또는 full list에 연결할 QTAILQ link
- `pos`: victim priority queue 위치
- `my_ru`: FDP에서 이 line을 소유하는 reclaim unit

현재 `entry`는 하나뿐이므로 한 line을 두 QTAILQ에 동시에 넣을 수 없다.
Hot/Cold free list를 따로 둘 경우 line은 둘 중 정확히 하나에만 들어가야 한다.

`my_ru`는 FDP 필드이며 non-FDP에서는 `g_malloc0()` 초기화 결과로 NULL이다.
Hot/Cold class 필드를 추가하더라도 이 필드를 삭제하거나 다른 용도로 재사용하면
안 된다.

## 3.4 `struct write_pointer`

위치: `ftl.h:180-188`

다음 program에 사용할 PPA 좌표와 active line을 보관한다.

- `curline`: 현재 쓰는 active line
- `ch`, `lun`, `pg`, `pl`: line 내부의 다음 physical page 좌표
- `blk`: `curline->id`와 일치해야 하는 block 번호

이 구조체 자체는 FDP도 사용한다. non-FDP는 `ssd->wp` 한 개를 embedded
field로 갖고, FDP는 각 `FemuReclaimUnit`이 동적으로 할당한
`ssd_wptr`을 갖는다.

따라서 `struct write_pointer`에 Hot/Cold 의미를 직접 넣으면 FDP pointer에도
그 필드가 생긴다. 가능하면 temperature는 non-FDP allocation helper의 인자나
non-FDP line metadata로 관리하고, 공통 pointer 구조는 좌표라는 기존 책임을
유지하는 편이 안전하다.

## 3.5 보조 구조: `struct nand_block`

위치: `ftl.h:86-93`

한 channel/LUN/plane 안의 실제 block 상태다.

- `pg`: block의 physical page 배열
- `npgs`: page 수
- `ipc`, `vpc`: 이 block 안의 invalid/valid page 수
- `erase_cnt`: 실제 block erase 횟수
- `wp`: 초기값은 0이지만 현재 non-FDP allocation 경로에서는 사용되지 않는다.

line은 여러 `nand_block`의 합계 상태다. 따라서 정상 시 닫힌 line의
`line->vpc/ipc`는 그 line에 속한 모든 block의 `vpc/ipc` 합과 일치해야 한다.
erase 편향은 `nand_block.erase_cnt`의 분포로 확인할 수 있다.

---

# 4. Hot/Cold 적용 시 수정이 필요한 함수와 구조

아래 목록은 최종 Hot/Cold 구조까지 갔을 때의 수정 후보를 단계별로 정리한
것이다. 지금 한 번에 수정하라는 의미가 아니다.

| 단계 | 위치 | 수정 목적 | 함수의 책임 경계 |
|---|---|---|---|
| Phase 1 | `ftl.h::struct ssd` 또는 작은 stats 구조 | non-FDP baseline counter 보관 | 정책 상태와 통계 상태를 구분 |
| Phase 1 | `ssd_init()` | baseline counter 초기화 | FDP 초기화 분기는 변경하지 않음 |
| Phase 1 | `ssd_write()` | host page write와 host-origin NAND program 계측 | temperature 판단은 아직 하지 않음 |
| Phase 1 | `gc_write_page()` | GC page write와 GC-origin NAND program 계측 | host counter는 건드리지 않음 |
| Phase 1 | `do_gc()` | 성공한 line GC와 block erase 계측 | victim policy는 변경하지 않음 |
| Phase 2 | `ftl.h::struct ssd` | LPN별 write history/temperature 배열과 global sequence 추가 | write pointer는 하나 유지 |
| Phase 2 | `ssd_init()` | `tt_pgs` 크기의 LPN metadata 할당/초기화 | FDP이면 allocation하지 않거나 사용하지 않음 |
| Phase 2 | `ssd_write()` | host write에 대해서만 history 갱신 | GC와 TRIM의 정책을 섞지 않음 |
| Phase 3 | `ftl.h::struct ssd` | non-FDP `wp_hot`, `wp_cold` 추가 | FDP RU pointer는 유지 |
| Phase 3 | `ssd_init_write_pointer()` | 서로 다른 두 active line 초기화 | free count를 pointer당 정확히 한 번 감소 |
| Phase 3 | `get_new_page()` | 호출자가 넘긴 `write_pointer *`에서 PPA 생성 | temperature 분류 책임을 갖지 않음 |
| Phase 3 | `ssd_advance_write_pointer()` | 전달받은 같은 pointer만 이동 | line 종료와 다음 line 할당 책임 유지 |
| Phase 3 | `ssd_write()`, `gc_write_page()` | 선택한 pointer를 allocation과 advance 양쪽에 동일하게 전달 | 아직 class별 victim 정책은 변경하지 않음 |
| Phase 4 | `ftl.h::struct line` | non-FDP line class/소유 상태 표현 | FDP `my_ru`와 독립된 필드 사용 |
| Phase 4 | `ftl.h::struct line_mgmt` | hot/cold free list 및 count 추가 | total count와 class count의 단일 갱신 지점 필요 |
| Phase 4 | non-FDP 전용 free-line helper | class에 맞는 line 할당, 고갈/borrowing 처리 | FDP가 쓰는 기존 `get_next_free_line()`과 분리 |
| Phase 4 | `mark_line_free()` | 회수한 line을 정해진 class pool로 한 번 반환 | GC policy와 pool policy를 분리 |
| Phase 4 | `should_gc()`, `should_gc_high()` | 우선 aggregate free count 기준을 유지하는지 확인 | 첫 구현에서는 class별 GC threshold를 도입하지 않음 |
| Phase 5 | `ssd_write()` | history 갱신 후 현재 temperature에 맞는 pointer 선택 | overwrite/mapping 순서는 보존 |
| Phase 6 | `gc_write_page()` | old PPA의 rmap으로 LPN을 찾고 현재 분류에 맞는 pointer 선택 | LPN history는 갱신하지 않음 |
| Phase 7 | `select_victim_line()`, `do_gc()` | 필요할 때만 class-aware victim 정책 검토 | 먼저 global greedy baseline을 유지 |
| 검증 | `clean_one_block()`, `mark_page_valid()`, `mark_page_invalid()` | 보통 정책 수정은 불필요하며 assertion 후보가 중심 | page/block/line 상태 갱신의 기존 단일 책임 유지 |
| 진입점 | `ftl_thread()` | FDP/non-FDP 분기 보존, 저빈도 stats snapshot 연결 후보 | per-page 로그는 넣지 않음 |

최종적으로 고려할 수 있는 짧은 helper 형태는 다음 정도다. 이는 완성 코드가
아니라 책임 분리를 위한 시그니처 예시다.

- `get_new_page(ssd, wpp)`: 전달받은 pointer로 PPA만 만든다.
- `ssd_advance_write_pointer(ssd, wpp, temperature)`: 같은 pointer를
  이동하고 line 종료 시 해당 class의 다음 line을 얻는다.
- `select_write_pointer(ssd, temperature)`: pointer 선택만 담당한다.
- `update_lpn_temperature(ssd, lpn)`: host access history만 갱신한다.
- `get_lpn_temperature(ssd, lpn)`: GC가 현재 분류를 읽을 때 사용한다.
- `account_page_program(ssd, io_type)`: host/GC/NAND counter를 한 곳에서
  일관되게 갱신한다.

Phase 3에서는 두 pointer가 하나의 global free list에서 서로 다른 line을
할당받게 만들고, Phase 4에서만 class별 pool로 나누는 것이 단계 분리에 맞다.

---

# 5. FDP 경로와 충돌할 수 있는 지점

## 5.1 `get_next_free_line()`은 FDP도 사용한다

가장 큰 충돌 지점이다.

- non-FDP의 `ssd_advance_write_pointer()`가 다음 active line을 얻을 때 사용한다.
- FDP의 `femu_fdp_init_ssd_reclaim_unit()`도 각 RU에 line을 배정할 때
  사용한다(`ftl.c:2230-2253`).

이 함수를 Hot/Cold pool 전용으로 바꾸면 FDP RU 초기화가 어느 list에서
line을 가져와야 하는지 알 수 없게 된다. 안전한 방향은 다음 중 하나다.

1. 기존 helper를 FDP/global 초기화용으로 보존하고 non-FDP class 전용
   helper를 별도로 둔다.
2. helper에 mode/class를 명시적으로 전달하되 FDP 호출은 기존 global
   동작을 정확히 선택하게 한다.

첫 구현에서는 1번이 검증하기 쉽다.

## 5.2 `ssd_init_lines()`는 FDP 분기 전에 실행된다

현재 `fdp_enabled` 설정은 `ssd_init_lines()` 뒤에 있다. 따라서 이 함수에서
무조건 free line을 Hot/Cold 두 pool로 나누면 FDP 초기화가 사용하는 기존
global free list가 비게 된다.

non-FDP pool 분배는 `fdp_enabled == false`가 확정된 뒤 실행되는 별도
초기화 단계로 두는 편이 안전하다. FDP에서는 현재의 단일 free list와 RU
line 소유권 설정을 그대로 유지해야 한다.

## 5.3 `struct write_pointer`는 공용 자료형이다

non-FDP `ssd->wp`뿐 아니라 FDP RU의 `ssd_wptr`도 같은 자료형이다. pointer
helper를 일반화할 때 FDP의 `fdp_get_new_page()`와
`fdp_advance_ru_pointer()`를 무심코 대체하면 RU rotation, RUH pointer,
RU queue count가 깨질 수 있다.

초기 Hot/Cold 작업은 non-FDP의 `get_new_page()`와
`ssd_advance_write_pointer()`만 parameterize하고 FDP 전용 두 함수는
그대로 두는 것이 좋다.

## 5.4 `struct line`의 `my_ru`와 FDP line 소유권

FDP 초기화는 global free list에서 line을 빼고 `line->my_ru`를 설정한다.
FDP의 `mark_page_valid_fdp()`와 `mark_page_invalid_fdp()`는 이 포인터를
사용해 RU vpc/ipc와 victim queue를 갱신한다.

Hot/Cold class를 `my_ru`에 겹쳐 표현하거나 FDP mode에서 class list에
line을 동시에 넣으면 안 된다.

## 5.5 page 상태 갱신 함수는 이미 분리되어 있다

- non-FDP: `mark_page_valid()`, `mark_page_invalid()`
- FDP: `mark_page_valid_fdp()`, `mark_page_invalid_fdp()`

겉으로 비슷해 보여도 FDP 함수는 RU/RUH의 live page count와 여러 victim
queue를 갱신한다. 중복 제거를 목적으로 성급하게 합치면 FDP bookkeeping이
깨질 가능성이 높다. Hot/Cold 첫 구현에서는 non-FDP 함수만 사용하고 두
계열을 합치지 않아야 한다.

## 5.6 write, GC, TRIM entry가 모두 분기되어 있다

- write: `ssd_write()` 대 `nvme_do_write_fdp()`/`ssd_stream_write()`
- GC: `do_gc()` 대 `do_gc_fdp_style()`
- TRIM: `ssd_trim()` 대 `ssd_trim_fdp_style()`
- background GC: `ftl_thread()`의 `fdp_enabled` 분기

Hot/Cold 상태 접근은 non-FDP branch에서만 일어나야 한다. 특히
`ftl_thread()`의 기존 분기를 없애고 하나의 공통 write 함수에서 mode를
추론하도록 바꾸는 것은 초기 단계에 적합하지 않다.

## 5.7 공용 `mark_block_free()`에서 통계를 세는 위험

`mark_block_free()`는 non-FDP `do_gc()`와 FDP GC/reset 경로가 모두
호출한다. non-FDP baseline erase counter를 이 함수에 무조건 넣으면 FDP
실험에서도 counter가 변한다.

Phase 1의 non-FDP 전용 erase 계측은 `do_gc()`의
`mark_block_free()` 호출 직후에 두거나, 명시적인 I/O mode를 받는 통계
helper를 사용해야 한다.

## 5.8 NAND timing 경로

non-FDP와 FDP 모두 `ssd_advance_status()`를 사용한다. Hot/Cold 기능은
allocation 결과인 PPA만 바꾸고 다음을 유지해야 한다.

- host program은 `USER_IO/NAND_WRITE`
- GC read/program/erase는 `GC_IO`
- GC delay enable/disable 의미
- `new_lun->gc_endtime`
- `req->stime`과 GC의 `stime = 0`

---

# 6. 현재 코드 기준으로 가장 먼저 수행할 작업

## Phase 1: non-FDP baseline 통계만 추가

Phase 0의 호출 흐름 확인 다음에 할 한 단계는 Hot/Cold metadata가 아니라
baseline 통계다. 배치 정책과 write pointer를 바꾸지 않은 상태에서 다음
값을 먼저 신뢰할 수 있어야 한다.

| 지표 | 권장 의미 |
|---|---|
| `host_page_writes` | `ssd_write()`가 실제 처리한 LPN 수. NVMe command 수나 sector 수가 아님 |
| `gc_page_writes` | `gc_write_page()`가 이동한 valid page 수 |
| `nand_page_writes` | host program 수 + GC migration program 수 |
| `gc_cycles` | victim line 하나를 끝까지 회수한 성공한 `do_gc()` 횟수 |
| `gc_attempts` | 선택 사항. victim을 못 찾은 시도까지 보고 싶을 때만 분리 |
| `block_erases` | `do_gc()`가 실제 erase한 block 수 |
| `valid_page_migrations` | 현재 구조에서는 `gc_page_writes`와 동일하므로 별도 중복 counter가 꼭 필요하지 않음 |

현재 모델에서 metadata program을 따로 모델링하지 않으므로 다음 관계를
baseline 불변식으로 사용할 수 있다.

`nand_page_writes = host_page_writes + gc_page_writes`

`host_page_writes > 0`일 때만 다음을 계산한다.

`WAF = nand_page_writes / host_page_writes`

정수 나눗셈을 피하고, 출력 시 계산하는 derived metric으로 두는 것이 좋다.
매 write마다 floating-point WAF를 갱신할 필요는 없다.

### Phase 1에서 수정할 위치

#### `ftl.h::struct ssd`

작은 non-FDP FTL stats 구조를 embedded field로 추가할 위치다. FDP에 이미
있는 `hbmw/mbmw/mbe`와 이름을 겹치지 않게 해야 한다.

왜 이 위치인가:

- SSD 인스턴스별 통계가 된다.
- 향후 reset/snapshot의 소유자가 분명하다.
- 전역 변수로 인해 여러 controller의 값이 섞이는 문제를 피할 수 있다.

#### `ftl.c::ssd_init()`

geometry/mapping 초기화와 함께 stats를 명시적으로 0으로 초기화할 위치다.
`struct ssd`는 현재 `g_malloc0()`으로 할당되지만 명시적 초기화 helper가 있으면
실험 reset과 초기 부팅이 같은 경로를 공유할 수 있다.

이 단계에서는 `fdp_enabled` 분기와 FDP stats를 변경하지 않는다.

#### `ftl.c::ssd_write()`

한 LPN의 새 page가 정상적으로 valid가 되고 program이 발행되는 지점에서:

- host page write를 한 번 증가
- NAND page write를 한 번 증가

해야 한다.

요청 시작에서 `req->nlb`만 보고 미리 더하면 range error 또는 중간 실패 시
실제 program 수와 달라질 수 있다. per-LPN loop 안의 성공 지점이 맞다.

#### `ftl.c::gc_write_page()`

destination page를 valid로 만들고 GC program을 발행하는 page당 한 지점에서:

- GC page write를 한 번 증가
- NAND page write를 한 번 증가

해야 한다.

계측을 `if (enable_gc_delay)` 안에 넣으면 GC delay를 끈 실험에서 실제
migration은 수행됐는데 counter가 증가하지 않는다. 통계는 timing enable
여부와 독립적이어야 한다.

#### `ftl.c::do_gc()`

- victim line 회수를 완료하고 `mark_line_free()`까지 성공한 뒤
  `gc_cycles`를 한 번 증가시킨다.
- 각 `mark_block_free()` 호출 직후 `block_erases`를 한 번 증가시킨다.

현재 geometry에서 성공한 line GC 한 번은
`nchs * luns_per_ch`개의 block을 erase한다. 따라서 다음 관계를 검증할 수 있다.

`block_erases = gc_cycles * nchs * luns_per_ch`

이는 현재 `pl = 0`, line당 동일 block 번호라는 구현에 한정된 관계다.

#### 통계 출력 또는 reset 경로

`bb.c::bb_flip()`의 기존 `FEMU_RESET_ACCT`는 현재 poller의 total/late I/O
counter만 초기화한다. FTL baseline stats를 여기에 연결할지는 별도 작은
설계 결정이 필요하다.

중요한 원칙은 다음과 같다.

- reset과 snapshot의 시점을 workload 시작/끝과 명확히 맞춘다.
- per-page `printf()`는 하지 않는다.
- 주기 출력이 필요하면 충분히 큰 page/GC 간격에서 snapshot만 출력한다.
- 기존 data-remanence `[EXP]` instrumentation과 의미를 섞지 않는다.
- FDP stats reset 동작을 암묵적으로 바꾸지 않는다.

### Phase 1에서 아직 하지 말아야 할 것

- LPN write count나 update interval 추가
- temperature threshold 결정
- `wp_hot`, `wp_cold` 추가
- line class와 class별 free pool 추가
- GC victim 선택 변경
- GC migration destination 변경

---

# 7. 첫 단계의 불변식과 위험 요소

## 7.1 반드시 지킬 불변식

### 통계 불변식

1. Host write 한 page는 `host_page_writes`와 `nand_page_writes`를 각각
   정확히 한 번 증가시킨다.
2. GC migration 한 page는 `gc_page_writes`와 `nand_page_writes`를 각각
   정확히 한 번 증가시킨다.
3. GC migration은 `host_page_writes`를 증가시키지 않는다.
4. TRIM은 세 page-write counter를 증가시키지 않는다.
5. 현재 모델에서는 항상
   `nand_page_writes == host_page_writes + gc_page_writes`여야 한다.
6. victim을 찾지 못한 `do_gc()`는 성공한 `gc_cycles`를 증가시키지 않는다.
7. line 하나를 실제 회수했을 때만 `gc_cycles`가 한 번 증가한다.
8. block erase counter는 `mark_block_free()`가 non-FDP GC에서 실제 호출된
   횟수와 일치해야 한다.

### 기존 FTL 불변식

1. page status는 `PG_FREE`, `PG_VALID`, `PG_INVALID` 중 하나다.
2. overwrite 후 최신 `maptbl[lpn]`은 새 PPA 하나만 가리킨다.
3. 최신 PPA의 `rmap`은 해당 LPN과 일치한다.
4. overwrite 순서는 현재 코드의
   `old invalid -> old rmap clear -> new allocation -> new map/rmap ->
   new valid`를 유지한다.
5. `get_new_page()`와 `ssd_advance_write_pointer()`는 같은 `ssd->wp`를
   사용한다.
6. active line은 free/full/victim container에 들어 있지 않아야 한다.
7. line 할당 시 `free_line_cnt`는 한 번만 감소한다.
8. GC 반환 시 `mark_line_free()`는 `free_line_cnt`를 한 번만 증가시킨다.
9. GC 중 destination pointer가 새 line을 할당할 수 있으므로
   `do_gc()` 전후의 순수 `free_line_cnt`가 반드시 정확히 +1이라고
   가정하면 안 된다. “회수 시 +1”과 “GC write frontier 교체 시 -1”을
   별도로 검사해야 한다.
10. FDP enabled 경로에서는 새 non-FDP counter가 변하지 않거나, 적어도
    FDP의 기존 동작과 controller FDP stats에 영향을 주지 않아야 한다.

## 7.2 주요 위험 요소

### 같은 NAND program을 두 번 세는 문제

`ssd_advance_status()`와 `ssd_write()` 양쪽에서 count하면 중복된다. 또한
`mark_page_valid()`는 host와 GC 양쪽에서 호출되므로 그 안에서 origin을
모른 채 count하면 host/GC 구분이 사라진다.

첫 구현에서는 origin을 아는 `ssd_write()`와 `gc_write_page()`가 page당
한 번만 통계 helper를 호출하는 구조가 검증하기 쉽다.

### `enable_gc_delay`와 통계를 결합하는 문제

GC timing을 끄더라도 mapping 이동과 page program 모델은 계속 실행된다.
통계 증가가 timing 조건문 안에 있으면 WAF가 잘못 낮아진다.

### GC attempt와 성공 cycle을 혼합하는 문제

`do_gc()`는 victim이 없거나 background 조건에서 invalid page가 충분하지
않으면 `-1`을 반환한다. 함수 진입 시 무조건 GC count를 올리면 실제 회수
횟수보다 커진다.

### `mark_block_free()`의 FDP 공용 호출

공용 helper에 non-FDP erase counter를 직접 넣으면 FDP GC와 FDP reset까지
섞인다. non-FDP `do_gc()` 안에서 계측하거나 mode를 명시해야 한다.

### default build에서 assertion이 비활성인 문제

현재 `ftl.h`의 `ftl_assert`는 `FEMU_DEBUG_FTL`이 정의되지 않으면 빈
매크로다. 현재 `ftl.c` 상단의 debug define도 주석 처리되어 있다. 따라서
소스에 assertion 후보를 추가했다는 사실만으로 runtime 검증이 된다고 볼 수
없다.

검증 build에서는 해당 매크로가 header 처리 전에 실제로 활성화되는 build
설정을 확인해야 한다. 단순히 `ftl.c`의 include 뒤에 define을 두는 방식은
이미 전처리된 `ftl.h` 매크로를 바꾸지 못한다.

### logging 자체의 latency 왜곡

FTL은 단일 worker thread에서 동작한다. page마다 `printf()`나 `fprintf()`를
실행하면 workload 처리율과 host-side tail latency를 크게 왜곡한다.
카운터는 메모리에서만 증가시키고, 출력은 실험 경계나 낮은 빈도로 제한해야
한다.

---

# 8. 검증 방법

## 8.1 빌드 전에 확인할 사항

1. 새 counter의 자료형이 모두 `uint64_t`인지 확인한다.
2. WAF 계산에서 host counter가 0인 경우를 처리했는지 확인한다.
3. counter 증가 위치가 `ssd_write()`와 `gc_write_page()`에서 각각 한 곳인지
   검색한다.
4. GC counter 증가가 `enable_gc_delay` 조건문 밖인지 확인한다.
5. `mark_page_valid_fdp()`, `mark_page_invalid_fdp()`,
   `gc_write_page_fdp_style()`, `do_gc_fdp_style()`에 변경이 없는지 확인한다.
6. 기존 `ssd_advance_status()` 호출과 `USER_IO/GC_IO` 값이 그대로인지
   확인한다.
7. runtime assertion을 사용할 build에서 `FEMU_DEBUG_FTL`이 실제로
   활성화되는지 전처리 조건을 확인한다.

## 8.2 최소 로그

workload 종료 또는 명시적 snapshot에서 한 줄만 출력하는 것이 좋다.

- host page writes
- NAND page writes
- GC page writes
- 성공한 line GC 횟수
- block erase 횟수
- WAF
- 현재 free/victim/full line count

매 page의 LPN/PPA는 baseline 통계 로그에 넣지 않는다. mapping 디버깅이
필요한 짧은 재현 workload에서만 기존 debug instrumentation 또는 제한된
trace를 사용한다.

## 8.3 assertion 후보

비용이 작은 hot-path assertion:

- 새 PPA는 `mark_page_valid()` 직전 `PG_FREE`
- host overwrite의 old PPA는 `mark_page_invalid()` 직전 `PG_VALID`
- `get_new_page()`가 만든 PPA의 block은 `wpp->curline->id`
- `free_line_cnt`, `victim_line_cnt`, `full_line_cnt`는 음수가 아님
- 최신 PPA의 `get_rmap_ent()`는 현재 LPN
- `nand_page_writes == host_page_writes + gc_page_writes`

비용이 큰 debug checkpoint assertion:

- 실제 free list 길이와 `free_line_cnt` 비교
- 실제 full list 길이와 `full_line_cnt` 비교
- victim priority queue 크기와 `victim_line_cnt` 비교
- 모든 block의 vpc/ipc 합과 line vpc/ipc 비교
- mapped LPN 전체에 대해 최신 PPA status와 rmap 검사

전체 LPN/PPA 순회 검사는 매 write마다 실행하지 말고 workload 종료나 드문
checkpoint에서만 실행해야 한다.

## 8.4 간단한 테스트 workload와 기대 상태

### 테스트 A: GC threshold보다 작은 순차 write

목적: host/NAND 기본 계수 확인

기대:

- `host_page_writes = 기록한 고유 LPN 수`
- `gc_page_writes = 0`
- `nand_page_writes = host_page_writes`
- `WAF = 1.0`
- `gc_cycles = 0`
- `block_erases = 0`

장치 전체를 채우지 말고 background GC threshold에 도달하지 않는 작은
범위로 제한해야 한다.

### 테스트 B: 같은 작은 LPN 범위를 반복 overwrite

목적: invalidation과 victim 생성 확인

초기에는 active line 안에서 invalid page가 늘 수 있다. line을 닫을 만큼
충분히 기록한 뒤에는 victim queue가 생겨야 한다.

기대:

- host counter는 실제 overwrite page 수만큼 증가
- GC가 아직 없으면 NAND counter와 host counter가 같음
- closed victim line의 `ipc > 0`
- overwrite된 old PPA의 rmap은 `INVALID_LPN`
- 최신 PPA의 rmap은 원래 LPN

### 테스트 C: prefill 후 overwrite로 실제 GC 유도

목적: WAF와 GC copy counter 검증

기대:

- `gc_page_writes > 0`
- `nand_page_writes = host_page_writes + gc_page_writes`
- `WAF > 1.0`일 수 있으나 workload와 GC 발생 여부에 따라 판단
- `block_erases = gc_cycles * nchs * luns_per_ch`
- 성공한 GC마다 victim line이 free list로 한 번 반환됨

WAF가 반드시 어떤 특정 값이어야 한다고 미리 가정하면 안 된다.

### 테스트 D: write 후 일부 LPN TRIM

목적: TRIM이 page write 통계에 섞이지 않는지 확인

기대:

- TRIM 직후 host/NAND/GC page write counter는 변하지 않음
- 대상 mapping은 unmapped
- old page는 invalid
- old rmap은 `INVALID_LPN`
- erase는 즉시 발생하지 않고 이후 GC에서만 증가

### 테스트 E: FDP smoke test

목적: non-FDP baseline 계측의 격리 확인

기대:

- 기존 FDP write/RU/GC 경로가 그대로 실행됨
- controller FDP `hbmw/mbmw/mbe` 의미가 바뀌지 않음
- non-FDP 전용 stats를 사용하기로 했다면 FDP 실행에서 증가하지 않음
- `ssd->wp`를 FDP 경로가 사용하지 않음

---

# 9. 다음 한 단계

지금 할 작업은 다음 세 개로 제한하는 것이 좋다.

1. Phase 1 counter 각각의 의미를 위 표대로 확정한다.
2. `struct ssd`, `ssd_init()`, `ssd_write()`, `gc_write_page()`,
   `do_gc()`에만 non-FDP baseline 계측을 추가한다.
3. 순차 write, 반복 overwrite, 실제 GC workload로
   `NAND writes = host writes + GC writes` 관계를 검증한다.

이 검증이 끝날 때까지 LPN temperature metadata, 두 write pointer,
Hot/Cold free pool 구현은 시작하지 않는다.
