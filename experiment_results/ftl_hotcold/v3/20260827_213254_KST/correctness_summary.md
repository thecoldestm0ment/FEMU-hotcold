# V3 correctness 검증 요약

## 판정

V3의 핵심 routing, erase-survival 보정, TRIM reset, 입력 검증 및 counter invariant가 모두 의도대로 동작했다.

## Host rewrite interval routing

기본 설정 `T_fast=1024`, `T_slow=4096`, `R=1`에서 marker LPN trace로 다음을 확인했다.

| 조건 | 관찰된 reason | 결과 |
|---|---|---|
| First write | `FIRST_WRITE` | COLD |
| interval 1, 961 | `FAST_REWRITE` | HOT |
| interval 3841 | `BOUNDARY_HOT`, survival=0 | HOT |
| interval 4111 | `SLOW_REWRITE` | COLD |
| TRIM 후 첫 write | `FIRST_WRITE` | COLD |

상세 원문은 `correctness_routing/classification_trace.txt`에 있다. 이 correctness workload의 marker는 최종 performance run에는 사용하지 않았다.

## Erase-survival feedback

GC와 경계 판정을 한 번의 짧은 trace에서 확실히 관찰하기 위해 correctness 전용으로 `T_slow=2000000`을 사용했다. LPN 0을 경계 HOT으로 만든 뒤 해당 LPN을 다시 host-write하지 않고 GC를 유도했다.

- 최초 경계 판정: `BOUNDARY_HOT`, `survival_before=0`
- LPN 0의 current version이 source block erase를 34회 생존
- 다음 host write: `ERASE_SURVIVAL_COLD`, `survival_before=34`, `R=1`
- GC relocation은 host rewrite history를 학습시키지 않고 survival feedback만 증가시킴
- 최종 `counter_invariant=PASS`

원문은 `correctness_survival_valid/gc_trace.txt`에 있다.

## 입력 검증

다음 잘못된 설정은 QEMU 시작 시 즉시 실패했다.

- `FEMU_HOT_REWRITE_WINDOW=0`
- `FEMU_HOT_BOUNDARY_WINDOW <= FEMU_HOT_REWRITE_WINDOW`
- `FEMU_ERASE_SURVIVAL_THRESHOLD=0`

## 제외한 setup attempt

`correctness_default`는 marker 형식 점검 전의 boot-only setup이고, `correctness_survival`은 이전 launcher copy가 correctness 전용 boundary override를 전달하지 않아 제외했다. 코드 수정으로 결과를 맞추지 않고 launcher를 현재 branch의 기존 버전과 동기화한 뒤 fresh FEMU에서 `correctness_survival_valid`를 다시 실행했다. 두 제외 디렉터리는 감사 추적을 위해 삭제하지 않았다.
