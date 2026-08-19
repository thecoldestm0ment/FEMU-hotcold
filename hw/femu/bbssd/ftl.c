#include "ftl.h"
#include "ftl-internal.h"

//#define FEMU_DEBUG_FTL
#define HOT_REWRITE_WINDOW_DEFAULT 1024ULL
#define HOT_ACCESS_THRESHOLD_DEFAULT 3U
#define HOT_CONFIRMATION_THRESHOLD_DEFAULT 2U
#define HOT_DECAY_WINDOW_DEFAULT 65536ULL

/*
 * 기본 bbssd(non-FDP) FTL 읽기 순서
 *   ssd_init() -> ftl_thread() -> ssd_read()/ssd_write()/ssd_trim()
 *                              -> should_gc()/do_gc()
 *
 * LPN은 호스트의 논리 페이지 번호, PPA는 NAND의 물리 페이지 주소다.
 * maptbl은 LPN->PPA, rmap은 PPA->LPN 변환에 사용한다.
 * line은 모든 channel/LUN에서 같은 block 번호를 묶은 GC 단위다.
 * 주소/매핑 helper는 ftl-internal.h, NAND 지연 계산은 ftl-media.c에 있다.
 */
static void *ftl_thread(void *arg);

/* FDP 함수 전방 선언 */
static void mark_page_valid_fdp(struct ssd *ssd, struct ppa *ppa,
                                FemuReclaimUnit *ru);
static void mark_page_invalid_fdp(struct ssd *ssd, struct ppa *ppa);
static int do_gc_fdp_style(struct ssd *ssd, uint16_t rgid, uint16_t ruhid,
                           bool force);
static void ssd_init_fdp_params(struct ssdparams *spp, FemuCtrl *n);
static void femu_fdp_ssd_init_reclaim_group(FemuCtrl *n, struct ssd *ssd);
static void femu_fdp_ssd_init_ru_handles(FemuCtrl *n, struct ssd *ssd);
static void ssd_trim_fdp_style(FemuCtrl *n, NvmeRequest *req, uint64_t slba,
                               uint32_t nlb);
static void ssd_reset_maptbl(struct ssd *ssd);
static void exp_load_cfg(void);

/* V2 runtime parameters: unset variables use the documented defaults. */
static uint64_t ssd_load_positive_u64(const char *name, uint64_t default_value)
{
    const char *value = getenv(name);
    char *end = NULL;
    unsigned long long parsed;

    if (!value || value[0] == '\0') {
        return default_value;
    }

    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (value[0] < '0' || value[0] > '9' ||
        errno != 0 || end == value || *end != '\0' || parsed == 0) {
        ftl_err("invalid %s='%s' (expected positive integer)\n", name, value);
        abort();
    }

    return (uint64_t)parsed;
}

static uint32_t ssd_load_positive_u32(const char *name, uint32_t default_value)
{
    uint64_t parsed = ssd_load_positive_u64(name, default_value);

    if (parsed > UINT32_MAX) {
        ftl_err("invalid %s='%" PRIu64 "' (maximum %u)\n",
                name, parsed, UINT32_MAX);
        abort();
    }

    return (uint32_t)parsed;
}

static bool ssd_load_bool(const char *name, bool default_value)
{
    const char *value = getenv(name);

    if (!value || value[0] == '\0') {
        return default_value;
    }
    if (!strcmp(value, "1") || !strcmp(value, "on") ||
        !strcmp(value, "true")) {
        return true;
    }
    if (!strcmp(value, "0") || !strcmp(value, "off") ||
        !strcmp(value, "false")) {
        return false;
    }

    ftl_err("invalid %s='%s' (expected 1/0, on/off, or true/false)\n",
            name, value);
    abort();
}

/* Reset measurement counters only. LPN history intentionally survives. */
void ssd_reset_stats(struct ssd *ssd)
{
    ssd->host_page_writes = 0;
    ssd->nand_page_writes = 0;
    ssd->gc_page_writes = 0;
    ssd->block_erases = 0;
    ssd->gc_count = 0;
    ssd->host_hot_writes = 0;
    ssd->host_cold_writes = 0;
    ssd->gc_hot_writes = 0;
    ssd->gc_cold_writes = 0;
    ssd->cold_to_hot_count = 0;
    ssd->hot_to_cold_count = 0;
    ssd->hot_pool_empty_count = 0;
    ssd->cold_pool_empty_count = 0;
    ssd->borrow_count = 0;
    ssd->emergency_gc_count = 0;
    ssd->decay_application_count = 0;
}

/*
 * 현재까지 누적된 non-FDP page-write 통계와 WAF를 출력한다.
 * FEMU_RESET_ACCT admin command가 실험 구간 끝에서 이 함수를 호출한다.
 */
void ssd_print_stats(struct ssd *ssd)
{
    char waf[32];
    char average_gc_copy[32];
    char hot_write_ratio[32];
    bool counters_valid =
        ssd->nand_page_writes ==
        ssd->host_page_writes + ssd->gc_page_writes;

    if (ssd->host_page_writes == 0) {
        snprintf(waf, sizeof(waf), "N/A");
    } else {
        snprintf(waf, sizeof(waf), "%.6f",
                 (double)ssd->nand_page_writes /
                 (double)ssd->host_page_writes);
    }

    if (ssd->gc_count == 0) {
        snprintf(average_gc_copy, sizeof(average_gc_copy), "N/A");
    } else {
        snprintf(average_gc_copy, sizeof(average_gc_copy), "%.6f",
                 (double)ssd->gc_page_writes / (double)ssd->gc_count);
    }

    if (ssd->host_page_writes == 0) {
        snprintf(hot_write_ratio, sizeof(hot_write_ratio), "N/A");
    } else {
        snprintf(hot_write_ratio, sizeof(hot_write_ratio), "%.6f",
                 (double)ssd->host_hot_writes /
                 (double)ssd->host_page_writes);
    }

    ftl_log("BBSSD-STATS version=V2 hot_rewrite_window=%" PRIu64
            " hot_access_threshold=%u"
            " hot_confirmation_threshold=%u"
            " hot_decay_window=%" PRIu64
            " hot_decay_enabled=%s"
            " host_page_writes=%" PRIu64
            " nand_page_writes=%" PRIu64
            " gc_page_writes=%" PRIu64
            " block_erases=%" PRIu64
            " waf=%s"
            " gc_count=%" PRIu64
            " average_gc_copy=%s"
            " host_hot_writes=%" PRIu64
            " host_cold_writes=%" PRIu64
            " hot_write_ratio=%s"
            " gc_hot_writes=%" PRIu64
            " gc_cold_writes=%" PRIu64
            " cold_to_hot_count=%" PRIu64
            " hot_to_cold_count=%" PRIu64
            " hot_pool_empty_count=%" PRIu64
            " cold_pool_empty_count=%" PRIu64
            " borrow_count=%" PRIu64
            " emergency_gc_count=%" PRIu64
            " decay_application_count=%" PRIu64
            " counter_invariant=%s\n",
            ssd->hot_rewrite_window, ssd->hot_access_threshold,
            ssd->hot_confirmation_threshold, ssd->hot_decay_window,
            ssd->hot_decay_enabled ? "on" : "off", ssd->host_page_writes,
            ssd->nand_page_writes, ssd->gc_page_writes,
            ssd->block_erases, waf, ssd->gc_count, average_gc_copy,
            ssd->host_hot_writes, ssd->host_cold_writes, hot_write_ratio,
            ssd->gc_hot_writes, ssd->gc_cold_writes,
            ssd->cold_to_hot_count, ssd->hot_to_cold_count,
            ssd->hot_pool_empty_count, ssd->cold_pool_empty_count,
            ssd->borrow_count, ssd->emergency_gc_count,
            ssd->decay_application_count,
            counters_valid ? "PASS" : "FAIL");
}

/*
 * ftl_fdp_alloc_event - FTL 계층에서 FDP event를 할당한다.
 * GC가 controller event를 생성할 때 사용한다.
 */
static NvmeFdpEvent *ftl_fdp_alloc_event(struct ssd *ssd,
                                          NvmeFdpEventBuffer *ebuf)
{
    NvmeFdpEvent *ret;
    bool is_full = ebuf->next == ebuf->start && ebuf->nelems;

    ret = &ebuf->events[ebuf->next++];
    if (unlikely(ebuf->next == NVME_FDP_MAX_EVENTS)) {
        ebuf->next = 0;
    }
    if (is_full) {
        ebuf->start = ebuf->next;
    } else {
        ebuf->nelems++;
    }

    memset(ret, 0, sizeof(NvmeFdpEvent));
    return ret;
}

static inline bool should_gc(struct ssd *ssd)
{
    /* background GC: 여유 line이 일반 임계값 이하인지 확인 */
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines);
}

static inline bool should_gc_high(struct ssd *ssd)
{
    /* foreground GC: 새 write 전에 확보해야 할 긴급 임계값 */
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_high);
}

/* FDP GC 판단: GC가 필요하면 RG index, 아니면 -1을 반환한다. */
static inline int16_t should_gc_fdp_style(struct ssd *ssd)
{
    /* free RU 수가 gc_thres_rus 이하이면 RG index 반환 */
    for (int i = 0; i < (int)ssd->nrg; i++) {
        if (ssd->rg[i].ru_mgmt->free_ru_cnt <=
            ssd->rg[i].ru_mgmt->gc_thres_rus) {
            return i;
        }
    }
    return -1;
}

static inline int should_gc_high_fdp_style(struct ssd *ssd)
{
    /* free RU 수가 긴급 임계값 이하이면 RG index 반환 */
    for (int i = 0; i < (int)ssd->nrg; i++) {
        if (ssd->rg[i].ru_mgmt->free_ru_cnt <=
            ssd->rg[i].ru_mgmt->gc_thres_rus_high) {
            return i;
        }
    }
    return -1;
}


static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
    return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
    return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
    ((struct line *)a)->pos = pos;
}

/* FDP: vpc 기반 greedy victim RU priority queue callback */
static inline int victim_ru_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_ru_get_pri(void *a)
{
    return ((FemuReclaimUnit *)a)->vpc;
}

static inline void victim_ru_set_pri(void *a, pqueue_pri_t pri)
{
    ((FemuReclaimUnit *)a)->vpc = pri;
}

static inline size_t victim_ru_get_pos(void *a)
{
    return ((FemuReclaimUnit *)a)->pos;
}

static inline void victim_ru_set_pos(void *a, size_t pos)
{
    ((FemuReclaimUnit *)a)->pos = pos;
}

/*
 * PI type RUH는 full RU를 per-RG(global) victim pqueue와 자체 per-RUH
 * victim pqueue 양쪽에 동시에 보관한다. 두 heap은 각 RU의 index를 독립적으로
 * 추적해야 하므로, per-RUH queue는 이 callback을 통해 ruh_pos를 사용한다.
 * 두 heap이 하나의 `pos` field를 공유하면(issue #189) 나중에 접근한 heap이
 * 손상되고 victim_ru_get_pri(NULL)에서 crash가 발생한다.
 */
static inline size_t victim_ru_get_pos_ruh(void *a)
{
    return ((FemuReclaimUnit *)a)->ruh_pos;
}

static inline void victim_ru_set_pos_ruh(void *a, size_t pos)
{
    ((FemuReclaimUnit *)a)->ruh_pos = pos;
}

/* FDP: my_cb 기반 cost-benefit victim RU priority queue callback */
static inline int victim_ru_cmp_pri_by_cb(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_ru_get_pri_by_cb(void *a)
{
    /* 정렬을 위해 float를 pqueue_pri_t로 변환 */
    return (pqueue_pri_t)((FemuReclaimUnit *)a)->my_cb;
}

static inline void victim_ru_set_pri_by_cb(void *a, pqueue_pri_t pri)
{
    ((FemuReclaimUnit *)a)->my_cb = (float)pri;
}

/* 모든 line을 free list에 넣고 victim/full 관리 자료구조를 준비한다. */
static void ssd_init_lines(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;

    lm->tt_lines = spp->blks_per_pl;
    ftl_assert(lm->tt_lines == spp->tt_lines);
    lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);

    QTAILQ_INIT(&lm->free_line_list);
    QTAILQ_INIT(&lm->free_hot_line_list);
    QTAILQ_INIT(&lm->free_cold_line_list);
    lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
            victim_line_get_pri, victim_line_set_pri,
            victim_line_get_pos, victim_line_set_pos);
    QTAILQ_INIT(&lm->full_line_list);

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
        /* 모든 line을 free line으로 초기화 */
        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    ftl_assert(lm->free_line_cnt == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;
}

/* free list에서 다음 line을 꺼내고 남은 free line 수를 줄인다. */
static struct line *get_next_free_line(struct ssd *ssd)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    if (!curline) {
        ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
        return NULL;
    }

    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    return curline;
}

/* non-FDP aggregate count와 class별 count가 항상 같은 총량인지 확인한다. */
static void ssd_validate_free_line_counts(struct ssd *ssd)
{
    struct line_mgmt *lm = &ssd->lm;

    if (lm->free_line_cnt < 0 || lm->free_hot_line_cnt < 0 ||
        lm->free_cold_line_cnt < 0 ||
        lm->free_line_cnt !=
        lm->free_hot_line_cnt + lm->free_cold_line_cnt) {
        ftl_err("invalid Hot/Cold free line counts: total=%d hot=%d cold=%d\n",
                lm->free_line_cnt, lm->free_hot_line_cnt,
                lm->free_cold_line_cnt);
        abort();
    }
}

/* global free list를 non-FDP용 50:50 Cold/Hot pool로 한 번만 분배한다. */
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

    /* Pool 사이로 이동했을 뿐이므로 aggregate free count는 바뀌지 않는다. */
    ssd_validate_free_line_counts(ssd);
}

/* 한 class pool에서 line 하나와 해당 count를 함께 제거한다. */
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

/* 요청 pool이 비면 반대 pool에서 빌리고 요청받은 class로 바꾼다. */
static struct line *get_next_free_line_by_class(struct ssd *ssd,
                                                LineClass data_class)
{
    struct line_mgmt *lm = &ssd->lm;
    union free_line_list *requested;
    union free_line_list *fallback;
    int *requested_cnt;
    int *fallback_cnt;
    struct line *line;
    bool borrowed = false;

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
    } else {
        ftl_err("invalid requested line class: %d\n", data_class);
        abort();
    }

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
    if (!line) {
        ftl_err("No Hot/Cold free lines left in [%s]\n", ssd->ssdname);
        return NULL;
    }

    if (!borrowed && line->data_class != data_class) {
        ftl_err("line %d is in the wrong class pool\n", line->id);
        abort();
    }

    if (borrowed) {
        ssd->borrow_count++;
    }

    line->data_class = data_class;
    lm->free_line_cnt--;
    ssd_validate_free_line_counts(ssd);
    ftl_debug("line %d allocated as class %d%s\n", line->id, data_class,
              borrowed ? " (borrowed)" : "");
    return line;
}

/* 주어진 non-FDP write pointer에 해당 class의 free line을 배정한다. */
static void ssd_init_write_pointer(struct ssd *ssd,
                                   struct write_pointer *wpp,
                                   LineClass data_class)
{
    struct line *curline =
        get_next_free_line_by_class(ssd, data_class);

    if (!curline) {
        abort();
    }

    wpp->curline = curline;
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->blk = curline->id;
    wpp->pl = 0;
}

/* 두 active pointer가 같은 line을 공유하면 즉시 중단한다. */
static void ssd_validate_write_pointers(struct ssd *ssd)
{
    struct write_pointer *hot = &ssd->wp_hot;
    struct write_pointer *cold = &ssd->wp_cold;

    if (!hot->curline || !cold->curline ||
        hot->curline == cold->curline ||
        hot->curline->data_class != LINE_CLASS_HOT ||
        cold->curline->data_class != LINE_CLASS_COLD ||
        hot->blk != hot->curline->id || cold->blk != cold->curline->id) {
        ftl_err("invalid Hot/Cold write pointer state\n");
        abort();
    }
}

