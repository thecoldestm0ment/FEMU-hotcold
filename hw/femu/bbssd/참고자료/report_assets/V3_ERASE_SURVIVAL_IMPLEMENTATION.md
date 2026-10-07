# FEMU V3: LBA rewrite interval + erase-survival classifier

## 1. V3의 목표

V3는 `hotcold/v1`을 기준으로 만들었다. 따라서 V1의 Hot/Cold line pool, 두 write pointer, borrowing, GC 정책은 그대로 두고 **Host classifier에 erase feedback만 추가**했다.

V3의 판단 원칙은 다음과 같다.

| 조건 | 판단 | 이유 |
|---|---|---|
| 첫 write | COLD | 빈도를 판단할 이전 write가 없음 |
| `interval <= T_fast` | HOT | 매우 빠른 rewrite |
| `T_fast < interval <= T_slow` 그리고 `survival < R` | HOT | 경계 구간이지만 erase survival 증거가 적음 |
| `T_fast < interval <= T_slow` 그리고 `survival >= R` | COLD | 현재 version이 block erase를 살아남은 증거로 보정 |
| `interval > T_slow` | COLD | 매우 느린 rewrite |

기본값은 `T_fast=1024`, `T_slow=4096`, `R=1`이다. 여기서 interval은 wall-clock 시간이 아니라 **Host page-write sequence 차이**다. 따라서 보고서에서는 다음과 같이 표현하는 것이 정확하다.

> Host-write sequence 기준 rewrite interval을 이용해 LBA update frequency를 추정하였다.

## 2. 전체 데이터 흐름

```text
Host write
  -> 이전 Host write와의 interval 계산
  -> 매우 빠름/경계/매우 느림 구간 구분
  -> 경계 구간에서만 erase_survival_count 확인
  -> HOT/COLD state 저장
  -> Hot/Cold write pointer로 Host page 기록
  -> 새 Host version이 시작되므로 survival count를 0으로 reset

GC
  -> 기존 state만 읽어 Hot/Cold write pointer 선택
  -> valid page relocation
  -> source block이 실제 free/erase 처리될 때
  -> 살아남은 현재 LPN version의 erase_survival_count만 +1
```

핵심은 GC가 `last_write_seq`, `write_count`, `update_interval`, `state`를 바꾸지 않는다는 점이다. GC는 Host 요청이 아니므로 LBA-frequency history를 학습하지 않고, erase feedback인 `erase_survival_count`만 갱신한다.

## 3. LPN metadata

`ftl.h`의 LPN별 metadata에 `erase_survival_count`를 하나 추가했다.

```c
typedef struct LpnMeta {
    uint64_t last_write_seq; /* 마지막 host page-write sequence */
    uint64_t update_interval; /* 직전 write와 현재 write의 sequence 차이 */
    uint32_t write_count; /* 이 LPN에서 관찰한 host write 수 */
    uint32_t erase_survival_count; /* 현재 version이 살아남은 block erase 수 */
    LpnState state; /* 현재 UNSEEN/COLD/HOT 분류 */
} LpnMeta;
```

이 counter의 단위는 LPN의 영구적 누적값이 아니라 **현재 Host-written version의 lifetime**이다.

- Host write로 새 version이 생기면 0으로 되돌린다.
- 그 version이 valid page로서 GC relocation되고 source block이 erase될 때 1 증가한다.
- 다음 Host write에서 분류 신호로 소비된다.
- 크기가 `UINT32_MAX`에 다르면 wrap하지 않고 포화한다.

## 4. Host classifier

실제 분기는 `ssd_classify_lpn_write()`에 집중했다. 내부 enum과 함수는 특정 실험 버전이 아니라 코드의 역할을 드러내는 이름을 사용한다.

```c
if (meta->write_count == 0) {
    meta->update_interval = 0;
    meta->state = LPN_STATE_COLD;
    reason = CLASS_REASON_FIRST_WRITE;
} else {
    meta->update_interval = ssd->host_write_seq - meta->last_write_seq;
    if (meta->update_interval <= ssd->hot_rewrite_window) {
        meta->state = LPN_STATE_HOT;
        reason = CLASS_REASON_FAST_REWRITE;
    } else if (meta->update_interval > ssd->hot_boundary_window) {
        meta->state = LPN_STATE_COLD;
        reason = CLASS_REASON_SLOW_REWRITE;
    } else if (*survival_before >= ssd->erase_survival_threshold) {
        meta->state = LPN_STATE_COLD;
        reason = CLASS_REASON_ERASE_SURVIVAL_COLD;
    } else {
        meta->state = LPN_STATE_HOT;
        reason = CLASS_REASON_BOUNDARY_HOT;
    }
}
```

erase feedback은 `1024 < interval <= 4096` 경계 구간에서만 사용한다. 이렇게 한 이유는 두 신호의 역할을 분리하기 위해서다.

- rewrite가 매우 빠르거나 매우 느리면 LBA 신호만으로 결정한다.
- LBA 신호가 애매할 때만 erase activity를 secondary signal로 사용한다.
- V1 `T=1024`와 V1 `T=4096` 사이의 행동을 V3가 선택하므로 control 실험이 명확해진다.

