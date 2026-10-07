# FEMU Blackbox FTL의 시간 창 LBA 요청 빈도와 Erase-Event Feedback 기반 Hot/Cold Data Placement

## WAF 및 GC 비용 평가

## 초록

본 프로젝트는 FEMU Blackbox FTL에서 Hot/Cold data를 서로 다른 line에 배치하고, 분리 전·후의 Write Amplification Factor(WAF)를 비교한다. 과제가 요구한 두 분류 신호를 직접 반영하기 위해 최종 V4는 LPN별 1초 observation window에서 Host write 횟수를 세고, 빈도가 애매한 구간에서는 현재 LPN version이 GC relocation 후 source block erase를 겪은 횟수를 보정 신호로 사용했다. Hot/Cold free-line pool, dual write pointer, pool borrowing을 구현했고, GC victim selection은 기존 global greedy 정책을 유지하여 placement 효과와 GC-policy 효과를 섞지 않았다.

실험은 raw NAND 8 GiB 중 6 GiB를 Host에 노출한 장치에서 전체 6 GiB를 sequential preconditioning한 뒤, 5 GiB 영역에 16 KiB Zipf 0.99 random write를 3600초 실행했다. 동일 seed `20260824`의 단일 run에서 Baseline WAF는 `7.990`, rewrite-interval 분류기를 쓴 V1-P20은 `6.972`, time-window frequency와 erase feedback을 결합한 V4-P20은 `7.044`였다. Baseline 대비 각각 12.7%, 11.8% 낮았지만, V4는 간단한 V1보다 낮은 WAF를 보이지 않았다. V4의 erase-related reason counter는 mechanism이 실제 placement에 사용되었음을 보여주지만, frequency-only V4 대조군이 없으므로 erase feedback 단독 효과는 분리할 수 없다. 또한 본 실험은 WAF와 NAND write를 측정한 것이며 SSD lifetime을 직접 측정한 것은 아니다.

---

# 1. Project Overview / Assignment Requirements

## 1.1 프로젝트 목표와 비교 시나리오

과제의 핵심은 기존 FEMU Blackbox FTL의 line 구조를 Hot/Cold pool로 나누고, 일정 시간 동안의 LBA 요청 빈도와 SSD 내부 block erase 정보로 data temperature를 판단하며, 분리 전·후 WAF를 비교하는 것이다. 본 보고서에서 시나리오를 다음과 같이 정의한다.

- **Scenario 1 — Baseline:** Hot/Cold 분리 없이 단일 write pointer와 free-line pool을 쓰는 Blackbox FTL
- **Scenario 2a — V1-P20 control:** rewrite interval 기반 분류 + 20:80 Hot/Cold pool
- **Scenario 2b — V4-P20 final:** 1초 LPN별 request-frequency window + actual erase-event feedback + 20:80 Hot/Cold pool

V1-P20은 복잡한 분류기가 실제로 더 나은지를 확인하기 위한 간단한 control이다. V1-P20과 V4-P20은 동일한 20:80 pool과 GC victim policy를 사용하므로 **classifier design 전체의 차이**를 비교한다. 단, 두 버전은 LBA 신호 자체가 다르므로 이 비교를 erase feedback 단독 효과라고 해석하지 않는다.

## 1.2 과제 요구사항과 실제 구현의 대응

| 과제 요구사항 | 실제 구현 | 검증 방법 |
|---|---|---|
| 일정 시간 동안 LBA 요청 빈도 | LPN별 `1,000,000,000 ns` window에서 Host page write 횟수 측정 | frequency reason counter와 Hot/Cold routing counter |
| SSD 내부 block erase 정보 | 현재 logical version이 GC 이동 후 source block의 실제 erase 경계를 통과할 때 `erase_event_count++` | erase-event histogram과 erase-signal reason counter |
| Hot/Cold line pool | 총 128 lines을 Hot 25 / Cold 103으로 초기화, dual write pointer와 borrowing 사용 | boot log, pool-empty/borrow counter |
| WAF 계산 | Host, GC, NAND page write를 별도 계수 | `NAND = Host + GC`, `counter_invariant=PASS` |
| 분리 전·후 비교 | Baseline, V1-P20, V4-P20에 동일 workload/seed 적용 | 3600초 final stats와 fio output |

V4의 erase counter는 표준 SMART metric이 아니라, 과제의 block-erase 정보를 현재 LPN version에 연결하기 위해 본 프로젝트가 정의한 **project-defined derived metric**이다. 전체 block의 누적 erase count를 모든 LPN에 같이 적용하면 해당 LPN이 어떤 GC cycle을 겪었는지 알 수 없기 때문에, reverse mapping으로 logical version에 event를 연결했다.

---

# 2. Hot/Cold Classification Design

## 2.1 V1-P20: rewrite interval control

V1은 전체 Host page-write stream의 sequence를 증가시키고, 동일 LPN의 직전 write와 현재 write 사이의 sequence 차이를 rewrite interval로 사용한다.

\[
Interval(LPN)=Current\ Host\ Page\ Write\ Seq-Last\ Write\ Seq(LPN)
\]

- 첫 write: COLD
- `interval <= 4096`: HOT
- `interval > 4096`: COLD

