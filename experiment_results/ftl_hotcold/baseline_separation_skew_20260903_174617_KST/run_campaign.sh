#!/bin/bash

set -uo pipefail

readonly WORKSPACE=/root/workspace/FEMU
readonly BASELINE_TREE=/tmp/femu_baseline_v4_env
readonly BASELINE_BUILD=${BASELINE_TREE}/build-femu
readonly QEMU_BIN=${BASELINE_BUILD}/qemu-system-x86_64
readonly BASELINE_COMMIT=c0237bb12f183d87ea1abf2bf52e92f900f6c587
readonly IMAGE=/root/images/u20s.qcow2
readonly RESULT_ROOT=${WORKSPACE}/experiment_results/ftl_hotcold/baseline_separation_skew_20260903_174617_KST
readonly GUEST_HOST=127.0.0.1
readonly GUEST_PORT=8080
readonly GUEST_USER=femu
readonly GUEST_DEVICE=/dev/nvme0n1
readonly GUEST_DEVICE_BYTES=6442450944
readonly MAX_ATTEMPTS=${MAX_ATTEMPTS:-3}
readonly RUN_ONLY=${RUN_ONLY:-all}

: "${FEMU_GUEST_PASSWORD:?Set FEMU_GUEST_PASSWORD in the process environment}"
export FEMU_GUEST_PASSWORD

qemu_pid=
fio_ssh_pid=
current_attempt=
ASKPASS=

timestamp()
{
    TZ=Asia/Seoul date '+%F %T KST'
}

campaign_log()
{
    printf '[%s] %s\n' "$(timestamp)" "$*" | tee -a "$RESULT_ROOT/campaign_status.log"
}

cleanup_processes()
{
    if [[ -n ${fio_ssh_pid:-} ]] && kill -0 "$fio_ssh_pid" 2>/dev/null; then
        kill -TERM "$fio_ssh_pid" 2>/dev/null || true
        wait "$fio_ssh_pid" 2>/dev/null || true
    fi
    fio_ssh_pid=

    if [[ -n ${qemu_pid:-} ]] && kill -0 "$qemu_pid" 2>/dev/null; then
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
    qemu_pid=
}

cleanup_all()
{
    cleanup_processes
    if [[ -n ${ASKPASS:-} && -f $ASKPASS ]]; then
        rm -f "$ASKPASS"
    fi
}

trap cleanup_all EXIT INT TERM

attempt_fail()
{
    local message=$1
    printf 'FAILED: %s\n' "$message" > "$current_attempt/attempt_status.txt"
    campaign_log "FAILED $(basename "$(dirname "$current_attempt")")/$(basename "$current_attempt"): $message"
    cleanup_processes
    return 1
}

guest_ssh()
{
    DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS="$ASKPASS" \
        setsid -w ssh \
        -p "$GUEST_PORT" \
        -o ConnectTimeout=5 \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o LogLevel=ERROR \
        "${GUEST_USER}@${GUEST_HOST}" "$@"
}

guest_scp()
{
    DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS="$ASKPASS" \
        setsid -w scp \
        -P "$GUEST_PORT" \
        -o ConnectTimeout=5 \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o LogLevel=ERROR \
        "$1" "${GUEST_USER}@${GUEST_HOST}:$2"
}

guest_sudo()
{
    local command=$1
    local quoted
    printf -v quoted '%q' "$command"
    printf '%s\n' "$FEMU_GUEST_PASSWORD" |
        guest_ssh "sudo -S -p '' bash -lc $quoted"
}

