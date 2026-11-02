#!/bin/bash
# sudo tools/run_bt.sh  — lf_drv 를 두 조건으로 돌리며 lf_offcpu.bt 로 추적한다.
# 결과는 tools/save_result.sh 로 results/<날짜>_bpf_sat.md, results/<날짜>_bpf_burst.md 에 남는다.
# 저장소 루트에서 make lf_drv 후 실행.
set -e
cd "$(dirname "$0")/.."
[ -x ./lf_drv ] || { echo "먼저 make lf_drv" >&2; exit 1; }
tools/save_result.sh bpf_sat bpftrace tools/lf_offcpu.bt -c "./lf_drv 8 200000"
tools/save_result.sh bpf_burst bpftrace tools/lf_offcpu.bt -c "./lf_drv 8 50000 1000"
rm -f lf_drv.log