/*
 * write pointer 순회: channel -> LUN -> page.
 * 한 line을 다 쓰면 valid page만 있으면 full, invalid page가 있으면 victim
 * 자료구조로 옮긴 뒤 다음 free line에서 다시 시작한다.
 */
static void ssd_advance_write_pointer(struct ssd *ssd,
                                      struct write_pointer *wpp)
{
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;

    check_addr(wpp->ch, spp->nchs);
    wpp->ch++;
    if (wpp->ch == spp->nchs) {
        wpp->ch = 0;
        check_addr(wpp->lun, spp->luns_per_ch);
        wpp->lun++;
        /* 현재 channel을 모두 돌았으므로 다음 LUN으로 이동 */
        if (wpp->lun == spp->luns_per_ch) {
            wpp->lun = 0;
            /* 모든 LUN을 돌았으므로 block의 다음 page로 이동 */
            check_addr(wpp->pg, spp->pgs_per_blk);
            wpp->pg++;
            if (wpp->pg == spp->pgs_per_blk) {
                LineClass data_class = wpp->curline->data_class;

                wpp->pg = 0;
                /* 사용이 끝난 line을 victim 또는 full 자료구조로 이동 */
                if (wpp->curline->vpc == spp->pgs_per_line) {
                    /* 모든 page가 valid이면 full line list로 이동 */
                    ftl_assert(wpp->curline->ipc == 0);
                    QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
                    lm->full_line_cnt++;
                } else {
                    ftl_assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
                    /* 이 line에는 invalid page가 하나 이상 있어야 한다. */
                    ftl_assert(wpp->curline->ipc > 0);
                    pqueue_insert(lm->victim_line_pq, wpp->curline);
                    lm->victim_line_cnt++;
                }
                /* 현재 line을 모두 사용했으므로 다음 free line 선택 */
                check_addr(wpp->blk, spp->blks_per_pl);
                wpp->curline = NULL;
                wpp->curline =
                    get_next_free_line_by_class(ssd, data_class);
                if (!wpp->curline) {
                    /* TODO: free line 고갈 처리 */
                    abort();
                }
                wpp->blk = wpp->curline->id;
                check_addr(wpp->blk, spp->blks_per_pl);
                /* 새 superblock이 page 0부터 시작하는지 확인 */
                ftl_assert(wpp->pg == 0);
                ftl_assert(wpp->lun == 0);
                ftl_assert(wpp->ch == 0);
                /* TODO: 현재는 LUN당 plane 수를 1로 가정하며 추후 수정 필요 */
                ftl_assert(wpp->pl == 0);
                ssd_validate_write_pointers(ssd);
            }
        }
    }
}

/* 현재 write pointer가 가리키는, 다음 write에 사용할 PPA를 만든다. */
static struct ppa get_new_page(struct write_pointer *wpp)
{
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    ftl_assert(ppa.g.pl == 0);

    return ppa;
}

/* non-FDP에서만 mapping table과 같은 크기의 LPN history를 준비한다. */
static void ssd_init_lpn_metadata(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t metadata_bytes;

    ssd->lpn_meta = NULL;
    ssd->host_write_seq = 0;
    ssd->hot_rewrite_window = HOT_REWRITE_WINDOW_DEFAULT;
    ssd->hot_access_threshold = HOT_ACCESS_THRESHOLD_DEFAULT;
    ssd->hot_confirmation_threshold = HOT_CONFIRMATION_THRESHOLD_DEFAULT;
    ssd->hot_decay_window = HOT_DECAY_WINDOW_DEFAULT;
    ssd->hot_decay_enabled = true;

    if (ssd->fdp_enabled) {
        return;
    }

    /* UNSEEN을 0으로 정의했으므로 zero allocation이 모든 초기값을 만든다. */
    ssd->lpn_meta = g_new0(LpnMeta, spp->tt_pgs);
    ssd->hot_rewrite_window =
        ssd_load_positive_u64("FEMU_HOT_REWRITE_WINDOW",
                              HOT_REWRITE_WINDOW_DEFAULT);
    ssd->hot_access_threshold =
        ssd_load_positive_u32("FEMU_HOT_ACCESS_THRESHOLD",
                              HOT_ACCESS_THRESHOLD_DEFAULT);
    ssd->hot_confirmation_threshold =
        ssd_load_positive_u32("FEMU_HOT_CONFIRM_THRESHOLD",
                              HOT_CONFIRMATION_THRESHOLD_DEFAULT);
    ssd->hot_decay_window =
        ssd_load_positive_u64("FEMU_HOT_DECAY_WINDOW",
                              HOT_DECAY_WINDOW_DEFAULT);
    ssd->hot_decay_enabled = ssd_load_bool("FEMU_HOT_DECAY", true);

    metadata_bytes = (uint64_t)spp->tt_pgs * sizeof(*ssd->lpn_meta);
    ftl_log("LPN metadata: entries=%d entry_size=%zu total=%" PRIu64
            " MiB T=%" PRIu64 " A=%u C=%u D=%" PRIu64 " decay=%s\n",
            spp->tt_pgs, sizeof(*ssd->lpn_meta), metadata_bytes / MiB,
            ssd->hot_rewrite_window, ssd->hot_access_threshold,
            ssd->hot_confirmation_threshold, ssd->hot_decay_window,
            ssd->hot_decay_enabled ? "on" : "off");
}

/*
 * 전체 metadata 배열을 순회하지 않고 이 LPN의 다음 host write에서만
 * 경과 epoch만큼 history를 감쇠한다. GC는 이 함수를 호출하지 않는다.
 */
static void ssd_apply_lpn_decay(struct ssd *ssd, LpnMeta *meta,
                                uint64_t current_epoch)
{
    uint64_t elapsed_epochs;

    if (!ssd->hot_decay_enabled || meta->state == LPN_STATE_UNSEEN) {
        meta->last_decay_epoch = current_epoch;
        return;
    }
    if (current_epoch <= meta->last_decay_epoch) {
        return;
    }

    elapsed_epochs = current_epoch - meta->last_decay_epoch;
    if (elapsed_epochs >= 32) {
        meta->access_count = 0;
        meta->short_interval_count = 0;
    } else {
        meta->access_count >>= elapsed_epochs;
        meta->short_interval_count >>= elapsed_epochs;
    }
    meta->last_decay_epoch = current_epoch;
    if (ssd->decay_application_count != UINT64_MAX) {
        ssd->decay_application_count++;
    }
}

/* 한 번의 host page write를 관찰해 V2 조건으로 해당 LPN의 온도를 갱신한다. */
static void ssd_update_lpn_temperature(struct ssd *ssd, uint64_t lpn)
{
    LpnMeta *meta;
    LpnState previous_state;
    uint64_t current_epoch;
    bool is_rewrite;
    bool is_short_rewrite;

    ftl_assert(ssd->lpn_meta != NULL);
    ftl_assert(valid_lpn(ssd, lpn));
    meta = &ssd->lpn_meta[lpn];
    previous_state = meta->state;

    /* 0은 미관찰용으로 비우고, wrap 대신 64-bit 최댓값에서 포화시킨다. */
    if (ssd->host_write_seq != UINT64_MAX) {
        ssd->host_write_seq++;
    }
    /* sequence 1..D를 첫 epoch로 묶고 D번이 지난 다음 write부터 감쇠한다. */
    current_epoch =
        (ssd->host_write_seq - 1) / ssd->hot_decay_window;
    ssd_apply_lpn_decay(ssd, meta, current_epoch);

    is_rewrite = meta->state != LPN_STATE_UNSEEN;
    if (!is_rewrite) {
        meta->update_interval = 0;
    } else {
        ftl_assert(meta->last_write_seq <= ssd->host_write_seq);
        meta->update_interval = ssd->host_write_seq - meta->last_write_seq;
    }
    is_short_rewrite =
        is_rewrite && meta->update_interval <= ssd->hot_rewrite_window;

    if (meta->access_count != UINT32_MAX) {
        meta->access_count++;
    }
    if (is_short_rewrite && meta->short_interval_count != UINT32_MAX) {
        meta->short_interval_count++;
    }

    meta->state =
        meta->access_count >= ssd->hot_access_threshold &&
        is_short_rewrite &&
        meta->short_interval_count >= ssd->hot_confirmation_threshold ?
        LPN_STATE_HOT : LPN_STATE_COLD;

    if ((previous_state == LPN_STATE_COLD ||
         previous_state == LPN_STATE_UNSEEN) &&
        meta->state == LPN_STATE_HOT) {
        ssd->cold_to_hot_count++;
    } else if (previous_state == LPN_STATE_HOT &&
               meta->state == LPN_STATE_COLD) {
        ssd->hot_to_cold_count++;
    }

    meta->last_write_seq = ssd->host_write_seq;
}

/* LPN의 현재 온도에 맞는 non-FDP write pointer를 선택한다. */
static struct write_pointer *ssd_select_write_pointer(struct ssd *ssd,
                                                      uint64_t lpn)
{
    LpnState state;

    ftl_assert(ssd->lpn_meta != NULL);
    ftl_assert(valid_lpn(ssd, lpn));
    state = ssd->lpn_meta[lpn].state;

    ftl_assert(state == LPN_STATE_COLD || state == LPN_STATE_HOT);
    return state == LPN_STATE_HOT ? &ssd->wp_hot : &ssd->wp_cold;
}

/* TRIM은 logical lifetime의 끝이므로 다음 write를 다시 첫 write로 본다. */
static void ssd_reset_lpn_metadata(struct ssd *ssd, uint64_t lpn)
{
    ftl_assert(ssd->lpn_meta != NULL);
    ftl_assert(valid_lpn(ssd, lpn));
    memset(&ssd->lpn_meta[lpn], 0, sizeof(ssd->lpn_meta[lpn]));
}



/* geometry, NAND, mapping, line을 초기화하고 FTL worker thread를 시작한다. */
void ssd_init(FemuCtrl *n)
{
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;

    ftl_assert(ssd);
    ssd->n = n;

    /* data-remanence 실험용 환경 변수를 한 번만 읽는다(debug 전용, 기본 off). */
    exp_load_cfg();

    /* V2 통계는 SSD 초기화부터 누적한다. */
    ssd_reset_stats(ssd);

    ssd_init_params(spp, n);

    /* SSD 내부 NAND 계층 구조 초기화 */
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    for (int i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp);
    }

    /* NAND media 계층의 timing 설정(spp 참조, ssd->ch 연결) */
    bb_nand_media_init(ssd);

    /* LPN -> PPA mapping table 초기화 */
    ssd_init_maptbl(ssd);

    /* PPA -> LPN reverse mapping table 초기화 */
    ssd_init_rmap(ssd);

    /* 모든 line 초기화 */
    ssd_init_lines(ssd);

    /* FDP와 non-FDP 초기화 경로 선택 */
    ssd->fdp_enabled = (n->subsys != NULL &&
                        n->subsys->params.fdp.enabled);
    ssd->fdp_debug = (getenv("FEMU_FDP_DEBUG") != NULL);
    ssd_init_lpn_metadata(ssd);

    if (ssd->fdp_enabled) {
        ssd_init_fdp_params(spp, n);

        ftl_log("FDP: initializing reclaim groups\n");
        femu_fdp_ssd_init_reclaim_group(n, ssd);
        ftl_log("FDP: initializing RU handles\n");
        femu_fdp_ssd_init_ru_handles(n, ssd);
        ftl_log("FDP: init complete (nrg=%lu, nruhs=%lu)\n",
                ssd->nrg, ssd->nruhs);
    } else {
        ssd_init_hotcold_line_pools(ssd);
        ssd_init_write_pointer(ssd, &ssd->wp_cold, LINE_CLASS_COLD);
        ssd_init_write_pointer(ssd, &ssd->wp_hot, LINE_CLASS_HOT);
        ssd_validate_write_pointers(ssd);
        ftl_log("Write pointers: hot_line=%d cold_line=%d free=%d "
                "hot_free=%d cold_free=%d\n",
                ssd->wp_hot.curline->id, ssd->wp_cold.curline->id,
                ssd->lm.free_line_cnt, ssd->lm.free_hot_line_cnt,
                ssd->lm.free_cold_line_cnt);
    }

    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n,
                       QEMU_THREAD_JOINABLE);
}


/*
 * 삭제 데이터 잔존 실험용 계측(debug 전용이며 FTL 동작은 바꾸지 않는다).
 * 초기화 시 한 번 읽는 환경 변수로 활성화한다.
 *   FEMU_EXP_LOG   비어 있지 않으면 stderr에 [EXP] log 출력
 *   FEMU_SECRET    marker string을 포함한 backend page만 추적하고 log 출력
 *   FEMU_DUMP_LPN  매 read마다 해당 LPN의 backend page를 hex dump
 * tee pipe를 거쳐도 출력이 buffering되지 않도록 stderr를 사용한다.
 */
static bool exp_log_enabled;
static const char *exp_secret;       /* NULL 또는 비어 있지 않은 marker string */
static uint64_t exp_dump_lpn;
static bool exp_dump_lpn_set;

static void exp_load_cfg(void)
{
    const char *v;

    v = getenv("FEMU_EXP_LOG");
    exp_log_enabled = (v && v[0] != '\0');

    v = getenv("FEMU_SECRET");
    exp_secret = (v && v[0] != '\0') ? v : NULL;

    v = getenv("FEMU_DUMP_LPN");
    if (v && v[0] != '\0') {
        exp_dump_lpn = strtoull(v, NULL, 0);
        exp_dump_lpn_set = true;
    }
}

#define EXP_LOG(fmt, ...) do { \
    if (exp_log_enabled) \
        fprintf(stderr, "[EXP] " fmt, ## __VA_ARGS__); \
} while (0)

#define PPA_FMT "ch=%u lun=%u pl=%u blk=%u pg=%u"
#define PPA_ARG(p) (unsigned)(p)->g.ch, (unsigned)(p)->g.lun, \
                   (unsigned)(p)->g.pl, (unsigned)(p)->g.blk, (unsigned)(p)->g.pg

/*
 * DRAM backend에서 한 LPN의 byte를 Hex+ASCII로 dump한다.
 * 데이터는 PPA가 아니라 logical space의 LBA offset(lpn * page_bytes)에 있다.
 */
static void femu_dbg_dump_lpn(struct ssd *ssd, uint64_t lpn)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t page_bytes = (uint64_t)spp->secs_per_pg * spp->secsz; /* 4096 */
    uint64_t off = lpn * page_bytes;
    struct ppa ppa = get_maptbl_ent(ssd, lpn);
    uint8_t *base;

    if (!ssd->n || !ssd->n->mbe || !ssd->n->mbe->logical_space) {
        fprintf(stderr, "[DUMP] backend not ready\n");
        return;
    }
    if (off + page_bytes > (uint64_t)ssd->n->mbe->size) {
        fprintf(stderr, "[DUMP] lpn=%lu out of range\n", lpn);
        return;
    }
    base = (uint8_t *)ssd->n->mbe->logical_space + off;

    fprintf(stderr, "[DUMP] lpn=%lu off=0x%lx mapped=%d", lpn, off,
            mapped_ppa(&ppa));
    if (mapped_ppa(&ppa))
        fprintf(stderr, " " PPA_FMT, PPA_ARG(&ppa));
    fprintf(stderr, "\n");
    for (uint64_t i = 0; i < page_bytes; i += 16) {
        fprintf(stderr, "  %08lx  ", off + i);
        for (int j = 0; j < 16; j++)
            fprintf(stderr, "%02x ", base[i + j]);
        fprintf(stderr, " |");
        for (int j = 0; j < 16; j++) {
            uint8_t c = base[i + j];
            fprintf(stderr, "%c", (c >= 0x20 && c < 0x7f) ? c : '.');
        }
        fprintf(stderr, "|\n");
    }
}

