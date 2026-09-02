#!/bin/bash

set -uo pipefail

readonly WORKSPACE=/root/workspace/FEMU
readonly RESULT_ROOT=${WORKSPACE}/experiment_results/ftl_hotcold/v4_validation_matrix_20260902_144410_KST
readonly BUILD_DIR=${WORKSPACE}/build-femu
readonly QEMU_BIN=${BUILD_DIR}/qemu-system-x86_64
readonly IMAGE=/root/images/u20s.qcow2
readonly GUEST_HOST=127.0.0.1
readonly GUEST_PORT=8080
readonly GUEST_USER=femu
readonly GUEST_PASSWORD=femu
readonly GUEST_DEVICE=/dev/nvme0n1
readonly GUEST_DEVICE_BYTES=6442450944
readonly ASKPASS=${RESULT_ROOT}/ssh_askpass.sh
readonly MAX_ATTEMPTS=3

QEMU_PID=
FIO_SSH_PID=
ORIGINAL_REF=
ORIGINAL_COMMIT=

status()
{
    printf '[%s] %s\n' "$(TZ=Asia/Seoul date '+%F %T KST')" "$*" \
        >> "${RESULT_ROOT}/campaign_status.log"
}

guest_ssh()
{
    DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS="$ASKPASS" \
        setsid -w ssh \
        -p "$GUEST_PORT" \
        -o ConnectTimeout=8 \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile="${RESULT_ROOT}/known_hosts" \
        "${GUEST_USER}@${GUEST_HOST}" "$@"
}

guest_scp()
{
    local source=$1
    local destination=$2

    DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS="$ASKPASS" \
        setsid -w scp \
        -P "$GUEST_PORT" \
        -o ConnectTimeout=8 \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile="${RESULT_ROOT}/known_hosts" \
        "$source" "${GUEST_USER}@${GUEST_HOST}:${destination}"
}

guest_sudo()
{
    local command=$1

    guest_ssh "printf '%s\\n' '${GUEST_PASSWORD}' | sudo -S -p '' ${command}"
}

