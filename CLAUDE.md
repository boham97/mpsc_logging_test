# logx

멀티스레드 C 로깅 라이브러리. 같은 문제를 세 설계로 구현하고 성능을 잰다. 지금 작업의 중심은 `lockfree_logger`(지연 포맷팅, `LFLOG_*`)를 Quill 수준으로 빠르게 만드는 것이다.

- 설계·사용법·벤치마크 요약: `README.md` (길다. 필요한 절만 읽는다)
- 남은 과제와 상태: `TODO.md` — 작업을 시작하기 전에 먼저 읽는다
- 측정 결과 원본과 목록: `results/README.md`

## 코드 위치

| 파일 | 내용 |
|---|---|
| `logger/log_common.[ch]` | 레벨, 접두어 직접 변환(`log_format_prefix`), `%g` 직접 변환(`log_format_g`), `write_all` |
| `logger/lockfree_logger.[ch]` | MPSC 링 + futex + TSC + 지연 포맷팅(`encode_record` / `render_record` / `render_message`) |
| `logger/async_logger.[ch]` | 뮤텍스 더블 버퍼 |
| `logger/sync_logger.[ch]` | 로깅 스레드 없음 (`O_APPEND` 직접 / 공유 버퍼) |
| `examples/fmt_test.c` | 지연 포맷팅·`log_format_g` 결과가 `snprintf`와 같은지 (회귀 테스트) |
| `examples/fmt_bench.c` | 로깅 스레드 포맷팅 비용. `lockfree_logger.c`를 `#include`해서 static 함수를 직접 잰다 |
| `examples/cmp_bench.c` | Quill `hot_path_latency`·`backend_throughput`, spdlog `bench.cpp`와 같은 조건 |
| `examples/bench.c`, `lf_drv.c` | 처리량·호출 지연 + 결과 파일 검증 / perf·bpftrace용 `LFLOG`만 돌리는 드라이버 |
| `tools/` | `save_result.sh`, `cmp_ab.sh`(전후 A/B), `cmp_all.sh`·`ref_build.sh`(Quill v13.1.0·spdlog v1.17.0 원본), `cmp_opt.sh`, `*.bt`, `perf_classify.py` |

## 빌드·테스트

```sh
make              # gcc -O2 -std=c11, 결과물은 저장소 루트에 생김 (gitignore 됨)
make test         # fmt_test + bench 4 50000 → results/*.md 로 저장
make tsan         # TSan 빌드로 덮어씀. 성능을 재기 전에 반드시 make clean && make
```

- C11 + GCC + Linux 전용(futex, `rdtsc`, `_Generic`). 경고 없이(`-Wall -Wextra`) 빌드돼야 한다.
- 로거 동작이나 포맷팅을 바꾸면 `make test`를 통과시킨다. `fmt_test`가 `snprintf`와 글자 단위로 비교하고, 문자열 뒤에 접근 금지 페이지를 둬서 범위 밖 읽기도 잡는다.
- 직접 변환 경로(`log_format_prefix`, `log_format_g`)는 결과가 `snprintf`와 똑같아야 한다. 확신할 수 없는 입력(잘릴 수 있는 경우, 범위 밖·비정규수·inf·nan)은 `snprintf`로 넘긴다. 새 변환을 넣으면 `fmt_test`에 대량 1:1 비교를 추가한다.
- `bpftrace`, `run_bt*.sh`는 sudo 가 필요하다. 직접 실행하지 말고 사용자에게 `! sudo tools/run_bt.sh` 처럼 돌려 달라고 한다.

## 측정 규칙 (중요)

- **모든 테스트·벤치·perf·bpftrace 결과는 `results/YYYY-MM-DD_HHMMSS_<이름>.md`로 남긴다.** `tools/save_result.sh <이름> <명령...>`으로 감싸 돌리고, "해석" 절을 채운 뒤 `results/README.md` 목록에 한 줄 추가한다. scratchpad 에서 돌린 임시 비교도 보고하기 전에 results 로 옮긴다. 원본 출력은 고치지 않는다.
- **이 머신(Ryzen 5 7430U 노트북)은 세션 사이 편차가 크다.** `LFLOG` 숫자 1T p50 이 약 30ns / 약 100ns 두 상태를 오가고, Quill 은 10ns 그대로다(TODO 1·2). 그래서:
  - 전후 비교는 다른 날의 숫자와 비교하지 않는다. 같은 세션에서 번갈아 돌린다: `tools/save_result.sh <이름> tools/cmp_ab.sh <A의 cmp_bench> <B의 cmp_bench> 5`
  - A 빌드는 `git worktree add <scratchpad>/old <커밋> && make -C <scratchpad>/old cmp_bench`.
  - 여러 라운드의 중앙값 [최소~최대]로 적고, 같은 라운드의 Quill 값을 기준선으로 같이 적는다.
- 개선을 주장하기 전에 원인을 숫자로 확인한다: `fmt_bench`로 구간 비용, `perf record -e cycles:u --call-graph dwarf` + `tools/perf_classify.py`로 스레드별 비중. ns 단위 핫패스 횟수는 uprobe 대신 계측 빌드로 센다(uprobe 1회 약 1.6µs 라 측정 대상을 바꿔 버린다).
- `perf.data` 같은 대용량 원본은 커밋하지 않는다. 요약 출력만 results 에 넣는다.

## 문서 갱신

과제를 하나 끝내면 같은 커밋에서 함께 고친다.
- `TODO.md`: 표의 상태, 해당 절에 "완료 (날짜)" + 근거 results 파일 + 숫자. 남은 일은 새 과제 번호로 넘긴다.
- `README.md`: 관련 벤치마크 절의 숫자와 결과 링크. 이전 값이 남은 표는 "변경 전 값"이라고 밝힌다.
- 문서 문체: 한국어, "~다" 체, 짧은 문장. 숫자에는 단위와 조건(스레드 수, 줄 수, 빌드)을 붙인다. 아직 안 잰 것은 "후보"·"아직 안 잼"으로 구분한다.

## Git

- 커밋 메시지는 한국어. 제목에 무엇을 해서 어떤 숫자가 어떻게 바뀌었는지 + `(TODO n)`. 본문은 `- ` 목록으로 변경점과 측정 결과. 코드 식별자는 그대로 쓴다.
- 이미 푸시된 커밋은 다시 쓰지 않는다.
- 측정 결과 md 는 관련 코드 변경과 같은 커밋에 넣는다.