/* 전체 backend를 검사해 marker string이 들어 있는 LPN을 출력한다. */
static void femu_dbg_scan_secret(struct ssd *ssd, const char *tag)
{
    const char *sec = exp_secret;
    struct ssdparams *spp = &ssd->sp;
    uint64_t page_bytes, size;
    uint8_t *buf;
    size_t slen;
    int hits = 0;

    if (!sec || !ssd->n || !ssd->n->mbe || !ssd->n->mbe->logical_space)
        return;

    page_bytes = (uint64_t)spp->secs_per_pg * spp->secsz;
    buf = (uint8_t *)ssd->n->mbe->logical_space;
    size = (uint64_t)ssd->n->mbe->size;
    slen = strlen(sec);

    for (uint64_t i = 0; i + slen <= size; i++) {
        if (buf[i] == (uint8_t)sec[0] && memcmp(buf + i, sec, slen) == 0) {
            fprintf(stderr, "[SCAN:%s] FOUND '%s' at off=0x%lx lpn=%lu\n",
                    tag, sec, i, i / page_bytes);
            hits++;
            i += slen - 1;
        }
    }
    if (!hits)
        fprintf(stderr, "[SCAN:%s] '%s' NOT present in backend\n", tag, sec);
}

/* 불필요한 log를 줄이기 위해 marker가 있는 LPN/block만 추적한다. */
#define EXP_MAX_WATCH 256
static uint64_t exp_watch_lpn[EXP_MAX_WATCH];
static int exp_watch_lpn_cnt = 0;
static uint8_t exp_watch_blk[1 << BLK_BITS]; /* block(line) id의 추적 여부 */

static bool exp_lpn_watched(uint64_t lpn)
{
    for (int i = 0; i < exp_watch_lpn_cnt; i++)
        if (exp_watch_lpn[i] == lpn)
            return true;
    return false;
}

static void exp_watch_lpn_add(uint64_t lpn)
{
    if (exp_lpn_watched(lpn))
        return;
    if (exp_watch_lpn_cnt < EXP_MAX_WATCH)
        exp_watch_lpn[exp_watch_lpn_cnt++] = lpn;
}

/* 이 LPN의 backend page에 현재 marker string이 있는지 확인한다. */
static bool femu_dbg_lpn_has_secret(struct ssd *ssd, uint64_t lpn)
{
    const char *sec = exp_secret;
    struct ssdparams *spp = &ssd->sp;
    uint64_t page_bytes, off;
    uint8_t *base;
    size_t slen;

    if (!sec)
        return false;
    if (!ssd->n || !ssd->n->mbe || !ssd->n->mbe->logical_space)
        return false;
    page_bytes = (uint64_t)spp->secs_per_pg * spp->secsz;
    off = lpn * page_bytes;
    if (off + page_bytes > (uint64_t)ssd->n->mbe->size)
        return false;
    base = (uint8_t *)ssd->n->mbe->logical_space + off;
    slen = strlen(sec);
    for (uint64_t i = 0; i + slen <= page_bytes; i++)
        if (base[i] == (uint8_t)sec[0] && memcmp(base + i, sec, slen) == 0)
            return true;
    return false;
}

/*
 * ssd_advance_status는 현재 NAND media-layer bridge인
 * hw/femu/bbssd/ftl-media.c에 있다. 동일한 per-LUN read/program/erase
 * timing을 공통 nand_media API를 통해 계산한다.
 */

/*
 * overwrite/TRIM된 page를 PG_VALID -> PG_INVALID로 바꾼다.
 * page뿐 아니라 block과 line의 vpc(valid)는 줄이고 ipc(invalid)는 늘린다.
 */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_line = false;
    struct line *line;

    /* 해당 page 상태 갱신 */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* 해당 block의 valid/invalid page 수 갱신 */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    /* 해당 line의 valid/invalid page 수 갱신 */
    line = get_line(ssd, ppa);
    ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    if (line->vpc == spp->pgs_per_line) {
        ftl_assert(line->ipc == 0);
        was_full_line = true;
    }
    line->ipc++;
    ftl_assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
    /* overwrite 후 변경된 vpc에 맞춰 victim line의 pq 위치 조정 */
    if (line->pos) {
        /* 이 호출 내부에서 line->vpc도 갱신된다. */
        pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
    } else {
        line->vpc--;
    }

    if (was_full_line) {
        /* line을 full list에서 victim pq로 이동 */
        QTAILQ_REMOVE(&lm->full_line_list, line, entry);
        lm->full_line_cnt--;
        pqueue_insert(lm->victim_line_pq, line);
        lm->victim_line_cnt++;
    }
}

/* 새로 기록한 page를 PG_FREE -> PG_VALID로 바꾸고 vpc를 늘린다. */
static void mark_page_valid(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    /* page 상태 갱신 */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* 해당 block의 valid page 수 갱신 */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

    /* 해당 line의 valid page 수 갱신 */
    line = get_line(ssd, ppa);
    ftl_assert(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
    line->vpc++;
}

/* erase가 끝난 block의 모든 page와 vpc/ipc를 free 상태로 초기화한다. */
static void mark_block_free(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = get_blk(ssd, ppa);
    struct nand_page *pg = NULL;

    for (int i = 0; i < spp->pgs_per_blk; i++) {
        /* page 상태 초기화 */
        pg = &blk->pg[i];
        ftl_assert(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE;
    }

    /* block 상태 초기화 */
    ftl_assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
    ssd->block_erases++;
    if (exp_watch_blk[ppa->g.blk])
        EXP_LOG("[ERASE] " PPA_FMT " erase_cnt=%d (vpc/ipc reset)\n",
                PPA_ARG(ppa), blk->erase_cnt);
}

static void gc_read_page(struct ssd *ssd, struct ppa *ppa)
{
    /* GC read의 NAND 상태를 진행한다. 반환 지연 값 자체는 사용하지 않는다. */
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        ssd_advance_status(ssd, ppa, &gcr);
    }
}

/*
 * victim의 valid page를 새 PPA로 이동한다.
 * rmap으로 기존 LPN을 찾고 maptbl/rmap을 새 PPA에 맞게 갱신한다.
 */
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    struct write_pointer *wpp;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);

    ftl_assert(valid_lpn(ssd, lpn));
    /* GC는 온도 이력을 갱신하지 않고 현재 분류에 맞는 line으로 이동한다. */
    wpp = ssd_select_write_pointer(ssd, lpn);
    if (ssd->lpn_meta[lpn].state == LPN_STATE_HOT) {
        ssd->gc_hot_writes++;
    } else {
        ssd->gc_cold_writes++;
    }
    new_ppa = get_new_page(wpp);
    /* LPN -> 새 PPA 갱신 */
    set_maptbl_ent(ssd, lpn, &new_ppa);
    /* 새 PPA -> LPN 갱신 */
    set_rmap_ent(ssd, lpn, &new_ppa);
    if (exp_lpn_watched(lpn)) {
        exp_watch_blk[new_ppa.g.blk] = 1; /* 이동한 새 block도 추적 */
        EXP_LOG("[GC_MOVE] lpn=%lu state=%s access=%u short=%u "
                "last_seq=%" PRIu64 " decay_epoch=%" PRIu64 " "
                PPA_FMT " -> " PPA_FMT "\n",
                lpn,
                ssd->lpn_meta[lpn].state == LPN_STATE_HOT ? "HOT" : "COLD",
                ssd->lpn_meta[lpn].access_count,
                ssd->lpn_meta[lpn].short_interval_count,
                ssd->lpn_meta[lpn].last_write_seq,
                ssd->lpn_meta[lpn].last_decay_epoch,
                PPA_ARG(old_ppa), PPA_ARG(&new_ppa));
    }

    mark_page_valid(ssd, &new_ppa);

    /* 새 PPA를 사용했으므로 write pointer 이동 */
    ssd_advance_write_pointer(ssd, wpp);

    /*
     * Phase 1 수정
     */
    ssd->gc_page_writes++;
    ssd->nand_page_writes++;

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    /* channel의 gc_endtime 갱신 코드(현재 비활성화) */
#if 0
    new_ch = get_ch(ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;
#endif

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return 0;
}

/*
 * victim priority queue에서 valid page 수가 적은 line을 고른다.
 * 일반 GC는 invalid page가 line의 1/8 미만이면 이동 비용 때문에 건너뛴다.
 */
static struct line *select_victim_line(struct ssd *ssd, bool force)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *victim_line = NULL;

    victim_line = pqueue_peek(lm->victim_line_pq);
    if (!victim_line) {
        return NULL;
    }

    if (!force && victim_line->ipc < ssd->sp.pgs_per_line / 8) {
        return NULL;
    }

    pqueue_pop(lm->victim_line_pq);
    victim_line->pos = 0;
    lm->victim_line_cnt--;

    /* 이제 victim_line은 어떤 queue/list에도 속하지 않는다. */
    return victim_line;
}

/* victim block을 훑으며 valid page만 새 위치로 복사한다. */
static void clean_one_block(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* victim block에는 free page가 없어야 한다. */
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(ssd, ppa);
            /* 실제 GC write 시점에 maptbl을 갱신한다. */
            gc_write_page(ssd, ppa);
            cnt++;
        }
    }

    ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
}

static void mark_line_free(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line = get_line(ssd, ppa);

    line->ipc = 0;
    line->vpc = 0;

    /* GC 후에도 line class를 유지해 같은 class pool로 반환한다. */
    if (line->data_class == LINE_CLASS_HOT) {
        QTAILQ_INSERT_TAIL(&lm->free_hot_line_list, line, entry);
        lm->free_hot_line_cnt++;
    } else if (line->data_class == LINE_CLASS_COLD) {
        QTAILQ_INSERT_TAIL(&lm->free_cold_line_list, line, entry);
        lm->free_cold_line_cnt++;
    } else {
        ftl_err("GC returned line %d without a data class\n", line->id);
        abort();
    }

    lm->free_line_cnt++;
    ssd_validate_free_line_counts(ssd);
}

/*
 * line 단위 GC: victim 선택 -> valid page 복사 -> block erase -> free line 회수.
 * 한 line은 각 channel/LUN에서 block 번호가 같은 block들의 묶음이다.
 */
static int do_gc(struct ssd *ssd, bool force)
{
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;

    victim_line = select_victim_line(ssd, force);
    if (!victim_line) {
        return -1;
    }

    ssd->gc_count++;
    if (force) {
        ssd->emergency_gc_count++;
    }

    ppa.g.blk = victim_line->id;
    ftl_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
              victim_line->ipc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
              ssd->lm.free_line_cnt);

    /* victim line의 valid data 복사 */
    for (ch = 0; ch < spp->nchs; ch++) {
        for (lun = 0; lun < spp->luns_per_ch; lun++) {
            ppa.g.ch = ch;
            ppa.g.lun = lun;
            ppa.g.pl = 0;
            lunp = get_lun(ssd, &ppa);
            clean_one_block(ssd, &ppa);
            mark_block_free(ssd, &ppa);

            if (spp->enable_gc_delay) {
                struct nand_cmd gce;
                gce.type = GC_IO;
                gce.cmd = NAND_ERASE;
                gce.stime = 0;
                ssd_advance_status(ssd, &ppa, &gce);
            }

            lunp->gc_endtime = lunp->next_lun_avail_time;
        }
    }

    /* erase가 끝난 line을 free 상태로 갱신 */
    mark_line_free(ssd, &ppa);

    return 0;
}

/*
 * Read: LBA 범위를 LPN으로 바꾸고 maptbl에서 PPA를 찾은 뒤 NAND 지연을 계산한다.
 * 여러 page의 완료 시간 중 최댓값을 요청 지연으로 반환한다.
 */
static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs - 1) / spp->secs_per_pg;
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("read past device geometry: end_lpn=%"PRIu64" tt_pgs=%d\n",
                end_lpn, ssd->sp.tt_pgs);
        req->status = NVME_LBA_RANGE | NVME_DNR;
        return 0;
    }

    /* 일반 user I/O read 경로 */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        /* 설정된 FEMU_DUMP_LPN page를 read할 때마다 dump */
        if (exp_dump_lpn_set && lpn == exp_dump_lpn)
            femu_dbg_dump_lpn(ssd, lpn);

        ppa = get_maptbl_ent(ssd, lpn);
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
            //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
            //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
            continue;
        }

        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        sublat = ssd_advance_status(ssd, &ppa, &srd);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }

    return maxlat;
}

/*
 * Write(out-of-place update):
 * old PPA invalid -> new PPA 할당 -> maptbl/rmap 갱신 -> NAND write 지연 계산.
 */
static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)
{
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    int r;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("write past device geometry: end_lpn=%"PRIu64" tt_pgs=%d\n",
                end_lpn, ssd->sp.tt_pgs);
        req->status = NVME_LBA_RANGE | NVME_DNR;
        return 0;
    }

    /* free line이 매우 부족하면 host write 처리 전에 foreground GC 수행 */
    while (should_gc_high(ssd)) {
        /* 긴급 GC 조건이 해소될 때까지 GC 수행 */
        r = do_gc(ssd, true);
        if (r == -1)
            break;
    }

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        /* host write temperature history 갱신 */
        ssd_update_lpn_temperature(ssd, lpn);

        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            /* 같은 LPN의 이전 물리 page는 더 이상 최신 데이터가 아니다. */
            if (exp_lpn_watched(lpn))
                EXP_LOG("[INVALIDATE:overwrite] lpn=%lu old " PPA_FMT "\n",
                        lpn, PPA_ARG(&ppa));
            mark_page_invalid(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }

        /* 갱신된 온도에 맞는 pointer에서 새 PPA를 배정한다. */
        wpp = ssd_select_write_pointer(ssd, lpn);
        if (ssd->lpn_meta[lpn].state == LPN_STATE_HOT) {
            ssd->host_hot_writes++;
        } else {
            ssd->host_cold_writes++;
        }
        ppa = get_new_page(wpp);
        set_maptbl_ent(ssd, lpn, &ppa);
        set_rmap_ent(ssd, lpn, &ppa);

        mark_page_valid(ssd, &ppa);
        /* marker string이 있는 page만 log와 추적 대상에 포함 */
        if (femu_dbg_lpn_has_secret(ssd, lpn)) {
            exp_watch_lpn_add(lpn);
            exp_watch_blk[ppa.g.blk] = 1;
            EXP_LOG("[CLASSIFY] lpn=%lu state=%s access=%u short=%u "
                    "interval=%" PRIu64 " seq=%" PRIu64
                    " decay_epoch=%" PRIu64 "\n",
                    lpn,
                    ssd->lpn_meta[lpn].state == LPN_STATE_HOT ?
                    "HOT" : "COLD",
                    ssd->lpn_meta[lpn].access_count,
                    ssd->lpn_meta[lpn].short_interval_count,
                    ssd->lpn_meta[lpn].update_interval,
                    ssd->lpn_meta[lpn].last_write_seq,
                    ssd->lpn_meta[lpn].last_decay_epoch);
            EXP_LOG("[WRITE] lpn=%lu -> " PPA_FMT " (secret)\n",
                    lpn, PPA_ARG(&ppa));
        }

        /* 현재 PPA를 사용했으므로 다음 free page로 이동한다. */
        ssd_advance_write_pointer(ssd, wpp);

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        /* NAND write 완료 지연 계산 */
        curlat = ssd_advance_status(ssd, &ppa, &swr);
        /* loop가 처리한 LPN 수를 센다. */
        ssd->host_page_writes++;
        ssd->nand_page_writes++;
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    return maxlat;
}

/*
 * TRIM: 요청 LPN의 mapping을 해제하고 기존 page를 invalid로 만든다.
 * 여기서는 즉시 erase하지 않으며 공간 회수는 이후 GC가 담당한다.
 */