next_attempt_number()
{
    local run_dir=$1
    local max=0
    local path number

    for path in "$run_dir"/attempt_*; do
        [[ -d $path ]] || continue
        number=${path##*_}
        number=$((10#$number))
        (( number > max )) && max=$number
    done
    printf '%02d\n' $((max + 1))
}

launch_femu()
{
    local log=$1

    (
        cd "$BASELINE_BUILD" || exit 1
        exec "$QEMU_BIN" \
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
            > "$log" 2>&1
    ) &
    qemu_pid=$!
}

wait_for_ssh()
{
    for _ in $(seq 1 150); do
        kill -0 "$qemu_pid" 2>/dev/null || return 1
        guest_ssh true >/dev/null 2>&1 && return 0
        sleep 2
    done
    return 1
}

write_manifest()
{
    local run_id=$1
    local distribution=$2
    local fio_file=$3
    local actual_commit=$4

    {
        printf 'run_id=%s\n' "$run_id"
        printf 'implementation=Baseline\n'
        printf 'branch=hotcold/base\n'
        printf 'commit=%s\n' "$actual_commit"
        printf 'distribution=%s\n' "$distribution"
        printf 'runtime_seconds=1800\n'
        printf 'raw_nand_gib=8\n'
        printf 'exposed_mib=6144\n'
        printf 'op_percent=25\n'
        printf 'gc_threshold_percent=75\n'
        printf 'gc_threshold_high_percent=95\n'
        printf 'precondition=6GiB sequential write\n'
        printf 'measurement_reset=nvme admin-passthru /dev/nvme0 --opcode=0xef --cdw10=5\n'
        printf 'rw=randwrite\n'
        printf 'bs=16k\n'
        printf 'working_set=5G\n'
        printf 'iodepth=128\n'
        printf 'numjobs=32\n'
        printf 'direct=1\n'
        printf 'ioengine=libaio\n'
        printf 'randrepeat=1\n'
        printf 'randseed=20260824\n'
        printf 'time_based=1\n'
        printf 'fresh_femu_process=yes\n'
        printf 'fio_sha256=%s\n' "$(sha256sum "$fio_file" | awk '{print $1}')"
    } > "$current_attempt/manifest.txt"
}

run_attempt()
{
    local run_id=$1
    local distribution=$2
    local fio_file=$3
    local attempt_number=$4
    local actual_commit device_bytes guest_fio fio_rc elapsed missing=0
    local host_writes gc_writes nand_writes fio_bytes

    current_attempt="$RESULT_ROOT/runs/$run_id/attempt_$attempt_number"
    mkdir -p "$current_attempt"
    campaign_log "START $run_id attempt=$attempt_number"

    actual_commit=$(git -C "$BASELINE_TREE" rev-parse HEAD 2>/dev/null) || {
        attempt_fail "cannot read baseline commit"; return 1;
    }
    [[ $actual_commit == "$BASELINE_COMMIT" ]] || {
        attempt_fail "unexpected baseline commit $actual_commit"; return 1;
    }
    [[ -z $(git -C "$BASELINE_TREE" status --porcelain --untracked-files=no) ]] || {
        attempt_fail "baseline tree has tracked changes"; return 1;
    }

    if ! ninja -C "$BASELINE_BUILD" qemu-system-x86_64 > "$current_attempt/build.log" 2>&1; then
        attempt_fail "build failed"; return 1
    fi
    if ! git -C "$BASELINE_TREE" diff --check -- hw/femu/bbssd > "$current_attempt/diff_check.log" 2>&1; then
        attempt_fail "source diff check failed"; return 1
    fi
    sha256sum \
        "$BASELINE_TREE/hw/femu/bbssd/ftl.c" \
        "$BASELINE_TREE/hw/femu/bbssd/ftl.h" \
        "$BASELINE_TREE/hw/femu/bbssd/bb.c" \
        > "$current_attempt/source_hashes.txt"
    cp "$fio_file" "$current_attempt/workload.fio"
    write_manifest "$run_id" "$distribution" "$fio_file" "$actual_commit"

    if pgrep -f '[q]emu-system-x86_64' >/dev/null; then
        attempt_fail "another QEMU process is running"; return 1
    fi
    if command -v fuser >/dev/null && fuser "$IMAGE" >/dev/null 2>&1; then
        attempt_fail "guest image is in use"; return 1
    fi
    if ss -H -ltn 2>/dev/null | grep -qE '[:.]8080[[:space:]]'; then
        attempt_fail "TCP port 8080 is in use"; return 1
    fi

    launch_femu "$current_attempt/host_femu.log"
    printf '%s\n' "$qemu_pid" > "$current_attempt/qemu.pid"
    if ! wait_for_ssh; then
        attempt_fail "FEMU/SSH startup failed"; return 1
    fi

    {
        guest_ssh 'uname -a'
        guest_ssh 'fio --version'
        guest_ssh 'nvme version'
        guest_ssh "test -b $GUEST_DEVICE"
        guest_sudo 'true'
        guest_sudo "blockdev --getsize64 $GUEST_DEVICE"
        guest_ssh "if findmnt -rn -S $GUEST_DEVICE | grep -q .; then exit 1; fi"
        guest_ssh "test \"\$(lsblk -nrpo NAME $GUEST_DEVICE | wc -l)\" -eq 1"
    } > "$current_attempt/preflight.txt" 2>&1
    if [[ $? -ne 0 ]]; then
        attempt_fail "guest preflight failed"; return 1
    fi

    device_bytes=$(guest_sudo "blockdev --getsize64 $GUEST_DEVICE" 2>/dev/null) || {
        attempt_fail "cannot read guest device size"; return 1;
    }
    [[ $device_bytes == "$GUEST_DEVICE_BYTES" ]] || {
        attempt_fail "unexpected guest device size $device_bytes"; return 1;
    }

    printf '%s\n' "sudo fio --name=precondition --filename=$GUEST_DEVICE --direct=1 --ioengine=libaio --rw=write --bs=1M --iodepth=32 --numjobs=1 --size=6G --group_reporting=1" > "$current_attempt/precondition_command.txt"
    if ! guest_sudo "fio --name=precondition --filename=$GUEST_DEVICE --direct=1 --ioengine=libaio --rw=write --bs=1M --iodepth=32 --numjobs=1 --size=6G --group_reporting=1" > "$current_attempt/precondition_fio.txt" 2>&1; then
        attempt_fail "preconditioning failed"; return 1
    fi
    sleep 2

    if ! guest_sudo 'nvme admin-passthru /dev/nvme0 --opcode=0xef --cdw10=5' > "$current_attempt/precondition_reset.txt" 2>&1; then
        attempt_fail "measurement reset failed"; return 1
    fi

    guest_fio="/tmp/${run_id}.fio"
    if ! guest_scp "$fio_file" "$guest_fio" > "$current_attempt/fio_upload.log" 2>&1; then
        attempt_fail "fio upload failed"; return 1
    fi
    printf 'sudo fio --output-format=json %s\n' "$guest_fio" > "$current_attempt/fio_command.txt"

    guest_sudo "fio --output-format=json $guest_fio" \
        > "$current_attempt/fio_raw.json" 2> "$current_attempt/fio_stderr.txt" &
    fio_ssh_pid=$!
    sleep 3

    while kill -0 "$fio_ssh_pid" 2>/dev/null; do
        if ! kill -0 "$qemu_pid" 2>/dev/null; then
            attempt_fail "FEMU exited during fio"; return 1
        fi
        elapsed=$(guest_ssh "ps -C fio -o etimes= 2>/dev/null | awk 'NF {if (\$1>m) m=\$1} END {if (m) print m}'" 2>/dev/null || true)
        if [[ -n $elapsed ]]; then
            missing=0
            printf '%s qemu_alive=yes fio_elapsed=%s\n' "$(timestamp)" "$elapsed" >> "$current_attempt/liveness.log"
        else
            missing=$((missing + 1))
            printf '%s qemu_alive=yes fio_elapsed=unavailable\n' "$(timestamp)" >> "$current_attempt/liveness.log"
            if (( missing >= 3 )); then
                attempt_fail "fio process was not observable"; return 1
            fi
        fi
        sleep 60
    done

    wait "$fio_ssh_pid"
    fio_rc=$?
    fio_ssh_pid=
    printf '%s\n' "$fio_rc" > "$current_attempt/fio_exit_code.txt"
    [[ $fio_rc -eq 0 ]] || {
        attempt_fail "fio exited with status $fio_rc"; return 1;
    }
    rg -q '"error"[[:space:]]*:[[:space:]]*0' "$current_attempt/fio_raw.json" || {
        attempt_fail "fio JSON did not report error=0"; return 1;
    }

    sleep 2
    if ! guest_sudo 'nvme admin-passthru /dev/nvme0 --opcode=0xef --cdw10=5' > "$current_attempt/final_stats_command.txt" 2>&1; then
        attempt_fail "final stats command failed"; return 1
    fi
    sleep 2
    guest_ssh sync > "$current_attempt/sync.txt" 2>&1 || true
    guest_sudo poweroff > "$current_attempt/poweroff.txt" 2>&1 || true

    for _ in $(seq 1 120); do
        kill -0 "$qemu_pid" 2>/dev/null || break
        sleep 1
    done
    if kill -0 "$qemu_pid" 2>/dev/null; then
        kill -TERM "$qemu_pid" 2>/dev/null || true
    fi
    wait "$qemu_pid" 2>/dev/null || true
    qemu_pid=

    rg 'BBSSD-STATS' "$current_attempt/host_femu.log" | tail -n 1 > "$current_attempt/stats.txt"
    [[ -s $current_attempt/stats.txt ]] || {
        attempt_fail "final BBSSD stats missing"; return 1;
    }
    rg -q 'counter_invariant=PASS' "$current_attempt/stats.txt" || {
        attempt_fail "counter invariant failed"; return 1;
    }

    host_writes=$(sed -n 's/.*host_page_writes=\([0-9][0-9]*\).*/\1/p' "$current_attempt/stats.txt")
    gc_writes=$(sed -n 's/.*gc_page_writes=\([0-9][0-9]*\).*/\1/p' "$current_attempt/stats.txt")
    nand_writes=$(sed -n 's/.*nand_page_writes=\([0-9][0-9]*\).*/\1/p' "$current_attempt/stats.txt")
    [[ $nand_writes -eq $((host_writes + gc_writes)) ]] || {
        attempt_fail "NAND=Host+GC invariant failed"; return 1;
    }
    fio_bytes=$(awk '/"write"[[:space:]]*:/ {in_write=1} in_write && /"io_bytes"[[:space:]]*:/ {gsub(/[^0-9]/, "", $0); print; exit}' "$current_attempt/fio_raw.json")
    [[ $fio_bytes -eq $((host_writes * 4096)) ]] || {
        attempt_fail "fio bytes do not equal Host pages x 4096"; return 1;
    }

    printf 'PASS\n' > "$current_attempt/attempt_status.txt"
    printf 'attempt_%s\n' "$attempt_number" > "$RESULT_ROOT/runs/$run_id/selected_attempt.txt"
    campaign_log "PASS $run_id attempt=$attempt_number"
    return 0
}

run_with_retries()
{
    local run_id=$1
    local distribution=$2
    local fio_file=$3
    local run_dir="$RESULT_ROOT/runs/$run_id"
    local attempt_number

    mkdir -p "$run_dir"
    if [[ -f $run_dir/selected_attempt.txt ]]; then
        campaign_log "SKIP $run_id already selected"
        return 0
    fi

    for _ in $(seq 1 "$MAX_ATTEMPTS"); do
        attempt_number=$(next_attempt_number "$run_dir")
        if run_attempt "$run_id" "$distribution" "$fio_file" "$attempt_number"; then
            return 0
        fi
        sleep 10
    done
    return 1
}

mkdir -p "$RESULT_ROOT/runs"
exec 9> "$RESULT_ROOT/campaign.lock"
if ! flock -n 9; then
    campaign_log 'FAILED: another campaign process holds the lock'
    exit 1
fi

ASKPASS=$(mktemp /tmp/femu-baseline-askpass.XXXXXX)
printf '%s\n' '#!/bin/sh' 'printf "%s\n" "$FEMU_GUEST_PASSWORD"' > "$ASKPASS"
chmod 700 "$ASKPASS"

[[ $EUID -eq 0 ]] || { campaign_log 'FAILED: run as root'; exit 1; }
[[ -f $IMAGE ]] || { campaign_log "FAILED: missing image $IMAGE"; exit 1; }
[[ -x $QEMU_BIN ]] || { campaign_log "FAILED: missing baseline binary $QEMU_BIN"; exit 1; }
if [[ ! -e /dev/kvm ]]; then
    modprobe kvm || { campaign_log 'FAILED: modprobe kvm failed'; exit 1; }
    modprobe kvm_intel || { campaign_log 'FAILED: modprobe kvm_intel failed'; exit 1; }
fi
[[ -c /dev/kvm ]] || { campaign_log 'FAILED: /dev/kvm unavailable'; exit 1; }
[[ $RUN_ONLY == all || $RUN_ONLY == uniform || $RUN_ONLY == zipf120 ]] || {
    campaign_log "FAILED: invalid RUN_ONLY=$RUN_ONLY"; exit 1;
}

campaign_log "CAMPAIGN START run_only=$RUN_ONLY"

if [[ $RUN_ONLY == all || $RUN_ONLY == uniform ]]; then
    if ! run_with_retries \
        H1_baseline_uniform_1800 \
        uniform \
        "$RESULT_ROOT/fio_settings/baseline_uniform_1800.fio"; then
        campaign_log 'CAMPAIGN FAILED at H1_baseline_uniform_1800'
        exit 1
    fi
fi

if [[ $RUN_ONLY == all || $RUN_ONLY == zipf120 ]]; then
    if ! run_with_retries \
        H2_baseline_zipf120_1800 \
        zipf:1.2 \
        "$RESULT_ROOT/fio_settings/baseline_zipf120_1800.fio"; then
        campaign_log 'CAMPAIGN FAILED at H2_baseline_zipf120_1800'
        exit 1
    fi
fi

if [[ -f $RESULT_ROOT/runs/H1_baseline_uniform_1800/selected_attempt.txt && \
      -f $RESULT_ROOT/runs/H2_baseline_zipf120_1800/selected_attempt.txt ]]; then
    printf 'COMPLETE\n' > "$RESULT_ROOT/campaign_status.txt"
    campaign_log 'CAMPAIGN COMPLETE'
else
    printf 'PARTIAL_COMPLETE\n' > "$RESULT_ROOT/campaign_status.txt"
    campaign_log 'PARTIAL COMPLETE'
fi
