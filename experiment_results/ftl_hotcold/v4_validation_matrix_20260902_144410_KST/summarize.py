#!/usr/bin/env python3

import csv
import json
import math
import re
import sys
from pathlib import Path


PAGES_PER_LINE = 16384
FIXED_HOST_PAGES = 134217728
REFERENCE_ROOT = Path(
    "/root/workspace/FEMU/experiment_results/ftl_hotcold/"
    "v4_global_vs_classgc_20260901_181151_KST"
)


def parse_number(value):
    if value in (None, "", "N/A"):
        return None
    try:
        return int(value)
    except ValueError:
        return float(value)


def parse_stats(path):
    text = path.read_text(encoding="utf-8", errors="replace").strip()
    if not text:
        raise ValueError(f"empty stats file: {path}")
    return {
        key: value
        for key, value in re.findall(r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)", text)
    }


def latency_value_ns(section, field):
    for unit, factor in (("ns", 1.0), ("us", 1_000.0), ("ms", 1_000_000.0)):
        block = section.get(f"{field}_{unit}")
        if isinstance(block, dict) and block.get("mean") is not None:
            return float(block["mean"]) * factor
    return None


def percentile_ns(section, percentile):
    for unit, factor in (("ns", 1.0), ("us", 1_000.0), ("ms", 1_000_000.0)):
        block = section.get(f"clat_{unit}")
        if not isinstance(block, dict):
            continue
        values = block.get("percentile", {})
        for key, value in values.items():
            try:
                if math.isclose(float(key), percentile, rel_tol=0, abs_tol=0.0001):
                    return float(value) * factor
            except (TypeError, ValueError):
                continue
    return None


def parse_fio(path):
    data = json.loads(path.read_text(encoding="utf-8"))
    jobs = data.get("jobs", [])
    if not jobs:
        raise ValueError(f"no fio jobs in {path}")

    # group_reporting emits one aggregate entry per group. Keep one entry for
    # each group id so cloned jobs are not double-counted on older fio versions.
    selected = []
    seen = set()
    for index, job in enumerate(jobs):
        group_id = job.get("groupid", index)
        if group_id in seen:
            continue
        seen.add(group_id)
        selected.append(job)

    iops = 0.0
    io_bytes = 0
    weighted_lat = 0.0
    lat_weight = 0
    p99 = []
    p999 = []
    p9999 = []
    runtime_ms = 0
    errors = []
    for job in selected:
        write = job.get("write", {})
        iops += float(write.get("iops", 0.0))
        io_bytes += int(write.get("io_bytes", 0))
        total_ios = int(write.get("total_ios", 0))
        lat = latency_value_ns(write, "lat")
        if lat is not None:
            weight = max(total_ios, 1)
            weighted_lat += lat * weight
            lat_weight += weight
        for target, bucket in ((99.0, p99), (99.9, p999), (99.99, p9999)):
            value = percentile_ns(write, target)
            if value is not None:
                bucket.append(value)
        runtime_ms = max(runtime_ms, int(write.get("runtime", 0)))
        errors.append(int(job.get("error", 0)))

    if any(errors):
        raise ValueError(f"fio reported job errors in {path}: {errors}")
    return {
        "fio_iops": iops,
        "fio_io_bytes": io_bytes,
        "fio_runtime_ms": runtime_ms,
        "avg_latency_ns": weighted_lat / lat_weight if lat_weight else None,
        "p99_clat_ns": max(p99) if p99 else None,
        "p999_clat_ns": max(p999) if p999 else None,
        "p9999_clat_ns": max(p9999) if p9999 else None,
    }


def selected_attempt(root, run_id):
    run_dir = root / "runs" / run_id
    selected = (run_dir / "selected_attempt.txt").read_text().strip()
    attempt = run_dir / f"attempt_{int(selected):02d}"
    if (attempt / "attempt_status.txt").read_text().strip() != "PASS":
        raise ValueError(f"selected attempt is not PASS: {attempt}")
    return attempt


