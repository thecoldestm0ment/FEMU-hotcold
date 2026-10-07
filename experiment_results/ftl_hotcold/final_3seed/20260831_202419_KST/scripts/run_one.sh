#!/bin/bash
set -euo pipefail

if [ "$#" -ne 3 ]; then
    echo "usage: $0 <base|v1|v3> <seed> <result-dir>" >&2
    exit 2
fi

version=$1
seed=$2
result_dir=$3
experiment_root=/tmp/femu-final-3seed
known_hosts=$experiment_root/known_hosts
askpass=$experiment_root/askpass.sh
guest=femu@127.0.0.1
qemu_launcher_pid=
run_complete=0

case "$version" in
    base)
        worktree=$experiment_root/base
        branch=hotcold/base
        expected_version=BASELINE
        env_args=()
        config_summary="classifier=none"
        ;;
    v1)
        worktree=$experiment_root/v1
        branch=hotcold/v1
        expected_version=V1
        env_args=(FEMU_HOT_REWRITE_WINDOW=4096)
        config_summary="T=4096"
        ;;
    v3)
        worktree=$experiment_root/v3
        branch=hotcold/v3
        expected_version=V3
        env_args=(
            FEMU_HOT_REWRITE_WINDOW=1024
            FEMU_HOT_BOUNDARY_WINDOW=4096
            FEMU_ERASE_SURVIVAL_THRESHOLD=1
        )
        config_summary="T=1024 B=4096 R=1"
        ;;
    *)
        echo "unknown version: $version" >&2
        exit 2
        ;;
esac

build_dir=$worktree/build-femu
run_script=$worktree/hw/femu/scripts/run-blackbox.sh
qemu_binary=$build_dir/qemu-system-x86_64

if [ -e "$result_dir" ]; then
    echo "refusing to overwrite existing result: $result_dir" >&2
    exit 3
fi
mkdir -p "$result_dir"

ssh_base=(
    /usr/bin/setsid -w
    /usr/bin/env
    SSH_ASKPASS="$askpass"
    SSH_ASKPASS_REQUIRE=force
    DISPLAY=:0
    /usr/bin/ssh
    -p 8080
    -o BatchMode=no
    -o ConnectTimeout=5
    -o ConnectionAttempts=1
    -o StrictHostKeyChecking=no
    -o UserKnownHostsFile="$known_hosts"
    -o LogLevel=ERROR
    "$guest"
)

ssh_guest()
{
    "${ssh_base[@]}" "$@"
}

stop_vm()
{
    if [ -n "$qemu_launcher_pid" ] && kill -0 "$qemu_launcher_pid" 2>/dev/null; then
        ssh_guest "sudo -n /usr/sbin/poweroff" >"$result_dir/poweroff_cleanup.txt" 2>&1 || true
        for _ in $(seq 1 60); do
            if ! kill -0 "$qemu_launcher_pid" 2>/dev/null; then
                break
            fi
            sleep 1
        done
        if kill -0 "$qemu_launcher_pid" 2>/dev/null; then
            kill -TERM -- "-$qemu_launcher_pid" 2>/dev/null || true
        fi
        wait "$qemu_launcher_pid" 2>/dev/null || true
    fi
}

cleanup()
{
    if [ "$run_complete" -ne 1 ]; then
        stop_vm
    fi
}
trap cleanup EXIT

test -x "$qemu_binary"
test -f "$run_script"
test -c /dev/kvm

if pgrep -f '[q]emu-system-x86_64' >/dev/null; then
    echo "another QEMU process is already running" >&2
    exit 4
fi
if [ -e "$build_dir/qmp-sock" ]; then
    echo "stale QMP socket exists: $build_dir/qmp-sock" >&2
    exit 4
fi

commit=$(git -C "$worktree" rev-parse HEAD)
git -C "$worktree" status --short --untracked-files=no >"$result_dir/git_status_before.txt"
if [ -s "$result_dir/git_status_before.txt" ]; then
    echo "tracked source is dirty in $worktree" >&2
    exit 5
fi
sha256sum \
    "$worktree/hw/femu/bbssd/ftl.c" \
    "$worktree/hw/femu/bbssd/ftl.h" \
    "$worktree/hw/femu/bbssd/bb.c" \
    "$run_script" >"$result_dir/source_hashes_before.txt"

{
    printf '# FEMU final three-seed extension run\n\n'
    printf -- '- version: %s\n' "$expected_version"
    printf -- '- branch: %s\n' "$branch"
    printf -- '- commit: %s\n' "$commit"
    printf -- '- seed: %s\n' "$seed"
    printf -- '- classifier config: %s\n' "$config_summary"
    printf -- '- fresh FEMU process/device: yes\n'
    printf -- '- exposed capacity: 6 GiB\n'
    printf -- '- raw NAND geometry: 8 GiB\n'
    printf -- '- preconditioning: full-device 6 GiB sequential write\n'
    printf -- '- pre-measurement command: cdw10=5 (accounting reset; classifier metadata follows existing implementation)\n'
    printf -- '- measurement runtime: 3600 seconds\n'
    printf -- '- workload: 16 KiB Zipf 0.99 random write, 32 jobs, iodepth 128, size 5 GiB/job address range\n'
    printf -- '- offset: none\n'
    printf -- '- marker: none\n'
    printf -- '- intermediate snapshots: none\n'
    printf -- '- fio randrepeat: 1\n'
    printf -- '- fio randseed: %s\n' "$seed"
} >"$result_dir/manifest.md"

