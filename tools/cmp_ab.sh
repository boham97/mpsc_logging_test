#!/bin/bash
# 두 cmp_bench 빌드(변경 전 A, 변경 후 B)를 같은 라운드 안에서 Quill 원본과 번갈아 잰다.
# 세션 사이 편차가 크므로(TODO 1번) 전후 비교는 반드시 같은 세션 안에서 번갈아 돌린다.
# 사용법: tools/save_result.sh <이름> tools/cmp_ab.sh <A의 cmp_bench> <B의 cmp_bench> [rounds=5] [iterations=10000]
#   A 빌드 예: git worktree add /tmp/old <커밋> && make -C /tmp/old cmp_bench
set -e
cd "$(dirname "$0")/.."
A=$(realpath "$1")
B=$(realpath "$2")
ROUNDS=${3:-5}
IT=${4:-10000}
[ -x build/ref/quill_hot ] || tools/ref_build.sh
ROOT=$PWD
RUN=build/cmp_ab_run
rm -rf $RUN && mkdir -p $RUN && cd $RUN
echo "A: $A"
echo "B: $B"
echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null), boost: $(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null)"
echo "rounds=$ROUNDS iterations=$IT"

lat() { # $1=라벨 $2...=명령. 표의 숫자 줄: 첫 번째가 1스레드, 두 번째가 4스레드
    local label=$1; shift
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
        lat "A_lflog_$sc" $A latency lflog $sc $IT
        lat "B_lflog_$sc" $B latency lflog $sc $IT
        rm -f *.log
    done
    thr quill $ROOT/build/ref/quill_throughput
    for v in A B; do
        bin=$A; [ $v = B ] && bin=$B
        thr "${v}_lflog" $bin throughput lflog
        thr "${v}_lflog_text" $bin throughput lflog_text
        thr "${v}_async" $bin throughput async
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