def build_row(meta, attempt):
    stats = parse_stats(attempt / "stats.txt")
    fio = parse_fio(attempt / "fio_raw.json")

    host = int(stats["host_page_writes"])
    gc_writes = int(stats["gc_page_writes"])
    nand = int(stats["nand_page_writes"])
    gc_count = int(stats["gc_count"])
    avg_copy = float(stats["average_gc_copy"]) if stats["average_gc_copy"] != "N/A" else None
    reclaim = PAGES_PER_LINE - avg_copy if avg_copy is not None else None
    invalid_ratio = reclaim / PAGES_PER_LINE if reclaim is not None else None
    current_hot = parse_number(stats.get("current_hot_lines"))
    current_cold = parse_number(stats.get("current_cold_lines"))
    hot_ownership = None
    if current_hot is not None and current_cold is not None and current_hot + current_cold:
        hot_ownership = current_hot / (current_hot + current_cold)

    row = dict(meta)
    row.update(
        {
            "selected_attempt": attempt.name,
            "host_page_writes": host,
            "gc_page_writes": gc_writes,
            "nand_page_writes": nand,
            "waf": float(stats["waf"]),
            "gc_count": gc_count,
            "gc_per_1m_host_pages": gc_count * 1_000_000.0 / host,
            "avg_gc_copy": avg_copy,
            "avg_victim_invalid_ratio": invalid_ratio,
            "reclaim_pages_per_gc": reclaim,
            "iops": fio["fio_iops"],
            "fio_io_bytes": fio["fio_io_bytes"],
            "fio_runtime_ms": fio["fio_runtime_ms"],
            "avg_latency_ns": fio["avg_latency_ns"],
            "p99_clat_ns": fio["p99_clat_ns"],
            "p999_clat_ns": fio["p999_clat_ns"],
            "p9999_clat_ns": fio["p9999_clat_ns"],
            "counter_invariant": stats.get("counter_invariant"),
            "hot_victim_count": parse_number(stats.get("hot_victim_gc_count")),
            "cold_victim_count": parse_number(stats.get("cold_victim_gc_count")),
            "hot_victim_invalid_ratio": parse_number(stats.get("avg_hot_victim_invalid_ratio")),
            "cold_victim_invalid_ratio": parse_number(stats.get("avg_cold_victim_invalid_ratio")),
            "borrowing_count": parse_number(stats.get("borrow_count")),
            "final_hot_lines": current_hot,
            "final_cold_lines": current_cold,
            "final_hot_ownership_ratio": hot_ownership,
            "opposite_normal_gc_count": parse_number(stats.get("opposite_normal_gc_count")),
            "opposite_forced_gc_count": parse_number(stats.get("opposite_forced_gc_count")),
            "global_emergency_fallback_count": parse_number(stats.get("global_emergency_fallback_count")),
            "emergency_gc_count": parse_number(stats.get("emergency_gc_count")),
            "pool_ownership_invariant": stats.get("pool_ownership_invariant"),
        }
    )

    if nand != host + gc_writes:
        raise ValueError(f"nand=host+gc failed for {meta['run_id']}")
    if row["counter_invariant"] != "PASS":
        raise ValueError(f"counter invariant failed for {meta['run_id']}")
    if meta["experiment"] == "B":
        if host != FIXED_HOST_PAGES:
            raise ValueError(f"fixed Host page count mismatch for {meta['run_id']}: {host}")
        if fio["fio_io_bytes"] != 512 * 1024**3:
            raise ValueError(f"fixed fio bytes mismatch for {meta['run_id']}: {fio['fio_io_bytes']}")
    return row


