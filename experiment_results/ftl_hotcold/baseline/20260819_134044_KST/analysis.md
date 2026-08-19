# Corrected baseline 분석

## 결과

무분리 baseline의 WAF는 `7.949892`이며 300초 동안 GC가 `4,814`회 발생했다. GC page write `69,005,287`을 GC count로 나눈 평균 GC copy는 `14,334.293103 pages/GC`로, 약 `55.99 MiB/GC`이다. `NAND writes = Host writes + GC writes` 불변식은 PASS다.

## 기존 baseline과 재현성

이전 WAF `7.946914`와 corrected WAF의 차이는 `+0.002978` (`+0.0375%`)에 불과하다. IOPS도 `8255 → 8254`, bandwidth는 모두 `129 MiB/s`여서, marker 2회 추가와 계측 변경이 baseline 거동을 실질적으로 바꾸지 않았다고 볼 수 있다. 이전 결과는 평균 GC copy를 산출할 수 없으므로 공식 비교에서는 corrected run을 사용한다.

## V1 T=1024 참고 비교

V1 T=1024의 WAF는 `7.297968`로 corrected baseline보다 `0.651924`, 상대적으로 `8.20%` 낮다. 평균 GC copy도 `14,334.293103 → 14,151.711918 pages/GC`로 `1.27%` 감소했다. V1 실행은 같은 300초 동안 더 많은 host page를 처리했으므로 절대 write 수보다 WAF와 GC copy 비율을 우선 비교해야 한다.

이번 수치는 각 설정 1회 실행 결과다. 통계적 분산을 주장하려면 동일 조건 반복 실행이 추가로 필요하다.