static uint64_t ssd_trim(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    NvmeDsmRange *ranges = req->dsm_ranges;
    int nr_ranges = req->dsm_nr_ranges;
    // uint32_t attributes = req->dsm_attributes;
    
    int total_trimmed_pages = 0;
    int total_already_invalid = 0;
    int total_out_of_bounds = 0;
    
    if (!ranges || nr_ranges <= 0) {
        printf("TRIM: Invalid ranges or count\n");
        return 0;
    }
    
    // printf("TRIM: Processing %d ranges (attributes=0x%x)\n", nr_ranges, attributes);
    
    for (int range_idx = 0; range_idx < nr_ranges; range_idx++) {
        uint64_t slba = le64_to_cpu(ranges[range_idx].slba);
        uint32_t nlb = le32_to_cpu(ranges[range_idx].nlb);
        // uint32_t cattr = le32_to_cpu(ranges[range_idx].cattr);
        
        uint64_t start_lpn = slba / spp->secs_per_pg;
        uint64_t end_lpn = (slba + nlb - 1) / spp->secs_per_pg;
        uint64_t lpn;
        struct ppa ppa;
        int trimmed_pages = 0;
        int already_invalid = 0;

        // ftl_debug("TRIM Range %d: LBA %lu + %u sectors, LPN range %lu-%lu (%lu pages), cattr=0x%x\n", 
        //        range_idx, slba, nlb, start_lpn, end_lpn, end_lpn - start_lpn + 1, cattr);

        /* 장치 범위를 벗어난 DSM range는 건너뛴다. */
        if (end_lpn >= spp->tt_pgs) {
            ftl_err("TRIM: Range %d exceeds FTL capacity - end_lpn=%lu, tt_pgs=%d\n", 
                   range_idx, end_lpn, spp->tt_pgs);
            total_out_of_bounds++;
            continue;  /* 현재 range를 건너뛰고 다음 range 처리 */
        }

        /* range에 포함된 LPN의 양방향 mapping을 하나씩 해제한다. */
        for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
            /* logical lifetime 종료 */
            ssd_reset_lpn_metadata(ssd, lpn);
            ppa = get_maptbl_ent(ssd, lpn);
            
            /* 이미 mapping이 없으면 상태 변경 없이 넘어간다. */
            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                already_invalid++;
                continue;
            }

            /* 실제 page는 invalid, LPN->PPA와 PPA->LPN은 모두 해제한다. */
            if (exp_lpn_watched(lpn))
                EXP_LOG("[INVALIDATE:trim] lpn=%lu old " PPA_FMT "\n",
                        lpn, PPA_ARG(&ppa));
            mark_page_invalid(ssd, &ppa);

            set_rmap_ent(ssd, INVALID_LPN, &ppa);
            
            ppa.ppa = UNMAPPED_PPA;
            set_maptbl_ent(ssd, lpn, &ppa);
            
            trimmed_pages++;
        }
        
        total_trimmed_pages += trimmed_pages;
        total_already_invalid += already_invalid;
        
        // ftl_debug("TRIM Range %d: %d pages trimmed, %d already invalid\n", 
        //        range_idx, trimmed_pages, already_invalid);
    }

    // ftl_debug("TRIM: Completed - %d pages trimmed, %d already invalid, %d out of bounds across %d ranges\n", 
    //        total_trimmed_pages, total_already_invalid, total_out_of_bounds, nr_ranges);

    /* nvme_dsm()에서 요청별로 할당한 range 배열을 반환한다. */
    g_free(ranges);
    req->dsm_ranges = NULL;
    req->dsm_nr_ranges = 0;
    req->dsm_attributes = 0;

    /* TRIM 직후 backend에 marker가 남아 있는지 확인 */
    if (exp_log_enabled)
        femu_dbg_scan_secret(ssd, "after_trim");

    return 0;  /* 이 모델에서 TRIM 자체의 NAND 지연은 0 */
}

/*
 * ========== FDP FTL 구현 ==========
 */

/*
 * get_next_free_ru - reclaim group의 free list에서 RU 하나를 꺼낸다.
 */
static FemuReclaimUnit *get_next_free_ru(struct ssd *ssd,
                                         FemuReclaimGroup *rg)
{
    struct ru_mgmt *rm = rg->ru_mgmt;
    FemuReclaimUnit *ru;

    ru = QTAILQ_FIRST(&rm->free_ru_list);
    if (!ru) {
        ftl_err("No free RUs left in rg[%d]\n", rg->rgidx);
        return NULL;
    }

    QTAILQ_REMOVE(&rm->free_ru_list, ru, entry);
    rm->free_ru_cnt--;
    return ru;
}

/*
 * fdp_set_ru_write_pointer - RU write pointer를 첫 line으로 초기화한다.
 */
static void fdp_set_ru_write_pointer(struct ssd *ssd, FemuReclaimUnit *ru)
{
    struct write_pointer *wptr = ru->ssd_wptr;

    ftl_assert(wptr != NULL);
    wptr->curline = ru->lines[0];
    wptr->ch = 0;
    wptr->lun = 0;
    wptr->pg = 0;
    wptr->blk = wptr->curline->id;
    wptr->pl = 0;
}

/*
 * fdp_get_new_ru - 지정한 RUH에 새 free RU를 할당한다.
 */
static FemuReclaimUnit *fdp_get_new_ru(struct ssd *ssd, uint16_t rgidx,
                                       uint16_t ruhid)
{
    FemuRuHandle *eruh = &ssd->ruhs[ruhid];
    FemuReclaimGroup *rg = &ssd->rg[rgidx];
    FemuReclaimUnit *new_ru;

    new_ru = get_next_free_ru(ssd, rg);
    if (!new_ru) {
        ftl_err("No reclaim unit available for ruh %d\n", ruhid);
        return NULL;
    }
    new_ru->rgidx = rgidx;
    new_ru->ruh = eruh;
    new_ru->last_init_time = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    new_ru->last_invalidated_time = 0;
    new_ru->erase_cnt = 0;
    new_ru->my_cb = 0.0f;
    new_ru->chance_token = 0;

    fdp_set_ru_write_pointer(ssd, new_ru);
    eruh->ru_in_use_cnt++;

    /* NvmeRuHandle에 새 active RU의 ruamw를 반영 */
    if (eruh->ruh && eruh->ruh->rus) {
        eruh->ruh->rus[rgidx] = new_ru->nvme_ru;
    }

    ftl_assert(new_ru->ruh == eruh);
    return new_ru;
}

/*
 * fdp_get_new_page - RU의 write pointer에서 다음 PPA를 얻는다.
 */
static struct ppa fdp_get_new_page(struct ssd *ssd, FemuReclaimUnit *ru)
{
    struct write_pointer *wpp = ru->ssd_wptr;
    struct ppa ppa;

    ftl_assert(ru != NULL);
    ftl_assert(wpp != NULL);

    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    ftl_assert(ppa.g.pl == 0);

    return ppa;
}

/*
 * fdp_advance_ru_pointer - RU write pointer를 이동한다. RU가 가득 차면
 * victim/full 자료구조로 옮기고 해당 RUH에 새 RU를 할당한다.
 * 이동 후 current RU를 반환하며 새 RU일 수도 있다.
 */
static FemuReclaimUnit *fdp_advance_ru_pointer(struct ssd *ssd,
                                               FemuReclaimGroup *rg,
                                               FemuRuHandle *ruh,
                                               FemuReclaimUnit *ru)
{
    struct ssdparams *spp = &ssd->sp;
    struct ru_mgmt *rm = rg->ru_mgmt;
    struct write_pointer *wpp = ru->ssd_wptr;
    FemuReclaimUnit *new_ru = NULL;
    bool is_full = true;
    bool ru_exhausted = false; /* RU 경계를 넘었는지 표시 */

    check_addr(wpp->ch, spp->nchs);
    wpp->ch++;
    if (wpp->ch == spp->nchs) {
        wpp->ch = 0;
        check_addr(wpp->lun, spp->luns_per_ch);
        wpp->lun++;
        if (wpp->lun == spp->luns_per_ch) {
            wpp->lun = 0;
            check_addr(wpp->pg, spp->pgs_per_blk);
            wpp->pg++;
            if (wpp->pg == spp->pgs_per_blk) {
                //if (ru->next_line_index == ru->n_lines) { /* TODO: 여러 line으로 구성된 RU 처리 */
                wpp->pg = 0;
                ru_exhausted = true;
                /* RU의 모든 line을 기록했으므로 상태 분류 */
                for (int i = 0; i < ru->n_lines; i++) {
                    struct line *line = ru->lines[i];
                    if (line->vpc != spp->pgs_per_line) {
                        is_full = false;
                    }
                }

                /* RU에 속한 line의 값을 이용해 RU vpc 갱신 */
                ru->vpc = 0;
                for (int i = 0; i < ru->n_lines; i++) {
                    ru->vpc += ru->lines[i]->vpc;
                }

                if (is_full) {
                    QTAILQ_INSERT_TAIL(&rm->full_ru_list, ru, entry);
                    rm->full_ru_cnt++;
                } else {
                    ru->utilization = (float)ru->vpc / ru->npages;
                    if (rm->mgmt_type == GC_GLOBAL_CB){ 
                        if (ru->utilization < 1.0f && ru->last_invalidated_time > 0) {
                        ru->my_cb = (uint64_t)(100000.0f * ru->utilization /
                            ((1.0f - ru->utilization + 0.001f) *
                            (float)ru->last_invalidated_time));
                        }
                        pqueue_insert(rm->victim_ru_cb, ru);
                    }else{
                        pqueue_insert(rm->victim_ru_pq, ru);
                        //TODO: per-RUH victim queue 처리(실험 단계)
                    }
                    rm->victim_ru_cnt++;
                }

                /* ruh->curr_ru가 가득 찼으므로 이 RUH에 새 RU 할당 */
                if (ruh != NULL) {
                    check_addr(wpp->blk, spp->blks_per_pl);
                    new_ru = fdp_get_new_ru(ssd, ru->rgidx, ruh->ruhid);
                    if (!new_ru) {
                        ftl_err("No free RU for ruh %d: device full - point %s L:%d\n",
                                ruh->ruhid, __FILE__, __LINE__);
                        /*
                         * device pressure 표시: active write frontier가 없음을
                         * caller가 알 수 있도록 curr_ru를 비운다.
                         */
                        ruh->curr_ru = NULL;
                        ftl_assert(false && __LINE__ );
                        /* TODO: free RU 고갈 후속 처리 */
                        return NULL;
                    }
                    FDP_TRACE(ssd, "RU_ROTATE ruhid=%u(curr_ru %u) old_ru=%u "
                              "new_ru=%u reason=%s victim_ru_cnt %d\n",
                              ruh->ruhid, ruh->curr_ru->ruidx, ru->ruidx, new_ru->ruidx, (is_full)? "full_valid":"full_victim", rm->victim_ru_cnt);
                    wpp = new_ru->ssd_wptr;
                    wpp->blk = wpp->curline->id;
                    check_addr(wpp->blk, spp->blks_per_pl);
                    ftl_assert(wpp->pg == 0);
                    ftl_assert(wpp->lun == 0);
                    ftl_assert(wpp->ch == 0);
                    ftl_assert(wpp->pl == 0);
                }
            }
        }
    }

    /*
     * 반환값 의미:
     *   new_ru non-NULL  -> RU 경계를 넘어 새 RU를 할당함
     *   NULL             -> RU를 다 썼지만 free RU가 없음(device full)
     *   ru               -> 아직 RU 중간이며 경계를 넘지 않음
     *
     * ru_exhausted로 device full인 NULL과 같은 ru를 반환하는
     * mid-RU 상황을 구분한다.
     */
    if (new_ru != NULL) {
        return new_ru;
    }
    if (ru_exhausted) {
        /* RU가 full_ru_list/victim_pq로 이동했음을 NULL로 caller에 알린다. */
        return NULL;
    }
    return ru;
}

/*
 * mark_page_valid_fdp - page를 valid로 표시하고 RU/line 통계를 갱신한다.
 */
static void mark_page_valid_fdp(struct ssd *ssd, struct ppa *ppa,
                                FemuReclaimUnit *ru)
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    blk = get_blk(ssd, ppa);
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

    line = get_line(ssd, ppa);
    ftl_assert(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
    line->vpc++;

    /* 해당 line에서 RU vpc 갱신(single-line RU fast path) */
    ftl_assert(line->my_ru == ru);
    if (ru->n_lines == 1) {
        ru->vpc = line->vpc;
    } else {
        ru->vpc = 0;
        for (int i = 0; i < ru->n_lines; i++) {
            ru->vpc += ru->lines[i]->vpc;
        }
    }

    ru->ruh->ruh_live_pages_cnt++;
}

/*
 * mark_page_invalid_fdp - page를 invalid로 만들고 RU/line/victim 상태를 갱신한다.
 */
static void mark_page_invalid_fdp(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;
    FemuReclaimUnit *ru;
    struct ru_mgmt *rm;
    bool was_full_ru = false;

    pg = get_pg(ssd, ppa);
    if (pg->status == PG_INVALID) {
        return;  /* 이미 invalid 처리된 page */
    }
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    blk = get_blk(ssd, ppa);
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    line = get_line(ssd, ppa);
    ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    if (line->vpc == spp->pgs_per_line) {
        ftl_assert(line->ipc == 0);
    }
    line->ipc++;
    ftl_assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
    line->vpc--;

    /* RU 상태 갱신 */
    ru = line->my_ru;
    ftl_assert(ru != NULL);
    rm = ssd->rg[ru->rgidx].ru_mgmt;
    /* 이 RU의 모든 line에 있는 ipc 합산(일반 설정에서는 n_lines=1) */
    ru->ipc = 0;
    for (int li = 0; li < ru->n_lines; li++) {
        ru->ipc += ru->lines[li]->ipc;
    }

    // FDP_TRACE(ssd, "INVAL ppa(ch=%u/lun=%u/blk=%u/pg=%u) "
    //           "ru=%u vpc=%d->%d pos %d was_full=%d victim_ru_cnt=%d\n",
    //           (unsigned)ppa->g.ch, (unsigned)ppa->g.lun,
    //           (unsigned)ppa->g.blk, (unsigned)ppa->g.pg,
    //           ru->ruidx, ru->vpc, ru->vpc - 1, ru->pos,
    //           (ru->vpc == spp->pgs_per_line * ru->n_lines),
    //           rm->victim_ru_cnt);

    /* full RU가 victim으로 이동해야 하는 상황인지 확인 */
    if (ru->vpc == spp->pgs_per_line * ru->n_lines) {
        was_full_ru = true;
    }

    /* GC strategy에 따라 RU vpc와 victim queue priority 갱신 */
    ru->vpc--;
    ru->utilization = (ru->vpc + ru->ipc > 0) ?
        (float)ru->vpc / (ru->vpc + ru->ipc) : 0.0f;
    ru->last_invalidated_time = qemu_clock_get_us(QEMU_CLOCK_REALTIME);

    switch (rm->mgmt_type) {
    case GC_GLOBAL_GREEDY:
    case GC_GLOBAL_RAND:
    case GC_NOISY_RUH_CUSTOM:
        if (ru->pos) {
            pqueue_change_priority(rm->victim_ru_pq, ru->vpc, ru);
        }
        /*
         * per-RUH victim queue는 GC_NOISY_RUH_CUSTOM만 참조하므로 이 queue의
         * 정렬도 함께 맞춘다. per-RUH heap은 ruh_pos로 indexing하므로 global
         * queue의 pos와 충돌하지 않는다(issue #189). 이 strategy에서는 모든
         * per-RUH pop/remove마다 ruh_pos를 reset하므로 ruh_pos != 0을
         * "per-RUH queue에 들어 있음"을 나타내는 값으로 사용할 수 있다.
         */
        if (rm->mgmt_type == GC_NOISY_RUH_CUSTOM && ru->ruh_pos &&
            ru->ruh && ru->ruh->ru_mgmt) {
            pqueue_change_priority(ru->ruh->ru_mgmt->victim_ru_pq, ru->vpc, ru);
        }
        if (was_full_ru) {
            QTAILQ_REMOVE(&rm->full_ru_list, ru, entry);
            rm->full_ru_cnt--;
            pqueue_insert(rm->victim_ru_pq, ru);
            rm->victim_ru_cnt++;
            /*
             * per-RUH queue를 사용하는 GC_NOISY_RUH_CUSTOM strategy에서만
             * 같은 RU를 이 queue에도 넣는다. GREEDY/RAND는 per-RUH queue에서
             * pop하지 않으므로 이중 등록하면 global pop 뒤 stale entry가 남아
             * 나중에 heap을 손상시킨다(issue #189).
             */
            if (rm->mgmt_type == GC_NOISY_RUH_CUSTOM &&
                ru->ruh && ru->ruh->ru_mgmt) {
               pqueue_insert(ru->ruh->ru_mgmt->victim_ru_pq, ru);
               ru->ruh->ru_mgmt->victim_ru_cnt++;
            }
        }
        break;

    case GC_GLOBAL_CB:
        if (ru->utilization < 1.0f && ru->last_invalidated_time > 0) {
            ru->my_cb = (uint64_t)(100000.0f * ru->utilization /
                ((1.0f - ru->utilization + 0.001f) *
                 (float)ru->last_invalidated_time));
        }
        if (ru->pos) {
            pqueue_change_priority(rm->victim_ru_cb, (pqueue_pri_t)ru->my_cb,
                                   ru);
        }
        if (was_full_ru) {
            QTAILQ_REMOVE(&rm->full_ru_list, ru, entry);
            rm->full_ru_cnt--;
            pqueue_insert(rm->victim_ru_cb, ru);
            rm->victim_ru_cnt++;
        }
        break;

    default:
        // Greedy 방식
        ftl_err( "Undefined rg mgmt type (rm->mgmt_type %d). Fallback to Greedy.\n",
              rm->mgmt_type);
        if (ru->pos) {
            pqueue_change_priority(rm->victim_ru_pq, ru->vpc, ru);
        }
        if (was_full_ru) {
            QTAILQ_REMOVE(&rm->full_ru_list, ru, entry);
            rm->full_ru_cnt--;
            pqueue_insert(rm->victim_ru_pq, ru);
            rm->victim_ru_cnt++;
        }
        break;
    }
    if (ru->ruh->ruh_live_pages_cnt > 0)
        ru->ruh->ruh_live_pages_cnt -=1 ;
    
}

