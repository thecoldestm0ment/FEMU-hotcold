#!/usr/bin/env python3
"""Rebuild report tables from archived FEMU counters and fio JSON; never run fio."""
import csv
import hashlib
import json
import math
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
MATRIX = ROOT / 'v4_validation_matrix_20260902_144410_KST'
THRESHOLD = ROOT / 'v4_classgc_threshold_off_20260903_001558_KST'
BASELINE = ROOT / 'baseline_separation_skew_20260903_174617_KST'
OLD = ROOT / 'v4_global_vs_classgc_20260901_181151_KST'
E = 16384
ROWS = []
AUDIT = []


def read_csv(path):
    with path.open(newline='') as f:
        return list(csv.DictReader(f))


def kv(text):
    return dict(re.findall(r'([A-Za-z_][A-Za-z_0-9]*)=([^\s]+)', text))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git_blob(commit, name):
    return subprocess.check_output(['git', '-C', str(REPO), 'show', f'{commit}:hw/femu/bbssd/{name}'])


def add_run(meta, campaign, old=False):
    code = meta['run_id'].split('_')[0]
    if old:
        code = meta['run_id']
        attempt = campaign
    else:
        attempt = campaign / 'runs' / meta['run_id'] / meta['selected_attempt']
    stats_file = attempt / 'stats.txt'
    if not stats_file.exists():
        stats_file = attempt / 'host_femu.log'
    lines = [s for s in stats_file.read_text(errors='replace').splitlines() if 'BBSSD-STATS ' in s]
    assert lines, (code, 'missing counters')
    s = kv(lines[-1])
    r = {**meta, 'code': code, 'raw_stats_path': str(stats_file.relative_to(ROOT)),
         'stats_sha256': sha(stats_file)}
    r.update(s)
    h, g, n, c = (int(s[k]) for k in ('host_page_writes', 'gc_page_writes', 'nand_page_writes', 'gc_count'))
    assert h + g == n and h > 0 and c > 0, code
    assert int(s['block_erases']) == c * 64, code
    assert s['counter_invariant'] == 'PASS', code
    assert abs(float(s['waf']) - n / h) < 0.00000051, code
    assert abs(float(s['average_gc_copy']) - g / c) < 0.00000051, code
    r.update(host_page_writes=h, gc_page_writes=g, nand_page_writes=n, gc_count=c,
             waf=n/h, avg_gc_copy=g/c, victim_invalid_ratio=1-g/c/E,
             reclaim_pages_per_gc=E-g/c, gc_per_1m_host_pages=c*1e6/h,
             gc_pages_per_host_page=g/h, host_gib=h/262144,
             free_page_balance_delta=E*c-n, raw_counter_check='PASS')
    if 'host_hot_writes' in s:
        assert int(s['host_hot_writes']) + int(s['host_cold_writes']) == h, code
        assert int(s['gc_hot_writes']) + int(s['gc_cold_writes']) == g, code
        assert int(s['host_write_seq']) == h, code
        r['host_hot_percent'] = 100 * int(s['host_hot_writes']) / h
        assert [int(s[x]) for x in ('hot_pool_percent','frequency_window_writes','hot_writes_per_window','cold_writes_per_window','erase_event_threshold')] == [20,4096,4,1,1], code
    if 'hot_victim_gc_count' in s:
        a, b = int(s['hot_victim_gc_count']), int(s['cold_victim_gc_count'])
        x, y = int(s['hot_victim_gc_page_copies']), int(s['cold_victim_gc_page_copies'])
        assert a+b == c and x+y == g, code
        assert int(s['current_hot_lines']) + int(s['current_cold_lines']) == 128, code
        assert s['pool_ownership_invariant'] == 'PASS', code
        r.update(hot_victim_percent=100*a/c, cold_victim_percent=100*b/c,
                 hot_invalid_percent=100*(1-x/a/E), cold_invalid_percent=100*(1-y/b/E),
                 emergency_gc_per_1m=int(s['emergency_gc_count'])*1e6/h)
        assert abs(float(s['avg_hot_victim_invalid_ratio']) - (1-x/a/E)) < 0.00000051, code
        assert abs(float(s['avg_cold_victim_invalid_ratio']) - (1-y/b/E)) < 0.00000051, code
    manifest = attempt / 'manifest.txt'
    if manifest.exists():
        m = kv(manifest.read_text())
        assert m.get('commit') == meta['commit'], (code, m.get('commit'), meta['commit'])
        r['manifest_check'] = 'PASS'
    else:
        r['manifest_check'] = 'MISSING; commit from archived summary'
    hash_file = attempt / 'source_hashes.txt'
    if hash_file.exists():
        for line in hash_file.read_text().splitlines():
            digest, file = line.split(maxsplit=1)
            name = Path(file).name
            if name in ('ftl.c', 'ftl.h', 'bb.c'):
                assert hashlib.sha256(git_blob(meta['commit'], name)).hexdigest() == digest, (code, name)
        r['source_hash_check'] = 'PASS'
    else:
        r['source_hash_check'] = 'MISSING'
    fio = attempt / 'fio_raw.json'
    if fio.exists():
        j = json.loads(fio.read_text())
        assert len(j['jobs']) == 1 and j['jobs'][0]['error'] == 0, code
        w = j['jobs'][0]['write']
        assert w['io_bytes'] == h*4096 and w['total_ios']*4 == h, code
        o = j['global options']
        expected = dict(rw='randwrite', bs='16k', size='5G', iodepth='128', numjobs='32', direct='1', ioengine='libaio', randseed='20260824', randrepeat='1', group_reporting='1', percentile_list='99:99.9:99.99')
        for key, value in expected.items():
            assert o[key] == value, (code, key, o.get(key))
        if code in ('B1', 'B2'):
            assert o['io_size'] == '16G' and 'time_based' not in o and h*4096 == 512*1024**3, code
        else:
            assert o['runtime'] == '1800' and o['time_based'] == '1', code
            assert 1800000 <= w['runtime'] < 1810000, code
        assert o.get('random_distribution','uniform') == meta['distribution'], code
        jobfile = attempt / 'workload.fio'
        assert jobfile.exists(), code
        config = kv(jobfile.read_text())
        for key,value in config.items():
            assert o[key] == value, (code, 'fio file/JSON mismatch', key)
        r.update(fio_source='RAW_JSON', fio_version=j['fio version'], fio_bytes_check='PASS',
                 fio_runtime_ms=w['runtime'], fio_io_bytes=w['io_bytes'], iops=w['iops'],
                 avg_latency_ns=w['lat_ns']['mean'], p99_clat_ns=w['clat_ns']['percentile']['99.000000'],
                 p999_clat_ns=w['clat_ns']['percentile']['99.900000'], p9999_clat_ns=w['clat_ns']['percentile']['99.990000'],
                 raw_fio_path=str(fio.relative_to(ROOT)), fio_sha256=sha(fio), fio_settings_sha256=sha(jobfile))
    elif code == 'T1':
        r.update(fio_source='ARCHIVED_SUMMARY_ONLY', fio_bytes_check='NOT_REVERIFIED', raw_fio_path='', fio_sha256='', fio_settings_sha256='')
        for k in ('fio_runtime_ms','fio_io_bytes','iops','avg_latency_ns','p99_clat_ns','p999_clat_ns','p9999_clat_ns'):
            r[k] = float(meta[k])
        assert r['fio_io_bytes'] == h*4096
    elif old:
        r.update(fio_source='LEGACY_TEXT', fio_bytes_check='NOT_JSON_VERIFIED')
    else:
        raise AssertionError((code, 'missing fio JSON'))
    AUDIT.append({k:r.get(k,'') for k in ('code','raw_stats_path','stats_sha256','raw_fio_path','fio_sha256','fio_settings_sha256','raw_counter_check','fio_bytes_check','fio_source','manifest_check','source_hash_check','commit')})
    return r


