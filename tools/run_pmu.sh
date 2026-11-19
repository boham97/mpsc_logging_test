#!/bin/bash
# sudo tools/run_pmu.sh [시나리오=string] [횟수=3] [iterations=10000] [간격ms=500]
#   cmp_bench latency lflog <시나리오> 를 여러 번 perf stat 으로 CPU 별·시간 간격별로 센다 (TODO 2a).
#   perf c2c 가 Zen3 에서 안 돼서(results/2026-11-19_104852_c2c_string_1.md) PMU 이벤트로 대신 본다.
#     dmnd_ccx = ls_dmnd_fills_from_sys.int_cache: 같은 CCX 의 L3 나 "다른 코어 L2" 에서 채운 demand load.
#                링이 작으니 대부분 다른 코어가 방금 쓴 줄을 가져온 것으로 본다 (L3 적중도 섞인 근사치).
#     dmnd_l2 / dmnd_mem: 자기 L2 / DRAM 에서 채움.  any_ccx: dmnd_ccx + 하드웨어·소프트웨어 프리페치.
#     cycles 를 같이 세서 CPU 별 실제 클럭(GHz)도 본다. 노트북이라 코어 클럭이 1~4GHz 를 오간다.
#   CPU 배치: 메인 0, 작업 스레드 i → CPU i, 로깅 스레드 5. 이 머신은 (0,1)(2,3)(4,5) 가 SMT 짝이다.
#   -a 로 CPU 0~5 전체를 세므로 sudo 가 필요하다 (perf_event_paranoid=2). 사용자 모드(:u)만 센다.
#   결과: results/<날짜>_pmu_<시나리오>_<n>.md (요약). perf stat CSV 원본은 build/pmu/ 에 남긴다(커밋 안 함).
#   Zen3 는 NMI 워치독이 카운터 하나를 쓰므로 이벤트를 5개로 맞췄다 (늘리면 다중화돼 정확도가 떨어진다).
set -e
cd "$(dirname "$0")/.."
EV=cycles:u,ls_dmnd_fills_from_sys.lcl_l2:u,ls_dmnd_fills_from_sys.int_cache:u,ls_dmnd_fills_from_sys.mem_io_local:u,ls_any_fills_from_sys.int_cache:u

if [ "$1" = "--one" ]; then
    # save_result.sh 가 감싸서 부르는 1회 실행: 세기 → 요약
    csv=$2 iv=$3
    shift 3
    perf stat -a -A -C 0-5 -I "$iv" -x, -e "$EV" -o "$csv" -- ./cmp_bench latency lflog "$@"
    echo
    echo "perf stat CSV: $csv"
    echo
    python3 tools/pmu_summary.py "$csv"
    exit 0
fi

SC=${1:-string}
N=${2:-3}
IT=${3:-10000}
IV=${4:-500}
[ "$(id -u)" -eq 0 ] || { echo "sudo 로 실행: sudo $0 $*" >&2; exit 1; }
[ -x ./cmp_bench ] || { echo "먼저 make cmp_bench" >&2; exit 1; }
mkdir -p build/pmu
echo "run_pmu: 시작 — cmp_bench latency lflog $SC $IT × ${N}회, ${IV}ms 간격 (1회 약 45초)" >&2
for i in $(seq 1 "$N"); do
    csv="build/pmu/$(date +%F_%H%M%S)_${SC}_$i.csv"
    tools/save_result.sh "pmu_${SC}_$i" tools/run_pmu.sh --one "$csv" "$IV" "$SC" "$IT"
done
[ -n "$SUDO_USER" ] && chown -R "$SUDO_USER" build/pmu
rm -f *.log