def reference_rows():
    rows = {}
    for name, directory in (("V4 Global", "final_global"), ("V4 ClassGC", "final_classgc")):
        stats = parse_stats(REFERENCE_ROOT / directory / "host_femu.log")
        # parse_stats over the full log would retain the final duplicate keys,
        # because dictionary assignment keeps the last occurrence.
        host = int(stats["host_page_writes"])
        gc_count = int(stats["gc_count"])
        avg_copy = float(stats["average_gc_copy"])
        rows[name] = {
            "host_page_writes": host,
            "waf": float(stats["waf"]),
            "gc_count": gc_count,
            "gc_per_1m_host_pages": gc_count * 1_000_000.0 / host,
            "avg_gc_copy": avg_copy,
            "reclaim_pages_per_gc": PAGES_PER_LINE - avg_copy,
            "avg_victim_invalid_ratio": (PAGES_PER_LINE - avg_copy) / PAGES_PER_LINE,
            "hot_victim_invalid_ratio": parse_number(stats.get("avg_hot_victim_invalid_ratio")),
            "cold_victim_invalid_ratio": parse_number(stats.get("avg_cold_victim_invalid_ratio")),
        }
    return rows


def pct_delta(new, old):
    if new is None or old in (None, 0):
        return None
    return (new / old - 1.0) * 100.0


def fnum(value, digits=3):
    return "N/A" if value is None else f"{value:,.{digits}f}"


def fpct(value, digits=2):
    return "N/A" if value is None else f"{value * 100:.{digits}f}%"


def delta_text(value):
    return "N/A" if value is None else f"{value:+.2f}%"


