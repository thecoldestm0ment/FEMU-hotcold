# Phase 5·6 핵심과 테스트 가이드

## 1. 무엇을 구현했나

Phase 5는 host가 LPN을 쓸 때 온도를 갱신하고 기록할 pointer를 선택한다.

```c
ssd_update_lpn_temperature(ssd, lpn);
wpp = ssd_select_write_pointer(ssd, lpn); /* HOT→wp_hot, COLD→wp_cold */
ppa = get_new_page(wpp);
ssd_advance_write_pointer(ssd, wpp);
```

처음 쓰는 LPN은 Cold다. 같은 LPN을 1,024 host-page-write 이내에 다시 쓰면 Hot이다.

Phase 6는 GC가 reverse mapping으로 LPN을 찾고 현재 온도에 맞는 pointer로 이동한다.

```c
lpn = get_rmap_ent(ssd, old_ppa);
wpp = ssd_select_write_pointer(ssd, lpn);
new_ppa = get_new_page(wpp);
ssd_advance_write_pointer(ssd, wpp);
```

GC는 온도를 다시 계산하지 않고 host write counter도 늘리지 않는다.
GC migration 한 page마다 `gc_page_writes`와 `nand_page_writes`만 증가한다.

## 2. “데이터를 파괴한다”는 말의 의미

`/dev/nvme0n1`은 Guest OS에서 보이는 FEMU 시험용 SSD다.
아래 `dd`와 fio는 파일을 만드는 것이 아니라 이 장치의 섹터를 직접 덮어쓴다.
따라서 그 SSD에 파티션, 파일시스템, 필요한 파일이 있었다면 모두 손상될 수 있다.

Guest OS가 설치된 가상 디스크가 `/dev/sda` 또는 `/dev/vda`이고,
`/dev/nvme0n1`이 오직 실험용 FEMU SSD라면 Guest OS 자체는 지워지지 않는다.
장치 이름이 다르거나 필요한 데이터가 있으면 아래 write 명령을 실행하면 안 된다.

## 3. 명령을 어디에서 실행하나

모든 명령을 빌드 직후 같은 셸에 한꺼번에 붙여 넣는 방식이 아니다.

```text
Host 터미널 A: 빌드하고 run-blackbox.sh 실행
Guest 콘솔    : VM 부팅 후 로그인하여 lsblk, dd, fio, nvme 실행
Host 터미널 B: build-femu/log 확인
```

`run-blackbox.sh`는 `-nographic` VM 콘솔을 터미널 A에 연결한다.
따라서 스크립트 실행 후에는 VM이 부팅될 때까지 기다리고 Guest에 로그인해야 한다.
각 fio가 끝난 것을 확인한 다음 다음 명령을 한 줄씩 실행한다.

## 4. Host 터미널 A: 정적 검사와 빌드

다음 명령은 Host의 `/root/workspace/FEMU`에서 실행한다.

```bash
cd /root/workspace/FEMU
git diff --check
rg -n "ssd_update_lpn_temperature|ssd_select_write_pointer" hw/femu/bbssd/ftl.c
ninja -C build-femu qemu-system-x86_64
cp hw/femu/scripts/run-blackbox.sh build-femu/run-blackbox.sh
```

정상 조건:

- `git diff --check`가 아무 오류도 출력하지 않는다.
- `ninja` 마지막에 `Linking target qemu-system-x86_64`가 나온다.
- `ssd_update_lpn_temperature()` 호출은 `ssd_write()`에만 존재한다.
- `ssd_select_write_pointer()` 호출은 host write와 `gc_write_page()`에 존재한다.

## 5. Host 터미널 A: FEMU 실행

Phase 5 배치 위치를 로그로 보기 위해 실험용 marker 로그를 켠다.

```bash
cd /root/workspace/FEMU/build-femu
FEMU_EXP_LOG=1 FEMU_SECRET=PHASE56 ./run-blackbox.sh
```

이 명령은 VM이 종료될 때까지 끝나지 않는 것이 정상이다.
부팅 로그에서 다음 초기 상태를 확인한다.

```text
Write pointers: hot_line=64 cold_line=0 free=126 hot_free=63 cold_free=63
```

VM 로그인이 나타나면 같은 터미널 A는 이제 Guest 콘솔로 사용한다.

## 6. Guest 콘솔: 시험용 SSD인지 확인

아직 write를 실행하지 말고 먼저 다음을 한 줄씩 확인한다.

```bash
lsblk -b -o NAME,SIZE,TYPE,FSTYPE,MOUNTPOINTS
sudo blockdev --getsize64 /dev/nvme0n1
findmnt --source /dev/nvme0n1
```

현재 설정의 정상 결과:

```text
/dev/nvme0n1 크기: 6442450944 bytes = 6 GiB
MOUNTPOINTS: 비어 있음
findmnt: 아무것도 출력하지 않음
```

`/dev/nvme0n1` 또는 그 partition에 mountpoint가 있으면 테스트를 중단한다.
필요한 데이터가 없는 FEMU 시험용 SSD라는 것을 확인한 후에만 다음 단계로 간다.

## 7. Phase 5 시험: 같은 LPN을 두 번 쓰기

아래 한 명령은 4 KiB NAND page 하나, 즉 LPN 0에 marker를 쓴다.
같은 명령을 바로 두 번 실행한다.

```bash
printf PHASE56 | sudo dd of=/dev/nvme0n1 bs=4K count=1 conv=sync oflag=direct
printf PHASE56 | sudo dd of=/dev/nvme0n1 bs=4K count=1 conv=sync oflag=direct
```

