# V2 correctness 분석

V2 classifier는 계획한 상태 전이를 그대로 보였다. Fill 이후 LPN0의 오래된 history는 첫 marker write에서 decay되어 access 1로 시작했고, 이어지는 빠른 rewrite에서 COLD access 2/short 1, HOT access 3/short 2 순으로 승격됐다.

D=8 correctness 설정에서 16 host page writes가 지난 뒤 같은 LPN은 epoch 196608에서 196610으로 이동했다. 기존 access 3과 short 2가 두 번 right shift된 뒤 현재 write가 반영되어 access 1/short 1 COLD가 됐다. 이어지는 두 빠른 rewrite에서 access 2/short 2 COLD, access 3/short 3 HOT으로 재승격되어 decay 후 재학습도 확인됐다.

TRIM 이후 다음 write는 access 1, short 0, interval 0의 COLD로 처리됐다. 따라서 mapping invalidation뿐 아니라 classifier history도 함께 초기화된다.

GC correctness workload에서는 LPN0이 다섯 번 이동했다. 다섯 로그 모두 state COLD, access 1, short 0, last sequence 2566169, decay epoch 320771로 동일했다. Host write 없이 GC만 반복됐으므로 GC relocation이 history를 학습하거나 decay하지 않는다는 직접적인 증거다.

두 30초 workload의 WAF는 성능 비교 값이 아니다. correctness용 D=8과 누적된 동일 device state를 사용했고 각 phase의 목적은 GC 및 불변식 발생 확인뿐이다. 공식 V2 성능 비교는 fresh device에서 기본 D=65536, T=1024, A=3, C=2, decay ON으로 별도 300초 실행해야 한다.
