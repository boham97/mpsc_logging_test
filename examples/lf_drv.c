/*
 * 락프리 로거만 돌리는 작은 드라이버. perf / bpftrace 로 어디서 시간이 드는지 볼 때 쓴다.
 * bench 는 다섯 모드를 다 돌리고 결과 파일 검증·백분위 정렬까지 해서 프로파일에 섞이므로 따로 둔다.
 * 호출마다 시각을 재지 않으므로 bench 보다 빨리 넣는다(같은 인자라도 큐가 더 잘 찬다).
 *
 * 사용법: ./lf_drv [threads=8] [lines_per_thread=200000] [burst=0] [mode=record]
 *   burst > 0 이면 burst 줄마다 2ms 쉰다.
 *   mode: record = LFLOG (지연 포맷팅), text = LFLOG_TEXT (호출 스레드가 포맷팅)
 * 끝나면 총 호출 수와 걸린 시간을 출력한다 (추적 도구가 얼마나 느리게 만들었는지 비교용).
 */
#define _GNU_SOURCE
#include "../logger/lockfree_logger.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static lf_logger *lg;
static int lines, burst, text_mode;
static pthread_barrier_t start_barrier;

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void *worker(void *p)
{
    int id = (int)(long)p;
    pthread_barrier_wait(&start_barrier);
    for (int i = 0; i < lines; i++) {
        if (text_mode)
            LFLOG_TEXT(lg, LOG_INFO, "T%d #%d payload=%s", id, i, "the quick brown fox jumps over the lazy dog");
        else
            LFLOG_INFO(lg, "T%d #%d payload=%s", id, i, "the quick brown fox jumps over the lazy dog");
        if (burst > 0 && (i + 1) % burst == 0) {
            struct timespec q = {0, 2 * 1000 * 1000};
            nanosleep(&q, NULL);
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int nthreads = argc > 1 ? atoi(argv[1]) : 8;
    lines = argc > 2 ? atoi(argv[2]) : 200000;
    burst = argc > 3 ? atoi(argv[3]) : 0;
    text_mode = argc > 4 && strcmp(argv[4], "text") == 0;
    if (nthreads < 1 || nthreads > 64)
        nthreads = 8;

    lf_logger_opts o = {.min_level = LOG_INFO, .capacity = 16384};
    remove("lf_drv.log");
    lg = lflog_open("lf_drv.log", &o);
    if (!lg) {
        perror("lflog_open");
        return 1;
    }

    pthread_t th[64];
    pthread_barrier_init(&start_barrier, NULL, (unsigned)nthreads + 1);
    for (long i = 0; i < nthreads; i++)
        pthread_create(&th[i], NULL, worker, (void *)i);
    pthread_barrier_wait(&start_barrier);
    double t0 = now_ms();
    for (int i = 0; i < nthreads; i++)
        pthread_join(th[i], NULL);
    double t_calls = now_ms() - t0;
    pthread_barrier_destroy(&start_barrier);

    lflog_close(lg);
    double t_total = now_ms() - t0;
    long n = (long)nthreads * lines;
    printf("lf_drv: mode=%s threads=%d lines/thread=%d burst=%d calls=%ld callers done %.1f ms, all on disk %.1f ms, %.2f M lines/s\n",
           text_mode ? "text" : "record", nthreads, lines, burst, n, t_calls, t_total, n / t_total / 1e3);
    return 0;
}
