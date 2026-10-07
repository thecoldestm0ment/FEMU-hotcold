#!/bin/bash
set -euo pipefail

repo=/root/workspace/FEMU
root=$repo/experiment_results/ftl_hotcold/final_3seed/20260831_202419_KST
first=$root/20260831_203323_KST_baseline_seed20260825

while [ ! -f "$first/git_status_after.txt" ]; do
    sleep 30
done
grep -qx 'status=VALID' "$first/results_summary.txt"
if pgrep -f '[q]emu-system-x86_64' >/dev/null; then
    echo 'first run finished files but QEMU is still alive' >&2
    exit 20
fi

run_next()
{
    version=$1
    seed=$2
    label=$3
    timestamp=$(date +%Y%m%d_%H%M%S_KST)
    output=$root/${timestamp}_${label}_seed${seed}
    echo "START version=$version seed=$seed output=$output"
    /tmp/femu-final-3seed/run_one.sh "$version" "$seed" "$output"
    grep -qx 'status=VALID' "$output/results_summary.txt"
    echo "DONE version=$version seed=$seed"
}

run_next v1 20260825 v1_T4096
run_next v3 20260825 v3_R1
run_next base 20260826 baseline
run_next v1 20260826 v1_T4096
run_next v3 20260826 v3_R1

echo 'ALL_REMAINING_RUNS_VALID'
