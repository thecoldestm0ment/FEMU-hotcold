# Phase 2: LPN Hot/Cold 메타데이터 쉽게 이해하기
## 1. 이 단계의 한 문장 목표
Phase 2는 데이터를 아직 나누어 쓰지 않는다.
각 LPN의 host write 이력만 기록한다.
그 이력으로 현재 상태를 Cold 또는 Hot으로 판단한다.
실제 PPA 할당은 당시의 기존 write pointer를 그대로 사용한다.
즉, 이 단계는 “관찰과 분류”만 담당한다.
## 2. 왜 LPN 단위로 기록하는가
Host는 SSD에 LBA 범위로 write를 요청한다.
FTL은 LBA를 NAND page 크기의 LPN으로 바꾼다.
같은 LPN이 자주 다시 쓰이면 그 데이터는 수명이 짧다.
이런 LPN을 Hot으로 볼 수 있다.
오랫동안 다시 쓰이지 않으면 Cold로 볼 수 있다.
Hot/Cold line 배치는 이후 단계에서 이 분류를 사용한다.
## 3. Phase 2에서 하지 않는 일
`wp_hot`과 `wp_cold`로 page를 나누어 쓰지 않는다.
Hot/Cold free line pool을 만들지 않는다.
victim selection 정책을 바꾸지 않는다.
GC migration 목적지를 바꾸지 않는다.
NAND latency 계산을 바꾸지 않는다.
FDP의 placement 동작도 바꾸지 않는다.
## 4. LPN 상태 타입
상태는 세 가지다.
```c
typedef enum LpnState {
    LPN_STATE_UNSEEN = 0,
    LPN_STATE_COLD,
    LPN_STATE_HOT,
} LpnState;
```
`UNSEEN`은 아직 host write가 없는 상태다.
`COLD`는 첫 write이거나 재기록 간격이 긴 상태다.
`HOT`은 짧은 간격으로 다시 기록된 상태다.
`UNSEEN`을 0으로 둔 이유가 중요하다.
`g_new0()`으로 배열을 만들면 모든 상태가 자동으로 `UNSEEN`이 된다.
## 5. LPN metadata 구조체
현재 구조체는 다음과 같다.
```c
typedef struct LpnMeta {
    uint64_t last_write_seq;
    uint64_t update_interval;
    uint32_t write_count;
    LpnState state;
} LpnMeta;
```
이 구조체 하나가 LPN 하나의 이력을 표현한다.
배열 index가 LPN이다.
예를 들어 LPN 100의 정보는 다음 위치에 있다.
```c
ssd->lpn_meta[100]
```
## 6. `last_write_seq`
이 필드는 마지막 host write의 논리적 순서를 저장한다.
실제 시각이나 나노초가 아니다.
Host page write가 처리될 때마다 sequence가 증가한다.
예를 들어 전체 write 순서가 다음과 같다고 하자.
```text
sequence 1: LPN 10
sequence 2: LPN 20
sequence 3: LPN 10
```
세 번째 write 후 LPN 10의 `last_write_seq`는 3이다.
## 7. 실제 시간 대신 sequence를 쓰는 이유
VM 실행 속도는 매번 조금씩 다를 수 있다.
Host CPU 부하도 달라질 수 있다.
NAND latency 설정도 실험마다 달라질 수 있다.
실제 시간을 쓰면 이런 조건이 분류에 섞인다.
Sequence는 host page write 순서만 본다.
따라서 같은 요청 순서에서 분류를 재현하기 쉽다.
## 8. `update_interval`
이 값은 현재 write와 직전 write 사이의 거리다.
계산식은 다음과 같다.
```c
meta->update_interval =
    ssd->host_write_seq - meta->last_write_seq;
```
간격이 작으면 최근에 같은 LPN이 다시 쓰인 것이다.
간격이 크면 그동안 다른 LPN write가 많이 발생한 것이다.
첫 write에는 직전 write가 없다.
그래서 첫 write의 interval은 0으로 둔다.
## 9. `write_count`
이 값은 해당 LPN에서 관찰한 host write 수다.
첫 write인지 rewrite인지 구분할 때 사용한다.
```c
if (meta->write_count == 0) {
    meta->state = LPN_STATE_COLD;
}
```
현재 분류 정책은 interval을 주로 사용한다.
`write_count`는 상태 검증과 디버깅에도 유용하다.
NVMe command 개수가 아니라 LPN 처리 횟수라는 점에 주의한다.
## 10. 전역 sequence
`struct ssd`에는 다음 필드가 있다.
```c
uint64_t host_write_seq;
```
이 값은 SSD 전체의 host page write 순서다.
각 LPN마다 별도 시계를 만들지 않는다.
한 LPN을 처리할 때 한 번만 증가한다.
GC page copy에서는 증가하면 안 된다.
Read와 TRIM에서도 증가하지 않는다.
## 11. Hot 판단 기준
`struct ssd`에는 threshold가 저장된다.
```c
uint64_t hot_rewrite_window;
```
초기 기본값은 다음 상수다.
```c
#define HOT_REWRITE_WINDOW_DEFAULT 1024ULL
```
1024는 검증용 시작값이다.
최적값으로 확정한 값이 아니다.
Workload에 따라 나중에 실험으로 조정해야 한다.
## 12. 초기화 함수
초기화 책임은 다음 helper가 가진다.
```c
static void ssd_init_lpn_metadata(struct ssd *ssd)
```
먼저 공통 필드를 안전한 값으로 만든다.
```c
ssd->lpn_meta = NULL;
ssd->host_write_seq = 0;
ssd->hot_rewrite_window = 0;
```
FDP mode이면 여기서 반환한다.
```c
if (ssd->fdp_enabled) {
    return;
}
```
그래서 FDP는 불필요한 LPN metadata 메모리를 사용하지 않는다.
## 13. Metadata 배열 할당
non-FDP에서는 mapping table과 같은 page 수로 할당한다.
```c
ssd->lpn_meta = g_new0(LpnMeta, spp->tt_pgs);
```
`tt_pgs`는 FTL이 관리하는 전체 physical page 수다.
배열은 LPN index로 직접 접근한다.
`g_new0()`은 모든 byte를 0으로 초기화한다.
따라서 초기 상태는 다음과 같다.
```text
last_write_seq = 0
update_interval = 0
write_count = 0
state = LPN_STATE_UNSEEN
```
## 14. 초기화 호출 위치
`ssd_init()`에서 geometry와 FDP 여부가 정해진 뒤 호출한다.
```c
ssd->fdp_enabled = (...);
ssd_init_lpn_metadata(ssd);
```
Geometry가 먼저 필요한 이유는 `tt_pgs`를 알아야 하기 때문이다.
FDP 여부가 먼저 필요한 이유는 non-FDP에서만 할당하기 때문이다.
Mapping과 line 동작은 이 호출로 바뀌지 않는다.
## 15. 메모리 사용량
현재 `LpnMeta` 하나는 24바이트다.
8 GiB physical geometry의 page 수는 2,097,152개다.
따라서 계산은 다음과 같다.
```text
2,097,152 × 24 bytes = 50,331,648 bytes
                         = 48 MiB
```
초기화 시 실제 값을 한 번 출력한다.
```text
LPN metadata: entries=2097152
entry_size=24 total=48 MiB hot_window=1024
```
Per-page 로그는 출력하지 않는다.
## 16. Host write 갱신 helper
갱신 책임은 한 함수에 모았다.
```c
static void ssd_update_lpn_temperature(
    struct ssd *ssd, uint64_t lpn)
```
이 함수는 PPA를 할당하지 않는다.
Mapping도 변경하지 않는다.
Line 상태도 변경하지 않는다.
오직 LPN metadata만 갱신한다.
책임을 분리하면 이후 placement 코드와 섞이지 않는다.
## 17. 갱신 함수의 첫 단계
먼저 배열과 LPN 범위를 확인한다.
```c
ftl_assert(ssd->lpn_meta != NULL);
ftl_assert(valid_lpn(ssd, lpn));
meta = &ssd->lpn_meta[lpn];
```
non-FDP write에서만 호출되므로 metadata가 있어야 한다.
유효하지 않은 LPN으로 배열을 접근하면 메모리를 손상시킬 수 있다.
그래서 bounds 확인이 필요하다.
## 18. Sequence 증가와 overflow
Sequence는 다음처럼 증가한다.
```c
if (ssd->host_write_seq != UINT64_MAX) {
    ssd->host_write_seq++;
}
```
`UINT64_MAX`에서 다시 0으로 돌아가지 않는다.
이를 saturation이라고 한다.
Wrap하면 최신 sequence가 과거보다 작아질 수 있다.
그러면 interval 계산이 잘못된다.
64-bit 최댓값 도달은 현실적으로 매우 어렵다.
그래도 코드의 의미를 명확히 하기 위해 처리했다.
## 19. 첫 write 처리
첫 write는 `write_count == 0`으로 구분한다.
```c
if (meta->write_count == 0) {
    meta->update_interval = 0;
    meta->state = LPN_STATE_COLD;
}
```
첫 write부터 Hot으로 만들지 않는다.
그렇게 하면 순차 write의 모든 새 LPN도 Hot이 될 수 있다.
처음 본 데이터는 재기록 빈도를 아직 알 수 없다.
그래서 안전하게 Cold로 시작한다.
## 20. Rewrite 처리
두 번째 write부터 interval을 계산한다.
```c
meta->update_interval =
    ssd->host_write_seq - meta->last_write_seq;
```
그다음 threshold와 비교한다.
```c
meta->state =
    meta->update_interval <= ssd->hot_rewrite_window ?
    LPN_STATE_HOT : LPN_STATE_COLD;
```
작거나 같으면 Hot이다.
크면 Cold다.
## 21. 마지막 정보 저장
판정이 끝나면 현재 sequence를 저장한다.
```c
meta->last_write_seq = ssd->host_write_seq;
```
다음 rewrite에서 이 값이 기준점이 된다.
그다음 write count를 올린다.
```c
if (meta->write_count != UINT32_MAX) {
    meta->write_count++;
}
```
Count도 최댓값에서 포화한다.
0으로 wrap하면 첫 write로 잘못 판단할 수 있기 때문이다.
## 22. `ssd_write()` 연결 위치
Metadata 갱신은 LPN loop 시작 부분에 있다.
```c
for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
    ssd_update_lpn_temperature(ssd, lpn);
    ppa = get_maptbl_ent(ssd, lpn);
    ...
}
```
Host page 하나마다 정확히 한 번 호출된다.
기존 PPA invalidation보다 먼저 분류한다.
이후 단계에서 방금 계산한 상태로 pointer를 고를 수 있기 때문이다.
Phase 2에서는 그 상태를 배치에 사용하지 않는다.
## 23. GC가 metadata를 바꾸지 않는 이유
GC write는 host가 요청한 새로운 write가 아니다.
이미 존재하는 valid page를 다른 PPA로 복사하는 작업이다.
GC가 sequence를 올리면 자주 이동된 LPN이 Hot으로 오인된다.
그러면 분류와 GC가 서로 영향을 주는 feedback loop가 생긴다.
따라서 `gc_write_page()`는 갱신 helper를 호출하지 않는다.
`gc_page_writes`만 Phase 1 통계로 증가한다.
## 24. Read가 metadata를 바꾸지 않는 이유
현재 정책은 write temperature만 본다.
Read 빈도와 write 수명은 같은 개념이 아니다.
읽기가 많아도 데이터가 곧 invalid될 것이라는 보장은 없다.
따라서 `ssd_read()`는 metadata를 변경하지 않는다.
Read temperature는 별도의 연구 정책으로 남긴다.
## 25. TRIM 정책
TRIM은 해당 logical data의 수명이 끝났다는 뜻으로 해석한다.
Reset helper는 다음과 같다.
```c
memset(&ssd->lpn_meta[lpn], 0,
       sizeof(ssd->lpn_meta[lpn]));
```
0 초기화 후 상태는 다시 `UNSEEN`이다.
다음 write는 새로운 첫 write이므로 Cold가 된다.
Global `host_write_seq`는 reset하지 않는다.
다른 LPN의 시간 순서를 보존해야 하기 때문이다.
## 26. Lazy decay 정책
전체 metadata 배열을 주기적으로 순회하지 않는다.
2백만 개가 넘는 entry를 반복해서 검사하면 비용이 크다.
현재는 같은 LPN에 다음 host write가 왔을 때 interval을 계산한다.
간격이 threshold보다 길면 그때 Cold로 내려간다.
이를 lazy decay라고 볼 수 있다.
장점은 background scan이 없다는 것이다.
단점은 다음 write 전까지 저장된 state가 Hot으로 남을 수 있다는 것이다.
현재 Phase 2에서는 state를 배치에 사용하지 않아 문제가 없다.
## 27. 간단한 예제: 첫 write
초기 상태를 가정한다.
```text
host_write_seq = 0
LPN 7 write_count = 0
LPN 7 state = UNSEEN
```
LPN 7에 write가 들어온다.
```text
host_write_seq = 1
update_interval = 0
write_count = 1
state = COLD
last_write_seq = 1
```
## 28. 간단한 예제: 빠른 rewrite
LPN 7의 마지막 sequence가 1이라고 하자.
현재 전체 sequence가 100이다.
LPN 7에 다시 write가 들어오면 sequence는 101이 된다.
```text
update_interval = 101 - 1 = 100
```
100은 기본 window 1024 이하이다.
따라서 상태는 Hot이다.
```text
state = HOT
```
## 29. 간단한 예제: 늦은 rewrite
LPN 7의 마지막 sequence가 101이라고 하자.
다른 LPN write가 많이 발생했다.
다음 LPN 7 write sequence가 5000이라면 다음과 같다.
```text
update_interval = 5000 - 101 = 4899
```
4899는 1024보다 크다.
따라서 상태는 다시 Cold가 된다.
Hot은 영구 표시가 아니다.
## 30. Mapping 불변식과의 관계
Metadata는 mapping table과 별도 배열이다.
분류 갱신이 `maptbl`을 직접 수정하지 않는다.
`rmap`도 직접 수정하지 않는다.
기존 overwrite 순서는 그대로 유지된다.
```text
old PPA 확인
old page invalid
old rmap 제거
new PPA 할당
new maptbl/rmap 등록
new page valid
```
Phase 2는 이 흐름 앞에 history 갱신 하나만 추가한다.
## 31. Phase 1 통계와의 관계
`host_write_seq`와 `host_page_writes`는 목적이 다르다.
`host_write_seq`는 분류의 논리 시계다.
`host_page_writes`는 WAF 측정 구간의 통계다.
`FEMU_RESET_ACCT`는 Phase 1 통계를 reset한다.
하지만 Phase 2 metadata와 sequence는 reset하지 않는다.
통계 구간이 바뀌어도 SSD가 학습한 write history는 유지된다.
## 32. FDP 경로 보존
FDP 여부를 먼저 확인한다.
FDP이면 `lpn_meta`를 할당하지 않는다.
```text
lpn_meta = NULL
host_write_seq = 0
hot_rewrite_window = 0
```
FDP host write는 별도 FDP 함수로 들어간다.
따라서 non-FDP temperature helper를 호출하지 않는다.
FDP RU와 placement hint 동작은 그대로 유지된다.
## 33. 핵심 불변식
유효한 LPN만 metadata 배열에 접근한다.
`UNSEEN`이면 `write_count == 0`이다.
첫 write 후 상태는 Cold다.
관찰된 LPN의 `last_write_seq`는 전역 sequence보다 클 수 없다.
Host page write당 sequence는 한 번만 증가한다.
GC와 read는 sequence를 증가시키지 않는다.
TRIM은 LPN entry만 초기화한다.
Mapping과 reverse mapping 관계는 바뀌지 않는다.
## 34. 최소 확인 방법
FEMU 시작 후 초기화 로그를 본다.
```bash
grep 'LPN metadata' build-femu/log
```
8 GiB geometry의 기대값은 다음과 같다.
```text
entries=2097152
entry_size=24
total=48 MiB
hot_window=1024
```
로그가 없으면 FDP mode인지 초기화 호출 위치를 확인한다.
## 35. 순차 write 테스트
처음 보는 LPN만 순서대로 기록한다.
모든 entry의 첫 상태는 Cold여야 한다.
`write_count`는 1이어야 한다.
`update_interval`은 0이어야 한다.
GC가 없다면 Phase 1 WAF는 1에 가까워야 한다.
이 테스트에서는 Hot 상태가 많이 나오면 안 된다.
## 36. 반복 write 테스트
같은 4 KiB LPN을 짧은 간격으로 반복 기록한다.
첫 write는 Cold다.
두 번째 write부터 interval이 window 이하면 Hot이다.
`write_count`는 write 횟수만큼 증가한다.
기존 PPA는 정상적으로 invalid되어야 한다.
최신 `maptbl`과 `rmap`은 서로 일치해야 한다.
## 37. 간격 write 테스트
먼저 특정 LPN을 Hot으로 만든다.
그다음 다른 LPN을 1024개보다 많이 기록한다.
원래 LPN을 다시 기록한다.
새 interval은 window보다 커야 한다.
상태는 Cold로 바뀌어야 한다.
이를 통해 Hot에서 Cold로 돌아갈 수 있는지 확인한다.
## 38. TRIM 테스트
특정 LPN을 여러 번 써서 Hot으로 만든다.
그 LPN에 TRIM을 보낸다.
기대 metadata는 다음과 같다.
```text
last_write_seq = 0
update_interval = 0
write_count = 0
state = UNSEEN
```
다시 write하면 첫 write이므로 Cold여야 한다.
## 39. 흔한 실수
GC에서 update helper를 호출하면 안 된다.
NVMe command마다 sequence를 한 번만 올리면 안 된다.
한 command가 여러 LPN을 포함할 수 있기 때문이다.
첫 write를 Hot으로 만들면 안 된다.
Count를 wrap시켜 0으로 만들면 안 된다.
FDP에서도 큰 metadata 배열을 무조건 할당하면 안 된다.
Threshold 1024를 최적값이라고 단정하면 안 된다.
## 40. Phase 2 완료 판단
Metadata 배열이 non-FDP에서만 존재한다.
초기 entry가 모두 `UNSEEN`이다.
첫 write가 Cold다.
짧은 rewrite가 Hot이다.
긴 rewrite가 Cold다.
TRIM 후 다시 `UNSEEN`이다.
GC와 read가 history를 바꾸지 않는다.
기존 mapping과 WAF 불변식이 유지된다.
이 조건을 확인한 뒤에만 write pointer 분리 단계로 넘어간다.
