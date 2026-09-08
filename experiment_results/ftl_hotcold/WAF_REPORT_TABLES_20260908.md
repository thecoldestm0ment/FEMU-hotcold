# 보고서 표: 원본 counter에서 재계산

## all_efficiency

| Run | WAF | Victim invalid (%) | Reclaim/GC (pages) | GC/1M Host pages | Avg copy (pages) |
| --- | --- | --- | --- | --- | --- |
| A1 | 6.961641 | 14.340 | 2,349.406 | 424.782 | 14,034.594 |
| A2 | 3.040172 | 32.828 | 5,378.611 | 185.379 | 11,005.389 |
| B1 | 7.133709 | 14.009 | 2,295.171 | 435.360 | 14,088.829 |
| B2 | 3.058862 | 32.640 | 5,347.783 | 186.555 | 11,036.217 |
| C1 | 3.061347 | 32.622 | 5,344.720 | 186.728 | 11,039.280 |
| D1 | 7.967380 | 12.534 | 2,053.554 | 486.194 | 14,330.446 |
| D2 | 3.322130 | 30.063 | 4,925.444 | 202.655 | 11,458.556 |
| E1 | 7.936123 | 12.581 | 2,061.347 | 484.276 | 14,322.653 |
| E2 | 3.312984 | 30.147 | 4,939.359 | 202.102 | 11,444.641 |
| F1 | 5.400359 | 18.495 | 3,030.300 | 329.524 | 13,353.700 |
| F2 | 2.512177 | 39.763 | 6,514.832 | 153.222 | 9,869.168 |
| G1 | 2.516456 | 39.693 | 6,503.314 | 153.477 | 9,880.686 |
| T1 | 7.967045 | 12.533 | 2,053.382 | 486.165 | 14,330.618 |
| T2 | 5.345066 | 18.689 | 3,061.966 | 326.156 | 13,322.034 |
| H1 | 7.987633 | 12.503 | 2,048.525 | 487.436 | 14,335.475 |
| H2 | 7.648358 | 13.051 | 2,138.312 | 466.693 | 14,245.688 |

## separation

| Run | WAF | Victim invalid (%) | Reclaim/GC (pages) | GC/1M Host pages | Avg copy (pages) |
| --- | --- | --- | --- | --- | --- |
| H1 | 7.987633 | 12.503 | 2,048.525 | 487.436 | 14,335.475 |
| D1 | 7.967380 | 12.534 | 2,053.554 | 486.194 | 14,330.446 |
| H2 | 7.648358 | 13.051 | 2,138.312 | 466.693 | 14,245.688 |
| F1 | 5.400359 | 18.495 | 3,030.300 | 329.524 | 13,353.700 |

## threshold

| Run | WAF | Victim invalid (%) | Reclaim/GC (pages) | GC/1M Host pages | Avg copy (pages) |
| --- | --- | --- | --- | --- | --- |
| D1 | 7.967380 | 12.534 | 2,053.554 | 486.194 | 14,330.446 |
| D2 | 3.322130 | 30.063 | 4,925.444 | 202.655 | 11,458.556 |
| T1 | 7.967045 | 12.533 | 2,053.382 | 486.165 | 14,330.618 |
| F1 | 5.400359 | 18.495 | 3,030.300 | 329.524 | 13,353.700 |
| F2 | 2.512177 | 39.763 | 6,514.832 | 153.222 | 9,869.168 |
| T2 | 5.345066 | 18.689 | 3,061.966 | 326.156 | 13,322.034 |

## age

| Run | WAF | Victim invalid (%) | Reclaim/GC (pages) | GC/1M Host pages | Avg copy (pages) |
| --- | --- | --- | --- | --- | --- |
| A2 | 3.040172 | 32.828 | 5,378.611 | 185.379 | 11,005.389 |
| C1 | 3.061347 | 32.622 | 5,344.720 | 186.728 | 11,039.280 |
| F2 | 2.512177 | 39.763 | 6,514.832 | 153.222 | 9,869.168 |
| G1 | 2.516456 | 39.693 | 6,503.314 | 153.477 | 9,880.686 |

