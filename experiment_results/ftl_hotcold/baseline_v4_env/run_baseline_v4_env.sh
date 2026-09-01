#!/bin/bash

set -euo pipefail

readonly WORKSPACE=/root/workspace/FEMU
readonly BASELINE_TREE=/tmp/femu_baseline_v4_env
readonly BASELINE_BUILD=${BASELINE_TREE}/build-femu
readonly QEMU_BIN=${BASELINE_BUILD}/qemu-system-x86_64
readonly IMAGE=/root/images/u20s.qcow2
readonly RESULT_ROOT=${WORKSPACE}/experiment_results/ftl_hotcold/baseline_v4_env
readonly BASELINE_COMMIT=c0237bb12f183d87ea1abf2bf52e92f900f6c587
readonly GUEST_HOST=127.0.0.1
readonly GUEST_PORT=8080
readonly GUEST_USER=femu
readonly GUEST_DEVICE=/dev/nvme0n1
readonly GUEST_DEVICE_BYTES=6442450944
readonly ASKPASS=${RESULT_ROOT}/ssh_askpass.sh

export FEMU_GUEST_PASSWORD=${FEMU_GUEST_PASSWORD:-femu}

run_id=$(TZ=Asia/Seoul date +%Y%m%d_%H%M%S_KST)
result_dir=${RESULT_ROOT}/${run_id}_baseline_seed20260824
mkdir -p "$result_dir"
touch "$result_dir/known_hosts"

qemu_pid=

status()
{
    printf '[%s] %s\n' "$(TZ=Asia/Seoul date '+%F %T KST')" "$*" |
        tee -a "$result_dir/runner_status.txt"
}

fail()
{
    status "FAILED: $*"
    printf 'FAILED\n' > "$result_dir/status.txt"
    exit 1
}

cleanup()
{
    local rc=$?

    if [[ $rc -ne 0 && -n ${qemu_pid:-} ]] && kill -0 "$qemu_pid" 2>/dev/null; then
        kill -TERM "$qemu_pid" 2>/dev/null || true
        for _ in $(seq 1 20); do
            kill -0 "$qemu_pid" 2>/dev/null || break
            sleep 1
        done
        if kill -0 "$qemu_pid" 2>/dev/null; then
            kill -KILL "$qemu_pid" 2>/dev/null || true
        fi
        wait "$qemu_pid" 2>/dev/null || true
    fi
}

trap cleanup EXIT

guest_ssh()
{
    DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS="$ASKPASS" \
        setsid -w ssh \
        -p "$GUEST_PORT" \
        -o ConnectTimeout=5 \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile="$result_dir/known_hosts" \
        "${GUEST_USER}@${GUEST_HOST}" "$@"
}

guest_sudo()
{
    local command=$1

    guest_ssh "printf '%s\\n' '$FEMU_GUEST_PASSWORD' | sudo -S -p '' $command"
}

status "Preparing baseline run"

[[ $EUID -eq 0 ]] || fail "run as root"
[[ -x $ASKPASS ]] || fail "SSH askpass helper is not executable"
[[ -f $IMAGE ]] || fail "guest image is missing: $IMAGE"
[[ -x $QEMU_BIN ]] || fail "baseline QEMU binary is missing: $QEMU_BIN"

if [[ ! -e /dev/kvm ]]; then
    modprobe kvm
    modprobe kvm_intel
fi
[[ -c /dev/kvm ]] || fail "/dev/kvm is unavailable"

if pgrep -f '[q]emu-system-x86_64' >/dev/null; then
    fail "another QEMU process is running"
fi
if command -v fuser >/dev/null && fuser "$IMAGE" >/dev/null 2>&1; then
    fail "guest image is in use"
fi
if ss -H -ltn 2>/dev/null | grep -qE '[:.]8080[[:space:]]'; then
    fail "TCP port 8080 is already in use"
fi

actual_commit=$(git -C "$BASELINE_TREE" rev-parse HEAD)
[[ $actual_commit == "$BASELINE_COMMIT" ]] ||
    fail "unexpected baseline commit: $actual_commit"
[[ -z $(git -C "$BASELINE_TREE" status --porcelain --untracked-files=no) ]] ||
    fail "baseline source has tracked modifications"

ninja -C "$BASELINE_BUILD" qemu-system-x86_64 > "$result_dir/build.log" 2>&1
git -C "$BASELINE_TREE" diff --check > "$result_dir/diff_check.log"
sha256sum \
    "$BASELINE_TREE/hw/femu/bbssd/ftl.c" \
    "$BASELINE_TREE/hw/femu/bbssd/ftl.h" \
    "$BASELINE_TREE/hw/femu/bbssd/bb.c" \
    > "$result_dir/source_hashes.txt"

{
    printf 'phase=final-baseline-v4-environment\n'
    printf 'source=hotcold/base\n'
    printf 'commit=%s\n' "$actual_commit"
    printf 'raw_nand_gib=8\n'
    printf 'exposed_mib=6144\n'
    printf 'op_percent=25\n'
    printf 'gc_threshold_percent=75\n'
    printf 'gc_threshold_high_percent=95\n'
    printf 'precondition=6GiB sequential write\n'
    printf 'workload=randwrite,size=5GiB,bs=16KiB,iodepth=128,numjobs=32,zipf=0.99,runtime=3600\n'
    printf 'randrepeat=1\n'
    printf 'randseed=20260824\n'
    printf 'guest=%s@%s:%s\n' "$GUEST_USER" "$GUEST_HOST" "$GUEST_PORT"
} > "$result_dir/manifest.txt"

