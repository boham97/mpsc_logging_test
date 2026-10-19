# logx — 멀티스레드 환경용 C 로깅 라이브러리

같은 문제(여러 스레드가 하나의 파일에 로그를 쓴다)를 세 가지 설계로 구현하고 비교한다.

| 로거 | 접두어 | 로깅 전용 스레드 | 동기화 방식 |
|---|---|---|---|
| `async_logger` | `alog_` / `ALOG_*` | 있음 | 뮤텍스 + 더블 버퍼 |
| `lockfree_logger` | `lflog_` / `LFLOG_*` | 있음 | 락프리 MPSC 링 버퍼 + futex |
| `sync_logger` | `slog_` / `SLOG_*` | 없음 | `O_APPEND` 직접 쓰기(락 없음) 또는 공유 버퍼 + 뮤텍스 |

요구 사항: Linux, GCC(C11), pthread. 락프리 버전은 Linux futex를 직접 쓴다.

## 빌드와 실행

```sh
make                    # liblogx.a, example, bench
./example               # app_async.log / app_lockfree.log / app_sync.log 생성
./bench 8 200000        # 스레드 8개 × 20만 줄로 네 가지 모드 비교 + 검증
make tsan               # ThreadSanitizer 빌드 후 ./bench 4 20000 (libtsan 필요)
make clean
```

`make tsan`을 실행한 뒤에는 바이너리가 TSan 빌드로 남는다. 성능을 측정하려면 `make clean && make`로 다시 빌드한다.

## 디렉터리 구조

```
logger/
  log_common.[ch]       공통: 레벨, 한 줄 포맷팅, write_all
  async_logger.[ch]     방식 1: 로깅 스레드 + 뮤텍스 더블 버퍼
  lockfree_logger.[ch]  방식 3: 로깅 스레드 + 락프리 MPSC 큐
  sync_logger.[ch]      방식 2: 로깅 스레드 없음
examples/
  example.c             사용 예
  bench.c               처리량/지연 측정, 결과 파일 정합성 검증
Makefile
```

## 로그 형식

```
2026-10-19 16:38:27.349272 [INFO ] 275093 example.c:15 | worker 0 step 0
└─ 로컬 시각(µs)            └ 레벨  └ TID  └ 파일:줄     └ 메시지
```

- 한 줄은 최대 `LOG_LINE_MAX`(1024바이트)이다. 넘으면 잘리고, 끝은 항상 `\n`이다.
- 포맷팅은 호출 스레드의 스택 버퍼에서 락 없이 한다.
- `localtime_r`/`strftime` 결과는 초 단위로 스레드별 캐싱하고, TID도 스레드별로 한 번만 조회한다.
- 타임스탬프는 락이나 큐에 들어가기 전에 찍는다. 그래서 스레드 사이에서는 파일 안의 시각 순서가 조금 뒤바뀔 수 있다. 같은 스레드 안에서는 항상 순서대로다.

## 사용법

```c
#include "logger/async_logger.h"
#include "logger/lockfree_logger.h"
#include "logger/sync_logger.h"

async_logger *a = alog_open("app.log", NULL);        /* INFO, 4MB 버퍼, 200ms, 대기 */
lf_logger    *l = lflog_open("app.log", NULL);       /* INFO, 16384 슬롯, 재시도 */
sync_logger  *s = slog_open("app.log", LOG_INFO, 0); /* 0: 락 없는 직접 쓰기 */

ALOG_INFO(a, "user=%s id=%d", name, id);
LFLOG_ERROR(l, "failed: %s", strerror(errno));
SLOG_WARN(s, "retry %d", n);

alog_flush(a);   /* 지금까지 넣은 로그가 디스크에 써질 때까지 대기 */
alog_close(a);   /* 남은 로그를 모두 쓰고 스레드 종료 */
lflog_close(l);
slog_close(s);
```

매크로는 레벨이 꺼져 있으면 인자를 평가하지 않는다. 런타임에 `*_set_level()`로 레벨을 바꿀 수 있다. 세 로거의 공통 API는 다음과 같다.

| 함수 | 설명 |
|---|---|
| `*_open(path, ...)` | 파일을 `O_APPEND`로 연다. 실패하면 `NULL` |
| `*_write(lg, lv, file, line, fmt, ...)` | printf 형식. 보통은 매크로로 호출 |
| `*_flush(lg)` | 지금까지 넣은 로그를 디스크로 |
| `*_close(lg)` | flush 후 해제 |
| `*_enabled` / `*_set_level` | 레벨 확인/변경 (atomic) |
| `alog_dropped` / `lflog_dropped` | 버퍼나 큐가 가득 차서 버린 개수 |

---

## 방식 1: 로깅 스레드 + 뮤텍스 더블 버퍼 (`async_logger`)