stop_attempt_processes()
{
    if [[ -n ${FIO_SSH_PID:-} ]] && kill -0 "$FIO_SSH_PID" 2>/dev/null; then
        kill -TERM "$FIO_SSH_PID" 2>/dev/null || true
        wait "$FIO_SSH_PID" 2>/dev/null || true
    fi
    FIO_SSH_PID=

    if [[ -n ${QEMU_PID:-} ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
        for _ in $(seq 1 30); do
            kill -0 "$QEMU_PID" 2>/dev/null || break
            sleep 1
        done
        if kill -0 "$QEMU_PID" 2>/dev/null; then
            kill -KILL "$QEMU_PID" 2>/dev/null || true
        fi
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    QEMU_PID=
}

restore_checkout()
{
    if [[ -n ${ORIGINAL_REF:-} ]]; then
        git -C "$WORKSPACE" switch "$ORIGINAL_REF" >/dev/null 2>&1 || \
            git -C "$WORKSPACE" switch --detach "$ORIGINAL_COMMIT" \
                >/dev/null 2>&1 || true
    fi
}

on_exit()
{
    stop_attempt_processes
    restore_checkout
}

trap on_exit EXIT
trap 'exit 130' INT TERM

attempt_error()
{
    local attempt_dir=$1
    shift
    printf '%s\n' "$*" > "${attempt_dir}/error.txt"
    printf 'FAILED\n' > "${attempt_dir}/attempt_status.txt"
    status "FAILED ${attempt_dir##*/}: $*"
    return 1
}

prepare_host()
{
    [[ $EUID -eq 0 ]] || return 1
    [[ -f $IMAGE && -x $QEMU_BIN && -x $ASKPASS ]] || return 1

    if [[ ! -c /dev/kvm ]]; then
        modprobe kvm || return 1
        modprobe kvm_intel || return 1
    fi
    [[ -c /dev/kvm ]] || return 1

    pgrep -f '[q]emu-system-x86_64' >/dev/null && return 1
    if command -v fuser >/dev/null && fuser "$IMAGE" >/dev/null 2>&1; then
        return 1
    fi
    if ss -H -ltn 2>/dev/null | grep -qE '[:.]8080[[:space:]]'; then
        return 1
    fi

    for commit in \
        032d29b83e3906593ef9f79e5d2739bdab27ef5e \
        d067a64de2974949807a6a6652cf2a08a76d1c4c \
        ea289e975127f2e63e510c0ce4ed22081ffec293
    do
        git -C "$WORKSPACE" cat-file -e "${commit}^{commit}" || return 1
    done

    [[ -z $(git -C "$WORKSPACE" status --porcelain --untracked-files=no) ]] || \
        return 1
    return 0
}

write_attempt_manifest()
{
    local attempt_dir=$1
    local run_id=$2
    local implementation=$3
    local branch=$4
    local commit=$5
    local distribution=$6
    local runtime=$7
    local io_size=$8

    {
        printf 'run_id=%s\n' "$run_id"
        printf 'implementation=%s\n' "$implementation"
        printf 'branch=%s\n' "$branch"
        printf 'commit=%s\n' "$commit"
        printf 'distribution=%s\n' "$distribution"
        printf 'runtime_seconds=%s\n' "$runtime"
        printf 'io_size_per_job=%s\n' "$io_size"
        printf 'raw_nand_gib=8\n'
        printf 'exposed_mib=6144\n'
        printf 'op_percent=25\n'
        printf 'gc_threshold_percent=75\n'
        printf 'gc_threshold_high_percent=95\n'
        printf 'precondition=6GiB sequential write\n'
        printf 'measurement_reset=nvme admin-passthru /dev/nvme0 --opcode=0xef --cdw10=5\n'
        printf 'rw=randwrite\nbs=16k\nworking_set=5G\n'
        printf 'iodepth=128\nnumjobs=32\nrandrepeat=1\nrandseed=20260824\n'
        printf 'fixed_write_total_gib=%s\n' "$([[ -n $io_size ]] && printf 512 || true)"
        printf 'fresh_femu_process=yes\n'
    } > "${attempt_dir}/manifest.txt"
}

select_fio_file()
{
    local experiment=$1

    case "$experiment" in
        A) printf '%s\n' "${RESULT_ROOT}/fio_settings/A_zipf099_1800.fio" ;;
        B) printf '%s\n' "${RESULT_ROOT}/fio_settings/B_zipf099_fixed_512GiB.fio" ;;
        C) printf '%s\n' "${RESULT_ROOT}/fio_settings/C_zipf099_3600.fio" ;;
        D) printf '%s\n' "${RESULT_ROOT}/fio_settings/D_uniform_1800.fio" ;;
        E) printf '%s\n' "${RESULT_ROOT}/fio_settings/E_zipf070_1800.fio" ;;
        F|G) printf '%s\n' "${RESULT_ROOT}/fio_settings/F_G_zipf120_1800.fio" ;;
        *) return 1 ;;
    esac
}

