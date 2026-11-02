#define _GNU_SOURCE
#include "log_common.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

const char *log_level_name(log_level lv)
{
    static const char *names[] = {"TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"};
    if ((unsigned)lv > LOG_FATAL)
        return "?????";
    return names[lv];
}

/* 커널 스레드 ID는 스레드마다 한 번만 조회 */
_Thread_local int log_tid_cache = 0;

int log_current_tid_slow(void)
{
    log_tid_cache = (int)syscall(SYS_gettid);
    return log_tid_cache;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

size_t log_format_prefix(char *out, size_t cap, log_level lv, const char *file, int line,
                         const struct timespec *ts, int tid)
{
    /* localtime_r/strftime 은 비싸므로 초 단위 문자열을 스레드별로 캐싱 */
    static _Thread_local time_t cached_sec = -1;
    static _Thread_local char cached_time[32];

    if (ts->tv_sec != cached_sec) {
        struct tm tm;
        localtime_r(&ts->tv_sec, &tm);
        strftime(cached_time, sizeof(cached_time), "%Y-%m-%d %H:%M:%S", &tm);
        cached_sec = ts->tv_sec;
    }

    int n = snprintf(out, cap, "%s.%06ld [%s] %d %s:%d | ",
                     cached_time, ts->tv_nsec / 1000, log_level_name(lv),
                     tid, base_name(file), line);
    return (n < 0) ? 0 : ((size_t)n >= cap ? cap - 1 : (size_t)n);
}

size_t log_format_line(char *out, size_t cap, log_level lv,
                       const char *file, int line, const char *fmt, va_list ap)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    size_t limit = cap - 1; /* 마지막 '\n' 자리 확보 */
    size_t len = log_format_prefix(out, limit, lv, file, line, &ts, log_current_tid());

    int n = vsnprintf(out + len, limit - len, fmt, ap);
    if (n > 0)
        len += ((size_t)n >= limit - len) ? limit - len - 1 : (size_t)n;

    out[len++] = '\n';
    out[len] = '\0';
    return len;
}

int log_write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t w = write(fd, buf, len);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += w;
        len -= (size_t)w;
    }
    return 0;
}
