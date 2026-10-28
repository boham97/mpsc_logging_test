/*
 * 로거들을 같은 조건(N 스레드 x M 줄)으로 돌려
 *  1) 처리량 / 호출 지연시간(평균, 최대)을 재고
 *  2) 결과 파일을 다시 읽어 줄 유실·섞임·스레드별 순서 뒤바뀜이 없는지 검증한다.
 *
 * 사용법: ./bench [threads=8] [lines_per_thread=200000] [burst=0]
 *   burst > 0 이면 burst 줄마다 2ms 쉰다 (평소엔 조용하다가 몰아서 찍는 상황).
 *   이때는 처리량보다 호출 지연시간을 보면 된다.
 */
#define _GNU_SOURCE
#include "../logger/async_logger.h"
#include "../logger/lockfree_logger.h"
#include "../logger/sync_logger.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef enum { MODE_ASYNC, MODE_LOCKFREE_TEXT, MODE_LOCKFREE, MODE_SYNC_DIRECT, MODE_SYNC_BUFFERED } mode_t_;

static const char *mode_name[] = {"async (logging thread, mutex double buffer)",
                                  "lock-free, caller formats (LFLOG_TEXT)",
                                  "lock-free, deferred formatting (LFLOG)", "sync direct (O_APPEND, no lock)",
                                  "sync buffered (mutex, 64KB)"};
static const char *mode_file[] = {"bench_async.log", "bench_lockfree_text.log", "bench_lockfree.log", "bench_sync_direct.log",
                                  "bench_sync_buffered.log"};

typedef struct {
    int id;
    int lines;
    mode_t_ mode;
    async_logger *alg;
    lf_logger *lfg;
    sync_logger *slg;
    double sum_ns;
    double max_ns;
    uint32_t *lat; /* 호출별 지연 (ns), 백분위 계산용 */
} worker_arg;