이 값은 시간당 요청 횟수가 아니다. 동일 LPN이 전체 Host write stream에서 얼마나 빨리 다시 나타났는지로 update frequency를 근사하는 **sequence-based rewrite interval**이다. 간단하고 emulator 실행 속도에 덜 민감하지만, 과제의 “일정 시간”을 literal하게 구현한 방식은 아니다. 이를 보완하기 위해 V4에서 request timestamp 기반 time window를 도입했다.

## 2.2 V4-P20: LPN별 time-window frequency

V4의 각 LPN metadata는 `window_start_ns`, `writes_in_window`, `erase_event_count`, `state`를 저장한다. 시각은 classifier에서 새로 wall-clock을 읽지 않고, FEMU의 NVMe request에 기록된 `req->stime`을 nanosecond 단위로 사용한다. 하나의 NVMe request가 여러 4 KiB LPN을 포함하면 그 LPN들은 동일한 request start time을 사용한다.

기본 parameter는 다음과 같다.

| Parameter | 값 | 의미 |
|---|---:|---|
| `FEMU_FREQUENCY_WINDOW_NS` | 1,000,000,000 ns | LPN별 fixed-duration window |
| `FEMU_COLD_WRITES_PER_WINDOW` | 1 | 현재 개수가 1 이하이면 COLD |
| `FEMU_HOT_WRITES_PER_WINDOW` | 4 | 현재 개수가 4 이상이면 HOT |
| `FEMU_ERASE_EVENT_THRESHOLD` | 1 | 중간 빈도에서 1회 이상 erase event면 COLD |

현재 write는 **판정 전에** `writes_in_window`에 포함된다. 따라서 window 내 판정은 정확히 다음과 같다.

| 현재 write 포함 횟수 | Erase event | 결과 | Reason |
|---:|---:|---|---|
| 1 | 무관 | COLD | `LOW_FREQUENCY` |
| 2–3 | 0 | HOT | `ERASE_SIGNAL_HOT` |
| 2–3 | 1 이상 | COLD | `ERASE_SIGNAL_COLD` |
| 4 이상 | 무관 | HOT | `HIGH_FREQUENCY` |

요청 사이의 idle gap이 1초 이상이거나 timestamp가 뒤로 간 경우, 다음 Host write가 새 window를 연다. 그 write가 count 1이 되므로 COLD로 배치된다. 다만 expiration은 다음 Host write 도착 시점에 lazy하게 적용된다. 따라서 이전 state가 HOT인 LPN이 idle 중 GC로 이동하면, 다음 Host write 전까지는 저장된 HOT state를 사용할 수 있다.

## 2.3 Erase-event feedback의 정의와 lifecycle

`erase_event_count` 증가 시점은 GC 함수의 단순 호출 시점이 아니다. Valid page를 새 PPA로 relocation한 뒤 source block을 `mark_block_free()`로 실제 free 상태로 바꾸는 erase 경로에서, source PPA의 reverse mapping으로 LPN을 찾아 증가시킨다. 즉 “물리 page가 erase를 견딌다”는 뜻이 아니라, **현재 logical data version이 Host overwrite로 invalid되지 않고 relocation되어 source-block erase 이후에도 valid data로 유지되었음**을 뜻한다.

Host write가 도착하면 classifier는 판정 직전의 erase count를 사용한 후 0으로 초기화한다. 새 Host write가 새 logical version을 만들기 때문이다. TRIM은 해당 LPN metadata 전체를 0으로 지워 `UNSEEN`으로 돌린다.

## 2.4 Host frequency와 GC relocation의 분리

GC relocation은 `ssd_classify_lpn_write()`를 호출하지 않고 저장된 LPN state만 읽어 Hot 또는 Cold write pointer를 선택한다. 따라서 GC는 `window_start_ns`와 `writes_in_window`를 증가시키지 않으며, SSD 내부 data movement를 Host request로 잘못 학습하지 않는다. GC가 classifier에 제공하는 정보는 source block의 실제 erase에 연결된 `erase_event_count`뿐이다.

이 설계는 명확한 low/high frequency는 Host 신호로 결정하고, 중간 빈도에서만 internal erase activity를 secondary signal로 쓴다. 전역 erase 횟수가 주 판단을 압도하지 않게 하고, erase cycle을 겪었지만 Host frequency가 명확히 높은 LPN이 COLD로 바뀌는 것을 막기 위한 선택이다.

---

# 3. Hot/Cold Line Pool & Placement

## 3.1 20:80 pool과 dual write pointer

한 line은 8 channels × 8 LUNs의 동일 block number를 묶은 64 blocks이며, `256 pages/block × 64 = 16,384 pages`를 갖는다. 총 128 lines에 `FEMU_HOT_POOL_PERCENT=20`을 적용하면 integer division에 의해 Hot 25 lines, Cold 103 lines이 된다. 초기 `wp_hot`, `wp_cold`가 각각 active line 하나를 선점하므로 boot 직후 free line은 Hot 24, Cold 102였다.

20:80은 최적값이 아니다. 이전 개발 단계의 Cold-dominant write 구성과 50:50에서의 Cold-pool borrowing을 바탕으로 선택한 실험 parameter이며, 본 보고서는 10:90·20:80·50:50 sensitivity를 수행하지 않았다. 특히 V4의 실측 Host Hot ratio는 27.3%이므로, 20:80이 classifier output의 정확한 비율을 반영한다고도 주장하지 않는다.