분류가 끝나면 현재 Host write가 새 version을 만들었으므로 survival history만 소비한다.

```c
meta->last_write_seq = ssd->host_write_seq;
if (meta->write_count != UINT32_MAX) {
    meta->write_count++;
}

/* 새 Host version은 erase survival을 다시 0부터 센다. */
meta->erase_survival_count = 0;
```

## 5. erase-survival feedback을 기록하는 시점

V3는 `gc_write_page()` 직후에 counter를 올리지 않는다. 그 시점은 단순히 relocation이 완료된 시점이기 때문이다.

대신 `mark_block_free()`에서 source block의 valid page를 확인한다. 이 함수는 같은 block의 `erase_cnt`와 전체 `block_erases`를 증가시키는 실제 logical erase commit 경로이다.

```c
for (int i = 0; i < spp->pgs_per_blk; i++) {
    pg = &blk->pg[i];
    if (!ssd->fdp_enabled && pg->status == PG_VALID) {
        source_ppa.g.pg = i;
        ssd_record_erase_survivor(ssd, &source_ppa);
    }
    pg->status = PG_FREE;
}

blk->erase_cnt++;
ssd->block_erases++;
```

`ssd_record_erase_survivor()`는 source PPA의 reverse mapping으로 LPN을 찾고 포화 증가한다.

```c
lpn = get_rmap_ent(ssd, source_ppa);
meta = &ssd->lpn_meta[lpn];

if (meta->erase_survival_count != UINT32_MAX) {
    meta->erase_survival_count++;
}
```

non-FDP GC는 `clean_one_block()`으로 valid page를 먼저 이동한 뒤 반드시 `mark_block_free()`로 같은 source block을 erase한다. 따라서 이 feedback은 단순 GC copy counter가 아니라 실제 block erase event와 1:1로 연결된다.

FDP 경로는 V3 Hot/Cold classifier의 범위가 아니므로 `!ssd->fdp_enabled`로 명시적으로 제외했다.

## 6. GC placement와 frequency metadata 불변

GC relocation은 V1처럼 저장된 `state`만 읽어 destination write pointer를 고른다.

```c
wpp = ssd_select_write_pointer(ssd, lpn);
if (ssd->lpn_meta[lpn].state == LPN_STATE_HOT) {
    ssd->gc_hot_writes++;
} else {
    ssd->gc_cold_writes++;
}
```

이 경로는 `ssd_classify_lpn_write()`를 호출하지 않는다. 따라서 GC는 다음 frequency metadata를 바꾸지 않는다.

- `last_write_seq`
- `update_interval`
- `write_count`
- `state`

GC가 바꾸는 classifier input은 `erase_survival_count`뿐이며, 이것도 source block erase가 확정되는 경로에서만 증가한다.

## 7. TRIM과 stats reset

TRIM은 logical lifetime을 끝낸다. 기존 V1의 `ssd_reset_lpn_metadata()`가 `LpnMeta` 전체를 0으로 만드므로 새 field도 자동으로 reset된다.

```c
static void ssd_reset_lpn_metadata(struct ssd *ssd, uint64_t lpn)
{
    ftl_assert(ssd->lpn_meta != NULL);
    ftl_assert(valid_lpn(ssd, lpn));
    memset(&ssd->lpn_meta[lpn], 0, sizeof(ssd->lpn_meta[lpn]));
}
```

반면 `cdw10=5`가 호출하는 `ssd_reset_stats()`는 measurement counter만 reset한다. LPN metadata와 physical mapping/page/line/write-pointer state는 유지된다. 즉 preconditioning 후의 기존 SSD를 overwrite하는 실험 의미를 그대로 보존한다.

## 8. 런타임 설정과 validation

새 설정은 다음 환경변수로 전달한다.

| 환경변수 | 기본값 | 의미 |
|---|---:|---|
| `FEMU_HOT_REWRITE_WINDOW` | 1024 | `T_fast` |
| `FEMU_HOT_BOUNDARY_WINDOW` | 4096 | `T_slow` |
| `FEMU_ERASE_SURVIVAL_THRESHOLD` | 1 | `R` |

모두 10진수 positive integer만 허용한다. 또한 `T_slow > T_fast`를 강제하여 경계 구간이 비어 있거나 뒤집히는 설정을 시작 시점에 거부한다.

`run-blackbox.sh`는 두 신규 변수를 기존 `sudo` 경계 너머 QEMU에 명시적으로 전달한다.

```bash
FEMU_HOT_REWRITE_WINDOW=${FEMU_HOT_REWRITE_WINDOW} \
FEMU_HOT_BOUNDARY_WINDOW=${FEMU_HOT_BOUNDARY_WINDOW} \
FEMU_ERASE_SURVIVAL_THRESHOLD=${FEMU_ERASE_SURVIVAL_THRESHOLD} \
./qemu-system-x86_64 \
```

## 9. 관찰용 counter

`BBSSD-STATS version=V3` 한 줄에 기존 WAF/GC/Hot-Cold 통계와 다음 reason counter를 함께 출력한다.