run_attempt()
{
    local run_id=$1
    local experiment=$2
    local implementation=$3
    local branch=$4
    local commit=$5
    local distribution=$6
    local runtime=$7
    local io_size=$8
    local attempt_no=$9
    local run_dir=${RESULT_ROOT}/runs/${run_id}
    local attempt_dir
    local fio_file
    local guest_fio_file=/tmp/${run_id}.fio
    local ssh_ready=false
    local fio_elapsed
    local last_elapsed=-1
    local stalled_checks=0
    local fio_rc
    local stats_line
    local host_pages

    printf -v attempt_dir '%s/attempt_%02d' "$run_dir" "$attempt_no"
    mkdir -p "$attempt_dir"
    write_attempt_manifest "$attempt_dir" "$run_id" "$implementation" \
        "$branch" "$commit" "$distribution" "$runtime" "$io_size"
    fio_file=$(select_fio_file "$experiment") || \
        { attempt_error "$attempt_dir" 'unknown experiment'; return 1; }
    cp "$fio_file" "${attempt_dir}/workload.fio"
    printf 'sudo fio --output-format=json %s\n' "$guest_fio_file" \
        > "${attempt_dir}/fio_command.txt"
    printf '%s\n' \
        'sudo fio --name=precondition --filename=/dev/nvme0n1 --direct=1 --ioengine=libaio --rw=write --bs=1M --iodepth=32 --numjobs=1 --size=6G --group_reporting=1' \
        > "${attempt_dir}/precondition_command.txt"

    status "START ${run_id} attempt=${attempt_no}"
    stop_attempt_processes

    if pgrep -f '[q]emu-system-x86_64' >/dev/null; then
        attempt_error "$attempt_dir" 'another QEMU process is running'
        return 1
    fi
    if command -v fuser >/dev/null && fuser "$IMAGE" >/dev/null 2>&1; then
        attempt_error "$attempt_dir" 'guest image is already in use'
        return 1
    fi

    git -C "$WORKSPACE" switch --detach "$commit" \
        > "${attempt_dir}/git_switch.log" 2>&1 || \
        { attempt_error "$attempt_dir" 'git switch failed'; return 1; }
    [[ $(git -C "$WORKSPACE" rev-parse HEAD) == "$commit" ]] || \
        { attempt_error "$attempt_dir" 'unexpected checked-out commit'; return 1; }
    [[ -z $(git -C "$WORKSPACE" status --porcelain --untracked-files=no) ]] || \
        { attempt_error "$attempt_dir" 'tracked source modifications detected'; return 1; }

    ninja -C "$BUILD_DIR" qemu-system-x86_64 \
        > "${attempt_dir}/build.log" 2>&1 || \
        { attempt_error "$attempt_dir" 'build failed'; return 1; }
    git -C "$WORKSPACE" diff --check > "${attempt_dir}/diff_check.log" 2>&1 || \
        { attempt_error "$attempt_dir" 'git diff check failed'; return 1; }
    sha256sum \
        "${WORKSPACE}/hw/femu/bbssd/ftl.c" \
        "${WORKSPACE}/hw/femu/bbssd/ftl.h" \
        "${WORKSPACE}/hw/femu/bbssd/bb.c" \
        > "${attempt_dir}/source_hashes.txt"

    (
        cd "$BUILD_DIR" || exit 1
        exec env \
            FEMU_HOT_POOL_PERCENT=20 \
            FEMU_FREQUENCY_WINDOW_WRITES=4096 \
            FEMU_HOT_WRITES_PER_WINDOW=4 \
            FEMU_COLD_WRITES_PER_WINDOW=1 \
            FEMU_ERASE_EVENT_THRESHOLD=1 \
            "$QEMU_BIN" \
            -name "FEMU-${run_id}" \
            -enable-kvm \
            -cpu host \
            -smp 4 \
            -m 4G \
            -device virtio-scsi-pci,id=scsi0 \
            -device scsi-hd,drive=hd0 \
            -drive "file=${IMAGE},if=none,aio=native,cache=none,format=qcow2,id=hd0" \
            -device femu,devsz_mb=6144,namespaces=1,femu_mode=1,secsz=512,secs_per_pg=8,pgs_per_blk=256,blks_per_pl=128,pls_per_lun=1,luns_per_ch=8,nchs=8,pg_rd_lat=40000,pg_wr_lat=200000,blk_er_lat=2000000,ch_xfer_lat=0,gc_thres_pcent=75,gc_thres_pcent_high=95 \
            -net user,hostfwd=tcp::8080-:22 \
            -net nic,model=virtio \
            -nographic
    ) > "${attempt_dir}/host_femu.log" 2>&1 &
    QEMU_PID=$!
    printf '%s\n' "$QEMU_PID" > "${attempt_dir}/qemu.pid"

    for _ in $(seq 1 150); do
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            wait "$QEMU_PID" 2>/dev/null || true
            QEMU_PID=
            attempt_error "$attempt_dir" 'FEMU exited before SSH became ready'
            return 1
        fi
        if guest_ssh true >/dev/null 2>&1; then
            ssh_ready=true
            break
        fi
        sleep 2
    done
    if [[ $ssh_ready != true ]]; then
        attempt_error "$attempt_dir" 'guest SSH did not become ready in 300 seconds'
        return 1
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
        # Guest util-linux is old and exposes MOUNTPOINT, not MOUNTPOINTS.
        guest_ssh "lsblk -b -o NAME,SIZE,TYPE,MOUNTPOINT $GUEST_DEVICE"
    } > "${attempt_dir}/preflight.txt" 2>&1 || \
        { attempt_error "$attempt_dir" 'guest raw-device safety preflight failed'; return 1; }
    [[ $(guest_sudo "blockdev --getsize64 $GUEST_DEVICE" 2>/dev/null) == \
        "$GUEST_DEVICE_BYTES" ]] || \
        { attempt_error "$attempt_dir" 'unexpected guest device size'; return 1; }

    guest_sudo \
        "fio --name=precondition --filename=$GUEST_DEVICE --direct=1 --ioengine=libaio --rw=write --bs=1M --iodepth=32 --numjobs=1 --size=6G --group_reporting=1" \
        > "${attempt_dir}/precondition_fio.txt" 2>&1 || \
        { attempt_error "$attempt_dir" 'preconditioning fio failed'; return 1; }
    sleep 2
    guest_sudo 'nvme admin-passthru /dev/nvme0 --opcode=0xef --cdw10=5' \
        > "${attempt_dir}/precondition_reset.txt" 2>&1 || \
        { attempt_error "$attempt_dir" 'measurement reset failed'; return 1; }
    sleep 2

    guest_scp "${attempt_dir}/workload.fio" "$guest_fio_file" \
        > "${attempt_dir}/fio_upload.log" 2>&1 || \
        { attempt_error "$attempt_dir" 'fio job upload failed'; return 1; }

    guest_sudo "fio --output-format=json $guest_fio_file" \
        > "${attempt_dir}/fio_raw.json" \
        2> "${attempt_dir}/fio_stderr.txt" &
    FIO_SSH_PID=$!
    sleep 10

    while kill -0 "$FIO_SSH_PID" 2>/dev/null; do
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            kill -TERM "$FIO_SSH_PID" 2>/dev/null || true
            wait "$FIO_SSH_PID" 2>/dev/null || true
            FIO_SSH_PID=
            attempt_error "$attempt_dir" 'FEMU exited while fio was running'
            return 1
        fi

        fio_elapsed=$(guest_ssh \
            "if pgrep -x fio >/dev/null; then ps -C fio -o etimes= | sort -nr | head -n 1 | tr -d ' '; else printf NONE; fi" \
            2>/dev/null || printf SSH_ERROR)
        printf '%s qemu_alive=yes fio_elapsed=%s\n' \
            "$(TZ=Asia/Seoul date '+%F %T KST')" "$fio_elapsed" \
            >> "${attempt_dir}/liveness.log"

        if [[ $fio_elapsed =~ ^[0-9]+$ ]]; then
            if (( fio_elapsed <= last_elapsed )); then
                stalled_checks=$((stalled_checks + 1))
            else
                stalled_checks=0
            fi
            last_elapsed=$fio_elapsed
        else
            stalled_checks=$((stalled_checks + 1))
        fi
        if (( stalled_checks >= 3 )); then
            kill -TERM "$FIO_SSH_PID" 2>/dev/null || true
            wait "$FIO_SSH_PID" 2>/dev/null || true
            FIO_SSH_PID=
            attempt_error "$attempt_dir" 'fio liveness or elapsed-time progression failed'
            return 1
        fi
        sleep 60
    done

    wait "$FIO_SSH_PID"
    fio_rc=$?
    FIO_SSH_PID=
    printf '%s\n' "$fio_rc" > "${attempt_dir}/fio_exit_code.txt"
    [[ $fio_rc -eq 0 ]] || \
        { attempt_error "$attempt_dir" "fio exited with rc=${fio_rc}"; return 1; }
    python3 -m json.tool "${attempt_dir}/fio_raw.json" >/dev/null 2>&1 || \
        { attempt_error "$attempt_dir" 'fio JSON validation failed'; return 1; }

    sleep 2
    guest_sudo 'nvme admin-passthru /dev/nvme0 --opcode=0xef --cdw10=5' \
        > "${attempt_dir}/final_stats_command.txt" 2>&1 || \
        { attempt_error "$attempt_dir" 'final stats command failed'; return 1; }
    sleep 2

    guest_ssh sync > "${attempt_dir}/sync.txt" 2>&1 || true
    guest_sudo poweroff > "${attempt_dir}/poweroff.txt" 2>&1 || true
    for _ in $(seq 1 120); do
        kill -0 "$QEMU_PID" 2>/dev/null || break
        sleep 1
    done
    if kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
    fi
    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=

    grep 'BBSSD-STATS' "${attempt_dir}/host_femu.log" | tail -n 1 \
        > "${attempt_dir}/stats.txt"
    stats_line=$(<"${attempt_dir}/stats.txt")
    [[ -n $stats_line && $stats_line == *'counter_invariant=PASS'* ]] || \
        { attempt_error "$attempt_dir" 'counter invariant missing or failed'; return 1; }
    [[ $stats_line == *'nand_page_writes='* && $stats_line == *'host_page_writes='* && \
       $stats_line == *'gc_page_writes='* ]] || \
        { attempt_error "$attempt_dir" 'required write counters missing'; return 1; }

    if [[ $implementation == *ClassGC* ]]; then
        for key in hot_victim_gc_count cold_victim_gc_count \
            avg_hot_victim_invalid_ratio avg_cold_victim_invalid_ratio \
            borrow_count current_hot_lines opposite_normal_gc_count \
            opposite_forced_gc_count global_emergency_fallback_count
        do
            [[ $stats_line == *"${key}="* ]] || \
                { attempt_error "$attempt_dir" "ClassGC counter missing: ${key}"; return 1; }
        done
        [[ $stats_line == *'pool_ownership_invariant=PASS'* ]] || \
            { attempt_error "$attempt_dir" 'pool ownership invariant missing or failed'; return 1; }
    fi

    if [[ -n $io_size ]]; then
        host_pages=$(printf '%s\n' "$stats_line" | \
            sed -n 's/.* host_page_writes=\([0-9][0-9]*\).*/\1/p')
        [[ $host_pages == 134217728 ]] || \
            { attempt_error "$attempt_dir" "fixed-write Host pages mismatch: ${host_pages}"; return 1; }
    fi

    printf 'PASS\n' > "${attempt_dir}/attempt_status.txt"
    printf '%02d\n' "$attempt_no" > "${run_dir}/selected_attempt.txt"
    status "PASS ${run_id} attempt=${attempt_no}"
    return 0
}