## 3.2 Borrowing과 reserve 정책

요청된 class의 free list가 비면 반대 pool에서 line 하나를 가져오고 요청 class로 재지정한다. 해당 line이 향후 GC로 회수될 때도 현재 class pool로 돌아가므로, 초기 20:80은 완전히 고정된 partition이 아니다. 현재 코드에는 class별 최소 reserve line은 없다. 대신 전체 free-line count에 대한 기존 75%/95% GC threshold와, 필요 시 foreground forced GC가 전체 공간 고갈을 방어한다.

## 3.3 Host placement, GC placement, victim selection

Host write는 classifier가 갱신한 state에 따라 `wp_hot` 또는 `wp_cold`에 기록된다. GC relocation은 저장된 state에 따라 destination write pointer를 고르지만, victim line은 Hot/Cold별 다른 policy가 아닌 **하나의 global valid-page-count priority queue**에서 선택한다. 일반 GC는 invalid page가 line의 1/8보다 적으면 victim을 건너뛰는 기존 정책을 유지한다.

Hot은 greedy, Cold는 cost-benefit처럼 class-aware victim policy까지 동시에 바꾸면 WAF 차이가 placement 때문인지 GC policy 때문인지 구분할 수 없다. 따라서 본 실험은 classification과 placement만 변수로 두고 GC victim policy를 고정했다.

## 3.4 Preconditioning이 초기 상태에 미친 영향

Sequential preconditioning의 모든 LPN은 첫 write이므로 COLD로 배치된다. 20:80에서 Cold 103 lines은 6 GiB fill을 수용할 수 있어 preconditioning stats의 borrowing은 0이었다. Measurement reset은 classifier metadata를 지우지만 mapping, page/line state, active write pointer는 유지한다. 따라서 performance workload의 첫 측정 write는 classifier 관점에서 `UNSEEN`이어서 COLD이지만, 물리 장치는 Cold line 중심으로 전체가 한 번 채워진 overwrite 상태에서 시작한다. 이 초기 조건은 V1-P20과 V4-P20에 동일하게 적용했다.

---

# 4. Implementation

구현은 주로 `hw/femu/bbssd/ftl.c`, `ftl.h`, `bb.c`와 launcher에 추가했다. 본문에는 의미를 결정하는 핵심 code path만 인용한다.

## 4.1 LPN metadata와 frequency classification

**Listing 1. V4 LPN metadata와 핵심 분류 규칙**

```c
typedef struct LpnMeta {
    uint64_t window_start_ns;
    uint32_t writes_in_window;
    uint32_t erase_event_count;
    LpnState state;
} LpnMeta;

if (ssd_frequency_window_expired(ssd, meta, now_ns)) {
    meta->window_start_ns = now_ns;
    meta->writes_in_window = 0;
    meta->state = LPN_STATE_COLD;
}
if (meta->writes_in_window != UINT32_MAX) {
    meta->writes_in_window++;
}

if (meta->writes_in_window <= ssd->cold_writes_per_window) {
    meta->state = LPN_STATE_COLD;
} else if (meta->writes_in_window >= ssd->hot_writes_per_window) {
    meta->state = LPN_STATE_HOT;
} else if (*erase_events_before >= ssd->erase_event_threshold) {
    meta->state = LPN_STATE_COLD;
} else {
    meta->state = LPN_STATE_HOT;
}
meta->erase_event_count = 0;
```

2,097,152 LPN × 24 bytes로 runtime log에 기록된 metadata 크기는 48 MiB이다. Counter는 overflow 시 wrap하지 않고 포화하도록 구현했다. Runtime parameter는 positive integer만 허용하고 `hot_writes_per_window > cold_writes_per_window`를 검사한다.

## 4.2 Actual erase event 갱신과 GC 분리

**Listing 2. Source-block erase 경계에 연결된 feedback**

```c
for (int i = 0; i < spp->pgs_per_blk; i++) {
    pg = &blk->pg[i];
    if (!ssd->fdp_enabled && pg->status == PG_VALID) {
        source_ppa.g.pg = i;
        ssd_record_erase_event(ssd, &source_ppa);
    }
    pg->status = PG_FREE;
}
blk->erase_cnt++;
ssd->block_erases++;
```

`ssd_record_erase_event()`는 source PPA의 reverse mapping으로 LPN metadata를 찾아 `erase_event_count`를 증가시킨다. 반면 `gc_write_page()`는 frequency classifier를 호출하지 않고 다음처럼 state만 읽는다.

```c
wpp = ssd_select_write_pointer(ssd, lpn);
if (ssd->lpn_meta[lpn].state == LPN_STATE_HOT) {
    ssd->gc_hot_writes++;
} else {
    ssd->gc_cold_writes++;
}
```

## 4.3 Pool allocation과 borrowing

**Listing 3. 요청 class pool 고갈 시 fallback**