| counter | 의미 |
|---|---|
| `host_cold_first_writes` | first write라 COLD |
| `host_hot_fast_writes` | `interval <= T_fast`라 HOT |
| `host_hot_boundary_writes` | 경계 구간 + `survival < R`라 HOT |
| `host_cold_survival_writes` | 경계 구간 + `survival >= R`라 COLD |
| `host_cold_slow_writes` | `interval > T_slow`라 COLD |

`boundary_survival_zero/one/two/three_plus`는 **경계 구간 Host write event**에서 관찰한 survival histogram이다. 이것은 unique LPN 수가 아니라 분류 event 수다. 300초 예비 실험에서 이 분포를 보고 `R`을 정할 수 있다.

`R=1`은 기본 시작값이며 최종 튜닝 결론이 아니다. 히스토그램이 거의 0/1에 몰리면 `R=1`을 유지하고, 의미 있게 퍼져 있을 때만 추가 검토하면 된다.

marker correctness 실험을 위해 두 trace도 제공한다.

```text
[CLASSIFY] lpn=... state=... reason=... interval=... survival_before=... R=...
[ERASE_SURVIVE] lpn=... count=... source ch=... lun=... blk=... pg=...
```

performance 실험에서 marker를 쓰지 않으면 이 LPN별 trace는 발생하지 않고 집계 counter만 남는다.

## 10. counter invariant

`counter_invariant=PASS`는 다음 관계를 모두 만족해야 출력된다.

```text
nand_page_writes = host_page_writes + gc_page_writes

host_hot_writes + host_cold_writes = host_page_writes

gc_hot_writes + gc_cold_writes = gc_page_writes

host_hot_writes
= host_hot_fast_writes + host_hot_boundary_writes

host_cold_writes
= host_cold_first_writes
  + host_cold_survival_writes
  + host_cold_slow_writes

boundary histogram total
= host_hot_boundary_writes + host_cold_survival_writes
```

즉 분류 reason 하나가 누락되거나 중복 집계되면 실험 결과에서 즉시 `FAIL`로 확인할 수 있다.

## 11. 최소 수정으로 유지한 부분

V3에서 변경하지 않은 핵심 경로는 다음과 같다.

- Hot/Cold line pool의 50:50 초기 구성
- pool 고갈 시 borrowing
- Hot/Cold dual write pointer
- victim line 선택과 GC 정책
- GC relocation이 기존 `state`를 따르는 placement
- WAF, average GC copy, GC count, block erase 계측
- TRIM의 매핑 무효화 경로

따라서 V1과 V3의 차이는 주로 경계 구간 classifier와 per-version erase feedback에 한정된다.

## 12. 권장 correctness 검증

실제 실험 전에 다음만 확인하면 된다.

1. first write가 `FIRST_WRITE/COLD`인지 확인한다.
2. `interval <= 1024`가 `FAST_REWRITE/HOT`인지 확인한다.
3. `1024 < interval <= 4096` 구간에서 survival 0은 `BOUNDARY_HOT/HOT`인지 확인한다.
4. 같은 구간에서 survival 1 이상은 기본 `R=1` 설정에서 `ERASE_SURVIVAL_COLD/COLD`인지 확인한다.
5. `interval > 4096`가 `SLOW_REWRITE/COLD`인지 확인한다.
6. GC 전후 `last_write_seq`, `write_count`, `update_interval`, `state`가 같고 survival만 증가하는지 확인한다.
7. TRIM 후 첫 write가 다시 `FIRST_WRITE/COLD`인지 확인한다.
8. 최종 stats의 `counter_invariant=PASS`를 확인한다.

## 13. 최종 비교가 설계 신호를 분리하는 방법

| 비교군 | 경계 구간 처리 |
|---|---|
| Baseline | Hot/Cold 분리 없음 |
| V1 `T=1024` | 경계 구간 전체 COLD |
| V1 `T=4096` | 경계 구간 전체 HOT |
| V3 `1024/4096/R` | erase survival에 따라 HOT 또는 COLD |

V3가 V1 `T=1024`보다 좋은지만 비교하면 Hot 범위를 넓힌 효과와 erase signal의 효과를 분리하기 어렵다. V1 `T=4096`을 control로 꼭 포함하면 다음과 같이 해석할 수 있다.

- V3가 두 V1 control보다 좋음: erase feedback에 의한 경계 선택이 유효했음.
- V3가 T=1024보다 좋지만 T=4096보다 나쁨: erase signal은 작동했지만 이 workload에서는 경계 전체를 HOT으로 두는 편이 더 유리했음.
- V3와 T=4096이 비슷함: survival이 0인 경계 event가 대부분인지 histogram으로 확인.
- V3가 두 control보다 나쁨: feedback threshold, pool 구성, 또는 분류 흔들림을 reason counter와 GC 지표로 추가 분석.

## 14. 구현 검증 상태

코드 작성 후 다음 정적 검증을 통과했다.

```text
git diff --check                         PASS
bash -n hw/femu/scripts/run-blackbox.sh PASS
ninja -C build-femu                     PASS
```

이 문서는 구현 설명이며, runtime correctness 결과나 performance 결과를 포함하지 않는다.
