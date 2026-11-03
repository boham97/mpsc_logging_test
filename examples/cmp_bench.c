/*
 * C++ 로깅 라이브러리들이 공개하는 벤치마크를 logx 로 같은 조건에서 돌린다.
 * 원본과 같은 머신에서 나란히 돌리려면 tools/ref_bench.sh 를 쓴다 (Quill, spdlog 원본 빌드).
 *
 * 1) latency    — Quill benchmarks/hot_path_latency (logger_benchmarks 저장소와 같은 하네스)
 *    - 스레드 1개, 4개. 반복마다 모든 스레드를 합쳐 20줄 (4스레드면 스레드당 5줄).
 *    - 스레드당 10,000번 반복. 반복 사이에 2000~2200µs 무작위로 바쁜 대기 (sleep 아님).
 *    - 한 반복의 줄들을 lfence+rdtsc 로 감싸 재고 줄 수로 나눈 값이 표본 하나.
 *    - 백분위 50/75/90/95/99/99.9/최악 (nearest rank).
 *    - 메인 스레드 CPU 0, 작업 스레드 i(1부터) → CPU i % ncpu, 로깅 스레드 CPU 5 + 잠들지 않음.
 *    - 각 스레드는 시작 때 "preallocate" 한 줄을 찍고 flush, 끝날 때 flush.
 *    - 메시지: numbers = "Logging iteration: {}, message: {}, double: {}" (Quill 소스)
 *              string  = "Logging int: {}, int: {}, string: {}" + 35자 넘는 문자열 (Quill README)
 * 2) throughput — Quill benchmarks/backend_throughput
 *    - 한 스레드(CPU 1)가 "Iteration: {} int: {} double: {}" 를 400만 줄 넣고,
 *      로깅 스레드(CPU 5, 잠들지 않음)가 파일에 다 쓸 때까지(flush) 걸린 시간.
 * 3) spdlog     — spdlog bench/bench.cpp 의 basic file sink 부분
 *    - "Hello logger: msg number {}" 25만 줄. 단일 스레드, 1 스레드, N 스레드(각 25만/N 줄).
 *    - spdlog 처럼 호출이 끝난 시점까지 잰다. 비동기 로거는 디스크까지(flush) 시간도 같이 출력.
 *    - 레벨이 꺼진 로거(level-off) 호출 비용도 잰다.
 *
 * fmt 의 {} 대신 printf 지정자를 쓴다. 정수 {} → %zu/%d, double {} → %g (값이 작아 같은 글자).
 *
 * 사용법: ./cmp_bench latency    <logger> <numbers|string> [iterations=10000]
 *         ./cmp_bench throughput <logger> [messages=4000000]
 *         ./cmp_bench spdlog     <logger> [messages=250000] [threads=4]
 *  logger: lflog (LFLOG, 지연 포맷팅) | lflog_text (LFLOG_TEXT) | async | sync_buf | sync_direct
 */
#define _GNU_SOURCE
#include "../logger/async_logger.h"
#include "../logger/lockfree_logger.h"
#include "../logger/sync_logger.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <x86intrin.h>

/* Quill 과 같은 CPU 배치 */
#define MAIN_CPU 0
#define BACKEND_CPU 5
#define THROUGHPUT_CPU 1

#define MESSAGES_PER_ITERATION 20
#define MIN_WAIT_NS 2000000LL
#define MAX_WAIT_NS 2200000LL

static const char *LARGE_STR = "Lorem ipsum dolor sit amet, consectetur adipiscing elit"; /* 55자 */

/* ---- 로거 선택 ---- */

typedef enum { LG_LFLOG, LG_LFLOG_TEXT, LG_ASYNC, LG_SYNC_BUF, LG_SYNC_DIRECT } logger_kind;
static const char *kind_name[] = {"lflog", "lflog_text", "async", "sync_buf", "sync_direct"};
static const char *kind_desc[] = {
    "logx lock-free, deferred formatting (LFLOG, busy_poll, consumer CPU 5)",
    "logx lock-free, caller formats (LFLOG_TEXT, busy_poll, consumer CPU 5)",
    "logx async (mutex double buffer, logging thread not pinned)",
    "logx sync buffered (mutex, 64KB, no logging thread)",
    "logx sync direct (O_APPEND write per line, no logging thread)",
};

typedef struct {
    logger_kind kind;
    lf_logger *lf;
    async_logger *al;
    sync_logger *sl;
} logger;

static int parse_kind(const char *s, logger_kind *out)
{
    for (int i = 0; i <= LG_SYNC_DIRECT; i++)
        if (strcmp(s, kind_name[i]) == 0) {
            *out = (logger_kind)i;
            return 0;
        }
    return -1;
}

