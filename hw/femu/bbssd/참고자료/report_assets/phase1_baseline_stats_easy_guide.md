# Phase 1 최소 구현 아주 쉽게 이해하기

## 1. 한 문장 요약

Phase 1에서는 Hot/Cold 기능을 만들지 않았다.

기존 FTL의 NAND write와 erase 작업량을 알기 위해 최소 counter를 추가했다.

```text
Host가 쓴 page 수
GC가 옮겨 쓴 page 수
NAND가 실제로 쓴 전체 page 수
NAND block을 erase한 횟수
```

자동차로 비유하면 엔진을 바꾼 것이 아니라 주행거리를 확인할 수 있는
계기판을 만든 것이다.

---

# 2. 왜 이 숫자가 필요한가?

Hot/Cold FTL의 목표는 GC가 복사하는 valid page를 줄이는 것이다.

그 효과를 확인하려면 Hot/Cold 적용 전과 적용 후의 WAF를 비교해야 한다.

```text
WAF = NAND page writes / Host page writes
```

기존 값을 측정하지 않으면 Hot/Cold 적용 후 WAF가 정말 감소했는지 알 수 없다.
그래서 배치 정책을 바꾸기 전에 최소한의 baseline counter부터 추가했다.

---

# 3. 추가한 카운터

## `host_page_writes`

Host 요청 때문에 기록한 logical page 수다.

예를 들어 `ssd_write()`가 LPN 10, 11, 12를 처리하면:

```text
host_page_writes = 3
```

NVMe command 개수가 아니라 실제 LPN loop가 처리한 page 수를 센다.

## `gc_page_writes`

GC가 victim line의 valid page를 다른 PPA로 옮긴 횟수다.

예를 들어 GC가 valid page 20개를 옮기면:

```text
gc_page_writes = 20
```

현재 코드에서 GC page write 수와 valid page migration 수는 같은 뜻이다.

## `nand_page_writes`

Host와 GC를 합친 전체 NAND page program 수다.

```text
nand_page_writes
= host_page_writes + gc_page_writes
```

이 관계가 Phase 1의 가장 중요한 불변식이다.

## `block_erases`

GC가 NAND block 하나를 free 상태로 되돌린 횟수다. WAF 계산에는 넣지 않고
erase 작업량과 편향을 확인할 때 별도로 사용한다.

---

# 4. WAF 계산 예제

## GC가 없을 때

```text
host_page_writes = 100
gc_page_writes   = 0
nand_page_writes = 100

WAF = 100 / 100 = 1.0
```

Host가 요청한 만큼만 NAND에 썼다.

## GC가 valid page 20개를 옮겼을 때

```text
host_page_writes = 100
gc_page_writes   = 20
nand_page_writes = 120

WAF = 120 / 100 = 1.2
```

Host는 100 page를 요청했지만 GC 때문에 NAND는 120 page를 기록했다.

---

# 5. `ftl.h`에서 한 일

`struct ssd` 안에 다음 필드를 직접 추가했다.

```c
uint64_t host_page_writes;
uint64_t nand_page_writes;
uint64_t gc_page_writes;
uint64_t block_erases;
```

별도의 복잡한 통계 구조체를 만들지 않았다.

전역 변수가 아니라 `struct ssd` 안에 둔 이유는 각 SSD가 자기 통계를
가지도록 하기 위해서다.

출력 함수 선언도 하나만 추가했다.

```c
void ssd_print_stats(struct ssd *ssd);
```

---

# 6. `ssd_init()`에서 한 일

SSD가 시작될 때 카운터를 0으로 만든다.

```text
host_page_writes = 0
nand_page_writes = 0
gc_page_writes = 0
block_erases = 0
```

따라서 현재 통계 측정 구간은 다음과 같다.

```text
SSD 초기화
-> Host I/O와 GC 실행
-> 현재까지 계속 누적
```

실행 중에는 `FEMU_RESET_ACCT` 명령으로 출력 후 다시 0부터 측정할 수 있다.

---

# 7. Host write는 어디에서 세는가?

함수:

```text
ssd_write()
```

`ssd_write()`는 요청 범위의 LPN을 하나씩 처리한다.

한 LPN에 대한 NAND write 처리가 끝날 때 다음 두 값을 증가시킨다.

```text
host_page_writes++
nand_page_writes++
```

Host write 하나는 동시에 실제 NAND write 하나이기 때문에 두 숫자가 같이
증가한다.

GC counter는 증가시키지 않는다.

---

# 8. GC write는 어디에서 세는가?

함수:

```text
gc_write_page()
```

GC가 valid page 하나를 새 PPA로 옮기면 다음 두 값을 증가시킨다.