```c
line = pop_free_line(requested, requested_cnt);
if (!line) {
    if (data_class == LINE_CLASS_HOT) {
        ssd->hot_pool_empty_count++;
    } else {
        ssd->cold_pool_empty_count++;
    }
    line = pop_free_line(fallback, fallback_cnt);
    borrowed = true;
}
if (borrowed) {
    ssd->borrow_count++;
}
line->data_class = data_class;
```

Borrowing한 line을 요청 class로 변경하기 때문에, 향후 GC 회수 후에도 그 class의 free list로 돌아간다. Boot와 run-time에 free count 합계와 dual write pointer가 유효한지 검사하여 pool 관리 오류를 조기에 중단한다.

## 4.4 WAF accounting과 invariant

**Listing 4. Stats에서 동시에 검증하는 핵심 관계**

```c
bool counters_valid =
    ssd->nand_page_writes ==
        ssd->host_page_writes + ssd->gc_page_writes &&
    ssd->host_hot_writes + ssd->host_cold_writes ==
        ssd->host_page_writes &&
    ssd->gc_hot_writes + ssd->gc_cold_writes ==
        ssd->gc_page_writes &&
    ssd->host_hot_writes ==
        ssd->host_hot_high_frequency_writes +
        ssd->host_hot_erase_signal_writes;
```

WAF와 average GC copy는 다음과 같이 계산한다.

\[
WAF=\frac{NAND\ Page\ Writes}{Host\ Page\ Writes}
=1+\frac{GC\ Page\ Writes}{Host\ Page\ Writes}
\]

\[
Average\ GC\ Copy=\frac{GC\ Page\ Writes}{GC\ Count}
\]

최종 V4 stats는 NAND/Host/GC, Host Hot/Cold, GC Hot/Cold, reason counter, mid-frequency histogram의 합계를 함께 검증하여 `counter_invariant=PASS`를 출력했다.

## 4.5 Reset, TRIM, runtime configuration

Admin command `opcode=0xef, cdw10=5`는 현재 stats를 먼저 출력한 뒤 accounting counter와 LPN classifier metadata를 초기화한다. Mapping table, page state, line state, active write pointer는 유지한다. TRIM은 해당 LPN의 mapping을 해제하고 metadata를 `UNSEEN` 상태로 돌린다.

V4 launcher는 `FEMU_HOT_POOL_PERCENT`, `FEMU_FREQUENCY_WINDOW_NS`, `FEMU_HOT_WRITES_PER_WINDOW`, `FEMU_COLD_WRITES_PER_WINDOW`, `FEMU_ERASE_EVENT_THRESHOLD`를 sudo 경계 너머 QEMU에 전달한다. Final run 전 300초 sanity test에서 25/103 pool, WP 할당 후 24/102 free lines, borrowing 4회, emergency GC 0회, crash 없음과 `counter_invariant=PASS`를 확인했다.

---

# 5. Experimental Setup

## 5.1 비교군과 source version

| 비교군 | Branch | Commit | 핵심 설정 | 실험 출처 |
|---|---|---|---|---|
| Baseline | `hotcold/base` | `c0237bb12f183d87ea1abf2bf52e92f900f6c587` | Hot/Cold 미적용 | 기존 valid 동일-seed run 재사용 |
| V1-P20 | `hotcold/v1-p20` | `a2adff42934f166a6bb092d58bbe98a2aa478ac6` | `T=4096`, pool 20:80 | 2026-09-01 신규 run |
| V4-P20 | `hotcold/v4` | `6a012b579dc80b3f1e0fbb649ebf7e2da31bc4d4` | 1초, cold=1, hot=4, erase=1, pool 20:80 | 2026-09-01 신규 run |

Baseline은 seed `20260824`, 동일 geometry, 6 GiB preconditioning, 5 GiB/no-offset workload, 3600초 조건을 만족한 이전 valid result를 재사용했다. V1-P20과 V4-P20은 각각 fresh FEMU process에서 순차 실행했다. 따라서 세 run이 동일 날짜의 하나의 세션에서 연속 실행된 것은 아니다.

## 5.2 Device geometry와 GC 설정

| 항목 | 설정 |
|---|---:|
| Host target | `/dev/nvme0n1`, 6 GiB (`6,442,450,944` bytes) |
| Raw NAND | 8 GiB |
| 미노출 용량 | 2 GiB, raw 기준 25% |
| Sector / page | 512 B / 4 KiB (`8 sectors/page`) |
| Pages per block | 256 |
| Blocks per plane | 128 |
| Planes per LUN | 1 |
| LUNs per channel / channels | 8 / 8 |
| Total lines / pages per line | 128 / 16,384 |
| GC threshold / high threshold | 75% / 95% |
| Guest | Ubuntu 20.04 계열, 4 vCPU, 4 GiB RAM |
| Acceleration | WSL2 + KVM |
| fio | `fio-3.16` |

## 5.3 Run procedure와 reset 범위

각 run은 다음 절차를 따랐다.

1. 해당 branch/commit을 build하고 fresh FEMU/QEMU process를 시작한다.
2. `/dev/nvme0n1`이 정확히 6 GiB이고 partition/mount가 없으며 Guest root `/dev/sda2`와 다른 disk임을 확인한다.
3. 1 MiB sequential write로 전체 6 GiB를 한 번 채운 뒤 `sync`한다.
4. `cdw10=5`로 preconditioning stats를 출력하고 accounting 및 classifier measurement metadata를 reset한다.
5. 3600초 fio를 pause, marker, offset, 중간 snapshot 없이 끝까지 실행한다.
6. fio가 완전히 종료된 뒤 `cdw10=5`로 final stats를 수집하고 정상 poweroff한다.

