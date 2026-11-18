/* 공통 정의: 로그 레벨, 한 줄 포맷팅 */
#ifndef LOG_COMMON_H
#define LOG_COMMON_H

#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#define LOG_LINE_MAX 1024 /* 한 줄 최대 길이 (넘으면 잘림) */

typedef enum {
    LOG_TRACE = 0,
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL
} log_level;

const char *log_level_name(log_level lv);

/*
 * 호출 스레드의 커널 TID (스레드별 캐싱).
 * 매 로그마다 불리므로 인라인으로 TLS 만 읽고, 스레드의 첫 호출만 gettid 로 조회한다.
 */
extern _Thread_local int log_tid_cache;
int log_current_tid_slow(void);

static inline int log_current_tid(void)
{
    int tid = log_tid_cache;
    if (__builtin_expect(tid == 0, 0))
        tid = log_current_tid_slow();
    return tid;
}

/*
 * "2026-10-19 16:30:01.123456 [INFO ] 12345 main.c:42 | " 접두어만 out 에 쓴다.
 * 시각/TID 를 인자로 받으므로 로그를 남긴 스레드가 아닌 곳(로깅 스레드)에서도 쓸 수 있다.
 * 반환값: 쓴 바이트 수 (cap-1 이하)
 */
size_t log_format_prefix(char *out, size_t cap, log_level lv, const char *file, int line,
                         const struct timespec *ts, int tid);

/*
 * "2026-10-19 16:30:01.123456 [INFO ] 12345 main.c:42 | 메시지\n" 형태로 out에 쓴다.
 * 반환값: 쓴 바이트 수 (항상 '\n'으로 끝나며 cap-1 이하)
 */
size_t log_format_line(char *out, size_t cap, log_level lv,
                       const char *file, int line, const char *fmt, va_list ap);

/*
 * snprintf(out, .., "%g", v) 와 같은 글자를 out 에 쓴다 (NUL 은 안 씀). 반환값: 쓴 바이트 수.
 * out 은 LOG_G_MAX 바이트 이상이어야 한다.
 * 1.4e-17 <= |v| < 2.3e21 과 0 은 정수 연산으로 직접 변환하고 (반올림: 가장 가까운 값, 동률이면 짝수),
 * 그 밖(아주 크거나 작은 수, 비정규수, inf, nan)은 snprintf 로 넘긴다.
 * 부동소수점 반올림 모드는 기본값(FE_TONEAREST)이라고 가정한다.
 */
#define LOG_G_MAX 32
size_t log_format_g(char *out, double v);

/* write()가 일부만 쓰거나 EINTR로 끊겨도 끝까지 쓴다. 성공 0, 실패 -1 */
int log_write_all(int fd, const char *buf, size_t len);

#endif