for m in read_csv(MATRIX / 'summary.csv'):
    ROWS.append(add_run(m, THRESHOLD if m['run_id'].startswith('T') else MATRIX))
for m in read_csv(BASELINE / 'campaign_manifest.csv'):
    m['selected_attempt'] = (BASELINE / 'runs' / m['run_id'] / 'selected_attempt.txt').read_text().strip()
    ROWS.append(add_run(m, BASELINE))
assert len(ROWS) == 16
by = {r['code']:r for r in ROWS}
for a,b in [('H1','D1'),('H2','F1')]:
    assert by[a]['fio_settings_sha256'] == by[b]['fio_settings_sha256'], (a,b)
LEGACY = []
for code,folder,commit,impl in [('L1','final_global','032d29b83e3906593ef9f79e5d2739bdab27ef5e','V4 Global'),('L2','final_classgc','c277fbcc99c016db265674cac7ce81c98addbdfc','V4 ClassGC')]:
    LEGACY.append(add_run(dict(run_id=code, commit=commit, implementation=impl, distribution='zipf:0.99', runtime_seconds=3600), OLD/folder, old=True))
by.update({r['code']:r for r in LEGACY})


def out_csv(name, rows):
    fields = list(dict.fromkeys(k for r in rows for k in r))
    with (ROOT/name).open('w', newline='') as f:
        w = csv.DictWriter(f, fields, lineterminator='\n')
        w.writeheader()
        w.writerows(rows)