static pthread_barrier_t start_barrier;
static int burst;

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static void *worker(void *p)
{
    worker_arg *a = p;
    pthread_barrier_wait(&start_barrier);
    for (int i = 0; i < a->lines; i++) {
        double t0 = now_ns();
        if (a->mode == MODE_ASYNC)
            ALOG_INFO(a->alg, "T%d #%d payload=%s", a->id, i, "the quick brown fox jumps over the lazy dog");
        else if (a->mode == MODE_LOCKFREE_TEXT)
            LFLOG_TEXT(a->lfg, LOG_INFO, "T%d #%d payload=%s", a->id, i, "the quick brown fox jumps over the lazy dog");
        else if (a->mode == MODE_LOCKFREE)
            LFLOG_INFO(a->lfg, "T%d #%d payload=%s", a->id, i, "the quick brown fox jumps over the lazy dog");
        else
            SLOG_INFO(a->slg, "T%d #%d payload=%s", a->id, i, "the quick brown fox jumps over the lazy dog");
        double dt = now_ns() - t0;
        a->lat[i] = dt < 4e9 ? (uint32_t)dt : UINT32_MAX;
        a->sum_ns += dt;
        if (dt > a->max_ns)
            a->max_ns = dt;
        if (burst > 0 && (i + 1) % burst == 0) {
            struct timespec ts = {0, 2 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

static int cmp_u32(const void *x, const void *y)
{
    uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y;
    return (a > b) - (a < b);
}

/* 모든 스레드의 호출 지연을 모아 백분위를 출력 */
static void print_percentiles(worker_arg *args, int nthreads, int lines)
{
    size_t n = (size_t)nthreads * lines;
    uint32_t *all = malloc(n * sizeof(*all));
    for (int i = 0; i < nthreads; i++)
        memcpy(all + (size_t)i * lines, args[i].lat, (size_t)lines * sizeof(*all));
    qsort(all, n, sizeof(*all), cmp_u32);
    static const double ps[] = {50, 90, 99, 99.9, 99.99};
    printf("  percentiles:");
    for (size_t k = 0; k < sizeof(ps) / sizeof(ps[0]); k++) {
        uint32_t v = all[(size_t)(ps[k] / 100 * (double)(n - 1))];
        if (v < 10000)
            printf(" p%g %u ns,", ps[k], v);
        else
            printf(" p%g %.1f us,", ps[k], v / 1e3);
    }
    /* 10us 이상 걸린 호출 (선점·큐 가득 참·뮤텍스 대기 등으로 실제로 막힌 경우) */
    size_t slow = 0;
    for (size_t i = n; i > 0 && all[i - 1] >= 10000; i--)
        slow++;
    printf(" >=10us %zu calls (%.3f%%)\n", slow, 100.0 * (double)slow / (double)n);
    free(all);
}

/* 파일을 읽어 각 스레드의 #번호가 0,1,2,... 순서로 빠짐없이 나오는지 확인 */
static int verify(const char *path, int nthreads, int lines)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        perror(path);
        return -1;
    }
    int *next = calloc((size_t)nthreads, sizeof(int));
    char line[LOG_LINE_MAX * 2];
    long total = 0, bad = 0;

    while (fgets(line, sizeof(line), fp)) {
        total++;
        int t, seq;
        const char *msg = strstr(line, "| T");
        if (!msg || sscanf(msg, "| T%d #%d payload=", &t, &seq) != 2 ||
            t < 0 || t >= nthreads || seq != next[t] ||
            line[strlen(line) - 1] != '\n') {
            if (bad++ < 5)
                fprintf(stderr, "  bad line %ld: %s", total, line);
            continue;
        }
        next[t]++;
    }
    fclose(fp);

    int missing = 0;
    for (int t = 0; t < nthreads; t++)
        if (next[t] != lines)
            missing++;
    free(next);

    long expect = (long)nthreads * lines;
    printf("  verify: %ld/%ld lines, bad=%ld, threads incomplete=%d -> %s\n",
           total, expect, bad, missing, (total == expect && bad == 0 && !missing) ? "OK" : "FAIL");
    return (total == expect && bad == 0 && !missing) ? 0 : -1;
}

static int run(mode_t_ mode, int nthreads, int lines)
{
    const char *path = mode_file[mode];
    unlink(path);

    async_logger *alg = NULL;
    lf_logger *lfg = NULL;
    sync_logger *slg = NULL;
    if (mode == MODE_ASYNC) {
        async_logger_opts o = {LOG_INFO, 8u << 20, 100, 0 /* 대기: 유실 없음 */};
        alg = alog_open(path, &o);
    } else if (mode == MODE_LOCKFREE || mode == MODE_LOCKFREE_TEXT) {
        lf_logger_opts o = {LOG_INFO, 16384, 0 /* 재시도: 유실 없음 */};
        lfg = lflog_open(path, &o);
    } else {
        slg = slog_open(path, LOG_INFO, mode == MODE_SYNC_BUFFERED ? 64 * 1024 : 0);
    }
    if (!alg && !lfg && !slg) {
        perror("open logger");
        return -1;
    }

    pthread_t *th = calloc((size_t)nthreads, sizeof(*th));
    worker_arg *args = calloc((size_t)nthreads, sizeof(*args));
    pthread_barrier_init(&start_barrier, NULL, (unsigned)nthreads + 1);

    for (int i = 0; i < nthreads; i++) {
        args[i] = (worker_arg){i, lines, mode, alg, lfg, slg, 0, 0, malloc((size_t)lines * sizeof(uint32_t))};
        pthread_create(&th[i], NULL, worker, &args[i]);
    }
    pthread_barrier_wait(&start_barrier);
    double t0 = now_ns();
    for (int i = 0; i < nthreads; i++)
        pthread_join(th[i], NULL);
    double t_calls = now_ns() - t0; /* 호출 스레드들이 끝난 시간 */

    if (alg)
        alog_close(alg);
    else if (lfg)
        lflog_close(lfg);
    else
        slog_close(slg);
    double t_total = now_ns() - t0; /* 디스크까지 다 쓴 시간 */

    double sum = 0, mx = 0;
    for (int i = 0; i < nthreads; i++) {
        sum += args[i].sum_ns;
        if (args[i].max_ns > mx)
            mx = args[i].max_ns;
    }
    long n = (long)nthreads * lines;

    printf("[%s]\n", mode_name[mode]);
    printf("  callers done: %7.1f ms, all on disk: %7.1f ms, throughput: %.2f M lines/s\n",
           t_calls / 1e6, t_total / 1e6, n / (t_total / 1e9) / 1e6);
    printf("  call latency: avg %.0f ns, max %.1f us\n", sum / n, mx / 1e3);
    print_percentiles(args, nthreads, lines);
    for (int i = 0; i < nthreads; i++)
        free(args[i].lat);

    pthread_barrier_destroy(&start_barrier);
    free(th);
    free(args);
    return verify(path, nthreads, lines);
}

int main(int argc, char **argv)
{
    int nthreads = argc > 1 ? atoi(argv[1]) : 8;
    int lines = argc > 2 ? atoi(argv[2]) : 200000;
    burst = argc > 3 ? atoi(argv[3]) : 0;
    printf("threads=%d, lines/thread=%d, total=%ld, burst=%d\n\n", nthreads, lines,
           (long)nthreads * lines, burst);

    int rc = 0;
    rc |= run(MODE_ASYNC, nthreads, lines);
    rc |= run(MODE_LOCKFREE_TEXT, nthreads, lines);
    rc |= run(MODE_LOCKFREE, nthreads, lines);
    rc |= run(MODE_SYNC_DIRECT, nthreads, lines);
    rc |= run(MODE_SYNC_BUFFERED, nthreads, lines);
    return rc ? 1 : 0;
}