// ru->gc_write_ptr의 상태와 정렬 여부 확인
static int check_gc_ruh_available(struct ssd *ssd, FemuRuHandle * ruh){
    /*
     * destination RG는 ruh->curr_ru에서 구한다. write로 RU를 모두 소진해
     * 이 RUH에 active RU가 없으면 RG를 구할 수도, reclaim data를 쓸 공간도
     * 없으므로 NULL을 역참조하지 않고 실패를 반환한다.
     */
    if (ruh->curr_ru == NULL) {
        return -1;
    }
    if(ruh->ruh_type == NVME_RUHT_INITIALLY_ISOLATED){
        if(ssd->ruhs[ssd->nruhs - 1].curr_ru == NULL){
            ssd->ruhs[ssd->nruhs - 1].curr_ru = fdp_get_new_ru(ssd, ruh->curr_ru->rgidx, ruh->ruhid);
            ssd->ruhs[ssd->nruhs - 1].rus[ssd->ruhs[ssd->nruhs - 1].curr_ru->rgidx] = ssd->ruhs[ssd->nruhs - 1].curr_ru;
            // 현재는 필요하지 않은 것으로 보임
            ssd->ruhs[ssd->nruhs - 1].ruh->rus[ssd->ruhs[ssd->nruhs - 1].curr_ru->rgidx] = ssd->ruhs[ssd->nruhs - 1].curr_ru->nvme_ru;  //qemu-system-x86_64: ../hw/femu/bbssd/ftl.c:2106: select_victim_ru: Assertion `victim_ru != ((void *)0)' failed.
        }
        if (ssd->ruhs[ssd->nruhs - 1].curr_ru == NULL){
            // 남은 공간이 없음을 의미
            return -1;
        }

    }
    else if(ruh->ruh_type == NVME_RUHT_PERSISTENTLY_ISOLATED){
        if(ruh->gc_ru == NULL){
            ruh->gc_ru = fdp_get_new_ru(ssd, ruh->curr_ru->rgidx, ruh->ruhid);
            ftl_debug("check_gc_ruh_available ruh %d gc_ru idx %d , %p call new ru  \n", ruh->ruhid, ruh->gc_ru->ruidx, ruh->gc_ru );
            if (ruh->gc_ru == NULL){
                //assert(ruh->gc_ru != NULL); /* 남은 공간이 없음을 의미 */
                return -1;
            }
        }
    }else{
        ftl_err("Unsupported RUH type : %d\n",ruh->ruh_type);
        ftl_assert(false && __LINE__); 
    }
    return 0;
}

/*
 * select_victim_ru_from_ruh - 특정 RUH의 queue에서 victim을 선택한다.
 */
static FemuReclaimUnit *select_victim_ru_from_ruh(struct ssd *ssd,
                                                   uint16_t rgid,
                                                   uint16_t ruhid)
{
    FemuReclaimUnit *victim_ru = NULL;
    struct ru_mgmt *ru_mgmt = ssd->ruhs[ruhid].ru_mgmt;

    if (!ru_mgmt) {
        return NULL;
    }

    victim_ru = pqueue_pop(ru_mgmt->victim_ru_pq);
    if (victim_ru) {
        /*
         * pqueue_pop은 pop한 element에 저장된 index를 지우지 않는다. 따라서
         * ruh_pos가 실제로 "per-RUH queue에 없음"을 나타내도록 reset한다
         * (NOISY 경로와 동일한 규칙). 그렇지 않으면 이후 change_priority나
         * insert가 stale per-RUH index를 사용해 issue #189 유형의 버그를 만든다.
         * 현재 이 경로에서 per-RUH queue를 채우는 strategy는 없지만 향후 사용을
         * 고려해 일관성을 보장한다.
         */
        victim_ru->ruh_pos = 0;
        ru_mgmt->victim_ru_cnt--;
    }
    return victim_ru;
}

/*
 * select_victim_ru - 설정된 GC strategy에 따라 가장 적합한 victim RU를 고른다.
 */
static FemuReclaimUnit *select_victim_ru(struct ssd *ssd, uint16_t rgid,
                                         uint16_t ruhid, bool force)
{
    struct ru_mgmt *rm = ssd->rg[rgid].ru_mgmt;
    FemuReclaimUnit *victim_ru = NULL;

    switch (rm->mgmt_type) {
    case GC_GLOBAL_GREEDY:
        victim_ru = pqueue_pop(rm->victim_ru_pq);
        break;

    case GC_GLOBAL_CB:
        victim_ru = pqueue_pop(rm->victim_ru_cb);

        break;

    case GC_GLOBAL_RAND:
        victim_ru = pqueue_randpop(rm->victim_ru_pq);
        break;

    case GC_NOISY_RUH_CUSTOM: {
        /*
         * Cross-RUH 선택: custom GC threshold를 넘은 모든 RUH에서
         * vpc가 가장 낮은 RU를 찾는다.
         */
        FemuReclaimUnit *ru = NULL;
        int best_ruh = -1;
        int i;
        for (i = 0; i < (int)ssd->nruhs; i++) {
            if (!ssd->ruhs[i].ru_mgmt) {
                continue;
            }
            if (ssd->ruhs[i].ru_in_use_cnt <=
                ssd->ruhs[i].ru_mgmt->custom_gc_threshold) {
                continue;
            }
            ru = pqueue_peek(ssd->ruhs[i].ru_mgmt->victim_ru_pq);
            if (!ru) {
                continue;
            }
            if (!victim_ru || ru->vpc < victim_ru->vpc) {
                best_ruh = i;
                victim_ru = ru;
            }
        }
        if (best_ruh >= 0) {
            victim_ru = pqueue_pop(
                ssd->ruhs[best_ruh].ru_mgmt->victim_ru_pq);
            if (victim_ru) {
                /*
                 * pqueue_pop은 element에 저장된 index를 지우지 않으므로
                 * ruh_pos를 명시적으로 reset한다. change_priority의 guard가
                 * 이 값을 per-RUH queue 등록 여부로 사용한다.
                 */
                victim_ru->ruh_pos = 0;
                ssd->ruhs[best_ruh].ru_mgmt->victim_ru_cnt--;
                /*
                 * 아직 유효한 pos를 사용해 global queue에서도 제거한다.
                 * global victim_ru_cnt 감소와 pos=0 처리는 아래 공통 cleanup에서
                 * 수행하므로 여기서는 건드리지 않는다. nrg>1이면 per-RUH queue가
                 * 어느 RG의 RU든 가질 수 있으므로, global 쪽 동일 entry는 caller의
                 * rgid가 아니라 victim 자신이 속한 RG queue에 있다.
                 */
                if (victim_ru->pos) {
                    pqueue_remove(ssd->rg[victim_ru->rgidx].ru_mgmt->victim_ru_pq,
                                  victim_ru);
                }
            }
        } else {
            /* 선택하지 못하면 global greedy로 fallback */
            victim_ru = pqueue_pop(rm->victim_ru_pq);
            /*
             * global queue에서 pop한 RU가 per-RUH queue에도 남아 있을 수 있다
             * (NOISY는 full RU를 두 queue에 모두 넣는다). 아직 유효한 ruh_pos로
             * per-RUH queue에서도 제거해 stale entry가 남지 않게 한다. 남겨 두면
             * 이후 change_priority 또는 put-back의 중복 insert 문제가 생긴다.
             */
            if (victim_ru && victim_ru->ruh_pos &&
                victim_ru->ruh && victim_ru->ruh->ru_mgmt) {
                pqueue_remove(victim_ru->ruh->ru_mgmt->victim_ru_pq, victim_ru);
                victim_ru->ruh_pos = 0;
                victim_ru->ruh->ru_mgmt->victim_ru_cnt--;
            }
        }
        break;
    }

    case GC_SELECTIVE_RUH:
    case GC_EXPLOIT_SEQUENTIAL:
        victim_ru = pqueue_pop(rm->victim_ru_pq);
        break;

    case GC_SELECTIVE_RUH_SOCIAL_WELFARE:
        victim_ru = select_victim_ru_from_ruh(ssd, rgid, ruhid);
        break;

    case GC_BIT_POPULATION:
    case GC_GLOBAL_WARM:
    case GC_SELECTIVE_RUH_ADV:
    case GC_SELECTIVE_MIDAS_OP:
    default:
        /* 선택하지 못하면 greedy로 fallback */
        victim_ru = pqueue_pop(rm->victim_ru_pq);
        break;
    }

    if (!victim_ru) {
        /*
         * victim_ru_pq가 비어 있다. 사용 중인 모든 RU가 아직 invalidation 없이
         * 채워지는 중인 경우다(예: sequential fill). reclaim할 victim이 없으므로
         * NULL 반환이 정상 동작이다.
         */
        return NULL;
    }

    if (!force && victim_ru->vpc > 0) {
        int threshold = victim_ru->npages / 8;
        if (victim_ru->ipc < threshold) {
            /*
             * GC를 미루고 victim을 원래 queue에 되돌린다. Cross-RG NOISY 선택은
             * caller의 rgid와 다른 reclaim group 소속 RU를 반환할 수 있으므로,
             * 위의 global 제거 및 아래의 공통 count 감소와 일치하도록 victim
             * 자신이 속한 RG queue에 다시 넣는다. caller의 rm을 사용하면 잘못된
             * heap에 entry가 기록되고, 저장된 pos가 이후 remove/change_priority에서
             * 그 잘못된 배열을 가리켜 queue가 손상된다.
             */
            struct ru_mgmt *victim_rm = ssd->rg[victim_ru->rgidx].ru_mgmt;
            /* victim을 원래 queue에 되돌림 */
            FDP_TRACE(ssd, "GC_BACK_RESERT triggered but delay GC (ru %d ipc %d threshold %d full %d)\n",victim_ru->ruidx, victim_ru->ipc, threshold, victim_ru->npages);
            if (victim_rm->mgmt_type == GC_GLOBAL_CB){
                pqueue_insert(victim_rm->victim_ru_cb, victim_ru);
            }else{
                pqueue_insert(victim_rm->victim_ru_pq, victim_ru);
                /*
                 * GC_NOISY_RUH_CUSTOM 선택은 이 RU를 per-RUH와 global queue
                 * 양쪽에서 pop하고 ruh_pos reset 및 per-RUH count 감소까지 했다.
                 * 두 자료구조가 어긋나지 않도록 per-RUH 등록과 count도 복구한다.
                 * pqueue_insert가 ruh_pos를 다시 설정한다.
                 */
                if (victim_rm->mgmt_type == GC_NOISY_RUH_CUSTOM &&
                    victim_ru->ruh && victim_ru->ruh->ru_mgmt) {
                    pqueue_insert(victim_ru->ruh->ru_mgmt->victim_ru_pq,
                                  victim_ru);
                    victim_ru->ruh->ru_mgmt->victim_ru_cnt++;
                }
            }
            return NULL;
        }
    }

    victim_ru->pos = 0;
    victim_ru->ruh_pos = 0;
    /*
     * victim 자신이 속한 reclaim group의 count를 줄인다. cross-RG NOISY 선택을
     * 제외하면 이 값은 caller의 rgid와 같다. NOISY는 다른 RG에서 victim을
     * 가져올 수 있으므로, 그 victim이 속한 RG의 global count를 조정해야 한다.
     */
    ssd->rg[victim_ru->rgidx].ru_mgmt->victim_ru_cnt--;

    return victim_ru;
}

/*
 * gc_write_page_fdp_style - valid page를 GC destination RU로 이동한다.
 */
static void gc_write_page_fdp_style(struct ssd *ssd, struct ppa *old_ppa,
                                    FemuRuHandle *dest_ruh)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);
    FemuReclaimUnit *dest_ru=NULL;
    FemuReclaimUnit *ret_ru=NULL;
    ftl_assert(valid_lpn(ssd, lpn));
    ftl_assert(dest_ruh!=NULL);
    /* 이 함수는 new_ru가 아닌 ruh->curr_ru 또는 gc_ru의 write pointer를 사용한다. */
    if (dest_ruh->ruh_type == NVME_RUHT_PERSISTENTLY_ISOLATED)
    {
        dest_ru = dest_ruh->gc_ru;
    }else if( dest_ruh->ruh_type == NVME_RUHT_INITIALLY_ISOLATED && dest_ruh->ruhid == ssd->ruhs[ssd->nruhs-1].ruhid ){
        dest_ru = dest_ruh->curr_ru;
    }else{
        ftl_err("Unidentified ruht. ");
        ftl_assert(false && __LINE__);
    }

    new_ppa = fdp_get_new_page(ssd, dest_ru);
    set_maptbl_ent(ssd, lpn, &new_ppa);
    set_rmap_ent(ssd, lpn, &new_ppa);
    mark_page_valid_fdp(ssd, &new_ppa, dest_ru);

    // FDP_TRACE(ssd, "GC_MIGRATE lpn=%lu src(ch=%u/lun=%u/blk=%u/pg=%u) "
    //           "dst(ch=%u/lun=%u/blk=%u/pg=%u) dest_ruhid=%u\n",
    //           lpn, (unsigned)old_ppa->g.ch, (unsigned)old_ppa->g.lun,
    //           (unsigned)old_ppa->g.blk, (unsigned)old_ppa->g.pg,
    //           (unsigned)new_ppa.g.ch, (unsigned)new_ppa.g.lun,
    //           (unsigned)new_ppa.g.blk, (unsigned)new_ppa.g.pg,
    //           dest_ruh->ruhid);

    /*
     * fdp_advance_ru_pointer()는 foreground write와 GC 양쪽에서 호출되며,
     * 전달받은 RU의 write pointer만 이동한다. 따라서 caller가 이동 직후
     * RUH type에 맞춰 curr_ru 또는 gc_ru를 갱신해야 한다.
     */

    if(dest_ruh->ruh_type == NVME_RUHT_PERSISTENTLY_ISOLATED ){
        // pointer 이동 후 RUH의 gc_ru 갱신
        if( (ret_ru = fdp_advance_ru_pointer(ssd, &ssd->rg[dest_ru->rgidx], dest_ru->ruh, dest_ru)) != dest_ru){
            dest_ruh->gc_ru = ret_ru;
        }
    }else if (dest_ruh->ruh_type == NVME_RUHT_INITIALLY_ISOLATED ){
        int gcruh_id = ssd->nruhs-1;
        ftl_assert( dest_ruh->ruhid == gcruh_id );
        if( (ret_ru = fdp_advance_ru_pointer(ssd, &ssd->rg[dest_ru->rgidx], dest_ruh, dest_ru)) != dest_ru ) {
            // II용 GC RUH의 관련 pointer를 함께 갱신
            ssd->ruhs[gcruh_id].rus[dest_ru->rgidx] = ret_ru;
            ssd->ruhs[gcruh_id].curr_ru = ret_ru;
            ssd->ruhs[gcruh_id].ruh->rus[dest_ru->rgidx] = ret_ru->nvme_ru;
        } 
    }
    
    ftl_assert((ret_ru != NULL));

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;
}