## runtime

| Run | WAF | Victim invalid (%) | Reclaim/GC (pages) | GC/1M Host pages | Avg copy (pages) |
| --- | --- | --- | --- | --- | --- |
| A1 | 6.961641 | 14.340 | 2,349.406 | 424.782 | 14,034.594 |
| L1 | 7.147977 | 13.980 | 2,290.476 | 436.227 | 14,093.524 |
| A2 | 3.040172 | 32.828 | 5,378.611 | 185.379 | 11,005.389 |
| L2 | 3.060527 | 32.653 | 5,349.911 | 186.742 | 11,034.089 |

## skew

| 분포 | Global WAF | ClassGC WAF | Global Hot writes (%) | ClassGC Hot writes (%) | ClassGC Hot victims (%) | ClassGC Cold invalid (%) |
| --- | --- | --- | --- | --- | --- | --- |
| Uniform | 7.967380 | 3.322130 | 0.301 | 0.299 | 0.115 | 30.010 |
| Zipf 0.7 | 7.936123 | 3.312984 | 0.758 | 0.766 | 0.306 | 30.010 |
| Zipf 0.99 | 6.961641 | 3.040172 | 15.445 | 14.355 | 6.346 | 30.009 |
| Zipf 1.2 | 5.400359 | 2.512177 | 37.892 | 38.690 | 18.848 | 30.010 |

## writes

| Run | Host pages | GC pages | NAND pages | GC count | Host GiB |
| --- | --- | --- | --- | --- | --- |
| A1 | 56,301,836 | 335,651,350 | 391,953,186 | 23,916 | 214.774 |
| A2 | 109,656,200 | 223,717,548 | 333,373,748 | 20,328 | 418.305 |
| B1 | 134,217,728 | 823,252,518 | 957,470,246 | 58,433 | 512.000 |
| B2 | 134,217,728 | 276,335,831 | 410,553,559 | 25,039 | 512.000 |
| C1 | 159,814,972 | 329,434,190 | 489,249,162 | 29,842 | 609.646 |
| D1 | 59,441,268 | 414,149,890 | 473,591,158 | 28,900 | 226.750 |
| D2 | 141,644,864 | 328,917,854 | 470,562,718 | 28,705 | 540.332 |
| E1 | 59,781,972 | 414,655,135 | 474,437,107 | 28,951 | 228.050 |
| E2 | 147,148,488 | 340,352,166 | 487,500,654 | 29,739 | 561.327 |
| F1 | 88,600,636 | 389,874,613 | 478,475,249 | 29,196 | 337.985 |
| F2 | 195,989,740 | 296,371,108 | 492,360,848 | 30,030 | 747.642 |
| G1 | 196,205,604 | 297,537,107 | 493,742,711 | 30,113 | 748.465 |
| T1 | 57,276,852 | 399,050,391 | 456,327,243 | 27,846 | 218.494 |
| T2 | 88,810,168 | 385,886,031 | 474,696,199 | 28,966 | 338.784 |
| H1 | 59,029,232 | 412,474,610 | 471,503,842 | 28,773 | 225.179 |
| H2 | 62,087,968 | 412,783,050 | 474,871,018 | 28,976 | 236.847 |

## performance

