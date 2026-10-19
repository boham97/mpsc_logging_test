#define _GNU_SOURCE
#include "sync_logger.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct sync_logger {
    int fd;
    _Atomic int min_level;

    /* (b) 모드에서만 사용 */
    pthread_mutex_t mu;
    char *buf;
    size_t cap;
    size_t len;
};

sync_logger *slog_open(const char *path, log_level min_level, size_t buf_size)
{
    sync_logger *lg = calloc(1, sizeof(*lg));
    if (!lg)
        return NULL;

    lg->fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (lg->fd < 0) {
        free(lg);
        return NULL;
    }
    atomic_init(&lg->min_level, min_level);

    if (buf_size > 0) {
        if (buf_size < LOG_LINE_MAX)
            buf_size = LOG_LINE_MAX;
        lg->buf = malloc(buf_size);
        if (!lg->buf) {
            close(lg->fd);
            free(lg);
            return NULL;
        }
        lg->cap = buf_size;
    }
    pthread_mutex_init(&lg->mu, NULL);
    return lg;
}

void slog_close(sync_logger *lg)
{
    if (!lg)
        return;
    slog_flush(lg);
    close(lg->fd);
    pthread_mutex_destroy(&lg->mu);
    free(lg->buf);
    free(lg);
}

void slog_write(sync_logger *lg, log_level lv, const char *file, int line,
                const char *fmt, ...)
{
    if (!slog_enabled(lg, lv))
        return;

    char line_buf[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    size_t n = log_format_line(line_buf, sizeof(line_buf), lv, file, line, fmt, ap);
    va_end(ap);

    if (lg->cap == 0) {
        /* (a) 락 없음: O_APPEND + 한 줄당 write() 한 번 */
        log_write_all(lg->fd, line_buf, n);
        return;
    }

    /* (b) 공유 버퍼: 차면 락을 쥔 스레드가 직접 비운다 (순서 보장) */
    pthread_mutex_lock(&lg->mu);
    if (lg->len + n > lg->cap) {
        log_write_all(lg->fd, lg->buf, lg->len);
        lg->len = 0;
    }
    memcpy(lg->buf + lg->len, line_buf, n);
    lg->len += n;
    if (lv >= LOG_ERROR) {
        log_write_all(lg->fd, lg->buf, lg->len);
        lg->len = 0;
    }
    pthread_mutex_unlock(&lg->mu);
}

void slog_flush(sync_logger *lg)
{
    if (lg->cap == 0)
        return; /* 버퍼 없음: 이미 다 커널에 넘어감 */
    pthread_mutex_lock(&lg->mu);
    if (lg->len > 0) {
        log_write_all(lg->fd, lg->buf, lg->len);
        lg->len = 0;
    }
    pthread_mutex_unlock(&lg->mu);
}

int slog_enabled(const sync_logger *lg, log_level lv)
{
    return (int)lv >= atomic_load_explicit(&((sync_logger *)lg)->min_level, memory_order_relaxed);
}

void slog_set_level(sync_logger *lg, log_level lv)
{
    atomic_store_explicit(&lg->min_level, lv, memory_order_relaxed);
}
