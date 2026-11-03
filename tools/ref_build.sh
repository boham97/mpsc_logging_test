#!/bin/bash
# Quill, spdlog 원본 벤치마크를 받아 build/ref/ 에 빌드한다 (cmp_bench 와 같은 머신에서 나란히 돌리려고).
#   build/ref/quill_hot       Quill hot_path_latency 하네스 + Quill (numbers | string)
#   build/ref/quill_throughput Quill backend_throughput 원본 그대로
#   build/ref/spdlog_hot      같은 하네스 + spdlog 비동기 로거
#   build/ref/spdlog_throughput  같은 조건의 spdlog 비동기 처리량
#   build/ref/spdlog_bench    spdlog bench/bench.cpp 원본 그대로
# 버전은 아래 태그로 고정한다. 둘 다 헤더 전용이라 cmake 없이 g++ 로 바로 빌드한다.
set -e
cd "$(dirname "$0")/.."
QUILL_TAG=v13.1.0
SPDLOG_TAG=v1.17.0
CXX=${CXX:-g++}
CXXFLAGS=${CXXFLAGS:--O3 -DNDEBUG -std=c++17}
R=build/ref
mkdir -p $R
[ -d $R/quill ] || git clone -q -c advice.detachedHead=false --depth 1 -b $QUILL_TAG https://github.com/odygrd/quill $R/quill
[ -d $R/spdlog ] || git clone -q -c advice.detachedHead=false --depth 1 -b $SPDLOG_TAG https://github.com/gabime/spdlog $R/spdlog
H=$R/quill/benchmarks/hot_path_latency
$CXX $CXXFLAGS -I$R/quill/include -I$H -o $R/quill_hot tools/ref/quill_hot.cpp -pthread &
$CXX $CXXFLAGS -I$R/quill/include -o $R/quill_throughput $R/quill/benchmarks/backend_throughput/quill_backend_throughput.cpp -pthread &
$CXX $CXXFLAGS -I$R/quill/include -I$H -I$R/spdlog/include -o $R/spdlog_hot tools/ref/spdlog_hot.cpp -pthread &
$CXX $CXXFLAGS -I$R/spdlog/include -o $R/spdlog_throughput tools/ref/spdlog_throughput.cpp -pthread &
$CXX $CXXFLAGS -I$R/spdlog/include -o $R/spdlog_bench $R/spdlog/bench/bench.cpp -pthread &
wait
echo "quill $QUILL_TAG ($(git -C $R/quill rev-parse --short HEAD)), spdlog $SPDLOG_TAG ($(git -C $R/spdlog rev-parse --short HEAD)), $($CXX --version | head -1), $CXXFLAGS"