```
[작업 스레드들] 포맷팅(락 없음) → 락 → front 버퍼에 memcpy → 언락
                                        │ swap (포인터 교환)
[로깅 스레드]   ←───────────────────── back 버퍼 → write() (락 없음)
```

- 호출 스레드는 디스크 I/O를 하지 않는다. 락을 잡는 구간은 `memcpy` 한 번뿐이다.
- 로깅 스레드가 깨어나는 조건:
  - `flush_ms` 주기가 지났을 때
  - 버퍼가 절반 넘게 찼을 때
  - ERROR 이상이 들어왔을 때
- 버퍼가 가득 찼을 때의 동작은 `drop_on_full`로 고른다.
  - `0`: 자리가 빌 때까지 대기한다.
  - `1`: 버리고 개수를 센다. 다음 쓰기 때 `*** dropped N messages ***`를 기록한다.
- `alog_flush()`는 교환 번호(`swap_seq`, `written_seq`)로 "내 로그가 포함된 교환이 디스크까지 갔는지"를 기다린다.
- 조건 변수는 `CLOCK_MONOTONIC`을 써서 시스템 시계 변경에 영향을 받지 않는다.

```c
async_logger_opts o = {
    .min_level = LOG_INFO,
    .buf_size = 8u << 20,   /* front/back 각각 */
    .flush_ms = 100,
    .drop_on_full = 0,
};
```

## 방식 2: 로깅 스레드 없음 (`sync_logger`)

호출 스레드가 직접 쓴다. `buf_size`로 모드를 고른다.

**(a) `buf_size == 0`: 락 없는 직접 쓰기**
- `O_APPEND`로 연 fd에 한 줄당 `write()`를 한 번 호출한다.
- Linux 일반 파일에서 `O_APPEND` write는 오프셋 이동과 쓰기가 원자적이라 줄이 섞이지 않는다.
- 크래시가 나도 이미 호출한 로그는 커널에 넘어가 있다.
- 대신 매 줄 시스템 콜이라 가장 느리다.

**(b) `buf_size > 0`: 공유 버퍼 + 뮤텍스**
- 락을 잡고 버퍼에 `memcpy`한다. 버퍼가 차면 그때 락을 쥔 스레드가 대신 `write()`한다.
- ERROR 이상은 즉시 비운다.
- 단점:
  - 버퍼를 비우는 동안 다른 스레드가 모두 막힌다. 디스크가 느리면 영향이 크다.
  - 백그라운드 타이머가 없어서 로그가 뜸하면 버퍼에 오래 남는다. `slog_flush()`를 직접 불러야 한다.

## 방식 3: 로깅 스레드 + 락프리 MPSC 큐 (`lockfree_logger`)

```
[작업 스레드들] 포맷팅 → CAS로 슬롯 번호 확보 → 슬롯에 memcpy → seq 공개(release)
[로깅 스레드]   seq 확인(acquire) → 로컬 1MB 버퍼에 모음 → write() 한 번
```

- **큐**: Dmitry Vyukov의 bounded MPMC 큐를 소비자 1개로 줄인 것이다.
  - 슬롯마다 `seq` 카운터가 있다.
    - `seq == pos`: 비어 있음 (생산자가 쓸 수 있음)
    - `seq == pos + 1`: 채워짐 (소비자가 가져갈 수 있음)
    - `seq < pos`: 한 바퀴 전 데이터가 남아 있음 → 큐가 가득 참
  - 생산자끼리는 `enqueue_pos` 하나만 CAS로 경쟁한다. 소비자는 혼자라 CAS가 필요 없다.
- **소비자 깨우기 (futex)**:
  - 할 일이 없으면 잠깐 스핀(`pause`)하다가 `futex_wait`로 잔다.
  - 생산자는 평소에 원자 변수 하나만 읽고, 소비자가 자고 있을 때만 `futex_wake`를 호출한다.
  - Dekker 패턴으로 깨우기 유실을 막는다.
    - 생산자: 데이터 공개 → `seq_cst` fence → `sleeping` 확인
    - 소비자: `sleeping = 1` → `seq_cst` fence → 큐 재확인 → 잠
  - 100ms 타임아웃은 안전망이다.
- 큐가 비는 순간 모아 둔 것을 바로 쓴다. 그래서 로그가 뜸해도 지연이 거의 없다.
- false sharing을 막으려고 생산자용 변수, 소비자용 변수, 각 슬롯을 64바이트 캐시라인에 정렬했다.

```c
lf_logger_opts o = {
    .min_level = LOG_INFO,
    .capacity = 16384,     /* 슬롯 수, 2의 거듭제곱으로 올림. 메모리 ≈ capacity × 1KB */
    .drop_on_full = 0,     /* 0: sched_yield 하며 재시도, 1: 버림 */
};
```