첫 write는 LPN 0의 첫 관찰이므로 Cold pointer를 사용한다.
두 번째 write는 update interval이 짧으므로 Hot pointer를 사용한다.

다른 Host 터미널 B에서 확인한다.

```bash
cd /root/workspace/FEMU/build-femu
grep '\[WRITE\] lpn=0' log | head -n 2
```

새로 부팅한 현재 128-line 설정의 예상 형태:

```text
[WRITE] lpn=0 -> ... blk=0  pg=0 ...  첫 write: Cold
[WRITE] lpn=0 -> ... blk=64 pg=0 ...  빠른 rewrite: Hot
```

로그가 없다면 FEMU를 `FEMU_EXP_LOG=1 FEMU_SECRET=PHASE56`로 실행했는지 확인한다.
다른 I/O가 먼저 실행됐으면 정확한 block 번호는 달라질 수 있으므로 새로 부팅해 다시 시험한다.

## 8. Guest 콘솔: GC가 발생할 상태 만들기

먼저 공개된 6 GiB 전체를 순차 write로 채운다.
이 과정이 끝날 때까지 기다린다.

```bash
sudo fio \
  --name=fill \
  --filename=/dev/nvme0n1 \
  --direct=1 \
  --ioengine=libaio \
  --rw=write \
  --bs=1M \
  --iodepth=32 \
  --numjobs=1 \
  --size=6G \
  --group_reporting=1
```

Fill 이후 LPN 0을 다시 두 번 써서 현재 상태를 Hot으로 만들고 추적 대상으로 남긴다.

```bash
printf PHASE56 | sudo dd of=/dev/nvme0n1 bs=4K count=1 conv=sync oflag=direct
printf PHASE56 | sudo dd of=/dev/nvme0n1 bs=4K count=1 conv=sync oflag=direct
```

이전 fill 통계를 출력한 뒤 Phase 6 측정 카운터를 0으로 만든다.

```bash
sudo nvme admin-passthru /dev/nvme0 --opcode=0xef --cdw10=5
```

Guest에는 성공 메시지만 보일 수 있다. 실제 `BBSSD-STATS`는 Host의 `build-femu/log`에 기록된다.

## 9. Guest 콘솔: random write로 GC 발생시키기

LPN 0을 다시 덮어쓰지 않도록 1~5 GiB 구간만 5분 동안 random write한다.

```bash
sudo fio \
  --name=phase6-gc \
  --filename=/dev/nvme0n1 \
  --offset=1G \
  --size=4G \
  --direct=1 \
  --ioengine=libaio \
  --rw=randwrite \
  --bs=16k \
  --iodepth=128 \
  --numjobs=32 \
  --time_based=1 \
  --runtime=300 \
  --random_distribution=zipf:0.99 \
  --group_reporting=1
```

fio가 완전히 끝난 뒤 NAND 작업이 정리될 시간을 조금 주고 통계를 출력·reset한다.

```bash
sleep 2
sudo nvme admin-passthru /dev/nvme0 --opcode=0xef --cdw10=5
```

## 10. Host 터미널 B: Phase 6 결과 확인

GC가 추적 중인 LPN 0을 이동했는지 확인한다.

```bash
cd /root/workspace/FEMU/build-femu
grep '\[GC_MOVE\] lpn=0' log | tail -n 5
```

한 줄 이상 나오면 LPN 0이 실제 `gc_write_page()`를 통과했다는 뜻이다.
5분 후에도 없다면 먼저 전체 GC 발생 여부를 통계로 확인한다.

```bash
grep 'BBSSD-STATS' log | tail -n 1
```

예상 형태:

```text
BBSSD-STATS host_page_writes=... nand_page_writes=... gc_page_writes=... block_erases=... waf=...
```

정상 조건:

```text
host_page_writes > 0
gc_page_writes > 0
block_erases > 0
nand_page_writes = host_page_writes + gc_page_writes
waf = nand_page_writes / host_page_writes
```

아래 명령은 마지막 통계 줄의 핵심 카운터 관계를 자동 검사한다.

```bash
grep 'BBSSD-STATS' log | tail -n 1 | awk '
{
    for (i = 1; i <= NF; i++) {
        split($i, field, "=");
        value[field[1]] = field[2];
    }
    ok = value["gc_page_writes"] > 0 &&
         value["block_erases"] > 0 &&
         value["nand_page_writes"] ==
         value["host_page_writes"] + value["gc_page_writes"];
    print ok ? "PASS: Phase 6 GC counters" : "FAIL: counters를 확인하세요";
}'
```

`gc_page_writes=0`이면 아직 GC가 충분히 발생하지 않은 것이다.
같은 random-write fio를 한 번 더 실행하거나 `--runtime=600`으로 늘린 뒤 다시 확인한다.

## 11. 이 테스트가 확인하는 범위

Phase 5의 첫 Cold write와 빠른 Hot rewrite는 `[WRITE]`의 destination block으로 확인한다.
Phase 6는 GC 이동 로그, source-level helper 호출, GC counter 불변식으로 확인한다.

현재는 Hot/Cold별 GC write counter가 없으므로 장시간 실행 중 borrowing까지 발생한 뒤
각 GC page가 어느 class로 갔는지를 통계 한 줄만으로 완전히 분리해 측정할 수는 없다.
그 검증이 필요하면 정책을 바꾸기 전에 저빈도 class별 debug counter를 별도 단계로 추가해야 한다.
