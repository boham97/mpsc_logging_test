#!/bin/bash
# TODO 4번: 비교 조건 맞추기. -O2 / -O3 빌드와 큐 크기를 바꿔 가며 Quill 원본과 같은 라운드 안에서 번갈아 잰다.
# 측정 편차가 크므로(TODO 1번) 라운드를 여러 번 돌리고, 매 라운드에 Quill 을 같이 돌려 머신 상태의 흔들림을 본다.
# 사용법: tools/save_result.sh cmp_opt tools/cmp_opt.sh [rounds=5] [iterations=10000]
#   build/o2/cmp_bench, build/o3/cmp_bench 를 직접 빌드한다 (Makefile 의 -O2 결과물은 건드리지 않음).
#   큐 크기: 16384 슬롯(기본, 약 16MB), 131072(약 128MB, Quill 이 이 측정에서 커진 최종 크기),
#            4194304(약 4GB, 400만 줄이 다 들어가서 큐가 차지 않음)
set -e
cd "$(dirname "$0")/.."
ROUNDS=${1:-5}
IT=${2:-10000}
SRC="examples/cmp_bench.c logger/log_common.c logger/async_logger.c logger/lockfree_logger.c logger/sync_logger.c"
for o in 2 3; do
    mkdir -p build/o$o
    gcc -O$o -g -Wall -Wextra -std=c11 -pthread -o build/o$o/cmp_bench $SRC -pthread -lm
done
[ -x build/ref/quill_hot ] || tools/ref_build.sh
ROOT=$PWD
RUN=build/cmp_opt_run
rm -rf $RUN && mkdir -p $RUN && cd $RUN
echo "gcc: $(gcc --version | head -1)"
echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null), boost: $(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null)"
echo "rounds=$ROUNDS iterations=$IT"

# 결과 한 줄: TAG <조건> <값...>  → 마지막에 모아서 표로 만든다
lat() { # $1=라벨 $2...=명령
    local label=$1; shift
    # 표의 숫자 줄: 첫 번째가 1스레드, 두 번째가 4스레드
    "$@" | awk -v l="$label" '/^ *\| *[0-9]+ +\|/ {n++; gsub(/ /,""); split($0,a,"|");
        print "LAT", l "_" (n==1 ? "1T" : "4T"), a[2], a[3], a[4], a[5], a[6], a[7], a[8]}'
}
thr() {
    local label=$1; shift
    "$@" 2>/dev/null | awk -v l="$label" '/Throughput is/ {print "THR", l, $3}'
    rm -f *.log
}

for r in $(seq 1 "$ROUNDS"); do
    echo "---- round $r ----" >&2
    for sc in numbers string; do
        lat "quill_$sc" $ROOT/build/ref/quill_hot $sc $IT
        lat "O2_lflog_$sc" $ROOT/build/o2/cmp_bench latency lflog $sc $IT
        lat "O3_lflog_$sc" $ROOT/build/o3/cmp_bench latency lflog $sc $IT
        rm -f *.log
    done
    thr quill $ROOT/build/ref/quill_throughput
    for o in 2 3; do
        for cap in 16384 131072 4194304; do
            CMP_LF_CAP=$cap thr "O${o}_lflog_cap$cap" $ROOT/build/o$o/cmp_bench throughput lflog
        done
        thr "O${o}_lflog_text" $ROOT/build/o$o/cmp_bench throughput lflog_text
    done
done | tee raw.txt

echo
echo "=== hot path latency (ns, $ROUNDS 라운드: 중앙값 [최소~최대]) ==="
awk '$1=="LAT"' raw.txt | python3 -c '
import sys, statistics as st
d = {}
for line in sys.stdin:
    f = line.split(); d.setdefault(f[1], []).append([float(x) for x in f[2:]])
hdr = ["p50","p75","p90","p95","p99","p99.9","max"]
print("| 조건 | " + " | ".join(hdr) + " |"); print("|---" * (len(hdr)+1) + "|")
for k, rows in d.items():
    cols = list(zip(*rows))
    print("| %s | %s |" % (k, " | ".join("%g [%g~%g]" % (st.median(c), min(c), max(c)) for c in cols)))
'
echo
echo "=== throughput (M줄/s, $ROUNDS 라운드: 중앙값 [최소~최대]) ==="
awk '$1=="THR"' raw.txt | python3 -c '
import sys, statistics as st
d = {}
for line in sys.stdin:
    f = line.split(); d.setdefault(f[1], []).append(float(f[2]))
print("| 조건 | 중앙값 | 최소 | 최대 | 값 |"); print("|---|---|---|---|---|")
for k, v in d.items():
    print("| %s | %.2f | %.2f | %.2f | %s |" % (k, st.median(v), min(v), max(v), " ".join("%.2f" % x for x in v)))
'
cd $ROOT && rm -rf $RUN