/*
 * clean_one_block_fdp_style - block의 valid page를 읽어 destination RUH로 옮긴다.
 * @ppa: 정리할 block을 가리키는 PPA
 * @dest_ruh: GC data를 기록할 RUH
 *
 * GC write 중 새 RU가 할당되면 curr_ru/gc_ru가 바뀔 수 있으므로,
 * 수명이 끝날 수 있는 RU pointer 대신 RUH를 전달한다.
 */
static int clean_one_block_fdp_style(struct ssd *ssd, struct ppa *ppa,
                                     FemuRuHandle *dest_ruh)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter;
    int cnt = 0;
    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(ssd, ppa);
            gc_write_page_fdp_style(ssd, ppa, dest_ruh);
            cnt++;
        }
    }

    ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
    return cnt;
}

/*
 * mark_ru_free - GC가 끝난 victim RU를 free 상태로 초기화한다.
 */
static void mark_ru_free(struct ssd *ssd, uint16_t rgid,
                         FemuReclaimUnit *ru)
{
    struct ssdparams *spp = &ssd->sp;
    struct ru_mgmt *rm = ssd->rg[rgid].ru_mgmt;
    struct ppa ppa;

    ftl_assert(ru != NULL);

    for (int i = 0; i < ru->n_lines; i++) {
        ru->lines[i]->ipc = 0;
        ru->lines[i]->vpc = 0;
        ru->lines[i]->pos = 0;
        ppa.g.blk = ru->lines[i]->id;
        for (int ch = 0; ch < spp->nchs; ch++) {
            for (int lun = 0; lun < spp->luns_per_ch; lun++) {
                ppa.g.ch = ch;
                ppa.g.lun = lun;
                ppa.g.pl = 0;
                mark_block_free(ssd, &ppa);
            }
        }
    }

    ru->vpc = 0;
    ru->ipc = 0;
    ru->pos = 0;
    ru->ruh_pos = 0;
    ru->next_line_index = 1;
    ru->utilization = 0.0f;
    ru->my_cb = 0.0f;
    ru->erase_cnt++;
    ru->chance_token = 0;

    fdp_set_ru_write_pointer(ssd, ru);

    /* ruamw를 초기값으로 복구 */
    ftl_assert(ru->nvme_ru != NULL);
    ftl_assert(ru->ruh != NULL);
    ftl_assert(ru->ruh->ruh != NULL);
    ru->nvme_ru->ruamw = ru->ruh->ruh->ruamw;

    QTAILQ_INSERT_TAIL(&rm->free_ru_list, ru, entry);
    rm->free_ru_cnt++;
}

/*
 * reinsert_victim_ru - 완전히 pop한 victim RU를 자신이 속한 reclaim group의
 * victim queue로 되돌린다. free destination RU가 없어 GC를 진행하지 못할 때
 * victim이 어느 queue에도 속하지 않는 상태가 되는 것을 막는다.
 * select_victim_ru()의 bookkeeping을 되돌리며, cross-RG NOISY victim도
 * 올바른 heap으로 돌아가도록 victim 자신의 reclaim group을 기준으로 처리한다.
 */
static void reinsert_victim_ru(struct ssd *ssd, FemuReclaimUnit *victim_ru)
{
    struct ru_mgmt *victim_rm = ssd->rg[victim_ru->rgidx].ru_mgmt;

    if (victim_rm->mgmt_type == GC_GLOBAL_CB) {
        pqueue_insert(victim_rm->victim_ru_cb, victim_ru);
    } else {
        pqueue_insert(victim_rm->victim_ru_pq, victim_ru);
        if (victim_rm->mgmt_type == GC_NOISY_RUH_CUSTOM &&
            victim_ru->ruh && victim_ru->ruh->ru_mgmt) {
            pqueue_insert(victim_ru->ruh->ru_mgmt->victim_ru_pq, victim_ru);
            victim_ru->ruh->ru_mgmt->victim_ru_cnt++;
        }
    }
    victim_rm->victim_ru_cnt++;
}

/*
 * do_gc_fdp_style - victim RU 선택 -> valid page를 GC RU로 이동 ->
 * victim free 순서로 FDP GC를 수행한다.
 * 유효한 victim을 선택하면 RU 하나를 회수한다.
 */
static int do_gc_fdp_style(struct ssd *ssd, uint16_t rgid, uint16_t ruhid,
                           bool force)
{
    struct ssdparams *spp = &ssd->sp;
    FemuReclaimUnit *victim_ru;
    //FemuReclaimUnit *new_ru;
    struct nand_lun *lunp;
    struct ppa ppa;
    int vpc_cnt = 0;
    int blk_cnt = 0;
    int ret = 0;
    victim_ru = select_victim_ru(ssd, rgid, ruhid, force);
    if (!victim_ru) {
        //FDP_TRACE(ssd,"GC_SKIP Unable to find victim RU, gc skip\n");
        return -1;
    }

    /*
     * RUH isolation type에 따라 GC destination을 선택한다.
     * - Initially Isolated(II): 마지막 RUH의 curr_ru에 기록
     * - Persistently Isolated(PI): victim과 같은 RUH의 전용 gc_ru에 기록
     * destination RU를 확보할 수 없으면 victim을 queue에 되돌리고 GC를 중단한다.
     */
    FemuRuHandle *victim_ruh = victim_ru->ruh;
    FemuRuHandle *dest_ruh = NULL;

    if (victim_ruh->ruh_type == NVME_RUHT_PERSISTENTLY_ISOLATED) {
        dest_ruh = victim_ruh;
        /* PI RUH: curr_ru와 분리된 전용 gc_ru에 GC data 기록 */
        if ((ret = check_gc_ruh_available(ssd, dest_ruh)) < 0 ){
            /*
             * GC destination으로 쓸 free RU가 없으면 실제로 회수 가능한 공간이
             * 없는 상태다. emulator를 중단하지 않고 victim을 되돌린 뒤 GC를
             * 실패 처리해 caller가 일반 device-full write로 처리하게 한다.
             */
            reinsert_victim_ru(ssd, victim_ru);
            return -1;
        }

    } else if (victim_ruh->ruh_type == NVME_RUHT_INITIALLY_ISOLATED){
        /* II RUH: 마지막 RUH의 curr_ru에 GC data 기록 */
        dest_ruh = &ssd->ruhs[ssd->nruhs - 1];
        if ((ret = check_gc_ruh_available(ssd, dest_ruh)) < 0 ){
            reinsert_victim_ru(ssd, victim_ru);
            return -1;
        }
    }else {
        ftl_err("Undefined RUHT.");
        ftl_assert(false && __LINE__);
    }
    ftl_assert(dest_ruh!=NULL);
    /* active RU를 victim으로 GC하지 않는지 검증 */
    if (victim_ru == victim_ru->ruh->curr_ru) {
        ftl_err("Victim RU %d is active, skipping GC\n", victim_ru->ruidx);
        // active RU가 선택됐다면 GC bookkeeping 오류다.
        ftl_assert(false && __LINE__);
        return -1;
    }

    FDP_TRACE(ssd, "GC_START rgid=%u ruhid=%u victim_ru=%u "
              "victim_vpc=%d isolation=%s gc_type=%s\n",
              rgid, ruhid, victim_ru->ruidx, victim_ru->vpc,
              (victim_ruh->ruh_type == NVME_RUHT_PERSISTENTLY_ISOLATED) ?
              "PI" : "II", (force) ? "FORCE" : "BACK" );

    /* victim RU의 valid page 이동 */
    for (int i = 0; i < spp->lines_per_ru; i++) {
        struct line *victim_line = victim_ru->lines[i];
        ppa.g.blk = victim_line->id;
        for (int ch = 0; ch < spp->nchs; ch++) {
            for (int lun = 0; lun < spp->luns_per_ch; lun++) {
                ppa.g.ch = ch;
                ppa.g.lun = lun;
                ppa.g.pl = 0;
                lunp = get_lun(ssd, &ppa);

                vpc_cnt += clean_one_block_fdp_style(ssd, &ppa, dest_ruh);
                
                blk_cnt++;
                mark_block_free(ssd, &ppa);
                if (spp->enable_gc_delay) {
                    struct nand_cmd gce;
                    gce.type = GC_IO;
                    gce.cmd = NAND_ERASE;
                    gce.stime = 0;
                    ssd_advance_status(ssd, &ppa, &gce);
                }
                lunp->gc_endtime = lunp->next_lun_avail_time;
            }
        }
    }

    /* FDP 통계 갱신: GC로 media에 기록한 byte */
    uint64_t gc_bytes = (uint64_t)vpc_cnt * spp->secsz * spp->secs_per_pg;
    uint64_t erase_bytes = (uint64_t)blk_cnt * spp->secsz * spp->secs_per_pg
                           * spp->pgs_per_blk;

    FDP_TRACE(ssd, "GC_DONE victim_ru=%u pages_migrated=%d "
              "blocks_erased=%d mbmw_delta=%lu mbe_delta=%lu\n",
              victim_ru->ruidx, vpc_cnt, blk_cnt, gc_bytes, erase_bytes);
    nvme_fdp_stat_inc(&ssd->n->subsys->endgrp.fdp.mbmw, gc_bytes);
    nvme_fdp_stat_inc(&victim_ru->ruh->mbmw, gc_bytes);
    nvme_fdp_stat_inc(&victim_ru->ruh->ruh->mbmw, gc_bytes);
    nvme_fdp_stat_inc(&ssd->n->subsys->endgrp.fdp.mbe, erase_bytes);
    nvme_fdp_stat_inc(&victim_ru->ruh->mbe, erase_bytes);
    nvme_fdp_stat_inc(&victim_ru->ruh->ruh->mbe, erase_bytes);

    if (ssd->ruhs[victim_ru->ruh->ruhid].ru_in_use_cnt > 0) {
        ssd->ruhs[victim_ru->ruh->ruhid].ru_in_use_cnt--;
    }
    ssd->ruhs[victim_ru->ruh->ruhid].ruh_live_pages_cnt -= vpc_cnt;

    /* GC에 따른 RU 변경 controller event 생성 */
    if (ssd->n->subsys) {
        NvmeEnduranceGroup *endgrp = &ssd->n->subsys->endgrp;
        NvmeRuHandle *nvme_ruh = victim_ru->ruh->ruh;
        if (nvme_ruh &&
            (nvme_ruh->event_filter >>
             nvme_fdp_evf_shifts[FDP_EVT_RUH_IMPLICIT_RU_CHANGE]) & 0x1) {
            NvmeFdpEvent *e = ftl_fdp_alloc_event(ssd,
                                    &endgrp->fdp.ctrl_events);
            e->type = FDP_EVT_RUH_IMPLICIT_RU_CHANGE;
            e->flags = FDPEF_LV;
            e->rgid = cpu_to_le16(victim_ru->rgidx);
            e->ruhid = victim_ru->ruh->ruhid;
        }
    }

    /*
     * victim을 자신이 속한 reclaim group으로 반환한다. cross-RG NOISY victim은
     * caller의 rgid와 다를 수 있다. caller rgid로 반환하면 per-RG free list가
     * 손상되고, 나중에 다른 group의 RU를 잘못 할당하게 된다.
     * non-NOISY 경로에서는 victim_ru->rgidx == rgid이므로 결과가 같다.
     */
    mark_ru_free(ssd, victim_ru->rgidx, victim_ru);
    return 0;
}

/*
 * ssd_stream_write - placement 정보를 반영해 page를 할당하는 FDP write 경로
 */