Preconditioning command와 performance workload는 다음과 같다.

```bash
fio --name=fill --filename=/dev/nvme0n1 --direct=1 \
    --ioengine=libaio --rw=write --bs=1M --iodepth=32 \
    --numjobs=1 --size=6G --group_reporting=1

fio --name=final --filename=/dev/nvme0n1 --direct=1 \
    --time_based=1 --runtime=3600 --ioengine=libaio --rw=randwrite \
    --iodepth=128 --numjobs=32 --size=5G --bs=16k \
    --random_distribution=zipf:0.99 --randrepeat=1 \
    --randseed=20260824 --group_reporting=1
```

과제 원문의 `group_reporting=0`과 달리 신규 P20 run manifest에는 `group_reporting=1`이 기록되어 있다. 이 option은 32 jobs의 **출력을 합쳐 표시할지**를 결정하며 I/O 주소 범위, pattern, queue depth, seed를 변경하지 않는다. Baseline의 per-job IOPS는 합산한 total을 사용하여 P20의 aggregate IOPS와 맞춰 표기했다.

## 5.4 Result validity

V1-P20과 V4-P20은 target safety check, preconditioning, reset, fio가 모두 return code 0으로 종료했고 runtime은 각각 3600.507초, 3600.517초였다. 두 run 모두 `NAND=Host+GC`, `Host Hot+Cold=Host`, `GC Hot+Cold=GC`가 성립했고 `counter_invariant=PASS`였다. V4에서 reason counter와 mid-frequency histogram의 합계도 routing counter와 일치했다. Raw fio output, final stats, manifest, Host FEMU log는 각 timestamp directory에 보존했다.

---

# 6. Results & Analysis

## 6.1 Main WAF comparison

![Final 20:80 pool WAF comparison](waf_final_p20_comparison.svg)

**Figure 1. Baseline, V1-P20, V4-P20의 3600초 WAF.** 동일 seed의 각 1회 run이며 오차 막대나 통계적 유의성을 제시하지 않는다.

| Version | Host page writes | GC page writes | GC/Host | Avg GC copy | GC count | GC/10⁶ Host | WAF | IOPS |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Baseline | 118,774,428 | 830,272,120 | 6.990 | 14,335.1 | 57,919 | 487.64 | 7.990 | 8,224 |
| V1-P20 | 136,133,656 | 812,983,476 | 5.972 | 14,035.6 | 57,923 | 425.49 | 6.972 | 9,452 |
| V4-P20 | 134,333,028 | 811,853,278 | 6.044 | 14,059.5 | 57,744 | 429.86 | 7.044 | 9,327 |

Baseline 대비 V1-P20 WAF는 12.7%, V4-P20 WAF는 11.8% 낮았다. Time-based workload에서 더 빠른 FTL은 동일 3600초 동안 더 많은 Host write를 처리하므로 GC count 절대값만 비교하면 공정하지 않다. 백만 Host pages당 GC count를 보면 Baseline `487.64`에서 V1 `425.49`, V4 `429.86`으로 감소했다. Average GC copy도 각각 2.1%, 1.9% 낮았다. 즉 두 Hot/Cold design의 낮은 WAF는 GC 한 번당 copy cost와 Host write당 GC frequency가 함께 낮아진 결과와 일치한다.

Block erase도 같은 이유로 정규화해야 한다.

| Version | Block erases | Block erases / 10⁶ Host pages |
|---|---:|---:|
| Baseline | 3,706,816 | 31,208.9 |
| V1-P20 | 3,707,072 | 27,231.1 |
| V4-P20 | 3,695,616 | 27,510.9 |

절대 erase 횟수는 비슷하지만 P20 버전들이 더 많은 Host write를 처리했으므로, Host page로 정규화한 erase frequency는 Baseline보다 낮았다.

## 6.2 V1-P20 vs V4-P20: classifier design effect

V4-P20 WAF는 V1-P20보다 1.03% 높았다. 단일 run의 작은 차이를 통계적 우열로 표현하지 않고, GC 구조로 관찰된 방향만 해석한다. V4의 average GC copy는 V1보다 0.17%, normalized GC frequency는 1.03% 높았다. 즉 V4에서 GC 한 번당 copy도 조금 늘고 Host write당 GC도 조금 자주 발생했으며, 그 결과 GC/Host와 WAF가 V1보다 높아졌다.

| Mechanism metric | V1-P20 | V4-P20 |
|---|---:|---:|
| Host Hot ratio | 15.24% | 27.28% |
| GC Hot routing ratio | 0.76% | 15.86% |
| Cold→Hot + Hot→Cold / Host writes | 11.56% | 20.53% |
| Cold-pool empty / borrow | 4 / 4 | 4 / 4 |
| Hot-pool empty / emergency GC | 0 / 0 | 0 / 0 |

