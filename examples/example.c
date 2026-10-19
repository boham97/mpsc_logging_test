/* 기본 사용 예 */
#include "../logger/async_logger.h"
#include "../logger/lockfree_logger.h"
#include "../logger/sync_logger.h"

#include <pthread.h>
#include <stdio.h>

static async_logger *g_alog;
static lf_logger *g_lflog;
static sync_logger *g_slog;

static void *job(void *arg)
{
    int id = (int)(long)arg;
    for (int i = 0; i < 3; i++) {
        ALOG_INFO(g_alog, "worker %d step %d", id, i);
        LFLOG_INFO(g_lflog, "worker %d step %d", id, i);
        SLOG_INFO(g_slog, "worker %d step %d", id, i);
    }
    ALOG_DEBUG(g_alog, "보이지 않음 (min_level=INFO)");
    if (id == 0) {
        ALOG_ERROR(g_alog, "ERROR 는 로깅 스레드를 즉시 깨운다");
        SLOG_ERROR(g_slog, "ERROR 는 버퍼를 즉시 비운다");
    }
    return NULL;
}

int main(void)
{
    g_alog = alog_open("app_async.log", NULL);  /* 기본값: INFO, 4MB, 200ms */
    g_lflog = lflog_open("app_lockfree.log", NULL);  /* 기본값: INFO, 16384 슬롯 */
    g_slog = slog_open("app_sync.log", LOG_INFO, 0);
    if (!g_alog || !g_lflog || !g_slog) {
        perror("logger open");
        return 1;
    }

    pthread_t th[4];
    for (long i = 0; i < 4; i++)
        pthread_create(&th[i], NULL, job, (void *)i);
    for (int i = 0; i < 4; i++)
        pthread_join(th[i], NULL);

    alog_flush(g_alog);  /* 여기서 파일에 다 써졌음이 보장됨 */
    alog_close(g_alog);
    lflog_close(g_lflog);
    slog_close(g_slog);
    puts("done: app_async.log, app_lockfree.log, app_sync.log");
    return 0;
}