out_csv('WAF_ALL_RUNS_20260908.csv', ROWS)
out_csv('WAF_LEGACY_3600_20260908.csv', LEGACY)
out_csv('WAF_EVIDENCE_AUDIT_20260908.csv', AUDIT)


def fmt(x, digits=3):
    if x is None or x == '':
        return '—'
    return f'{float(x):,.{digits}f}'


def table(headers, rows):
    return '\n'.join(['| ' + ' | '.join(headers) + ' |', '| ' + ' | '.join(['---']*len(headers)) + ' |'] + ['| ' + ' | '.join(str(x) for x in row) + ' |' for row in rows])


def eff_table(codes):
    return table(['Run','WAF','Victim invalid (%)','Reclaim/GC (pages)','GC/1M Host pages','Avg copy (pages)'], [[c,fmt(by[c]['waf'],6),fmt(100*by[c]['victim_invalid_ratio']),fmt(by[c]['reclaim_pages_per_gc']),fmt(by[c]['gc_per_1m_host_pages']),fmt(by[c]['avg_gc_copy'])] for c in codes])


T = {}
T['all_efficiency'] = eff_table(list(r['code'] for r in ROWS))
T['separation'] = eff_table(['H1','D1','H2','F1'])
T['threshold'] = eff_table(['D1','D2','T1','F1','F2','T2'])
T['age'] = eff_table(['A2','C1','F2','G1'])
T['runtime'] = eff_table(['A1','L1','A2','L2'])
T['skew'] = table(['분포','Global WAF','ClassGC WAF','Global Hot writes (%)','ClassGC Hot writes (%)','ClassGC Hot victims (%)','ClassGC Cold invalid (%)'], [[dist,fmt(by[a]['waf'],6),fmt(by[b]['waf'],6),fmt(by[a]['host_hot_percent']),fmt(by[b]['host_hot_percent']),fmt(by[b]['hot_victim_percent']),fmt(by[b]['cold_invalid_percent'])] for dist,a,b in [('Uniform','D1','D2'),('Zipf 0.7','E1','E2'),('Zipf 0.99','A1','A2'),('Zipf 1.2','F1','F2')]])
T['writes'] = table(['Run','Host pages','GC pages','NAND pages','GC count','Host GiB'], [[r['code'],fmt(r['host_page_writes'],0),fmt(r['gc_page_writes'],0),fmt(r['nand_page_writes'],0),fmt(r['gc_count'],0),fmt(r['host_gib'])] for r in ROWS])
T['performance'] = table(['Run','IOPS','Avg lat (ms)','P99 clat (ms)','P99.9 clat (ms)','P99.99 clat (ms)','fio 근거'], [[r['code'],fmt(r['iops'],1),fmt(float(r['avg_latency_ns'])/1e6),fmt(float(r['p99_clat_ns'])/1e6),fmt(float(r['p999_clat_ns'])/1e6),fmt(float(r['p9999_clat_ns'])/1e6),'기존 요약†' if r['code']=='T1' else 'JSON'] for r in ROWS])
T['class_mix'] = table(['Run','Hot victims','Cold victims','Hot 비중 (%)','Hot invalid (%)','Cold invalid (%)','Hot copy','Cold copy'], [[r['code'],r['hot_victim_gc_count'],r['cold_victim_gc_count'],fmt(r['hot_victim_percent']),fmt(r['hot_invalid_percent']),fmt(r['cold_invalid_percent']),fmt(r['avg_hot_gc_copy']),fmt(r['avg_cold_gc_copy'])] for r in ROWS if 'hot_victim_gc_count' in r])
T['resources'] = table(['Run','Borrow','Final Hot/Cold','Opposite normal','Opposite forced','Global fallback','Emergency','Emergency/1M'], [[r['code'],r['borrow_count'],r['current_hot_lines']+'/'+r['current_cold_lines'],r['opposite_normal_gc_count'],r['opposite_forced_gc_count'],r['global_emergency_fallback_count'],r['emergency_gc_count'],fmt(r['emergency_gc_per_1m'])] for r in ROWS if 'hot_victim_gc_count' in r])
T['fixed'] = table(['지표','B1 Global','B2 ClassGC','ClassGC 상대 변화 (%)'], [[label,fmt(by['B1'][k],digits),fmt(by['B2'][k],digits),fmt(100*(by['B2'][k]/by['B1'][k]-1),2)] for label,k,digits in [('Host pages','host_page_writes',0),('GC pages','gc_page_writes',0),('NAND pages','nand_page_writes',0),('WAF','waf',6),('GC count','gc_count',0),('GC/1M','gc_per_1m_host_pages',3),('Avg copy','avg_gc_copy',3),('Reclaim/GC','reclaim_pages_per_gc',3),('IOPS','iops',1),('Runtime (ms)','fio_runtime_ms',0)]])
T['audit'] = table(['Run','FEMU counter','fio','Manifest','Source hash'], [[a['code'],a['raw_counter_check'],a['fio_source'],a['manifest_check'],a['source_hash_check']] for a in AUDIT])
T['sources'] = table(['Run','분포/방식','구현','원본 counter','fio JSON'], [[r['code'],r['distribution']+(' / 512GiB' if r['code'].startswith('B') else ' / 1800s'),r['implementation'],f"[원본]({r['raw_stats_path']})",f"[JSON]({r['raw_fio_path']})" if r.get('raw_fio_path') else '없음†'] for r in ROWS])