### 한계
1. **소비자 쪽은 엄밀히 락프리가 아니다.** 슬롯 번호를 확보한 생산자가 데이터를 다 쓰기 전에 선점되면, 소비자는 순서를 지키느라 그 슬롯에서 기다린다.
   - 코어보다 스레드가 훨씬 많고 큐가 가득 차면 꼬리 지연이 커진다. 32 스레드(12코어)에서 최대 50ms까지 관측했다.
   - 큐를 키우거나 `drop_on_full = 1`로 완화할 수 있다.
2. **슬롯이 1KB 고정이다.** 짧은 로그가 많으면 메모리가 낭비된다. 이를 줄이려면 가변 길이 바이트 링 버퍼가 필요하다.
3. `lflog_flush()`는 50µs 간격 폴링으로 구현했다(드문 호출이라고 가정).

---

## 벤치마크

`./bench`를 12코어 Linux, 로컬 디스크, `-O2`에서 돌린 결과다. 각 줄은 약 120바이트다.

**8 스레드 × 20만 줄 (총 160만 줄)**

| 모드 | 처리량 | 평균 호출 지연 | 최대 지연 |
|---|---|---|---|
| async (뮤텍스 더블 버퍼) | 4.3 M줄/s | 1.7 µs | 0.2~0.8 ms |
| **lock-free (MPSC 링)** | **11.2~11.8 M줄/s** | **0.59 µs** | **~80 µs** |
| sync direct (O_APPEND) | 0.4 M줄/s | 20 µs | ~0.1~0.2 ms |
| sync buffered (뮤텍스 64KB) | 4.2~4.5 M줄/s | 1.7 µs | 0.2~0.3 ms |

**2 스레드 × 50만 줄**

| 모드 | 처리량 | 평균 | 최대 |
|---|---|---|---|
| async | 5.66 M줄/s | 315 ns | 207 µs |
| lock-free | 5.40 M줄/s | 312 ns | **17 µs** |
| sync direct | 0.47 M줄/s | 4.2 µs | 84 µs |
| sync buffered | 4.78 M줄/s | 381 ns | 129 µs |

벤치마크는 끝난 뒤 결과 파일을 다시 읽어서 다음을 검증한다. 모든 모드에서 통과했다.
- 총 줄 수가 맞는지
- 깨진 줄이 없는지
- 스레드별 순번이 `0, 1, 2, …`로 빠짐없이 순서대로인지

### 해석
- 경쟁이 적으면(2 스레드) 처리량은 비슷하다. 락프리는 **꼬리 지연**이 크게 줄어드는 것이 이점이다.
- 스레드가 많을수록 락프리의 이점이 커진다. 뮤텍스는 경쟁이 붙으면 futex로 잠들었다 깨는 비용이 든다.
- 이 벤치마크는 쉬지 않고 쓰는 로컬 디스크 환경이라 sync buffered도 빠르게 나온다. 차이가 나는 경우는 두 가지다.
  - **디스크가 느릴 때** (NFS, fsync 등): sync buffered는 버퍼를 비우는 스레드와 뒤에서 기다리는 스레드가 모두 멈춘다. 로깅 스레드 방식은 로깅 스레드만 기다린다.
  - **로그가 뜸할 때**: 로깅 스레드 방식은 일정 시간 안에 디스크에 써진다. sync buffered에는 그런 보장이 없다.
- sync direct는 느리지만 버퍼가 없어 크래시에 가장 안전하다.

## 어떤 걸 쓸까

| 상황 | 추천 |
|---|---|
| 일반적인 서버, 단순함 우선 | `async_logger` |
| 핫패스에서 로그 지연을 최소화, 스레드 많음 | `lockfree_logger` |
| 스레드를 늘릴 수 없음, 크래시 직전 로그까지 반드시 남겨야 함 | `sync_logger` (a) 모드 |
| 스레드 없이 빠르게, 주기적으로 flush 가능 | `sync_logger` (b) 모드 |

## 검증 상태

- `make tsan`(ThreadSanitizer, 4 스레드 × 2만 줄)에서 **데이터 레이스 경고 없음**.
- 단, TSan은 단독 `atomic_thread_fence`를 제대로 모델링하지 못한다. 그래서 락프리 버전의 futex 깨우기(Dekker 패턴)는 TSan으로 증명되지 않았다. 일반 빌드 반복 실행에서는 멈춘 적이 없고, 깨우기가 유실돼도 100ms 타임아웃으로 복구된다.

## 다음에 해볼 만한 것

- 파일 로테이션 (크기/날짜 기준)
- 가변 길이 락프리 바이트 링 버퍼 (메모리 효율)
- 스레드별 SPSC 큐 + 소비자 병합 (생산자 간 CAS 경쟁 제거)
- 크래시 시그널 핸들러에서 남은 버퍼 flush
