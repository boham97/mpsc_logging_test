# 측정 결과

테스트·벤치마크·프로파일 결과를 실행할 때마다 여기에 md 파일 하나로 남긴다. 메인 README의 표는 이 파일들을 요약한 것이다.

## 규칙

- 파일 이름: `YYYY-MM-DD_HHMMSS_<이름>.md` (실행 시각 순으로 정렬된다).
- 내용: 날짜, 코드 상태(커밋, 커밋 안 된 변경 여부), 환경(CPU, 커널, clocksource), 명령, **해석**, **원본 출력**.
- 원본 출력은 손대지 않는다. 숫자를 고치거나 요약하려면 해석 절에 쓴다.
- 자동 저장: `tools/save_result.sh <이름> <명령...>`이 명령을 실행하고 위 형식으로 저장한다. 해석 절은 "(작성 전)"으로 남으니 결과를 보고 채운 뒤 커밋한다.
  - `make test`, `make tsan`, `make results`(대표 측정 세트), `sudo tools/run_bt.sh`(bpftrace)는 모두 이 스크립트로 저장된다.
  - 그 밖의 측정도 `tools/save_result.sh`로 감싸서 돌린다. 예: `tools/save_result.sh bench_busy taskset -c 0-9 ./bench 8 50000 1000 11`
- 대용량 원본(`perf.data` 등)은 커밋하지 않는다. 필요한 요약(`perf report` 출력)만 넣는다.

## 목록

| 파일 | 내용 |
|---|---|
| [2026-11-02_141500_tid_cost](2026-11-02_141500_tid_cost.md) | TID를 인자로 넘겨도 1ns 정도라 API 유지 |
| [2026-11-02_142500_perf_lf_drv](2026-11-02_142500_perf_lf_drv.md) | perf: LFLOG 포화 시 로깅 스레드 69%가 `snprintf`/`vsnprintf` |
| [2026-11-02_143300_bpf_offcpu](2026-11-02_143300_bpf_offcpu.md) | bpftrace 1·2차: 로깅 스레드는 잠들지 않고, 호출자는 `sched_yield` 대기 |
| [2026-11-02_145000_tsc_busy_poll](2026-11-02_145000_tsc_busy_poll.md) | TSC는 p50 개선, `busy_poll`은 1스레드 꼬리·포화 처리량 개선 |
| [2026-11-02_154000_prefix_direct](2026-11-02_154000_prefix_direct.md) | 접두어 직접 변환: LFLOG 포화 처리량 1.8 → 3.3M줄/s |
| [2026-11-02_154600_contention](2026-11-02_154600_contention.md) | 접두어 변경 후 `LFLOG_TEXT` CAS 실패 0.9 → 2.5회/호출, async 경쟁은 그대로 |
| [2026-11-02_155915_fmt_test](2026-11-02_155915_fmt_test.md) | `make test`: fmt_test 90/90 |
| [2026-11-02_155915_bench_test](2026-11-02_155915_bench_test.md) | `make test`: bench 4×5만 검증 OK |
| [2026-11-02_160105_fmt_bench](2026-11-02_160105_fmt_bench.md) | 접두어 변경 후 fmt_bench: render_record 171ns, 메시지 캐시까지 하면 77ns |

이 날 이전의 결과(지연 백분위 표 등)는 원본 출력이 남아 있지 않고 메인 README의 표로만 남아 있다.
