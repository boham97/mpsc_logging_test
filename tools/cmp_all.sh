#!/bin/bash
# C++ 로깅 라이브러리 벤치마크(Quill, spdlog)와 logx 를 같은 조건·같은 머신에서 차례로 돌린다.
# 사용법: tools/save_result.sh cmp_bench tools/cmp_all.sh [iterations=10000]
#   REF=0 tools/cmp_all.sh ...  → Quill/spdlog 원본은 받지도 돌리지도 않고 logx 만 잰다
#   iterations: hot path 지연의 스레드당 반복 횟수 (Quill 기본 10,000; 반복마다 약 2.1ms 쉼)
# 결과 파일들은 build/cmp_run/ 에서 만들어지고 지워진다.
set -e
cd "$(dirname "$0")/.."
IT=${1:-10000}
make -s cmp_bench
REF=${REF:-1}
[ "$REF" = 1 ] && tools/ref_build.sh
ROOT=$PWD
RUN=build/cmp_run
rm -rf $RUN && mkdir -p $RUN/logs && cd $RUN
LOGX="lflog lflog_text async sync_buf"

for sc in numbers string; do
    echo "=================== hot path latency: $sc ==================="
    if [ "$REF" = 1 ]; then
        $ROOT/build/ref/quill_hot $sc $IT
        $ROOT/build/ref/spdlog_hot $sc $IT
    fi
    for k in $LOGX; do $ROOT/cmp_bench latency $k $sc $IT; done
    rm -f *.log
done

echo "=================== backend throughput (4M) ==================="
if [ "$REF" = 1 ]; then
    echo "Quill"; $ROOT/build/ref/quill_throughput
    echo "spdlog async"; $ROOT/build/ref/spdlog_throughput
fi
for k in $LOGX; do $ROOT/cmp_bench throughput $k; done
rm -f *.log

echo "=================== spdlog bench.cpp (250000, 4 threads) ==================="
[ "$REF" = 1 ] && $ROOT/build/ref/spdlog_bench 250000 4 2>&1 | grep -E "Single|Multi|basic_|level-off"
for k in $LOGX sync_direct; do $ROOT/cmp_bench spdlog $k 250000 4; done
cd $ROOT && rm -rf $RUN