static uint64_t ssd_stream_write(FemuCtrl *n, struct ssd *ssd,
                                 NvmeRequest *req)
{
    NvmeNamespace *ns = req->ns;
    struct ssdparams *spp = &ssd->sp;
    FemuReclaimGroup *rg;
    FemuRuHandle *ruh;
    FemuReclaimUnit *ru;

    uint64_t lba = req->slba;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    int r;

    /* request에서 placement 정보 해석 */
    uint16_t pid = req->fdp_dspec;
    uint8_t dtype = req->fdp_dtype;
    uint16_t ph, rgid, ruhid;

    if (dtype != NVME_DIRECTIVE_DATA_PLACEMENT ||
        !nvme_parse_pid(ns, pid, &ph, &rgid)) {
        /* placement가 요청됐지만 PID가 잘못되면 INVALID_PID event 생성 */
        if (dtype == NVME_DIRECTIVE_DATA_PLACEMENT && ssd->n->subsys) {
            NvmeEnduranceGroup *endgrp = &ssd->n->subsys->endgrp;
            NvmeRuHandle *def_ruh = &endgrp->fdp.ruhs[ns->fdp.phs[0]];
            if ((def_ruh->event_filter >>
                 nvme_fdp_evf_shifts[FDP_EVT_INVALID_PID]) & 0x1) {
                NvmeFdpEvent *e = ftl_fdp_alloc_event(ssd,
                                        &endgrp->fdp.host_events);
                e->type = FDP_EVT_INVALID_PID;
                e->flags = FDPEF_PIV | FDPEF_NSIDV;
                e->pid = cpu_to_le16(pid);
                e->nsid = cpu_to_le32(ns->id);
            }
        }
        ph = 0;
        rgid = 0;
    }

    ruhid = ns->fdp.phs[ph];
    /* nvme_parse_pid가 ph를 검증하지만 ruhid 범위도 추가로 확인 */
    if (unlikely(ruhid >= (uint16_t)ssd->nruhs)) {
        ftl_err("ssd_stream_write: ruhid %u >= nruhs %lu, clamping to 0\n",
                (unsigned)ruhid, (unsigned long)ssd->nruhs);
        ruhid = 0;
    }
    rg = &ssd->rg[rgid];
    ruh = &ssd->ruhs[ruhid];

    // FDP_TRACE(ssd, "WRITE lpn=%lu-%lu dtype=%u dspec=0x%x ph=%u "
    //           "ruhid=%u rgid=%u\n", start_lpn, end_lpn, dtype, pid,
    //           ph, ruhid, rgid);

    /*
     * 이 RUH에 active RU가 있는지 확인한다. sequential fill 뒤 마지막 RU가
     * full_ru_list에 들어가고 새 RU를 할당하지 못하면
     * fdp_advance_ru_pointer()가 curr_ru를 NULL로 만든다.
     * 이 경우 foreground GC로 공간을 먼저 확보한다.
     */
    if (unlikely(!ruh->curr_ru)) {
        /* 새 RU 할당 전에 GC로 공간 회수 시도 */
        // 실험용 GC 동작 비활성화
        //int max_fg_gc = (int)(ssd->nrg > 0 ?
        //    ssd->rg[0].ru_mgmt->tt_rus : 64);
        //for (int gi = 0; gi < max_fg_gc && !ruh->curr_ru; gi++) {
        //    r = do_gc_fdp_style(ssd, rgid, ruhid, true);
        //    if (r == -1) break;
            /* GC가 RU를 회수했을 수 있으므로 새 RU 할당 시도 */
            FemuReclaimUnit *fresh = fdp_get_new_ru(ssd, rgid, ruhid);
            if (fresh) {
                ruh->rus[rgid] = fresh;
                ruh->ruh->rus[rgid] = fresh->nvme_ru;
                ruh->curr_ru = fresh;
            }else{
                ftl_err("NO reclaim Unit. Device is full error\n");
                // 할당 실패 경로
            }
        //}
        if (!ruh->curr_ru) {
            /*
             * foreground backpressure GC로도 reclaim unit을 확보하지 못한
             * 실제 공간 고갈 상태다. data를 쓰지 않고 success로 완료하지 않도록
             * command를 capacity error로 실패 처리한다.
             */
            ftl_err("ssd_stream_write: device full, no RU for ruh %d\n", ruhid);
            req->status = NVME_CAP_EXCEEDED | NVME_DNR;
            return 0;
        }
    }
    ru = ruh->rus[rgid];
    ru = ruh->curr_ru;
    ftl_assert(ruh->curr_ru == ruh->rus[rgid]);
    
    if (end_lpn >= spp->tt_pgs) {
        ftl_err("write past device geometry: end_lpn=%" PRIu64 " tt_pgs=%d\n",
                end_lpn, spp->tt_pgs);
        req->status = NVME_LBA_RANGE | NVME_DNR;
        return 0;
    }

    /*
     * Foreground GC backpressure: 고정 횟수가 아니라 write pressure가
     * 해소될 때까지 reclaim한다. 한 pass에서 최대 RU 하나만 회수하므로 기존의
     * 고정 상한(nrg)은 지속적인 overwrite에서 free-RU pool이 0까지 줄어들게 해
     * 잘못된 device-full을 만들 수 있었다.
     * should_gc_high_fdp_style()이 pressure 해소를 알릴 때까지 실행하면 host
     * write가 GC를 기다리며 pool을 threshold 위로 유지한다. victim이 더 없으면
     * do_gc_fdp_style()이 -1을 반환하며 이는 정상 종료다. 반복 count는 예기치
     * 않은 무한 반복만 막으며, 실제 진행 중에는 걸리지 않도록 전체 RU 수를
     * 기준으로 제한한다.
     */
    {
        uint64_t fg_gc_iters = 0;
        uint64_t max_fg_gc = (uint64_t)ssd->nrg * ssd->rg[0].ru_mgmt->tt_rus
                             + ssd->nrg;
        while (should_gc_high_fdp_style(ssd) >= 0 &&
               fg_gc_iters < max_fg_gc) {
            r = do_gc_fdp_style(ssd, rgid, ruhid, true);
            if (r == -1) {
                break;
            }
            fg_gc_iters++;
        }
    }

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        /* curr_ru 갱신은 fdp_advance_ru_pointer()가 처리한다. */
        ru = ruh->curr_ru;
        /*
         * 이전 반복에서 마지막 free reclaim unit을 소진했을 수 있다.
         * 더 할당할 RU가 없으면 fdp_advance_ru_pointer()가 curr_ru를 비운다.
         * NULL RU를 fdp_get_new_page()에서 역참조하지 않도록 중단하고,
         * crash 또는 실제 write 없는 success 대신 capacity error를 반환한다.
         */
        if (unlikely(!ru)) {
            req->status = NVME_CAP_EXCEEDED | NVME_DNR;
            break;
        }

        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            mark_page_invalid_fdp(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }
        /* 새 PPA에 out-of-place write */
        ppa = fdp_get_new_page(ssd, ru);
        set_maptbl_ent(ssd, lpn, &ppa);
        set_rmap_ent(ssd, lpn, &ppa);
        mark_page_valid_fdp(ssd, &ppa, ru);

        /* 이 RU의 남은 ruamw 감소 */
        if (ru->nvme_ru && ru->nvme_ru->ruamw > 0) {
            ru->nvme_ru->ruamw--; //TODO: page KiB 단위 감소가 맞는지 확인
        }

        /* RU write pointer 이동, 필요하면 새 RU 할당 */
        FemuReclaimUnit *ret = fdp_advance_ru_pointer(ssd, rg, ruh, ru);
        if (ret && ret != ruh->curr_ru) {
            ruh->rus[rgid] = ret;
            ruh->curr_ru = ret;
            ruh->ruh->rus[rgid] = ret->nvme_ru;
            ru = ret;
        } else if (!ret) {
            /*
             * free RU가 없어 fdp_advance_ru_pointer가 curr_ru를 비웠다.
             * 다음 반복 시작 부분에서 이 상태를 처리한다.
             */
            ru = NULL;
        }

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        curlat = ssd_advance_status(ssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    return maxlat;
}

/*
 * nvme_do_write_fdp - FDP write 진입점: 통계 갱신 후 stream write 수행
 */
uint64_t nvme_do_write_fdp(FemuCtrl *n, NvmeRequest *req, uint64_t slba,
                           uint32_t nlb)
{
    NvmeNamespace *ns = req->ns;
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;
    uint64_t data_bytes;

    /* FDP host bytes written 통계 갱신 */
    data_bytes = (uint64_t)nlb * spp->secsz;
    nvme_fdp_stat_inc(&ns->endgrp->fdp.hbmw, data_bytes);
    nvme_fdp_stat_inc(&ns->endgrp->fdp.mbmw, data_bytes);

    /* per-RUH 통계 갱신 */
    uint16_t pid = req->fdp_dspec;
    uint8_t dtype = req->fdp_dtype;
    uint16_t ph, rg, ruhid;

    if (dtype != NVME_DIRECTIVE_DATA_PLACEMENT ||
        !nvme_parse_pid(ns, pid, &ph, &rg)) {
        ph = 0;
        rg = 0;
    }
    ruhid = ns->fdp.phs[ph];
    nvme_fdp_stat_inc(&ssd->ruhs[ruhid].hbmw, data_bytes);
    nvme_fdp_stat_inc(&ssd->ruhs[ruhid].ruh->hbmw, data_bytes);
    nvme_fdp_stat_inc(&ssd->ruhs[ruhid].mbmw, data_bytes);
    nvme_fdp_stat_inc(&ssd->ruhs[ruhid].ruh->mbmw, data_bytes);

    return ssd_stream_write(n, ssd, req);
}

/* ========== FDP 초기화 함수 ========== */

/*
 * femu_fdp_init_ru_mgmt - reclaim group의 RU 관리 자료구조를 초기화한다.
 */
static void femu_fdp_init_ru_mgmt(struct ssd *ssd, FemuReclaimGroup *rg)
{
    struct ru_mgmt *rm = rg->ru_mgmt;

    rm->tt_rus = rg->tt_nru;
    rm->free_ru_cnt = rg->tt_nru;
    rm->victim_ru_cnt = 0;
    rm->full_ru_cnt = 0;
    rm->custom_gc_threshold = 0;

    /* 기본 GC strategy */
    rm->mgmt_type = GC_GLOBAL_GREEDY;

    rm->is_gc_triggered = false;
    rm->is_force_gc_triggered = false;
    rm->waf_score_global = 0.0f;
    rm->waf_score_transitory = 0.0f;
    rm->utilization_overall = 0.0f;

    QTAILQ_INIT(&rm->free_ru_list);
    QTAILQ_INIT(&rm->full_ru_list);

    rm->victim_ru_pq = pqueue_init(rm->tt_rus, victim_ru_cmp_pri,
                                   victim_ru_get_pri, victim_ru_set_pri,
                                   victim_ru_get_pos, victim_ru_set_pos);

    rm->victim_ru_cb = pqueue_init(rm->tt_rus, victim_ru_cmp_pri_by_cb,
                                   victim_ru_get_pri_by_cb,
                                   victim_ru_set_pri_by_cb,
                                   victim_ru_get_pos, victim_ru_set_pos);
}

/*
 * femu_fdp_init_ssd_reclaim_unit - 하나의 RU에 line과 write pointer를 초기화한다.
 */
static void femu_fdp_init_ssd_reclaim_unit(struct ssd *ssd,
                                           FemuReclaimUnit *femu_ru,
                                           int rgidx, int index)
{
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp;

    femu_ru->n_lines = spp->lines_per_ru;
    femu_ru->next_line_index = 1;
    femu_ru->vpc = 0;
    femu_ru->ipc = 0;
    femu_ru->pos = 0;
    femu_ru->ruh_pos = 0;     /* 아직 어떤 victim pqueue에도 등록되지 않음 */
    femu_ru->ssd_wptr = g_malloc0(sizeof(struct write_pointer));
    femu_ru->npages = spp->lines_per_ru * spp->pgs_per_line;

    wpp = femu_ru->ssd_wptr;
    femu_ru->lines = g_malloc0(femu_ru->n_lines * sizeof(struct line *));
    for (int i = 0; i < femu_ru->n_lines; i++) {
        femu_ru->lines[i] = get_next_free_line(ssd);
        if (!femu_ru->lines[i]) {
            ftl_err("FDP: no free line for RU %d (rg %d, line %d/%d)\n",
                    index, rgidx, i, femu_ru->n_lines);
            abort();
        }
        femu_ru->lines[i]->my_ru = femu_ru;
    }
    wpp->curline = femu_ru->lines[0];
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pl = 0;
    wpp->blk = wpp->curline->id;
    wpp->pg = 0;
}

/*
 * femu_fdp_ssd_init_reclaim_group - 모든 RG와 각 RU pool을 초기화한다.
 */
static void femu_fdp_ssd_init_reclaim_group(FemuCtrl *n, struct ssd *ssd)
{
    NvmeSubsystem *subsys = n->subsys;
    uint64_t rgs = subsys->params.fdp.nrg;
    FemuReclaimGroup *rg;
    uint64_t tt_nru = ssd->sp.total_ru_cnt;

    ftl_assert(tt_nru > 0);

    ssd->rg = g_malloc0(rgs * sizeof(FemuReclaimGroup));
    ssd->nrg = rgs;
    ssd->rus = g_malloc0(rgs * sizeof(FemuReclaimUnit *));

    for (int i = 0; i < (int)rgs; i++) {
        rg = &ssd->rg[i];
        rg->rgidx = i;
        rg->tt_nru = tt_nru / rgs;
        ssd->rus[i] = g_malloc0(tt_nru * sizeof(FemuReclaimUnit));
        rg->rus = ssd->rus[i];
        rg->ru_mgmt = g_malloc0(sizeof(struct ru_mgmt));
        femu_fdp_init_ru_mgmt(ssd, rg);
        fdp_log("Allocated %lu RUs to rg[%d]\n", tt_nru, i);
    }

    /* NvmeReclaimUnit pointer를 연결하고 SSD 계층의 각 RU 초기화 */
    NvmeReclaimUnit **russ = subsys->endgrp.fdp.rus;
    if (russ) {
        for (int i = 0; i < (int)rgs; i++) {
            rg = &ssd->rg[i];
            rg->ru_mgmt->free_ru_cnt = 0;
            for (int j = 0; j < rg->tt_nru; j++) {
                rg->rus[j].rgidx = i;
                rg->rus[j].nvme_ru = &russ[i][j];
                rg->rus[j].ruidx = j;
                femu_fdp_init_ssd_reclaim_unit(ssd, &rg->rus[j], i, j);
                QTAILQ_INSERT_TAIL(&rg->ru_mgmt->free_ru_list,
                                   &rg->rus[j], entry);
                rg->ru_mgmt->free_ru_cnt++;
            }
            rg->ru_mgmt->gc_thres_pcent =
                n->bb_params.gc_thres_pcent / 100.0;
            rg->ru_mgmt->gc_thres_pcent_high =
                n->bb_params.gc_thres_pcent_high / 100.0;
            rg->ru_mgmt->gc_thres_rus =
                (uint64_t)((1 - rg->ru_mgmt->gc_thres_pcent) *
                           rg->tt_nru);
            rg->ru_mgmt->gc_thres_rus_high =
                (uint64_t)((1 - rg->ru_mgmt->gc_thres_pcent_high) *
                           rg->tt_nru);
            ftl_log("rg[%d] gc threshold (%d%%) %lu/%d RU\n",
                    i, n->bb_params.gc_thres_pcent,
                    rg->ru_mgmt->gc_thres_rus, rg->tt_nru);
            ftl_log("rg[%d] gc threshold_high (%d%%) %lu/%d RU\n",
                    i, n->bb_params.gc_thres_pcent_high,
                    rg->ru_mgmt->gc_thres_rus_high, rg->tt_nru);

            /* 설정된 GC strategy 적용 */
            rg->ru_mgmt->mgmt_type = n->bb_params.gc_strategy;
            ftl_log("rg[%d] gc strategy=%d\n", i,
                    rg->ru_mgmt->mgmt_type);
        }
    }
}

/*
 * femu_fdp_ssd_init_ru_handles - namespace의 각 PH에 FemuRuHandle을 초기화한다.
 */
static void femu_fdp_ssd_init_ru_handles(FemuCtrl *n, struct ssd *ssd)
{
    NvmeNamespace *ns = &n->namespaces[0];
    NvmeSubsystem *subsys = n->subsys;
    NvmeEnduranceGroup *endgrp = &subsys->endgrp;
    uint16_t nruh = subsys->params.fdp.nruh;
    uint16_t ph, *ruhid;

    ssd->ruhs = g_malloc0(nruh * sizeof(FemuRuHandle));
    ssd->nruhs = nruh;
    ruhid = ns->fdp.phs;

    for (ph = 0; ph < ns->fdp.nphs; ph++, ruhid++) {
        uint16_t i = *ruhid;
        NvmeRuHandle *nvme_ruh = &endgrp->fdp.ruhs[i];

        ssd->ruhs[i].ruh = nvme_ruh;
        ssd->ruhs[i].ruh_type = nvme_ruh->ruht;
        ssd->ruhs[i].ruhid = i;
        ssd->ruhs[i].ruh_live_pages_cnt = 0;
        ssd->ruhs[i].ru_in_use_cnt = 0;
        ssd->ruhs[i].curr_rg = 0;
        ssd->ruhs[i].hbmw = 0;
        ssd->ruhs[i].mbmw = 0;
        ssd->ruhs[i].mbe = 0;

        /* per-RG RU pointer 배열 할당 */
        ssd->ruhs[i].rus = g_malloc0(sizeof(FemuReclaimUnit *) *
                                     endgrp->fdp.nrg);
        for (int j = 0; j < (int)endgrp->fdp.nrg; j++) {
            ssd->ruhs[i].rus[j] = fdp_get_new_ru(ssd, j, i);
            ssd->ruhs[i].rus[j]->ruh = &ssd->ruhs[i];
            ssd->ruhs[i].ruh->rus[j] = ssd->ruhs[i].rus[j]->nvme_ru;
        }
        /*
         * active RU는 위 loop에서 마지막으로 할당한 RU가 아니라 default
         * reclaim group(rgid 0, curr_rg 0)의 RU와 일치해야 한다. non-placement
         * write는 rgid 0을 사용하고 ssd->rg[rgid]의 관리 객체를 통해
         * ruh->curr_ru를 이동한다. curr_ru를 마지막 group(nrg-1)에 두면 default
         * write가 rg[nrg-1]의 RU를 rg[0] bookkeeping으로 처리한다. 그 결과 RU가
         * 한 group의 victim queue에 들어간 뒤 stale heap position으로 자기 group의
         * queue에서 조회되어 nrg>1에서 NULL 역참조 crash가 발생한다.
         * cross-group placement write에는 여전히 per-(RUH,RG) active-RU model이
         * 필요하며, 여기서는 default 경로를 바로잡는다.
         */
        ssd->ruhs[i].curr_ru = ssd->ruhs[i].rus[0];

        /* PI type RUH는 per-RUH victim queue용 자체 ru_mgmt를 가진다. */
        if (nvme_ruh->ruht == NVME_RUHT_PERSISTENTLY_ISOLATED) {
            ssd->ruhs[i].ru_mgmt = g_malloc0(sizeof(struct ru_mgmt));
            ssd->ruhs[i].ru_mgmt->mgmt_type = n->bb_params.gc_strategy;
            ssd->ruhs[i].ru_mgmt->victim_ru_cnt = 0;
            ssd->ruhs[i].ru_mgmt->full_ru_cnt = 0;
            ssd->ruhs[i].ru_mgmt->custom_gc_threshold = 0;
            QTAILQ_INIT(&ssd->ruhs[i].ru_mgmt->free_ru_list);
            QTAILQ_INIT(&ssd->ruhs[i].ru_mgmt->full_ru_list);
            /*
             * Per-RUH queue는 ruh_pos로 indexing하므로
             * (victim_ru_*_pos_ruh 참고) per-RG queue의 pos와 충돌하지 않는다
             * (issue #189).
             */
            ssd->ruhs[i].ru_mgmt->victim_ru_pq =
                pqueue_init(ssd->rg[0].tt_nru, victim_ru_cmp_pri,
                            victim_ru_get_pri, victim_ru_set_pri,
                            victim_ru_get_pos_ruh, victim_ru_set_pos_ruh);
            ssd->ruhs[i].ru_mgmt->victim_ru_cb =
                pqueue_init(ssd->rg[0].tt_nru, victim_ru_cmp_pri_by_cb,
                            victim_ru_get_pri_by_cb,
                            victim_ru_set_pri_by_cb,
                            victim_ru_get_pos_ruh, victim_ru_set_pos_ruh);
        }

        ftl_log("FDP: ruh[%d] type=%d, curr_ru=%d (line=%d)\n",
                i, ssd->ruhs[i].ruh_type, ssd->ruhs[i].curr_ru->ruidx,
                ssd->ruhs[i].curr_ru->lines[0]->id);
    }
}

/*
 * ssd_init_fdp_params - FDP 전용 SSD parameter를 계산한다.
 */
static void ssd_init_fdp_params(struct ssdparams *spp, FemuCtrl *n)
{
    NvmeSubsystem *subsys = n->subsys;
    NvmeEnduranceGroup *endgrp = &subsys->endgrp;
    uint64_t runs = endgrp->fdp.runs;

    /* lines_per_ru: reclaim unit 하나에 포함되는 line(superblock) 수 */
    spp->lines_per_ru = 1; /* M1에서는 단순화를 위해 RU당 line 1개 사용 */

    /*
     * device geometry로 전체 RU 수 계산:
     * total_ru = tt_lines / lines_per_ru
     * nvme_subsys_setup_fdp()에서 할당한 NvmeReclaimUnit 배열을 넘지 않도록
     * endgrp->fdp.nru를 상한으로 제한한다.
     */
    spp->total_ru_cnt = spp->tt_lines / spp->lines_per_ru;

    if (endgrp->fdp.nru == 0) {
        endgrp->fdp.nru = spp->total_ru_cnt;
    } else if (spp->total_ru_cnt > (int)endgrp->fdp.nru) {
        ftl_log("FDP: clamping total_ru from %d to %lu (endgrp.nru)\n",
                spp->total_ru_cnt, (unsigned long)endgrp->fdp.nru);
        spp->total_ru_cnt = endgrp->fdp.nru;
    }

    ftl_log("FDP params: lines_per_ru=%d, total_ru=%d, runs=%lu\n",
            spp->lines_per_ru, spp->total_ru_cnt, (unsigned long)runs);
}

/*
 * ssd_reset_maptbl - 전체 mapping table을 초기화한다(FDP TRIM에서 사용).
 */
static void ssd_reset_maptbl(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
        ssd->rmap[i] = INVALID_LPN;
    }
}