| Run | IOPS | Avg lat (ms) | P99 clat (ms) | P99.9 clat (ms) | P99.99 clat (ms) | fio 근거 |
| --- | --- | --- | --- | --- | --- | --- |
| A1 | 7,815.6 | 524.040 | 2,332.033 | 5,804.917 | 12,280.922 | JSON |
| A2 | 15,228.0 | 268.929 | 2,332.033 | 8,153.727 | 17,112.760 | JSON |
| B1 | 9,246.7 | 442.838 | 834.666 | 1,166.017 | 2,021.655 | JSON |
| B2 | 22,250.6 | 183.985 | 400.556 | 530.579 | 1,266.680 | JSON |
| C1 | 22,192.3 | 184.560 | 400.556 | 675.283 | 1,249.903 | JSON |
| D1 | 8,253.1 | 496.276 | 918.553 | 1,736.442 | 3,003.122 | JSON |
| D2 | 19,670.4 | 208.220 | 530.579 | 1,468.006 | 2,264.924 | JSON |
| E1 | 8,300.1 | 493.453 | 1,266.680 | 1,686.110 | 3,103.785 | JSON |
| E2 | 20,434.6 | 200.437 | 425.722 | 675.283 | 1,484.784 | JSON |
| F1 | 12,303.3 | 332.896 | 658.506 | 876.610 | 2,164.261 | JSON |
| F2 | 27,217.2 | 150.482 | 341.836 | 599.785 | 1,333.789 | JSON |
| G1 | 27,248.6 | 150.306 | 333.447 | 438.305 | 1,233.125 | JSON |
| T1 | 7,952.6 | 514.975 | 1,216.348 | 2,499.805 | 3,841.982 | 기존 요약† |
| T2 | 12,331.8 | 332.079 | 666.894 | 1,010.827 | 1,669.333 | JSON |
| H1 | 8,196.0 | 499.716 | 994.050 | 1,702.887 | 2,634.023 | JSON |
| H2 | 8,621.7 | 475.049 | 801.112 | 985.661 | 1,233.125 | JSON |

## class_mix

| Run | Hot victims | Cold victims | Hot 비중 (%) | Hot invalid (%) | Cold invalid (%) | Hot copy | Cold copy |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A2 | 1290 | 19038 | 6.346 | 74.445 | 30.009 | 4,186.893 | 11,467.405 |
| B2 | 1410 | 23629 | 5.631 | 76.743 | 30.009 | 3,810.468 | 11,467.395 |
| C1 | 1661 | 28181 | 5.566 | 76.958 | 30.008 | 3,775.217 | 11,467.427 |
| D2 | 33 | 28672 | 0.115 | 75.432 | 30.010 | 4,025.212 | 11,467.112 |
| E2 | 91 | 29648 | 0.306 | 74.940 | 30.010 | 4,105.802 | 11,467.166 |
| F2 | 5660 | 24370 | 18.848 | 81.759 | 30.010 | 2,988.528 | 11,467.215 |
| G1 | 5576 | 24537 | 18.517 | 82.320 | 30.006 | 2,896.764 | 11,467.773 |
| T1 | 13 | 27833 | 0.047 | 75.265 | 12.504 | 4,052.615 | 14,335.419 |
| T2 | 2570 | 26396 | 8.872 | 81.898 | 12.535 | 2,965.864 | 14,330.344 |

## resources

| Run | Borrow | Final Hot/Cold | Opposite normal | Opposite forced | Global fallback | Emergency | Emergency/1M |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A2 | 18 | 7/121 | 1290 | 0 | 0 | 18 | 0.164 |
| B2 | 18 | 7/121 | 1410 | 0 | 0 | 19 | 0.142 |
| C1 | 18 | 7/121 | 1661 | 0 | 0 | 35 | 0.219 |
| D2 | 18 | 7/121 | 33 | 0 | 0 | 13 | 0.092 |
| E2 | 18 | 7/121 | 91 | 0 | 0 | 13 | 0.088 |
| F2 | 18 | 7/121 | 5660 | 0 | 0 | 35 | 0.179 |
| G1 | 18 | 7/121 | 5576 | 0 | 0 | 94 | 0.479 |
| T1 | 4 | 21/107 | 13 | 0 | 0 | 0 | 0.000 |
| T2 | 4 | 21/107 | 2570 | 0 | 0 | 0 | 0.000 |

## fixed

