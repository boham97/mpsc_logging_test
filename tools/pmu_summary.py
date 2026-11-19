#!/usr/bin/env python3
"""perf stat -a -A -I <ms> -x, 출력(CSV)을 구간(1T / 4T)·CPU 별로 요약한다. run_pmu.sh 가 부른다.

구간은 CPU 가 바쁜지로 나눈다(cmp_bench 는 작업 스레드 i 를 CPU i 에, 로깅 스레드를 CPU 5 에 고정하고
모두 잠들지 않는다). 사이클이 ACTIVE_GHZ 를 넘으면 그 간격에 그 CPU 가 바빴다고 본다.
  1T = CPU 1 만 바쁨, 4T = CPU 1~4 가 모두 바쁨, 나머지(시작·전환·끝)는 버린다.
fill 이벤트는 1000 사이클당 횟수와 메시지 1줄당 횟수로 적는다. 1T 는 호출 스레드가 busy_wait 로 대부분의
사이클을 쓰므로 1줄당 값이 더 읽기 쉽다. 구간마다 메시지는 MSGS(기본 200000, cmp_bench 의 Total messages)줄.
"""
import sys
from collections import defaultdict

ACTIVE_GHZ = 0.3
SHORT = {
    "cycles": "cyc",
    "ls_dmnd_fills_from_sys.lcl_l2": "dmnd_l2",
    "ls_dmnd_fills_from_sys.int_cache": "dmnd_ccx",
    "ls_dmnd_fills_from_sys.mem_io_local": "dmnd_mem",
    "ls_any_fills_from_sys.int_cache": "any_ccx",
}


def short(ev):
    return SHORT.get(ev.split(":")[0], ev)


def parse(path):
    data = defaultdict(lambda: defaultdict(dict))  # t -> cpu -> ev -> value
    for line in open(path):
        f = line.strip().split(",")
        if len(f) < 5 or not f[1].startswith("CPU"):
            continue
        try:
            t, cpu, v = float(f[0]), int(f[1][3:]), float(f[2])
        except ValueError:  # <not counted> 등
            continue
        data[t][cpu][short(f[4])] = v
    return data


def main():
    data = parse(sys.argv[1])
    msgs = int(sys.argv[2]) if len(sys.argv) > 2 else 200000
    times = sorted(data)
    fills = [e for e in SHORT.values() if e != "cyc"]
    rows = []  # (t, dt, phase, {cpu: {ev: v}})
    prev = 0.0
    for t in times:
        dt = t - prev
        prev = t
        ghz = {c: data[t][c].get("cyc", 0) / dt / 1e9 for c in data[t]}
        act = {c for c, g in ghz.items() if g > ACTIVE_GHZ}
        if {1, 2, 3, 4} <= act:
            ph = "4T"
        elif 1 in act and not act & {2, 3, 4}:
            ph = "1T"
        else:
            ph = "-"
        rows.append((t, dt, ph, data[t]))

    print("## 구간 × CPU 요약 (fill 은 1000 사이클당)")
    print()
    print("| 구간 | CPU | 간격 수 | 평균 GHz | " + " | ".join(fills) + " |")
    print("|---|---|---|---|" + "---|" * len(fills))
    for ph, cpus in (("1T", (1, 5)), ("4T", (1, 2, 3, 4, 5))):
        sel = [r for r in rows if r[2] == ph]
        if not sel:
            print(f"| {ph} | - | 0 | | " + " | ".join("" for _ in fills) + " |")
            continue
        for c in cpus:
            cyc = sum(r[3].get(c, {}).get("cyc", 0) for r in sel)
            sec = sum(r[1] for r in sel)
            vals = [sum(r[3].get(c, {}).get(e, 0) for r in sel) / cyc * 1000 if cyc else 0 for e in fills]
            print(f"| {ph} | {c} | {len(sel)} | {cyc / sec / 1e9:.2f} | " + " | ".join(f"{v:.3f}" for v in vals) + " |")

    print()
    print(f"## 구간 × CPU 요약 (fill 은 메시지 1줄당, 구간마다 {msgs}줄)")
    print()
    print("| 구간 | CPU | " + " | ".join(fills) + " |")
    print("|---|---|" + "---|" * len(fills))
    for ph, cpus in (("1T", (1, 5)), ("4T", (1, 2, 3, 4, 5))):
        sel = [r for r in rows if r[2] == ph]
        for c in cpus:
            vals = [sum(r[3].get(c, {}).get(e, 0) for r in sel) / msgs for e in fills]
            print(f"| {ph} | {c} | " + " | ".join(f"{v:.2f}" for v in vals) + " |")

    print()
    print("## 1T 구간 시계열 (CPU 1 = 호출 스레드, CPU 5 = 로깅 스레드)")
    print()
    print("| t(s) | CPU1 GHz | CPU1 dmnd_ccx | CPU1 dmnd_l2 | CPU5 GHz | CPU5 dmnd_ccx | CPU5 any_ccx |")
    print("|---|---|---|---|---|---|---|")
    for t, dt, ph, d in rows:
        if ph != "1T":
            continue
        out = [f"{t:.1f}"]
        for c, evs in ((1, ("dmnd_ccx", "dmnd_l2")), (5, ("dmnd_ccx", "any_ccx"))):
            cyc = d.get(c, {}).get("cyc", 0)
            out.append(f"{cyc / dt / 1e9:.2f}")
            out += [f"{d[c].get(e, 0) / cyc * 1000:.3f}" if cyc else "-" for e in evs]
        print("| " + " | ".join(out) + " |")


if __name__ == "__main__":
    main()