static int open_logger(logger *lg, logger_kind kind, const char *path, log_level min_level)
{
    memset(lg, 0, sizeof *lg);
    lg->kind = kind;
    unlink(path); /* Quill/spdlog 처럼 매번 새 파일 */
    switch (kind) {
    case LG_LFLOG:
    case LG_LFLOG_TEXT: {
        /* Quill: backend sleep_duration = 0, cpu_affinity = {5} */
        lf_logger_opts o = {.min_level = min_level, .capacity = 16384, .drop_on_full = 0,
                            .busy_poll = 1, .pin_consumer = 1, .consumer_cpu = BACKEND_CPU};
        lg->lf = lflog_open(path, &o);
        return lg->lf ? 0 : -1;
    }
    case LG_ASYNC: {
        async_logger_opts o = {.min_level = min_level, .buf_size = 4u << 20, .flush_ms = 200, .drop_on_full = 0};
        lg->al = alog_open(path, &o);
        return lg->al ? 0 : -1;
    }
    case LG_SYNC_BUF:
        lg->sl = slog_open(path, min_level, 64 * 1024);
        return lg->sl ? 0 : -1;
    case LG_SYNC_DIRECT:
        lg->sl = slog_open(path, min_level, 0);
        return lg->sl ? 0 : -1;
    }
    return -1;
}

static void flush_logger(logger *lg)
{
    if (lg->lf)
        lflog_flush(lg->lf);
    else if (lg->al)
        alog_flush(lg->al);
    else
        slog_flush(lg->sl);
}

static void close_logger(logger *lg)
{
    if (lg->lf)
        lflog_close(lg->lf);
    else if (lg->al)
        alog_close(lg->al);
    else
        slog_close(lg->sl);
}

/* 매크로가 호출 위치마다 static 캐시를 두므로 로거 종류별로 호출 위치를 따로 둔다 */
#define LOG_ANY(lg, fmt, ...)                                         \
    do {                                                              \
        switch ((lg)->kind) {                                         \
        case LG_LFLOG: LFLOG_INFO((lg)->lf, fmt, __VA_ARGS__); break; \
        case LG_LFLOG_TEXT:                                           \
            LFLOG_TEXT((lg)->lf, LOG_INFO, fmt, __VA_ARGS__);         \
            break;                                                    \
        case LG_ASYNC: ALOG_INFO((lg)->al, fmt, __VA_ARGS__); break;  \
        default: SLOG_INFO((lg)->sl, fmt, __VA_ARGS__); break;        \
        }                                                             \
    } while (0)

/* ---- 공통 도구 ---- */

static int ncpu;

