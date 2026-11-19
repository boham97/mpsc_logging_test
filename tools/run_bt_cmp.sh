#!/bin/bash
# sudo tools/run_bt_cmp.sh [시나리오=string] [횟수=3] [iterations=10000]
#   cmp_bench latency lflog <시나리오> 를 여러 번 돌리며 lf_offcpu.bt 로 추적한다 (TODO 1·2).
#   빠른 반복과 느린 반복의 비율이 실행마다 달라서, 실행별 백분위와 bpf 숫자를 나란히 보려고 여러 번 돌린다.
#   결과는 tools/save_result.sh 로 results/<날짜>_bpf_cmp_<시나리오>_<n>.md 에 남는다.
#   cmp_bench 는 busy_poll = 1 이라 로깅 스레드 futex 는 0 이어야 정상이다.
set -e
cd "$(dirname "$0")/.."
SC=${1:-string}
N=${2:-3}
IT=${3:-10000}
[ -x ./cmp_bench ] || { echo "먼저 make cmp_bench" >&2; exit 1; }
for i in $(seq 1 "$N"); do
    tools/save_result.sh "bpf_cmp_${SC}_$i" bpftrace tools/lf_offcpu.bt -c "./cmp_bench latency lflog $SC $IT"
done
rm -f *.log