rm -f "$build_dir/log"

(
    cd "$build_dir"
    exec /usr/bin/setsid /usr/bin/env "${env_args[@]}" /bin/bash "$run_script"
) >"$result_dir/launcher_output.txt" 2>&1 &
qemu_launcher_pid=$!
echo "$qemu_launcher_pid" >"$result_dir/launcher_pid.txt"

ssh_ready=0
for _ in $(seq 1 90); do
    if ssh_guest "printf 'guest-ready\\n'" >"$result_dir/ssh_ready.txt" 2>&1; then
        ssh_ready=1
        break
    fi
    if ! kill -0 "$qemu_launcher_pid" 2>/dev/null; then
        echo "QEMU exited before SSH became ready" >&2
        exit 6
    fi
    sleep 2
done
if [ "$ssh_ready" -ne 1 ]; then
    echo "guest SSH did not become ready" >&2
    exit 6
fi

ssh_guest 'set -eu
target=/dev/nvme0n1
test -b "$target"
size=$(lsblk -bndo SIZE "$target" | tr -d " ")
test "$size" = 6442450944
parts=$(lsblk -nrpo TYPE "$target" | awk '\''$1 == "part" { n++ } END { print n + 0 }'\'')
test "$parts" = 0
mounts=$(lsblk -nrpo MOUNTPOINT "$target" | sed '\''/^$/d'\'' | wc -l)
test "$mounts" = 0
root_source=$(findmnt -n -o SOURCE /)
test "$root_source" != "$target"
printf "target_size_bytes=%s\\npartition_count=%s\\nmount_count=%s\\nroot_source=%s\\n" "$size" "$parts" "$mounts" "$root_source"
lsblk -b -o NAME,PATH,SIZE,TYPE,MOUNTPOINT
' >"$result_dir/safety.txt" 2>&1

ssh_guest "sudo -n /usr/bin/fio --name=fill --filename=/dev/nvme0n1 --direct=1 --ioengine=libaio --rw=write --bs=1M --iodepth=32 --numjobs=1 --size=6G --group_reporting=1" \
    >"$result_dir/precondition_fio.txt" 2>&1
ssh_guest "sync" >"$result_dir/sync.txt" 2>&1

ssh_guest "sudo -n /usr/sbin/nvme admin-passthru /dev/nvme0 --opcode=0xef --cdw10=5" \
    >"$result_dir/precondition_reset.txt" 2>&1

stats_ready=0
for _ in $(seq 1 20); do
    count=$(grep -c 'BBSSD-STATS version=' "$build_dir/log" 2>/dev/null || true)
    if [ "$count" -ge 1 ]; then
        stats_ready=1
        break
    fi
    sleep 1
done
if [ "$stats_ready" -ne 1 ]; then
    echo "preconditioning stats were not emitted" >&2
    exit 7
fi
grep 'BBSSD-STATS version=' "$build_dir/log" | tail -n 1 >"$result_dir/precondition_stats.txt"

fio_start=$(date +%s)
printf '%s\n' femu | ssh_guest "sudo -S -p '' /usr/bin/fio --name=test --filename=/dev/nvme0n1 --direct=1 --time_based=1 --runtime=3600 --ioengine=libaio --rw=randwrite --iodepth=128 --numjobs=32 --size=5G --bs=16k --random_distribution=zipf:0.99 --randrepeat=1 --randseed=$seed --group_reporting=0" \
    >"$result_dir/fio_raw.txt" 2>&1
fio_end=$(date +%s)
fio_elapsed=$((fio_end - fio_start))
printf 'fio_rc=0\nhost_elapsed_sec=%s\n' "$fio_elapsed" >"$result_dir/fio_status.txt"
if [ "$fio_elapsed" -lt 3550 ] || [ "$fio_elapsed" -gt 3720 ]; then
    echo "fio elapsed time outside valid range: $fio_elapsed" >&2
    exit 8
fi

ssh_guest "sudo -n /usr/sbin/nvme admin-passthru /dev/nvme0 --opcode=0xef --cdw10=5" \
    >"$result_dir/final_stats_command.txt" 2>&1

stats_ready=0
for _ in $(seq 1 20); do
    count=$(grep -c 'BBSSD-STATS version=' "$build_dir/log" 2>/dev/null || true)
    if [ "$count" -ge 2 ]; then
        stats_ready=1
        break
    fi
    sleep 1