static void pin_self(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu % ncpu, &set);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof set, &set);
    if (rc != 0)
        fprintf(stderr, "Failed to set cpu affinity to %d: %s\n", cpu, strerror(rc));
}

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Quill serialized_benchmark_clock: rdtsc 앞뒤로 lfence */
static inline uint64_t ser_rdtsc(void)
{
    _mm_lfence();
    uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

static double calibrate_ns_per_tick(void)
{
    int64_t n0 = mono_ns();
    uint64_t t0 = __rdtsc();
    struct timespec ts = {0, 200 * 1000 * 1000};
    nanosleep(&ts, NULL);
    int64_t n1 = mono_ns();
    uint64_t t1 = __rdtsc();
    return (double)(n1 - n0) / (double)(t1 - t0);
}

/* splitmix64: 스레드마다 고정 시드 → 반복 실행해도 같은 대기 순서 (Quill 은 mt19937) */
static uint64_t next_rand(uint64_t *s)
{
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void busy_wait(uint64_t *rng)
{
    int64_t d = MIN_WAIT_NS + (int64_t)(next_rand(rng) % (uint64_t)(MAX_WAIT_NS - MIN_WAIT_NS + 1));
    int64_t end = mono_ns() + d;
    while (mono_ns() < end) {
    }
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* ---- 1) Quill hot path latency ---- */

typedef enum { SC_NUMBERS, SC_STRING } scenario;

typedef struct {
    logger *lg;
    scenario sc;
    int thread_num; /* 1부터 */
    int thread_count;
    size_t iterations;
    size_t msgs; /* 이 스레드가 반복마다 찍는 줄 수 */
    double ns_per_tick;
    atomic_size_t *started;
    uint64_t *lat;
} lat_arg;

static void *lat_worker(void *p)
{
    lat_arg *a = p;
    pin_self(a->thread_num);

    /* on_thread_start */
    LOG_ANY(a->lg, "preallocate%s", "");
    flush_logger(a->lg);

    atomic_fetch_add(a->started, 1);
    while (atomic_load(a->started) != (size_t)a->thread_count) {
    }

    uint64_t rng = 0x51ee50a7u + (uint64_t)a->thread_num;
    for (size_t it = 0; it < a->iterations; it++) {
        double d = (double)it + 0.1 * (double)it;
        uint64_t t0 = ser_rdtsc();
        if (a->sc == SC_NUMBERS)
            for (size_t i = 0; i < a->msgs; i++)
                LOG_ANY(a->lg, "Logging iteration: %zu, message: %zu, double: %g", it, i, d);
        else
            for (size_t i = 0; i < a->msgs; i++)
                LOG_ANY(a->lg, "Logging int: %zu, int: %zu, string: %s", it, i, LARGE_STR);
        uint64_t t1 = ser_rdtsc();
        a->lat[it] = (uint64_t)((double)(t1 - t0) / (double)a->msgs * a->ns_per_tick);
        busy_wait(&rng);
    }

    /* on_thread_exit: 다음 측정이 빈 큐에서 시작하도록 */
    flush_logger(a->lg);
    return NULL;
}

static int run_latency(logger_kind kind, scenario sc, size_t iterations)
{
    static const int thread_list[] = {1, 4};
    pin_self(MAIN_CPU);
    logger lg;
    char path[64];
    snprintf(path, sizeof path, "cmp_hot_path_%s.log", kind_name[kind]);
    if (open_logger(&lg, kind, path, LOG_INFO) != 0) {
        perror("open logger");
        return 1;
    }
    struct timespec ts = {1, 0}; /* Quill: 백엔드 시작 후 1초 대기 */
    nanosleep(&ts, NULL);

    double ns_per_tick = calibrate_ns_per_tick();
    for (size_t t = 0; t < sizeof thread_list / sizeof thread_list[0]; t++) {
        int tc = thread_list[t];
        printf("running for %d thread(s)\n", tc);
        pthread_t th[8];
        lat_arg args[8];
        atomic_size_t started = 0;
        for (int i = 0; i < tc; i++) {
            size_t base = MESSAGES_PER_ITERATION / tc, rem = MESSAGES_PER_ITERATION % tc;
            args[i] = (lat_arg){.lg = &lg, .sc = sc, .thread_num = i + 1, .thread_count = tc,
                                .iterations = iterations, .msgs = base + ((size_t)i < rem),
                                .ns_per_tick = ns_per_tick, .started = &started,
                                .lat = malloc(iterations * sizeof(uint64_t))};
            pthread_create(&th[i], NULL, lat_worker, &args[i]);
        }
        size_t n = 0;
        uint64_t *all = malloc(iterations * tc * sizeof(uint64_t));
        for (int i = 0; i < tc; i++) {
            pthread_join(th[i], NULL);
            memcpy(all + n, args[i].lat, iterations * sizeof(uint64_t));
            n += iterations;
            free(args[i].lat);
        }
        qsort(all, n, sizeof all[0], cmp_u64);
#define PCT(p) all[(size_t)ceil((double)n * (p)) == 0 ? 0 : (size_t)ceil((double)n * (p)) - 1]
        printf("Thread Count %d - Total messages %zu - Logger: %s - Benchmark: Hot Path Latency (%s) / Nanoseconds\n",
               tc, iterations * MESSAGES_PER_ITERATION, kind_desc[kind], sc == SC_NUMBERS ? "numbers" : "large string");
        printf(" |  50th | 75th | 90th | 95th | 99th | 99.9th | Worst |\n");
        printf(" |  %lu  |  %lu  |  %lu  |  %lu  |  %lu  |  %lu  |  %lu  |\n\n", (unsigned long)PCT(0.5),
               (unsigned long)PCT(0.75), (unsigned long)PCT(0.9), (unsigned long)PCT(0.95), (unsigned long)PCT(0.99),
               (unsigned long)PCT(0.999), (unsigned long)all[n - 1]);
#undef PCT
        free(all);
    }
    if (lg.lf)
        printf("dropped: %lu\n", lflog_dropped(lg.lf));
    close_logger(&lg);
    return 0;
}

/* ---- 2) Quill backend throughput ---- */

static int run_throughput(logger_kind kind, size_t total)
{
    pin_self(THROUGHPUT_CPU);
    logger lg;
    char path[64];
    snprintf(path, sizeof path, "cmp_throughput_%s.log", kind_name[kind]);
    if (open_logger(&lg, kind, path, LOG_INFO) != 0) {
        perror("open logger");
        return 1;
    }
    LOG_ANY(&lg, "preallocate%s", "");
    flush_logger(&lg);

    int64_t t0 = mono_ns();
    for (size_t i = 0; i < total; i++)
        LOG_ANY(&lg, "Iteration: %zu int: %zu double: %g", i, i * 2, (double)i / 2);
    flush_logger(&lg);
    double sec = (double)(mono_ns() - t0) / 1e9;

    printf("%s\nThroughput is %.2f million msgs/sec average, total time elapsed: %.0f ms for %zu log messages\n",
           kind_desc[kind], (double)total / sec / 1e6, sec * 1e3, total);
    close_logger(&lg);
    return 0;
}

/* ---- 3) spdlog bench.cpp (basic file sink) ---- */

typedef struct {
    logger *lg;
    int n;
} sp_arg;

static void *sp_worker(void *p)
{
    sp_arg *a = p;
    for (int j = 0; j < a->n; j++)
        LOG_ANY(a->lg, "Hello logger: msg number %d", j);
    return NULL;
}

/* threads == 0: 단일 스레드(호출 스레드에서 직접) */
static void sp_bench(logger_kind kind, const char *label, int howmany, int threads, log_level min_level)
{
    logger lg;
    char path[64];
    snprintf(path, sizeof path, "cmp_spdlog_%s.log", kind_name[kind]);
    if (open_logger(&lg, kind, path, min_level) != 0) {
        perror("open logger");
        exit(1);
    }
    int64_t t0 = mono_ns();
    if (threads == 0) {
        sp_arg a = {&lg, howmany};
        sp_worker(&a);
    } else {
        pthread_t th[1000];
        sp_arg a = {&lg, howmany / threads};
        for (int t = 0; t < threads; t++)
            pthread_create(&th[t], NULL, sp_worker, &a);
        for (int t = 0; t < threads; t++)
            pthread_join(th[t], NULL);
    }
    double call = (double)(mono_ns() - t0) / 1e9;
    flush_logger(&lg);
    double disk = (double)(mono_ns() - t0) / 1e9;
    printf("%-30s Elapsed: %0.2f secs %16.0f/sec   (to disk: %0.3f secs %12.0f/sec)\n", label, call, howmany / call,
           disk, howmany / disk);
    close_logger(&lg);
}

static int run_spdlog(logger_kind kind, int howmany, int threads)
{
    char label[64];
    printf("%s\n", kind_desc[kind]);
    printf("**************************************************************\n");
    printf("Single threaded: %d messages\n", howmany);
    printf("**************************************************************\n");
    snprintf(label, sizeof label, "%s_st", kind_name[kind]);
    sp_bench(kind, label, howmany, 0, LOG_INFO);
    sp_bench(kind, "level-off", howmany, 0, LOG_FATAL + 1);
    int tl[2] = {1, threads};
    for (int k = 0; k < 2; k++) {
        printf("**************************************************************\n");
        printf("Multi threaded: %d threads, %d messages\n", tl[k], howmany);
        printf("**************************************************************\n");
        snprintf(label, sizeof label, "%s_mt", kind_name[kind]);
        sp_bench(kind, label, howmany, tl[k], LOG_INFO);
        sp_bench(kind, "level-off", howmany, 0, LOG_FATAL + 1);
    }
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: cmp_bench latency    <logger> <numbers|string> [iterations=10000]\n"
            "       cmp_bench throughput <logger> [messages=4000000]\n"
            "       cmp_bench spdlog     <logger> [messages=250000] [threads=4]\n"
            "  logger: lflog | lflog_text | async | sync_buf | sync_direct\n");
    exit(2);
}

int main(int argc, char **argv)
{
    ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (argc < 3)
        usage();
    logger_kind kind;
    if (parse_kind(argv[2], &kind) != 0)
        usage();
    if (strcmp(argv[1], "latency") == 0) {
        if (argc < 4)
            usage();
        scenario sc = strcmp(argv[3], "string") == 0 ? SC_STRING : SC_NUMBERS;
        return run_latency(kind, sc, argc > 4 ? strtoul(argv[4], NULL, 10) : 10000);
    }
    if (strcmp(argv[1], "throughput") == 0)
        return run_throughput(kind, argc > 3 ? strtoul(argv[3], NULL, 10) : 4000000);
    if (strcmp(argv[1], "spdlog") == 0) {
        int threads = argc > 4 ? atoi(argv[4]) : 4;
        if (threads < 1 || threads > 1000)
            usage();
        return run_spdlog(kind, argc > 3 ? atoi(argv[3]) : 250000, threads);
    }
    usage();
    return 2;
}