| 지표 | B1 Global | B2 ClassGC | ClassGC 상대 변화 (%) |
| --- | --- | --- | --- |
| Host pages | 134,217,728 | 134,217,728 | 0.00 |
| GC pages | 823,252,518 | 276,335,831 | -66.43 |
| NAND pages | 957,470,246 | 410,553,559 | -57.12 |
| WAF | 7.133709 | 3.058862 | -57.12 |
| GC count | 58,433 | 25,039 | -57.15 |
| GC/1M | 435.360 | 186.555 | -57.15 |
| Avg copy | 14,088.829 | 11,036.217 | -21.67 |
| Reclaim/GC | 2,295.171 | 5,347.783 | 133.00 |
| IOPS | 9,246.7 | 22,250.6 | 140.63 |
| Runtime (ms) | 3,628,802 | 1,508,021 | -58.44 |

## audit

| Run | FEMU counter | fio | Manifest | Source hash |
| --- | --- | --- | --- | --- |
| A1 | PASS | RAW_JSON | PASS | PASS |
| A2 | PASS | RAW_JSON | PASS | PASS |
| B1 | PASS | RAW_JSON | PASS | PASS |
| B2 | PASS | RAW_JSON | PASS | PASS |
| C1 | PASS | RAW_JSON | PASS | PASS |
| D1 | PASS | RAW_JSON | PASS | PASS |
| D2 | PASS | RAW_JSON | PASS | PASS |
| E1 | PASS | RAW_JSON | PASS | PASS |
| E2 | PASS | RAW_JSON | PASS | PASS |
| F1 | PASS | RAW_JSON | PASS | PASS |
| F2 | PASS | RAW_JSON | PASS | PASS |
| G1 | PASS | RAW_JSON | PASS | PASS |
| T1 | PASS | ARCHIVED_SUMMARY_ONLY | MISSING; commit from archived summary | MISSING |
| T2 | PASS | RAW_JSON | PASS | PASS |
| H1 | PASS | RAW_JSON | PASS | PASS |
| H2 | PASS | RAW_JSON | PASS | PASS |
| L1 | PASS | LEGACY_TEXT | PASS | MISSING |
| L2 | PASS | LEGACY_TEXT | PASS | MISSING |

## sources

