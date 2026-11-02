#!/bin/bash
# sudo tools/run_bt_cas.sh
#   LFLOG_TEXT 의 슬롯 번호 CAS 경합을 접두어 직접 변환 전(lf_drv_snprintf)과 후(lf_drv)로 비교한다.
#   두 판을 번갈아 3번씩: 추적 없이 한 번(기준 처리량) + bpftrace 로 한 번.
#   결과는 tools/save_result.sh 로 results/<날짜>_bpf_cas.md 하나에 남는다.
# 준비: make lf_drv lf_drv_snprintf
set -e
cd "$(dirname "$0")/.."

ARGS="8 200000 0 text"

if [ "$1" != "--inner" ]; then
    for b in lf_drv lf_drv_snprintf; do
        [ -x "./$b" ] || { echo "먼저 make lf_drv lf_drv_snprintf" >&2; exit 1; }
    done
    command -v bpftrace >/dev/null || { echo "bpftrace 가 없음" >&2; exit 1; }
    [ "$(id -u)" = 0 ] || { echo "sudo 로 실행" >&2; exit 1; }
    exec tools/save_result.sh bpf_cas "$0" --inner
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"; rm -f lf_drv.log' EXIT

for b in lf_drv_snprintf lf_drv; do
    off=$(objdump -d --no-show-raw-insn "./$b" | awk '
        /<enqueue_or_wait>:/ { f = 1; base = strtonum("0x" $1) }
        f && /lock cmpxchg/ { getline; split($1, a, ":"); printf "0x%x", strtonum("0x" a[1]) - base; exit }')
    [ -n "$off" ] || { echo "$b: enqueue_or_wait 에서 lock cmpxchg 를 못 찾음" >&2; exit 1; }
    echo "$b: uprobe enqueue_or_wait+$off"
    sed -e "s#@BIN@#$PWD/$b#" -e "s#@OFF@#$off#" tools/lf_cas.bt > "$tmp/$b.bt"
done
echo "인자: $ARGS"

for rep in 1 2 3; do
    for b in lf_drv_snprintf lf_drv; do
        echo
        echo "=== rep $rep: $b (추적 없음)"
        "./$b" $ARGS
        echo "=== rep $rep: $b (bpftrace)"
        # objdump 로 찾은 명령 경계라 안전하지만, 이 bpftrace 빌드가 경계를 직접 확인하지 못하면
        # 오프셋 uprobe 를 거부하므로 --unsafe 로 허용한다
        bpftrace --unsafe -q "$tmp/$b.bt" -c "./$b $ARGS"
    done
done
