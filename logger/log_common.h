/* 공통 정의: 로그 레벨, 한 줄 포맷팅 */
#ifndef LOG_COMMON_H
#define LOG_COMMON_H

#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>

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
 * "2026-10-19 16:30:01.123456 [INFO ] 12345 main.c:42 | 메시지\n" 형태로 out에 쓴다.
 * 반환값: 쓴 바이트 수 (항상 '\n'으로 끝나며 cap-1 이하)
 */
size_t log_format_line(char *out, size_t cap, log_level lv,
                       const char *file, int line, const char *fmt, va_list ap);

/* write()가 일부만 쓰거나 EINTR로 끊겨도 끝까지 쓴다. 성공 0, 실패 -1 */
int log_write_all(int fd, const char *buf, size_t len);

#endif