V4는 V1보다 Host write를 HOT으로 분류한 비율이 12.0 percentage points 높고, GC copy를 Hot path로 이동한 비율은 약 20배 높았다. Hot 판정이 많다고 분류가 더 정확한 것은 아니다. 이 workload에서는 time-window 정책이 Hot line에 더 많은 lifetime을 섞었거나, lazy expiration으로 저장된 HOT state가 GC placement에 사용되었을 가능성이 있다. 다만 line 내 lifetime mixing을 직접 측정하지 않았으므로 원인으로 단정하지 않는다.

State transition도 V4에서 더 자주 관찰되었다. 전체 Host writes의 20.53%에 해당하는 transition count는 일부 LPN의 판정이 window reset과 count threshold 사이에서 빈번히 변했음을 보여준다. 이 churn이 placement consistency에 영향을 주었을 가능성은 있지만 WAF와의 직접 인과관계는 입증하지 않았다.

## 6.3 V4 reason counter와 erase mechanism coverage

| V4 classification reason | Count | Host writes 대비 | Placement |
|---|---:|---:|---|
| Low frequency | 95,453,808 | 71.06% | COLD |
| High frequency | 20,478,612 | 15.24% | HOT |
| Mid frequency, erase=0 | 16,163,345 | 12.03% | HOT |
| Mid frequency, erase≥1 | 2,237,263 | 1.67% | COLD |
| **Total** | **134,333,028** | **100.00%** | — |

Mid-frequency decision은 18,400,608건으로 전체 Host writes의 13.70%였다. 그중 2,237,263건은 판정 직전 erase event가 1회 이상이어 `ERASE_SIGNAL_COLD`로 배치되었다. 이 counter는 block erase 신호가 코드에만 존재한 것이 아니라 실제 classifier 분기와 placement에 사용되었음을 보여준다.

그러나 V1-P20은 sequence rewrite interval, V4-P20은 time-window frequency이므로 두 버전 사이의 1.03% WAF 차이를 erase feedback만의 결과로 분리할 수 없다. 이를 위해서는 같은 1초 window와 같은 threshold를 쓰되 erase feedback만 끄거나, 중간 빈도를 항상 같은 class로 보내는 **frequency-only V4 control**이 필요하다.

## 6.4 Pool behavior과 20:80의 해석

V1-P20과 V4-P20은 모두 measurement 구간에서 Cold pool이 4회 비었고 반대 pool에서 4회 borrowing했다. Hot pool exhaustion과 emergency GC는 없었다. 즉 20:80 설정은 crash 없이 동작했고 borrowing은 의도한 safety valve로 작동했다. 그러나 Cold pool이 완전히 고갈되지 않았다거나 20:80이 최소 WAF 비율이라는 근거는 아니다.

이전 50:50 V1 T=4096 run의 borrowing은 10회, WAF는 `6.973`이었고, 이번 V1-P20은 borrowing 4회, WAF `6.972`였다. 다만 이전 run은 classifier reset semantics와 실행 시점이 다르므로 pool ratio의 독립적 ablation으로 사용하지 않고, “20:80에서 관찰된 borrowing이 줄었다”는 개발 단계의 기술적 참고로만 남긴다.

## 6.5 Absolute WAF가 높은 이유

세 비교군의 WAF는 7 전후로 절대값이 높다. 한 line은 16,384 pages인데 GC 한 번당 평균 14,000 pages 이상을 복사했다. 즉 victim line에 valid page가 많은 상태에서 line을 회수하여, 새로 얻는 free page 대비 copy overhead가 크게 관찰되었다. 5 GiB active range, 6 GiB exposed capacity, 25% raw-capacity reserve, 3600초 sustained Zipf overwrite와 기존 global greedy victim policy가 결합한 조건의 결과이다. 단, 이 실험은 각 요소의 독립적 기여를 분리하지 않았으므로 특정 하나를 높은 absolute WAF의 단일 원인으로 단정하지 않는다.

## 6.6 이전 개발 단계 참고 결과

50:50 pool의 이전 V3는 sequence rewrite interval `1024 < interval <= 4096`의 경계에서 `erase_survival_count`를 사용했다. 동일 seed의 V1 T=4096 WAF `6.973`과 V3 WAF `6.981`은 매우 비슷했고, V3의 erase-survival branch는 Host writes의 0.133%에서만 placement를 바꾸었다. 이 결과는 V3 mechanism이 동작했지만 threshold-only control 대비 추가 WAF 이득은 확인되지 않았음을 보여준 개발 기록이다.

이전 V3/P50와 최종 V4/P20은 pool ratio, classifier semantics, measurement reset 범위가 모두 다르다. 따라서 V3 결과를 최종 P20 결과와 직접 ablation처럼 섞지 않았고, 본 보고서의 메인 결론은 Baseline·V1-P20·V4-P20에서만 도출했다.

---

# 7. Discussion / Limitations / Conclusion

## 7.1 설계에 대한 평가

본 구현은 과제가 요구한 LBA request frequency, block erase information, Hot/Cold line pool, WAF 비교를 각각 실제 code path과 counter로 연결했다. 특히 erase feedback을 전역 block-erase total이 아닌 current LPN version의 actual source-block erase event로 구체화한 점, GC relocation이 Host frequency를 학습하지 않도록 분리한 점, reason counter와 invariant로 mechanism coverage와 accounting correctness를 같이 확인한 점은 설계의 강점이다.