status "Launching fresh FEMU baseline process"
(
    cd "$BASELINE_BUILD"
    exec env \
        FEMU_HOT_POOL_PERCENT=20 \
        FEMU_FREQUENCY_WINDOW_WRITES=4096 \
        FEMU_HOT_WRITES_PER_WINDOW=4 \
        FEMU_COLD_WRITES_PER_WINDOW=1 \
        FEMU_ERASE_EVENT_THRESHOLD=1 \
        "$QEMU_BIN" \
        -name FEMU-BBSSD-BASELINE \
        -enable-kvm \
        -cpu host \
        -smp 4 \
        -m 4G \
        -device virtio-scsi-pci,id=scsi0 \
        -device scsi-hd,drive=hd0 \
        -drive "file=$IMAGE,if=none,aio=native,cache=none,format=qcow2,id=hd0" \
        -device femu,devsz_mb=6144,namespaces=1,femu_mode=1,secsz=512,secs_per_pg=8,pgs_per_blk=256,blks_per_pl=128,pls_per_lun=1,luns_per_ch=8,nchs=8,pg_rd_lat=40000,pg_wr_lat=200000,blk_er_lat=2000000,ch_xfer_lat=0,gc_thres_pcent=75,gc_thres_pcent_high=95 \
        -net user,hostfwd=tcp::8080-:22 \
        -net nic,model=virtio \
        -nographic \
        > "$result_dir/host_femu.log" 2>&1
) &
qemu_pid=$!
printf '%s\n' "$qemu_pid" > "$result_dir/qemu.pid"

ssh_ready=false
for _ in $(seq 1 150); do
    if ! kill -0 "$qemu_pid" 2>/dev/null; then
        wait "$qemu_pid" || true
        fail "FEMU exited before SSH became ready"
    fi
    if guest_ssh true >/dev/null 2>&1; then
        ssh_ready=true
        break
    fi
    sleep 2
done
[[ $ssh_ready == true ]] || fail "guest SSH was not ready within 300 seconds"

status "Running guest preflight"
{
    guest_ssh "uname -a"
    guest_ssh "fio --version"
    guest_ssh "nvme version"
    guest_ssh "test -b $GUEST_DEVICE"
    guest_sudo "true"
    guest_sudo "blockdev --getsize64 $GUEST_DEVICE"
    guest_ssh "if findmnt -rn -S $GUEST_DEVICE | grep -q .; then exit 1; fi"
    guest_ssh "test \"\$(lsblk -nrpo NAME $GUEST_DEVICE | wc -l)\" -eq 1"
} > "$result_dir/preflight.txt" 2>&1

device_bytes=$(guest_sudo "blockdev --getsize64 $GUEST_DEVICE")
[[ $device_bytes == "$GUEST_DEVICE_BYTES" ]] ||
    fail "unexpected guest device size: $device_bytes"

status "Running 6 GiB sequential preconditioning"
guest_sudo "fio --name=precondition --filename=$GUEST_DEVICE --direct=1 --ioengine=libaio --rw=write --bs=1M --iodepth=32 --numjobs=1 --size=6G --group_reporting=1" \
    > "$result_dir/precondition_fio.txt" 2>&1
sleep 2

status "Resetting measurement counters after preconditioning"
guest_sudo "nvme admin-passthru /dev/nvme0 --opcode=0xef --cdw10=5" \
    > "$result_dir/precondition_reset.txt" 2>&1

status "Running 3600-second baseline fio workload"
guest_sudo "fio --name=final-baseline --filename=$GUEST_DEVICE --direct=1 --ioengine=libaio --rw=randwrite --bs=16k --iodepth=128 --numjobs=32 --size=5G --time_based=1 --runtime=3600 --random_distribution=zipf:0.99 --randrepeat=1 --randseed=20260824 --group_reporting=1" \
    > "$result_dir/fio_raw.txt" 2>&1
sleep 2

status "Collecting final FEMU statistics"
guest_sudo "nvme admin-passthru /dev/nvme0 --opcode=0xef --cdw10=5" \
    > "$result_dir/final_stats_command.txt" 2>&1
sleep 2

guest_ssh sync > "$result_dir/sync.txt" 2>&1
guest_sudo "poweroff" > "$result_dir/poweroff.txt" 2>&1 || true

for _ in $(seq 1 120); do
    kill -0 "$qemu_pid" 2>/dev/null || break
    sleep 1
done
if kill -0 "$qemu_pid" 2>/dev/null; then
    kill -TERM "$qemu_pid"
fi
wait "$qemu_pid"
qemu_pid=

grep 'BBSSD-STATS' "$result_dir/host_femu.log" | tail -n 1 > "$result_dir/stats.txt"
[[ -s $result_dir/stats.txt ]] || fail "final BBSSD statistics were not found"
grep -q 'counter_invariant=PASS' "$result_dir/stats.txt" ||
    fail "counter invariant did not pass"

host_writes=$(sed -n 's/.*host_page_writes=\([0-9][0-9]*\).*/\1/p' "$result_dir/stats.txt")
nand_writes=$(sed -n 's/.*nand_page_writes=\([0-9][0-9]*\).*/\1/p' "$result_dir/stats.txt")
gc_writes=$(sed -n 's/.*gc_page_writes=\([0-9][0-9]*\).*/\1/p' "$result_dir/stats.txt")
[[ $nand_writes -eq $((host_writes + gc_writes)) ]] ||
    fail "NAND=Host+GC invariant failed"

{
    cat "$result_dir/stats.txt"
    grep 'write: IOPS=' "$result_dir/fio_raw.txt" | tail -n 1
} > "$result_dir/results_summary.txt"

printf 'completed\n' >> "$result_dir/manifest.txt"
printf 'PASS\n' > "$result_dir/status.txt"
status "COMPLETED: $result_dir"