```text
gc_page_writes++
nand_page_writes++
```

GC migration도 NAND에는 실제 page program을 발생시키므로
`nand_page_writes`가 같이 증가한다.

하지만 GC는 새로운 Host 요청이 아니므로 `host_page_writes`는 증가시키지
않는다.

---

# 9. GC delay를 꺼도 GC write를 세는 이유

`enable_gc_delay`는 GC latency를 시간에 반영할지 결정한다.

GC delay를 꺼도 다음 작업은 그대로 수행된다.

- valid page 이동
- 새 mapping 등록
- 새 physical page 사용

따라서 GC counter는 `if (enable_gc_delay)` 밖에서 증가시킨다.

그렇지 않으면 GC delay를 끈 실험에서 실제 page migration이 있었는데도
`gc_page_writes`가 0으로 측정된다.

---

# 10. 출력 함수

추가한 함수:

```text
ssd_print_stats()
```

이 함수는 다음 값을 한 줄로 출력한다.

```text
host_page_writes
nand_page_writes
gc_page_writes
block_erases
WAF
```

출력 예:

```text
BBSSD-STATS host_page_writes=1000
nand_page_writes=1250
gc_page_writes=250
block_erases=64
waf=1.250000
```

실제 출력은 한 줄이며 위 예제는 읽기 쉽게 줄을 나눈 것이다.

Host write가 아직 하나도 없으면 0으로 나눌 수 없으므로 다음처럼 출력한다.

```text
waf=N/A
```

---

# 11. 출력과 초기화는 언제 하는가

`ssd_print_stats()`는 기존 `FEMU_RESET_ACCT` admin command에 연결했다.
Guest OS에서 fio가 끝난 뒤 다음 명령을 실행한다.

```bash
sudo nvme admin-passthru /dev/nvme0 --opcode=0xef --cdw10=5
```

이 명령을 받으면 non-FDP Blackbox 경로에서 다음 순서로 동작한다.

1. 방금 끝난 측정 구간의 counter와 WAF를 출력한다.
2. 네 counter를 모두 0으로 만든다.
3. 기존 FEMU poller accounting counter도 원래 코드대로 초기화한다.

출력보다 초기화를 먼저 하면 측정값이 사라지므로 반드시 출력이 먼저다.
또한 FDP 경로의 accounting 동작을 바꾸지 않기 위해 Phase 1 counter의
출력과 초기화는 `ssd->fdp_enabled == false`일 때만 수행한다.

fio가 실행 중일 때 초기화하면 측정 구간이 중간에서 잘릴 수 있다. 따라서
fio 종료 후 I/O가 멈춘 상태에서 이 명령을 사용한다.

---

# 12. 이번 최소 구현에서 제거한 것

다음 기능은 Phase 1 최소 범위에서 제외했다.

- GC 수행 횟수
- 별도의 `struct bbssd_stats`
- accounting helper 함수
- `qatomic`
- runtime reset 함수
- free/victim/full line count 출력

이 기능들이 잘못된 것은 아니지만 현재 baseline 측정에 꼭 필요하지
않으므로 제거했다. 별도 reset helper는 만들지 않고 기존
`FEMU_RESET_ACCT` 경계에서 counter를 직접 0으로 초기화한다.

---

# 13. 바뀌지 않은 기존 FTL 동작

다음 동작은 전혀 바꾸지 않았다.

- Phase 1 계측 자체는 write pointer 선택을 변경하지 않음
- 새 PPA 할당 방법
- mapping table 갱신
- reverse mapping 갱신
- page valid/invalid 처리
- victim line 선택
- GC 정책
- TRIM
- NAND timing
- `ssd_advance_status()` 호출
- FDP write/GC/TRIM 경로

즉 Phase 1은 배치 정책을 바꾼 것이 아니라 숫자만 누적한다.

---

# 14. 코드가 맞는지 확인하는 핵심 식

언제나 다음 관계가 맞아야 한다.

```text
nand_page_writes
= host_page_writes + gc_page_writes
```

예를 들어:

```text
host = 500
gc   = 40
nand = 540
```

정상이다.

다음처럼 나오면 증가 위치를 다시 확인해야 한다.

```text
host = 500
gc   = 40
nand = 530
```

---

# 15. 아직 하지 말아야 할 것

Phase 1 측정이 확인되기 전에는 다음 기능을 시작하지 않는다.

- LPN별 write history
- Hot/Cold 판정
- `wp_hot`, `wp_cold`
- Hot/Cold line pool
- GC migration의 temperature 분류

먼저 작은 workload에서 세 page-write counter의 합 관계, WAF와
`block_erases`를 확인한 뒤 다음 단계로 넘어간다.