반면 더 복잡한 classifier가 더 낮은 WAF를 보장하지는 않았다. V4는 V1보다 더 많은 write를 HOT으로 분류했고 transition churn도 높았으며, average copy와 normalized GC frequency가 모두 조금 악화되었다. 이는 classifier의 요구사항 충족과 workload에서의 WAF 최적화가 별개의 문제임을 보여준다.

## 7.2 한계

- **단일 seed·단일 run:** 평균, 표준편차, 신뢰구간이 없으므로 V1과 V4의 작은 차이를 통계적 우열로 주장할 수 없다.
- **Erase-only control 부재:** V1은 sequence interval, V4는 time-window frequency이므로 erase feedback의 독립 효과를 분리하지 못했다.
- **Baseline run 재사용:** 조건과 seed가 같은 valid run이지만 P20 run과 실행 날짜가 달라 Host 환경 변동을 완전히 제거하지 못했다.
- **Steady-state curve 부재:** 3600초 전체 cumulative WAF만 수집했으며, reset 없는 주기적 snapshot으로 평탄화를 직접 보이지 못했다.
- **Lazy idle handling:** Time window는 다음 Host write에서만 만료되므로 idle 중 GC는 이전 state를 사용할 수 있다.
- **20:80 sensitivity 부재:** 선택한 pool ratio는 정상 동작했지만 workload 최적값임을 입증하지 않았다.
- **Fixed parameter:** 1초, 1/4 write threshold, erase threshold 1을 여러 workload와 seed에서 tuning하지 않았다.
- **Derived erase signal:** `erase_event_count`는 LPN의 logical lifetime뿐 아니라 해당 LPN이 배치된 line의 GC 빈도에도 영향을 받으므로 완전히 독립적인 data-hotness 신호가 아니다.
- **Metadata overhead:** V4 LPN metadata는 FEMU에서 48 MiB를 사용했으며 실제 controller DRAM에서는 bit packing과 축소가 필요할 수 있다.
- **평가 범위:** WAF, GC copy, IOPS를 측정했지만 tail latency, wear distribution, P/E endurance 소진 시점은 측정하지 않았다.

## 7.3 결론

FEMU Blackbox FTL에 Hot/Cold line pool, dual write pointer, borrowing, WAF accounting을 구현했고, 최종 V4에서 LPN별 1초 request-frequency window와 actual source-block erase event를 함께 사용했다. Host frequency history와 GC relocation을 분리했으며 erase event는 중간 빈도의 secondary signal로 실제 placement에 반영되었다. 20:80 pool과 borrowing도 crash 없이 동작했고 모든 final counter invariant가 PASS했다.

동일 seed의 3600초 workload에서 V1-P20은 Baseline 대비 12.7%, V4-P20은 11.8% 낮은 WAF를 보였다. 즉 Hot/Cold placement를 포함한 두 design은 무분리 Baseline보다 낮은 NAND write amplification을 보였다. 그러나 V4는 더 간단한 V1-P20보다 낮은 WAF를 보이지 않았다. 따라서 최종 결론은 **time-window + erase-feedback mechanism은 의도대로 동작했지만, 이 단일 workload/run에서 간단한 rewrite-interval classifier 대비 추가 WAF 이득은 확인되지 않았다**는 것이다.

WAF 감소는 Host write당 NAND program 부담이 줄었음을 의미하며 endurance 관점에서 유리할 수 있다. 다만 실제 SSD의 lifetime은 wear leveling, NAND endurance, retention, bad block 등의 영향을 함께 받으므로 본 WAF 감소율을 수명 증가율로 변환하지 않는다.

---

# 예상 질문과 짧은 답변

### Q1. 왜 LBA frequency를 이 방식으로 구현했나?

V1의 sequence rewrite interval은 간단하지만 wall-clock 시간과 다르다. V4는 과제의 “일정 시간”을 더 직접 반영하기 위해 `req->stime`을 쓰는 LPN별 1초 window로 바꾸었다. 전역 scan 없이 해당 LPN의 write 시점에만 갱신할 수 있는 것도 이유다.

### Q2. 왜 erase count를 주 판단이 아닌 secondary signal로 쓰나?

Host request frequency가 data hotness에 더 직접적이고, erase event는 LPN 자체 뿐 아니라 배치된 line의 GC 환경에도 영향을 받는다. 따라서 명확한 low/high frequency는 Host 신호로 결정하고 2–3 writes/window의 중간 구간에서만 erase signal로 보정했다.

### Q3. 과제의 “block erase 횟수”를 정확히 어디에서 세나?

`mark_block_free()`에서 source block의 valid page를 free로 바꾸고 `blk->erase_cnt`/`block_erases`를 증가시키는 실제 erase 경로에서 세었다. Reverse mapping으로 그 valid logical version의 `erase_event_count`를 증가시킨다.

### Q4. 왜 Baseline, V1-P20, V4-P20을 비교했나?

Baseline은 과제의 무분리 Scenario 1이다. V1-P20은 동일 20:80 pool의 간단한 rewrite-interval control이고, V4-P20은 최종 time-window + erase-feedback design이다. 이로써 분리 자체의 Baseline 대비 효과와 두 classifier design의 차이를 보았다.

