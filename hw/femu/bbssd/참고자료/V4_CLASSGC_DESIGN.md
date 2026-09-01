# V4 Class-aware GC 핵심 의미

## Cold line age

Cold victim의 `age`는 NAND page가 program된 실제 시간이 아니다.
그 line에 마지막 Host write가 기록된 후 몇 개의 Host 4 KiB page write가 지났는지를 나타내는 **Host-write logical age**다.

```c
age = line->last_host_write_seq == 0 ? ssd->host_write_seq :
      ssd->host_write_seq - line->last_host_write_seq;
```

- Host write가 해당 line에 page를 기록할 때만 `last_host_write_seq`를 갱신한다.
- GC relocation은 Host access가 아니므로 이 값을 갱신하지 않는다.
- measurement reset은 physical mapping과 line 상태를 유지하고 이 값만 0으로 초기화한다.
- 값이 0인 line은 현재 측정 구간에서 Host write가 없었던 line이므로 `age = host_write_seq`로 계산한다.

Cold score의 `age × ipc`는 모든 line의 크기가 같은 현재 geometry에서 `age × invalid_ratio`를 비교하는 것과 같다.

## Pool ownership

`current_hot_lines`/`current_cold_lines`는 초기 pool 비율이 아니라 현재 `line->data_class`를 직접 세어 계산한다.
borrowing으로 class가 변경된 line도 현재 owner에 포함된다.
GC pressure 계산과 final stats는 같은 helper를 사용한다.

```text
current_hot_lines + current_cold_lines = total lines
```

`pool_ownership_invariant=PASS`는 이 관계가 유지되었음을 뜻한다.
