#!/bin/bash
# 사용법: tools/save_result.sh <이름> <명령...>
#   명령을 실행해 화면에 그대로 보여 주고, results/YYYY-MM-DD_HHMMSS_<이름>.md 에
#   날짜·코드(커밋, 커밋 안 된 변경 여부)·환경·명령·원본 출력을 남긴다.
#   "해석" 절은 비워 두므로, 결과를 보고 채운 뒤 커밋한다.
# 예: tools/save_result.sh bench_sat ./bench 8 200000
set -o pipefail
cd "$(dirname "$0")/.." || exit 1
[ $# -ge 2 ] || { echo "사용법: $0 <이름> <명령...>" >&2; exit 2; }
name=$1
shift

mkdir -p results
out="results/$(date +%F_%H%M%S)_${name}.md"
git_() { git -c safe.directory="$PWD" "$@" 2>/dev/null; } # sudo 로 돌려도 읽히게
commit=$(git_ rev-parse --short HEAD)
dirty=$(git_ status --porcelain --untracked-files=no | grep -q . && echo " + 커밋 안 된 변경")
cpu=$(lscpu 2>/dev/null | sed -n 's/^Model name: *//p' | head -1)
clk=$(cat /sys/devices/system/clocksource/clocksource0/current_clocksource 2>/dev/null)

tmp=$(mktemp)
"$@" 2>&1 | tee "$tmp"
rc=${PIPESTATUS[0]}

{
    echo "# $name"
    echo
    echo "- 날짜: $(date '+%F %T %z')"
    echo "- 코드: \`$commit\`$dirty"
    echo "- 환경: $cpu, $(nproc) CPU, Linux $(uname -r), clocksource=$clk"
    echo "- 명령: \`$*\`"
    echo "- 종료 코드: $rc"
    echo
    echo "## 해석"
    echo
    echo "(작성 전)"
    echo
    echo "## 원본 출력"
    echo
    echo '```'
    cat "$tmp"
    echo '```'
} > "$out"
rm -f "$tmp"
[ -n "$SUDO_USER" ] && chown "$SUDO_USER" results "$out" 2>/dev/null
echo "저장: $out" >&2
exit "$rc"
