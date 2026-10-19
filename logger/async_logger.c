#define _GNU_SOURCE
#include "async_logger.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct async_logger {
    int fd;
    async_logger_opts opts;
    _Atomic int min_level;

    pthread_mutex_t mu;
    pthread_cond_t wake;      /* 로깅 스레드 깨우기 */
    pthread_cond_t not_full;  /* 버퍼 비었음 → 대기 중인 생산자 깨우기 */
    pthread_cond_t flushed;   /* alog_flush() 대기자 깨우기 */

    char *front;              /* 생산자들이 채우는 버퍼 (mu 보호) */
    size_t front_len;
    char *back;               /* 로깅 스레드 전용 버퍼 */

    unsigned long swap_seq;   /* front<->back 교환 횟수 */
    unsigned long written_seq;/* 디스크까지 쓴 교환 번호 */
    int urgent;
    int stop;

    _Atomic unsigned long dropped;
    pthread_t thread;
};

static void deadline_after_ms(struct timespec *ts, int ms)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static void *worker_main(void *arg)
{
    async_logger *lg = arg;
    unsigned long last_dropped = 0;

    for (;;) {
        pthread_mutex_lock(&lg->mu);

        struct timespec deadline;
        deadline_after_ms(&deadline, lg->opts.flush_ms);
        while (!lg->stop && !lg->urgent && lg->front_len < lg->opts.buf_size / 2) {
            if (pthread_cond_timedwait(&lg->wake, &lg->mu, &deadline) != 0)
                break; /* 타임아웃 → 있는 만큼 쓴다 */
        }

        /* 락 안에서는 포인터 교환만 */
        char *tmp = lg->back;
        lg->back = lg->front;
        lg->front = tmp;
        size_t len = lg->front_len;
        lg->front_len = 0;
        lg->urgent = 0;
        unsigned long seq = ++lg->swap_seq;
        int stopping = lg->stop;

        pthread_cond_broadcast(&lg->not_full);
        pthread_mutex_unlock(&lg->mu);

        /* ---- 여기부터 락 없이 I/O ---- */
        unsigned long d = atomic_load_explicit(&lg->dropped, memory_order_relaxed);
        if (d != last_dropped) {
            char note[128];
            int n = snprintf(note, sizeof(note),
                             "*** async_logger: dropped %lu messages (buffer full) ***\n",
                             d - last_dropped);
            log_write_all(lg->fd, note, (size_t)n);
            last_dropped = d;
        }
        if (len > 0)
            log_write_all(lg->fd, lg->back, len);

        pthread_mutex_lock(&lg->mu);
        lg->written_seq = seq;
        pthread_cond_broadcast(&lg->flushed);
        int done = stopping && lg->front_len == 0;
        pthread_mutex_unlock(&lg->mu);

        if (done)
            return NULL; /* stop 이후 들어온 로그까지 비운 뒤 종료 */
    }
}

async_logger *alog_open(const char *path, const async_logger_opts *opts)
{
    async_logger *lg = calloc(1, sizeof(*lg));
    if (!lg)
        return NULL;

    if (opts) {
        lg->opts = *opts;
    } else {
        lg->opts = (async_logger_opts){LOG_INFO, 4u << 20, 200, 0};
    }
    if (lg->opts.buf_size < 2 * LOG_LINE_MAX)
        lg->opts.buf_size = 2 * LOG_LINE_MAX;
    if (lg->opts.flush_ms <= 0)
        lg->opts.flush_ms = 200;
    atomic_init(&lg->min_level, lg->opts.min_level);

    lg->fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    lg->front = malloc(lg->opts.buf_size);
    lg->back = malloc(lg->opts.buf_size);
    if (lg->fd < 0 || !lg->front || !lg->back)
        goto fail;

    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC); /* 시계 변경에 영향 안 받게 */
    pthread_mutex_init(&lg->mu, NULL);
    pthread_cond_init(&lg->wake, &ca);
    pthread_cond_init(&lg->not_full, NULL);
    pthread_cond_init(&lg->flushed, NULL);
    pthread_condattr_destroy(&ca);

    if (pthread_create(&lg->thread, NULL, worker_main, lg) != 0) {
        pthread_mutex_destroy(&lg->mu);
        pthread_cond_destroy(&lg->wake);
        pthread_cond_destroy(&lg->not_full);
        pthread_cond_destroy(&lg->flushed);
        goto fail;
    }
    return lg;

fail:
    if (lg->fd >= 0)
        close(lg->fd);
    free(lg->front);
    free(lg->back);
    free(lg);
    return NULL;
}

void alog_close(async_logger *lg)
{
    if (!lg)
        return;
    pthread_mutex_lock(&lg->mu);
    lg->stop = 1;
    pthread_cond_signal(&lg->wake);
    pthread_mutex_unlock(&lg->mu);

    pthread_join(lg->thread, NULL);

    close(lg->fd);
    pthread_mutex_destroy(&lg->mu);
    pthread_cond_destroy(&lg->wake);
    pthread_cond_destroy(&lg->not_full);
    pthread_cond_destroy(&lg->flushed);
    free(lg->front);
    free(lg->back);
    free(lg);
}

void alog_write(async_logger *lg, log_level lv, const char *file, int line,
                const char *fmt, ...)
{
    if (!alog_enabled(lg, lv))
        return;

    /* 포맷팅은 락 밖에서, 스택 버퍼에 */
    char buf[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    size_t n = log_format_line(buf, sizeof(buf), lv, file, line, fmt, ap);
    va_end(ap);

    int urgent = lv >= LOG_ERROR;

    pthread_mutex_lock(&lg->mu);
    while (lg->front_len + n > lg->opts.buf_size) {
        if (lg->opts.drop_on_full || lg->stop) {
            pthread_mutex_unlock(&lg->mu);
            atomic_fetch_add_explicit(&lg->dropped, 1, memory_order_relaxed);
            return;
        }
        lg->urgent = 1;
        pthread_cond_signal(&lg->wake);
        pthread_cond_wait(&lg->not_full, &lg->mu);
    }
    memcpy(lg->front + lg->front_len, buf, n);
    lg->front_len += n;
    if (urgent)
        lg->urgent = 1;
    int wake = urgent || lg->front_len >= lg->opts.buf_size / 2;
    pthread_mutex_unlock(&lg->mu);

    if (wake)
        pthread_cond_signal(&lg->wake);
}

void alog_flush(async_logger *lg)
{
    pthread_mutex_lock(&lg->mu);
    /* 현재 front 에 있는 내용은 "다음 교환"에 포함되므로 그 번호까지 기다린다 */
    unsigned long target = lg->swap_seq + (lg->front_len > 0 ? 1 : 0);
    if (lg->written_seq < target) {
        lg->urgent = 1;
        pthread_cond_signal(&lg->wake);
        while (lg->written_seq < target)
            pthread_cond_wait(&lg->flushed, &lg->mu);
    }
    pthread_mutex_unlock(&lg->mu);
}

int alog_enabled(const async_logger *lg, log_level lv)
{
    return (int)lv >= atomic_load_explicit(&((async_logger *)lg)->min_level, memory_order_relaxed);
}

void alog_set_level(async_logger *lg, log_level lv)
{
    atomic_store_explicit(&lg->min_level, lv, memory_order_relaxed);
}

unsigned long alog_dropped(async_logger *lg)
{
    return atomic_load_explicit(&lg->dropped, memory_order_relaxed);
}