run_one()
{
    local run_id=$1
    local experiment=$2
    local implementation=$3
    local branch=$4
    local commit=$5
    local distribution=$6
    local runtime=$7
    local io_size=$8
    local attempt
    local first_attempt=1
    local latest_attempt

    mkdir -p "${RESULT_ROOT}/runs/${run_id}"
    latest_attempt=$(find "${RESULT_ROOT}/runs/${run_id}" -maxdepth 1 -mindepth 1 \
        -type d -name 'attempt_*' -printf '%f\n' | \
        sed -n 's/^attempt_0*\([0-9][0-9]*\)$/\1/p' | sort -n | tail -n 1)
    if [[ -n $latest_attempt ]]; then
        first_attempt=$((10#$latest_attempt + 1))
    fi
    for attempt in $(seq "$first_attempt" "$((first_attempt + MAX_ATTEMPTS - 1))"); do
        if run_attempt "$run_id" "$experiment" "$implementation" "$branch" \
            "$commit" "$distribution" "$runtime" "$io_size" "$attempt"; then
            return 0
        fi
        stop_attempt_processes
        sleep 10
    done
    status "ABORT ${run_id}: all ${MAX_ATTEMPTS} attempts failed"
    return 1
}

mkdir -p "${RESULT_ROOT}/runs"
touch "${RESULT_ROOT}/known_hosts"
: > "${RESULT_ROOT}/campaign_status.log"
ORIGINAL_COMMIT=$(git -C "$WORKSPACE" rev-parse HEAD)
ORIGINAL_REF=$(git -C "$WORKSPACE" symbolic-ref --quiet --short HEAD || true)
[[ -n $ORIGINAL_REF ]] || ORIGINAL_REF=$ORIGINAL_COMMIT

if ! prepare_host; then
    status 'PRECHECK FAILED'
    printf 'PRECHECK_FAILED\n' > "${RESULT_ROOT}/campaign_status.txt"
    printf 'Host precheck failed; see campaign_status.log and verify KVM, QEMU/image exclusivity, exact commits, and clean tracked source.\n' >&2
    exit 1
fi

status 'CAMPAIGN START'

run_one A1_zipf099_1800_global A 'V4 Global' hotcold/v4 \
    032d29b83e3906593ef9f79e5d2739bdab27ef5e zipf:0.99 1800 '' || exit 1
run_one A2_zipf099_1800_classgc A 'V4 ClassGC' hotcold/v4-classgc \
    d067a64de2974949807a6a6652cf2a08a76d1c4c zipf:0.99 1800 '' || exit 1
run_one B1_zipf099_fixed_global B 'V4 Global' hotcold/v4 \
    032d29b83e3906593ef9f79e5d2739bdab27ef5e zipf:0.99 '' 16G || exit 1
run_one B2_zipf099_fixed_classgc B 'V4 ClassGC' hotcold/v4-classgc \
    d067a64de2974949807a6a6652cf2a08a76d1c4c zipf:0.99 '' 16G || exit 1
run_one C1_zipf099_3600_classgc_noage C 'V4 ClassGC without age' \
    hotcold/v4-classgc-noage ea289e975127f2e63e510c0ce4ed22081ffec293 \
    zipf:0.99 1800 '' || exit 1
run_one D1_uniform_1800_global D 'V4 Global' hotcold/v4 \
    032d29b83e3906593ef9f79e5d2739bdab27ef5e uniform 1800 '' || exit 1
run_one D2_uniform_1800_classgc D 'V4 ClassGC' hotcold/v4-classgc \
    d067a64de2974949807a6a6652cf2a08a76d1c4c uniform 1800 '' || exit 1
run_one E1_zipf070_1800_global E 'V4 Global' hotcold/v4 \
    032d29b83e3906593ef9f79e5d2739bdab27ef5e zipf:0.7 1800 '' || exit 1
run_one E2_zipf070_1800_classgc E 'V4 ClassGC' hotcold/v4-classgc \
    d067a64de2974949807a6a6652cf2a08a76d1c4c zipf:0.7 1800 '' || exit 1
run_one F1_zipf120_1800_global F 'V4 Global' hotcold/v4 \
    032d29b83e3906593ef9f79e5d2739bdab27ef5e zipf:1.2 1800 '' || exit 1
run_one F2_zipf120_1800_classgc F 'V4 ClassGC' hotcold/v4-classgc \
    d067a64de2974949807a6a6652cf2a08a76d1c4c zipf:1.2 1800 '' || exit 1
run_one G1_zipf120_1800_classgc_noage G 'V4 ClassGC without age' \
    hotcold/v4-classgc-noage ea289e975127f2e63e510c0ce4ed22081ffec293 \
    zipf:1.2 1800 '' || exit 1

python3 "${RESULT_ROOT}/summarize.py" "$RESULT_ROOT" \
    > "${RESULT_ROOT}/summarize.log" 2>&1 || {
        status 'SUMMARIZE FAILED'
        printf 'RUNS_COMPLETE_SUMMARY_FAILED\n' > "${RESULT_ROOT}/campaign_status.txt"
        exit 1
    }

status 'CAMPAIGN COMPLETE'
printf 'COMPLETE\n' > "${RESULT_ROOT}/campaign_status.txt"
printf 'Campaign complete: %s\n' "$RESULT_ROOT"