def row_table(rows):
    lines = [
        "| Run | 구현 | 분포 | Host pages | WAF | GC/1M | Avg copy | Reclaim/GC | Invalid ratio | IOPS | Avg lat ms | P99 ms | P99.9 ms | P99.99 ms |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in rows:
        lines.append(
            "| {run_id} | {implementation} | {distribution} | {host:,} | {waf:.6f} | "
            "{gcpm:.3f} | {copy:.3f} | {reclaim:.3f} | {invalid} | {iops:.1f} | "
            "{lat} | {p99} | {p999} | {p9999} |".format(
                run_id=row["run_id"], implementation=row["implementation"],
                distribution=row["distribution"], host=row["host_page_writes"],
                waf=row["waf"], gcpm=row["gc_per_1m_host_pages"],
                copy=row["avg_gc_copy"], reclaim=row["reclaim_pages_per_gc"],
                invalid=fpct(row["avg_victim_invalid_ratio"], 3), iops=row["iops"],
                lat=fnum(row["avg_latency_ns"] / 1e6 if row["avg_latency_ns"] is not None else None),
                p99=fnum(row["p99_clat_ns"] / 1e6 if row["p99_clat_ns"] is not None else None),
                p999=fnum(row["p999_clat_ns"] / 1e6 if row["p999_clat_ns"] is not None else None),
                p9999=fnum(row["p9999_clat_ns"] / 1e6 if row["p9999_clat_ns"] is not None else None),
            )
        )
    return "\n".join(lines)


def classgc_table(rows):
    selected = [r for r in rows if "ClassGC" in r["implementation"]]
    lines = [
        "| Run | Hot victims | Cold victims | Hot invalid | Cold invalid | Borrow | Final Hot ownership | Opposite normal | Opposite forced | Global fallback | Emergency GC |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in selected:
        lines.append(
            "| {run_id} | {hot} | {cold} | {hir} | {cir} | {borrow} | {own} ({hl}/{total}) | {on} | {of} | {gf} | {eg} |".format(
                run_id=row["run_id"], hot=row["hot_victim_count"], cold=row["cold_victim_count"],
                hir=fpct(row["hot_victim_invalid_ratio"], 3),
                cir=fpct(row["cold_victim_invalid_ratio"], 3),
                borrow=row["borrowing_count"], own=fpct(row["final_hot_ownership_ratio"], 2),
                hl=row["final_hot_lines"],
                total=(row["final_hot_lines"] + row["final_cold_lines"]),
                on=row["opposite_normal_gc_count"], of=row["opposite_forced_gc_count"],
                gf=row["global_emergency_fallback_count"], eg=row["emergency_gc_count"],
            )
        )
    return "\n".join(lines)


def write_summary(root, rows):
    by_id = {row["run_id"]: row for row in rows}
    ref = reference_rows()
    a_global = by_id["A1_zipf099_1800_global"]
    a_class = by_id["A2_zipf099_1800_classgc"]
    b_global = by_id["B1_zipf099_fixed_global"]
    b_class = by_id["B2_zipf099_fixed_classgc"]
    c_noage = by_id["C1_zipf099_3600_classgc_noage"]
    g_noage = by_id["G1_zipf120_1800_classgc_noage"]
    f_class = by_id["F2_zipf120_1800_classgc"]

    rank_1800 = "ClassGC가 Global보다 낮음" if a_class["waf"] < a_global["waf"] else "Global이 ClassGC보다 낮음"
    rank_3600 = "ClassGC가 Global보다 낮음" if ref["V4 ClassGC"]["waf"] < ref["V4 Global"]["waf"] else "Global이 ClassGC보다 낮음"
    trend = "유지" if rank_1800 == rank_3600 else "불일치"

    skew_rows = [
        by_id[run_id]
        for run_id in (
            "D1_uniform_1800_global", "D2_uniform_1800_classgc",
            "E1_zipf070_1800_global", "E2_zipf070_1800_classgc",
            "A1_zipf099_1800_global", "A2_zipf099_1800_classgc",
            "F1_zipf120_1800_global", "F2_zipf120_1800_classgc",
        )
    ]

    lines = [
        "# FEMU V4 Global / ClassGC validation 결과",
        "",
        "모든 값은 seed 20260824의 단일 run 결과다. 차이는 관찰값이며 통계적 우월성이나 SSD 수명 향상을 입증하지 않는다. `Avg victim invalid ratio`는 모든 구현에 동일하게 적용하기 위해 `1 - Avg GC copy / 16384`로 계산했다.",
        "",
        "## 전체 결과",
        "",
        row_table(rows),
        "",
        "모든 run에서 `nand_page_writes = host_page_writes + gc_page_writes`와 `counter_invariant=PASS`를 재검증했다.",
        "",
        "## 1. 1800초와 기존 3600초 경향",
        "",
        f"- 1800초 WAF: Global {a_global['waf']:.6f}, ClassGC {a_class['waf']:.6f} ({rank_1800}).",
        f"- 기존 3600초 WAF: Global {ref['V4 Global']['waf']:.6f}, ClassGC {ref['V4 ClassGC']['waf']:.6f} ({rank_3600}).",
        f"- WAF 순위 경향은 **{trend}**됐다. 다만 runtime 변화와 Host write량 변화가 함께 있으므로 절대값 일치가 아니라 normalized metric의 방향만 비교한다.",
        f"- Global의 1800초/3600초 GC/1M Host pages는 {a_global['gc_per_1m_host_pages']:.3f}/{ref['V4 Global']['gc_per_1m_host_pages']:.3f}, ClassGC는 {a_class['gc_per_1m_host_pages']:.3f}/{ref['V4 ClassGC']['gc_per_1m_host_pages']:.3f}다.",
        "",
        "## 2. Fixed-write Global vs ClassGC",
        "",
        f"두 run 모두 Host page writes가 정확히 {FIXED_HOST_PAGES:,}개(총 512 GiB)다.",
        "",
        f"- WAF: Global {b_global['waf']:.6f}, ClassGC {b_class['waf']:.6f}; ClassGC의 상대 차이 {delta_text(pct_delta(b_class['waf'], b_global['waf']))}.",
        f"- GC/1M Host pages: Global {b_global['gc_per_1m_host_pages']:.3f}, ClassGC {b_class['gc_per_1m_host_pages']:.3f}; 상대 차이 {delta_text(pct_delta(b_class['gc_per_1m_host_pages'], b_global['gc_per_1m_host_pages']))}.",
        f"- IOPS: Global {b_global['iops']:.1f}, ClassGC {b_class['iops']:.1f}; 상대 차이 {delta_text(pct_delta(b_class['iops'], b_global['iops']))}.",
        "",
        "## 3. Cold victim age 제거 효과",
        "",
        "Zipf 0.99의 without-age C는 같은 1800초 조건인 A2 Full ClassGC와 비교한다. no-age commit은 Cold score 식만 변경한다.",
        "",
        f"- WAF: Full {a_class['waf']:.6f} -> no-age {c_noage['waf']:.6f} ({delta_text(pct_delta(c_noage['waf'], a_class['waf']))}).",
        f"- Avg victim invalid ratio: Full {fpct(a_class['avg_victim_invalid_ratio'], 3)} -> no-age {fpct(c_noage['avg_victim_invalid_ratio'], 3)}.",
        f"- Reclaim pages/GC: Full {a_class['reclaim_pages_per_gc']:.3f} -> no-age {c_noage['reclaim_pages_per_gc']:.3f}.",
        f"- GC/1M Host pages: Full {a_class['gc_per_1m_host_pages']:.3f} -> no-age {c_noage['gc_per_1m_host_pages']:.3f}.",
        f"- Zipf 1.2에서는 Full ClassGC WAF {f_class['waf']:.6f}와 no-age {g_noage['waf']:.6f}를 같은 1800초 조건으로 비교한다 ({delta_text(pct_delta(g_noage['waf'], f_class['waf']))}).",
        "",
        "## 4. Skew 변화",
        "",
        row_table(skew_rows),
        "",
        "Uniform -> Zipf 0.7 -> 0.99 -> 1.2 순서의 변화는 각 구현 내부에서 비교한다. Global과 ClassGC 사이의 단일-run 차이와 skew에 따른 변화가 섞이지 않도록 WAF, GC/1M, reclaim/GC를 함께 본다.",
        "",
        "## 5. Victim invalid ratio에서 WAF까지",
        "",
        "한 GC가 회수하는 page 수는 `16384 - Avg GC copy`이고, 같은 Host write량에서 reclaim/GC가 클수록 일반적으로 필요한 GC/1M Host writes가 줄어드는 방향을 기대할 수 있다. GC 횟수와 copy량이 함께 NAND writes를 결정하므로 최종 WAF는 이 두 항의 결합 결과다.",
        "",
        "이 표의 관계는 관찰된 연쇄를 정리한 것이며, 단일 run만으로 인과성을 확정하지 않는다.",
        "",
        "| Run | Invalid ratio | Reclaim/GC | GC/1M Host pages | WAF |",
        "|---|---:|---:|---:|---:|",
    ]
    for row in rows:
        lines.append(
            f"| {row['run_id']} | {fpct(row['avg_victim_invalid_ratio'], 3)} | "
            f"{row['reclaim_pages_per_gc']:.3f} | {row['gc_per_1m_host_pages']:.3f} | {row['waf']:.6f} |"
        )
    lines.extend(
        [
            "",
            "## ClassGC 상세 counter",
            "",
            classgc_table(rows),
            "",
            "## 재현성 및 한계",
            "",
            "- Source commit, exact fio job, precondition/reset command, build log, Host FEMU log, fio JSON, liveness log는 각 selected attempt에 보존했다.",
            "- 비교는 seed 하나의 run 하나씩이며 반복실험의 분산이나 신뢰구간이 없다.",
            "- lower WAF는 이 emulator workload에서 NAND page write amplification이 낮았다는 뜻이며 실제 SSD 수명 향상을 직접 입증하지 않는다.",
            "- Hot ownership 변화와 borrowing/fallback counter는 정책 동작의 관찰값이지 20:80 pool의 최적성을 입증하지 않는다.",
            "",
        ]
    )
    (root / "SUMMARY.md").write_text("\n".join(lines), encoding="utf-8")


def main():
    root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parent
    manifest_path = root / "campaign_manifest.csv"
    with manifest_path.open(newline="", encoding="utf-8") as handle:
        manifest = list(csv.DictReader(handle))
    if len(manifest) != 12:
        raise ValueError(f"expected 12 runs, found {len(manifest)}")

    rows = []
    for meta in manifest:
        attempt = selected_attempt(root, meta["run_id"])
        rows.append(build_row(meta, attempt))

    fieldnames = list(rows[0].keys())
    with (root / "summary.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)
    write_summary(root, rows)


if __name__ == "__main__":
    main()