### Q5. 왜 V1-P20 vs V4-P20을 erase feedback 단독 효과라고 못 하나?

V1의 primary signal은 Host-write sequence rewrite interval이고 V4는 request-time window frequency이다. Erase feedback 외에 primary classifier도 바뀌었기 때문에 차이는 전체 classifier design effect이다. Erase 단독 효과를 보려면 V4에서 erase만 제거한 frequency-only control이 필요하다.

### Q6. 왜 20:80인가? 최적값인가?

이전 Cold-dominant 관찰과 50:50에서의 borrowing을 반영한 실험 parameter이다. 총 128 lines에서 실제 25/103으로 할당되었고 정상 동작했지만, 다른 비율 sensitivity를 하지 않았으므로 최적값이라고 주장하지 않는다.

### Q7. 왜 V4가 V1보다 낫지 않았나?

V4는 Host Hot ratio와 state-transition rate가 더 높았고, GC page의 Hot routing도 크게 늘었다. 그 결과 average GC copy와 normalized GC frequency가 둘 다 조금 높아졌다. 다만 line mixing을 직접 측정하지 않았으므로 “Hot 과분류가 원인”이라고 확정하지는 않는다.

### Q8. Idle 후에는 어떻게 되나?

1초 이상 지난 뒤 첫 Host write가 새 window의 count 1이 되어 COLD로 배치된다. 단 만료는 Host write 시 lazy하게 적용되므로, idle 중 GC는 다음 Host write 전까지 이전 state를 사용할 수 있다.

### Q9. 왜 Hot/Cold별 GC policy를 쓰지 않았나?

Classifier/placement와 victim-selection policy를 함께 바꾸면 WAF 변화의 원인을 분리할 수 없다. 따라서 global greedy victim queue를 고정하고 placement separation의 효과를 보도록 실험 범위를 제한했다.

### Q10. Preconditioning 후 무엇을 reset했나?

Accounting counter와 LPN classifier metadata는 reset했고, mapping/page/line/write-pointer 상태는 유지했다. 따라서 classifier는 새로 측정하지만 SSD는 6 GiB가 한 번 채워진 physical overwrite 상태에서 performance run을 시작했다.

### Q11. WAF가 줄었으니 SSD 수명이 12%나 늘었다고 할 수 있나?

그렇게 말할 수 없다. 측정한 것은 Host write당 NAND page write이며, 실제 lifetime은 wear leveling, P/E endurance, retention, bad block 등에 의해 결정된다. WAF 감소는 endurance에 유리한 방향의 결과로만 해석한다.

### Q12. 다음에 한 개의 실험만 추가한다면?

동일 V4 time-window 설정에서 erase feedback만 뺈 frequency-only control을 추가한다. 그런 뒤 최소 3개 seed로 V4 frequency-only와 V4 erase-feedback을 paired comparison해야 erase 신호의 독립적 효과를 판단할 수 있다.

---

# 참고문헌

1. H. Li, M. Hao, M. H. Tong, S. Sundararaman, M. Bjørling, and H. S. Gunawi, “The CASE of FEMU: Cheap, Accurate, Scalable and Extensible Flash Emulator,” *16th USENIX Conference on File and Storage Technologies (FAST '18)*, pp. 83–90, 2018.
2. D. Park and D. H. C. Du, “Hot Data Identification for Flash-based Storage Systems Using Multiple Bloom Filters,” *IEEE Symposium on Mass Storage Systems and Technologies (MSST)*, pp. 1–11, 2011.
3. J. Kim and I. Shin, “Clustering Data According to Update Frequency to Reduce Garbage-Collection Overhead in Solid-State Drives,” *IEICE Electronics Express*, vol. 13, no. 1, 2016.
4. B. Van Houdt, “On the Necessity of Hot and Cold Data Identification to Reduce the Write Amplification in Flash-based SSDs,” *Performance Evaluation*, vol. 82, pp. 1–14, 2014.
5. L. Xiang and B. M. Kurkoski, “An Improved Analytical Expression for Write Amplification in NAND Flash,” *International Conference on Computing, Networking and Communications (ICNC)*, pp. 497–501, 2012.

# 재현성 자료

- 과제 원문: `hw/femu/bbssd/참고자료/프로젝트.pdf`
- V1-P20 source: branch `hotcold/v1-p20`, commit `a2adff42934f166a6bb092d58bbe98a2aa478ac6`
- V4-P20 source: branch `hotcold/v4`, commit `6a012b579dc80b3f1e0fbb649ebf7e2da31bc4d4`
- Baseline source: branch `hotcold/base`, commit `c0237bb12f183d87ea1abf2bf52e92f900f6c587`
- Final P20 experiment root: `experiment_results/ftl_hotcold/pool20/20260901_014504_KST/`
- Main comparison: `comparison.csv`, `analysis_summary.txt`
- V1-P20 raw artifacts: `final_v1_p20_T4096_seed20260824/`
- V4-P20 raw artifacts: `final_v4_p20_seed20260824/`
- 이전 V3 개발 자료: `experiment_results/ftl_hotcold/v3/20260827_213254_KST/`