/*
 * ssd_trim_fdp_range - 기본 FDP DSM deallocate 동작. 요청된 LBA range에
 * 포함된 logical page만 invalid 처리한다. FDP 경로에서 RU/line vpc를 줄이고
 * RU를 victim queue로 옮긴 뒤, reverse map과 L2P entry를 해제한다.
 * erase는 GC가 담당한다. 일부 LBA에 대한 host TRIM이 다른 logical data를
 * 변경하지 않는 일반 SSD의 deallocate 동작과 같다.
 */
static void ssd_trim_fdp_range(FemuCtrl *n, NvmeRequest *req)
{
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;
    NvmeDsmRange *ranges = req->dsm_ranges;
    int nr_ranges = req->dsm_nr_ranges;
    struct ppa ppa;
    uint64_t lpn;
    int total_trimmed_pages = 0;
    int total_already_invalid = 0;

    if (!ranges || nr_ranges <= 0) {
        return;
    }

    for (int range_idx = 0; range_idx < nr_ranges; range_idx++) {
        uint64_t r_slba = le64_to_cpu(ranges[range_idx].slba);
        uint32_t r_nlb = le32_to_cpu(ranges[range_idx].nlb);
        uint64_t start_lpn = r_slba / spp->secs_per_pg;
        uint64_t end_lpn = (r_slba + r_nlb - 1) / spp->secs_per_pg;

        if (end_lpn >= spp->tt_pgs) {
            ftl_err("FDP TRIM: range %d exceeds capacity (end_lpn=%lu "
                    "tt_pgs=%d)\n", range_idx, end_lpn, spp->tt_pgs);
            continue;
        }

        for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
            ppa = get_maptbl_ent(ssd, lpn);
            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                total_already_invalid++;
                continue;
            }
            mark_page_invalid_fdp(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
            ppa.ppa = UNMAPPED_PPA;
            set_maptbl_ent(ssd, lpn, &ppa);
            total_trimmed_pages++;
        }
    }

    ftl_debug("FDP TRIM: %d pages trimmed, %d already invalid across %d ranges\n",
              total_trimmed_pages, total_already_invalid, nr_ranges);
}

/*
 * ssd_trim_fdp_reset_all - FDP DSM 전체 장치 reset.
 * test 전용 fdp_trim_erase_all device property로 명시적으로 활성화한다.
 * 모든 block을 erase하고 reclaim unit과 RUH 상태, mapping table을 초기화한다.
 * 이는 non-filesystem fio+trim sweep용 초기 prototype 동작으로 요청 LBA range를
 * 무시하므로 실제 SSD의 DSM deallocate와 다르며 기본값은 비활성화다.
 */
static void ssd_trim_fdp_reset_all(FemuCtrl *n, NvmeRequest *req, uint64_t slba,
                                   uint32_t nlb)
{
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;
    struct ppa ppa;
    NvmeEnduranceGroup *endgrp = &n->subsys->endgrp;
    FemuReclaimUnit *v_ru;
    struct nand_lun *lunp;
    NvmeRuHandle *ruh;
    int rg_idx;

    /* 모든 block erase */
    for (int ch = 0; ch < spp->nchs; ch++) {
        for (int lun = 0; lun < spp->luns_per_ch; lun++) {
            for (int blk = 0; blk < spp->blks_per_pl; blk++) {
                ppa.g.ch = ch;
                ppa.g.lun = lun;
                ppa.g.pl = 0;
                ppa.g.blk = blk;
                lunp = get_lun(ssd, &ppa);
                mark_block_free(ssd, &ppa);
                if (spp->enable_gc_delay) {
                    struct nand_cmd gce;
                    gce.type = GC_IO;
                    gce.cmd = NAND_ERASE;
                    gce.stime = 0;
                    ssd_advance_status(ssd, &ppa, &gce);
                }
                lunp->gc_endtime = lunp->next_lun_avail_time;
            }
        }
    }

    /* 모든 reclaim group의 victim/full RU queue 비우기 */
    for (rg_idx = 0; rg_idx < (int)ssd->nrg; rg_idx++) {
        struct ru_mgmt *rm = ssd->rg[rg_idx].ru_mgmt;
        while ((v_ru = pqueue_peek(rm->victim_ru_pq)) != NULL) {
            pqueue_remove(rm->victim_ru_pq, v_ru);
            rm->victim_ru_cnt--;
            mark_ru_free(ssd, v_ru->rgidx, v_ru);
        }
        /*
         * GC_GLOBAL_CB는 full victim을 victim_ru_pq가 아니라 victim_ru_cb에
         * 보관한다. TRIM/format 시 이 queue도 비우지 않으면 stale
         * victim_ru_cnt와 함께 RU가 누락된다. RG mode마다 두 queue는 상호
         * 배타적이며 비활성 queue는 비어 있고, victim_ru_cb도 같은 pos field로
         * indexing하므로 이 처리는 안전하다. 기존 count가 이미 어긋났을
         * 가능성에 대비해 shared count의 underflow를 막는다.
         */
        while ((v_ru = pqueue_peek(rm->victim_ru_cb)) != NULL) {
            pqueue_remove(rm->victim_ru_cb, v_ru);
            if (rm->victim_ru_cnt > 0) {
                rm->victim_ru_cnt--;
            }
            mark_ru_free(ssd, v_ru->rgidx, v_ru);
        }
        while ((v_ru = QTAILQ_FIRST(&rm->full_ru_list)) != NULL) {
            QTAILQ_REMOVE(&rm->full_ru_list, v_ru, entry);
            rm->full_ru_cnt--;
            mark_ru_free(ssd, v_ru->rgidx, v_ru);
        }
    }

    /* 모든 RG에 걸쳐 각 RUH의 active RU와 통계 초기화 */
    ruh = endgrp->fdp.ruhs;
    for (int i = 0; i < (int)endgrp->fdp.nruh; i++, ruh++) {
        /*
         * 이 RUH의 victim queue도 비운다. 위의 per-RG drain에서 RU를 free하고
         * mark_ru_free로 ruh_pos를 0으로 만들었지만, per-RUH heap은 여전히 해당
         * RU를 참조한다. stale entry를 제거해 TRIM 후 per-RUH queue의 일관성을
         * 유지한다(PI RUH만 해당). RU 객체는 이미 free 상태이므로 heap과
         * count만 비운다.
         */
        if (ssd->ruhs[i].ru_mgmt && ssd->ruhs[i].ru_mgmt->victim_ru_pq) {
            FemuReclaimUnit *pru;
            /*
             * pqueue_pop은 percolate_down 중 ruh_pos를 재할당하지만 pop된
             * element 자신의 index는 지우지 않는다. 명시적으로 0으로 만들지
             * 않으면 drain된 RU에 stale ruh_pos가 남아 이후 queue에 있는
             * 것으로 잘못 판단된다.
             */
            while ((pru = pqueue_pop(ssd->ruhs[i].ru_mgmt->victim_ru_pq))) {
                pru->ruh_pos = 0;
            }
            ssd->ruhs[i].ru_mgmt->victim_ru_cnt = 0;
        }
        ruh->hbmw = 0;
        ruh->mbmw = 0;
        ruh->mbe = 0;
        ssd->ruhs[i].hbmw = 0;
        ssd->ruhs[i].mbmw = 0;
        ssd->ruhs[i].mbe = 0;
        if (ssd->ruhs[i].curr_ru) {
            mark_ru_free(ssd, ssd->ruhs[i].curr_ru->rgidx,
                         ssd->ruhs[i].curr_ru);
        }
        ssd->ruhs[i].curr_ru = NULL;
        for (rg_idx = 0; rg_idx < (int)ssd->nrg; rg_idx++) {
            ssd->ruhs[i].rus[rg_idx] =
                fdp_get_new_ru(ssd, rg_idx, ssd->ruhs[i].ruhid);
            ssd->ruhs[i].ruh->rus[rg_idx] =
                ssd->ruhs[i].rus[rg_idx]->nvme_ru;
        }
        /* primary RG(index 0)를 active RG로 설정 */
        ssd->ruhs[i].curr_ru = ssd->ruhs[i].rus[0];
    }

    ssd_reset_maptbl(ssd);

    endgrp->fdp.hbmw = 0;
    endgrp->fdp.mbmw = 0;
    endgrp->fdp.mbe = 0;

    ftl_log("FDP TRIM: all RUs reset\n");
}

/*
 * ssd_trim_fdp_style - FDP DSM deallocate 경로를 선택한다. 기본은 요청 range만
 * 처리하며 fdp_trim_erase_all knob가 설정된 경우에만 전체 장치를 reset한다.
 * 두 경로 모두 command별 DSM range list를 해제한다. 이 list는 nvme_dsm()이
 * command마다 할당하고 non-FDP의 ssd_trim()과 마찬가지로 FTL이 반환한다.
 */
static void ssd_trim_fdp_style(FemuCtrl *n, NvmeRequest *req, uint64_t slba,
                               uint32_t nlb)
{
    if (n->bb_params.fdp_trim_erase_all) {
        ssd_trim_fdp_reset_all(n, req, slba, nlb);
    } else {
        ssd_trim_fdp_range(n, req);
    }

    g_free(req->dsm_ranges);
    req->dsm_ranges = NULL;
    req->dsm_nr_ranges = 0;
    req->dsm_attributes = 0;
}

/*
 * FTL worker: to_ftl dequeue -> opcode 처리 -> 지연 반영 -> to_poller enqueue.
 * 각 요청을 돌려보낸 뒤 여유 공간이 부족하면 background GC 한 번을 시도한다.
 */
static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct ssd *ssd = n->ssd;
    NvmeRequest *req = NULL;
    uint64_t lat = 0;
    int rc;
    int i;

    /* NVMe dataplane과 poller가 준비될 때까지 FTL 처리를 시작하지 않는다. */
    while (!*(ssd->dataplane_started_ptr)) {
        usleep(100000);
    }

    /* FIXME: to_ftl/to_poller를 안전하게 연결하고 종료하는 처리 필요 */
    ssd->to_ftl = n->to_ftl;
    ssd->to_poller = n->to_poller;

    while (1) {
        for (i = 1; i <= n->nr_pollers; i++) {
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
                continue;

            /* NVMe poller가 넘긴 요청 한 개를 FTL queue에서 꺼낸다. */
            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
            if (rc != 1) {
                printf("FEMU: FTL to_ftl dequeue failed\n");
            }

            ftl_assert(req);
            lat = 0;
            /*
             * I/O 계층의 validation에서 이미 실패한 request는 기존 error
             * status를 poller가 완료 처리하도록 FTL ring만 통과한다. 이 경우
             * opcode handler를 실행하면 안 된다. I/O 계층이 req->slba와 req->nlb를
             * 설정하기 전에 반환했을 수 있어 값이 stale할 수 있고, ssd_read/write가
             * 무관한 mapping 상태를 변경하거나 error를 덮어쓸 수 있기 때문이다.
             * latency는 0으로 두고 poller가 전달된 status를 CQ에 기록하게 한다.
             */
            if (req->status == NVME_SUCCESS) {
                /* opcode에 따라 mapping/GC 상태를 바꾸고 NAND 지연을 받는다. */
                switch (req->cmd.opcode) {
                case NVME_CMD_WRITE:
                    if (ssd->fdp_enabled) {
                        lat = nvme_do_write_fdp(n, req, req->slba, req->nlb);
                    } else {
                        lat = ssd_write(ssd, req);
                    }
                    break;
                case NVME_CMD_READ:
                    lat = ssd_read(ssd, req);
                    break;
                case NVME_CMD_DSM:
                    if (ssd->fdp_enabled) {
                        ssd_trim_fdp_style(n, req, req->slba, req->nlb);
                        lat = 0;
                    } else if (req->dsm_ranges && req->dsm_nr_ranges > 0) {
                        lat = ssd_trim(ssd, req);
                    }
                    break;
                default:
                    ;
                }
            }

            /* poller가 완료 시점을 판단할 수 있도록 계산된 지연을 더한다. */
            req->reqlat = lat;
            req->expire_time += lat;

            /* 완료 후보 요청을 원래 NVMe poller 쪽으로 돌려보낸다. */
            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                ftl_err("FTL to_poller enqueue failed\n");
            }

            /* host 요청 처리 뒤 일반 임계값을 확인하는 background GC */
            if (ssd->fdp_enabled) {
                int16_t rgidx;
                /*
                 * threshold를 넘은 reclaim group에 GC pass 한 번을 요청한다.
                 * 실제 GC policy는 여기 아닌 do_gc_fdp_style()에 있다.
                 */
                if (!((rgidx = should_gc_fdp_style(ssd)) < 0))
                {
                    //do_gc_fdp_style(ssd, rgidx, 0, false);
                    if (ssd->nrg == 1)
                        do_gc_fdp_style(ssd, 0, 0, false);
                    else
                    {
                        do_gc_fdp_style(ssd, rgidx, 0, false);
                    }
                }
            } else if (should_gc(ssd)) {
                do_gc(ssd, false);
            }
        }
    }

    return NULL;
}