comparisons=[]
for name,a,b in [('uniform_separation','H1','D1'),('zipf120_separation','H2','F1'),('uniform_classgc','D1','D2'),('zipf120_classgc','F1','F2'),('uniform_threshold_relaxation','D2','T1'),('zipf120_threshold_relaxation','F2','T2'),('zipf099_noage','A2','C1'),('zipf120_noage','F2','G1'),('fixed_write','B1','B2')]:
    cr=dict(comparison=name,reference=a,variant=b)
    for k in ('waf','gc_per_1m_host_pages','avg_gc_copy','reclaim_pages_per_gc','host_page_writes','iops'):
        cr[k+'_delta']=float(by[b][k])-float(by[a][k])
        cr[k+'_change_percent']=100*(float(by[b][k])/float(by[a][k])-1)
    cr['invalid_delta_pp']=100*(by[b]['victim_invalid_ratio']-by[a]['victim_invalid_ratio'])
    comparisons.append(cr)
out_csv('WAF_COMPARISONS_20260908.csv',comparisons)

(ROOT/'WAF_REPORT_TABLES_20260908.md').write_text('# 보고서 표: 원본 counter에서 재계산\n\n' + '\n\n'.join('## '+k+'\n\n'+v for k,v in T.items())+'\n')
template=ROOT/'WAF_REPORT_20260908.template.md'
if template.exists():
    report=template.read_text()
    for k,v in T.items():
        report=report.replace('{{'+k+'}}',v)
    assert not re.search(r'\{\{[a-z_]+\}\}',report)
    (ROOT/'WAF_ANALYSIS_REPORT_20260908.md').write_text(report)
print('PASS: 16 measured counter sets + 2 legacy references; 15 measured fio JSONs; T1 fio missing and explicitly labeled.')
print('PASS: source hashes where available, byte-identical H1/D1 and H2/F1 jobs, exact 512 GiB B1/B2 writes.')
print('Tables and CSVs written under', ROOT)
