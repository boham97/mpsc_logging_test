/*
 * LFLOG 만 돌리는 작은 드라이버. perf / bpftrace 로 어디서 시간이 드는지 볼 때 쓴다.
 * bench 는 다섯 모드를 다 돌리고 결과 파일 검증·백분위 정렬까지 해서 프로파일에 섞이므로 따로 둔다.
 * 호출마다 시각을 재지 않으므로 bench 보다 빨리 넣는다(같은 인자라도 큐가 더 잘 찬다).
 *
 * 사용법: ./lf_drv [threads=8] [lines_per_thread=200000] [burst=0]
 *   burst > 0 이면 burst 줄마다 2ms 쉰다.
 */
#define _GNU_SOURCE
#include "../logger/lockfree_logger.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static lf_logger *lg;
static int lines, burst;
static pthread_barrier_t start_barrier;

static void *worker(void *p)
{
    int id = (int)(long)p;
    pthread_barrier_wait(&start_barrier);
    for (int i = 0; i < lines; i++) {
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
    pthread_barrier_init(&start_barrier, NULL, (unsigned)nthreads);
    for (long i = 0; i < nthreads; i++)
        pthread_create(&th[i], NULL, worker, (void *)i);
    for (int i = 0; i < nthreads; i++)
        pthread_join(th[i], NULL);
    pthread_barrier_destroy(&start_barrier);

    lflog_close(lg);
    return 0;
}
