#!/bin/bash
# sudo tools/run_c2c.sh [시나리오=string] [횟수=3] [iterations=10000]
#   cmp_bench latency lflog <시나리오> 를 여러 번 perf c2c 로 기록해서
#   호출 스레드와 로깅 스레드가 같은 캐시 라인을 주고받는지(TODO 2a) 본다.
#   빠른 실행과 느린 실행이 섞이므로 run_bt_cmp.sh 처럼 여러 번 돌린다.
#   결과는 tools/save_result.sh 로 results/<날짜>_c2c_<시나리오>_<n>.md 에 남는다.
#   perf.data 는 build/c2c/ 에 두었다가 보고서를 만든 뒤 지운다 (커밋하지 않음).
#
# AMD(이 머신, Zen3)는 IBS(ibs_op)로 샘플링한다. Intel 의 HITM 대신
# "Peer" (다른 코어 캐시에서 가져온 load) 열을 본다. 그래서 -d peer 로 보고한다.
# 보고서가 길어서 앞 C2C_LINES 줄(기본 400)만 남긴다.
set -e
cd "$(dirname "$0")/.."

if [ "$1" = "--one" ]; then
    # save_result.sh 가 감싸서 부르는 1회 실행: 기록 → 보고서
    data=$2
    shift 2
    perf c2c record -o "$data" -- ./cmp_bench latency lflog "$@"
    if grep -q AuthenticAMD /proc/cpuinfo; then disp=peer; else disp=tot; fi
    echo
    echo "===== perf c2c report --stdio -d $disp (앞 ${C2C_LINES:-400}줄) ====="
    perf c2c report -i "$data" --stdio -d "$disp" 2>&1 | head -n "${C2C_LINES:-400}"
    exit 0
fi

SC=${1:-string}
N=${2:-3}
IT=${3:-10000}
[ "$(id -u)" -eq 0 ] || { echo "sudo 로 실행: sudo $0 $*" >&2; exit 1; }
[ -x ./cmp_bench ] || { echo "먼저 make cmp_bench" >&2; exit 1; }
mkdir -p build/c2c
for i in $(seq 1 "$N"); do
    data="build/c2c/perf_${SC}_$i.data"
    tools/save_result.sh "c2c_${SC}_$i" tools/run_c2c.sh --one "$data" "$SC" "$IT"
    rm -f "$data"
done
rm -f *.log