| Run | 분포/방식 | 구현 | 원본 counter | fio JSON |
| --- | --- | --- | --- | --- |
| A1 | zipf:0.99 / 1800s | V4 Global | [원본](v4_validation_matrix_20260902_144410_KST/runs/A1_zipf099_1800_global/attempt_04/stats.txt) | [JSON](v4_validation_matrix_20260902_144410_KST/runs/A1_zipf099_1800_global/attempt_04/fio_raw.json) |
| A2 | zipf:0.99 / 1800s | V4 ClassGC | [원본](v4_validation_matrix_20260902_144410_KST/runs/A2_zipf099_1800_classgc/attempt_01/stats.txt) | [JSON](v4_validation_matrix_20260902_144410_KST/runs/A2_zipf099_1800_classgc/attempt_01/fio_raw.json) |
| B1 | zipf:0.99 / 512GiB | V4 Global | [원본](v4_validation_matrix_20260902_144410_KST/runs/B1_zipf099_fixed_global/attempt_01/stats.txt) | [JSON](v4_validation_matrix_20260902_144410_KST/runs/B1_zipf099_fixed_global/attempt_01/fio_raw.json) |
| B2 | zipf:0.99 / 512GiB | V4 ClassGC | [원본](v4_validation_matrix_20260902_144410_KST/runs/B2_zipf099_fixed_classgc/attempt_01/stats.txt) | [JSON](v4_validation_matrix_20260902_144410_KST/runs/B2_zipf099_fixed_classgc/attempt_01/fio_raw.json) |
| C1 | zipf:0.99 / 1800s | V4 ClassGC without age | [원본](v4_validation_matrix_20260902_144410_KST/runs/C1_zipf099_3600_classgc_noage/attempt_01/stats.txt) | [JSON](v4_validation_matrix_20260902_144410_KST/runs/C1_zipf099_3600_classgc_noage/attempt_01/fio_raw.json) |
| D1 | uniform / 1800s | V4 Global | [원본](v4_validation_matrix_20260902_144410_KST/runs/D1_uniform_1800_global/attempt_01/stats.txt) | [JSON](v4_validation_matrix_20260902_144410_KST/runs/D1_uniform_1800_global/attempt_01/fio_raw.json) |
| D2 | uniform / 1800s | V4 ClassGC | [원본](v4_validation_matrix_20260902_144410_KST/runs/D2_uniform_1800_classgc/attempt_01/stats.txt) | [JSON](v4_validation_matrix_20260902_144410_KST/runs/D2_uniform_1800_classgc/attempt_01/fio_raw.json) |
| E1 | zipf:0.7 / 1800s | V4 Global | [원본](v4_validation_matrix_20260902_144410_KST/runs/E1_zipf070_1800_global/attempt_01/stats.txt) | [JSON](v4_validation_matrix_20260902_144410_KST/runs/E1_zipf070_1800_global/attempt_01/fio_raw.json) |
| E2 | zipf:0.7 / 1800s | V4 ClassGC | [원본](v4_validation_matrix_20260902_144410_KST/runs/E2_zipf070_1800_classgc/attempt_01/stats.txt) | [JSON](v4_validation_matrix_20260902_144410_KST/runs/E2_zipf070_1800_classgc/attempt_01/fio_raw.json) |
| F1 | zipf:1.2 / 1800s | V4 Global | [원본](v4_validation_matrix_20260902_144410_KST/runs/F1_zipf120_1800_global/attempt_01/stats.txt) | [JSON](v4_validation_matrix_20260902_144410_KST/runs/F1_zipf120_1800_global/attempt_01/fio_raw.json) |
| F2 | zipf:1.2 / 1800s | V4 ClassGC | [원본](v4_validation_matrix_20260902_144410_KST/runs/F2_zipf120_1800_classgc/attempt_01/stats.txt) | [JSON](v4_validation_matrix_20260902_144410_KST/runs/F2_zipf120_1800_classgc/attempt_01/fio_raw.json) |
| G1 | zipf:1.2 / 1800s | V4 ClassGC without age | [원본](v4_validation_matrix_20260902_144410_KST/runs/G1_zipf120_1800_classgc_noage/attempt_01/stats.txt) | [JSON](v4_validation_matrix_20260902_144410_KST/runs/G1_zipf120_1800_classgc_noage/attempt_01/fio_raw.json) |
| T1 | uniform / 1800s | V4 ClassGC threshold-off | [원본](v4_classgc_threshold_off_20260903_001558_KST/runs/T1_uniform_1800_classgc_threshold_off/attempt_01/host_femu.log) | 없음† |
| T2 | zipf:1.2 / 1800s | V4 ClassGC threshold-off | [원본](v4_classgc_threshold_off_20260903_001558_KST/runs/T2_zipf120_1800_classgc_threshold_off/attempt_03/stats.txt) | [JSON](v4_classgc_threshold_off_20260903_001558_KST/runs/T2_zipf120_1800_classgc_threshold_off/attempt_03/fio_raw.json) |
| H1 | uniform / 1800s | Baseline | [원본](baseline_separation_skew_20260903_174617_KST/runs/H1_baseline_uniform_1800/attempt_01/stats.txt) | [JSON](baseline_separation_skew_20260903_174617_KST/runs/H1_baseline_uniform_1800/attempt_01/fio_raw.json) |
| H2 | zipf:1.2 / 1800s | Baseline | [원본](baseline_separation_skew_20260903_174617_KST/runs/H2_baseline_zipf120_1800/attempt_01/stats.txt) | [JSON](baseline_separation_skew_20260903_174617_KST/runs/H2_baseline_zipf120_1800/attempt_01/fio_raw.json) |