done
if [ "$stats_ready" -ne 1 ]; then
    echo "final stats were not emitted" >&2
    exit 9
fi
grep 'BBSSD-STATS version=' "$build_dir/log" | tail -n 1 >"$result_dir/stats.txt"

stats_line=$(cat "$result_dir/stats.txt")
field()
{
    printf '%s\n' "$stats_line" | tr ' ' '\n' | awk -F= -v key="$1" '$1 == key { print $2; exit }'
}

actual_version=$(field version)
host_writes=$(field host_page_writes)
nand_writes=$(field nand_page_writes)
gc_writes=$(field gc_page_writes)
gc_count=$(field gc_count)
average_gc_copy=$(field average_gc_copy)
waf=$(field waf)
block_erases=$(field block_erases)
invariant=$(field counter_invariant)

test "$actual_version" = "$expected_version"
test "$invariant" = PASS
test "$nand_writes" -eq $((host_writes + gc_writes))
if [ "$version" != base ]; then
    gc_hot=$(field gc_hot_writes)
    gc_cold=$(field gc_cold_writes)
    test $((gc_hot + gc_cold)) -eq "$gc_writes"
fi

iops=$(awk -v pages="$host_writes" -v seconds="$fio_elapsed" 'BEGIN { printf "%.3f", pages / 4.0 / seconds }')
bw_mib=$(awk -v value="$iops" 'BEGIN { printf "%.3f", value / 64.0 }')

{
    printf 'status=VALID\n'
    printf 'version=%s\n' "$actual_version"
    printf 'branch=%s\n' "$branch"
    printf 'commit=%s\n' "$commit"
    printf 'seed=%s\n' "$seed"
    printf 'config=%s\n' "$config_summary"
    printf 'fio_rc=0\n'
    printf 'fio_host_elapsed_sec=%s\n' "$fio_elapsed"
    printf 'estimated_iops_from_host_pages=%s\n' "$iops"
    printf 'estimated_bw_mib_per_sec=%s\n' "$bw_mib"
    printf 'host_page_writes=%s\n' "$host_writes"
    printf 'nand_page_writes=%s\n' "$nand_writes"
    printf 'gc_page_writes=%s\n' "$gc_writes"
    printf 'waf=%s\n' "$waf"
    printf 'gc_count=%s\n' "$gc_count"
    printf 'average_gc_copy=%s\n' "$average_gc_copy"
    printf 'block_erases=%s\n' "$block_erases"
    printf 'counter_invariant=%s\n' "$invariant"
    if [ "$version" != base ]; then
        printf 'host_hot_writes=%s\n' "$(field host_hot_writes)"
        printf 'host_cold_writes=%s\n' "$(field host_cold_writes)"
        printf 'hot_write_ratio=%s\n' "$(field hot_write_ratio)"
        printf 'gc_hot_writes=%s\n' "$gc_hot"
        printf 'gc_cold_writes=%s\n' "$gc_cold"
    fi
    if [ "$version" = v3 ]; then
        printf 'host_cold_survival_writes=%s\n' "$(field host_cold_survival_writes)"
        printf 'host_hot_boundary_writes=%s\n' "$(field host_hot_boundary_writes)"
        printf 'boundary_survival_zero=%s\n' "$(field boundary_survival_zero)"
        printf 'boundary_survival_one=%s\n' "$(field boundary_survival_one)"
        printf 'boundary_survival_two=%s\n' "$(field boundary_survival_two)"
        printf 'boundary_survival_three_plus=%s\n' "$(field boundary_survival_three_plus)"
    fi
} >"$result_dir/results_summary.txt"

ssh_guest "sudo -n /usr/sbin/poweroff" >"$result_dir/poweroff.txt" 2>&1 || true
for _ in $(seq 1 120); do
    if ! kill -0 "$qemu_launcher_pid" 2>/dev/null; then
        break
    fi
    sleep 1
done
if kill -0 "$qemu_launcher_pid" 2>/dev/null; then
    echo "QEMU did not exit after guest poweroff" >&2
    exit 10
fi
wait "$qemu_launcher_pid" 2>/dev/null || true
qemu_launcher_pid=

cp "$build_dir/log" "$result_dir/host_femu.log"
sha256sum \
    "$worktree/hw/femu/bbssd/ftl.c" \
    "$worktree/hw/femu/bbssd/ftl.h" \
    "$worktree/hw/femu/bbssd/bb.c" \
    "$run_script" >"$result_dir/source_hashes_after.txt"
cmp "$result_dir/source_hashes_before.txt" "$result_dir/source_hashes_after.txt"
git -C "$worktree" status --short --untracked-files=no >"$result_dir/git_status_after.txt"
test ! -s "$result_dir/git_status_after.txt"

run_complete=1
trap - EXIT
printf 'RUN_VALID version=%s seed=%s waf=%s iops=%s\n' "$actual_version" "$seed" "$waf" "$iops"
