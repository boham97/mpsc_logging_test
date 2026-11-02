#!/bin/bash
# sudo tools/run_bt.sh  — lf_drv 를 두 조건으로 돌리며 lf_offcpu.bt 로 추적한다.
# 결과: bt_sat.txt (쉬지 않고), bt_burst.txt (1000줄마다 2ms 휴식). 저장소 루트에서 make lf_drv 후 실행.
set -e
cd "$(dirname "$0")/.."
[ -x ./lf_drv ] || { echo "먼저 make lf_drv" >&2; exit 1; }
bpftrace tools/lf_offcpu.bt -o bt_sat.txt -c "./lf_drv 8 200000"
bpftrace tools/lf_offcpu.bt -o bt_burst.txt -c "./lf_drv 8 50000 1000"
chown "${SUDO_USER:-$USER}" bt_sat.txt bt_burst.txt lf_drv.log 2>/dev/null || true
echo "완료: bt_sat.txt, bt_burst.txt"
