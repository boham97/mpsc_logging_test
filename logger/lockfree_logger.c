#define _GNU_SOURCE
#include "lockfree_logger.h"

#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define CACHE_LINE 64
#define WRITE_BATCH (1u << 20) /* 소비자가 한 번에 write() 하는 최대 크기 */
#define SPIN_BEFORE_SLEEP 200

#if defined(__x86_64__) || defined(__i386__)
#define cpu_relax() __builtin_ia32_pause()
#elif defined(__aarch64__)
#define cpu_relax() __asm__ __volatile__("yield")
#else
#define cpu_relax() ((void)0)
#endif

typedef struct {
    _Alignas(CACHE_LINE) _Atomic size_t seq;  /* == pos: 비어서 생산자 대기, == pos+1: 채워져서 소비자 대기 */
    uint32_t len;
    char data[LOG_LINE_MAX];
} lf_cell;

struct lf_logger {
    /* 생산자들이 경쟁하는 변수 → 자기만의 캐시라인 (false sharing 방지) */
    _Alignas(CACHE_LINE) _Atomic size_t enqueue_pos;

    /* 소비자만 쓰는 변수 */
    _Alignas(CACHE_LINE) size_t dequeue_pos;
    _Atomic size_t written_pos;  /* 디스크까지 쓴 위치 (flush 대기용) */

    /* 생산자가 읽고 소비자가 쓰는 변수 */
    _Alignas(CACHE_LINE) _Atomic int sleeping;  /* futex 워드 */
    _Atomic int stop;

    _Alignas(CACHE_LINE) _Atomic unsigned long dropped;
    _Atomic int min_level;
    int drop_on_full;
    int fd;
    size_t mask;
    lf_cell *cells;
    pthread_t thread;
};

static long futex(_Atomic int *addr, int op, int val, const struct timespec *timeout)
{
    return syscall(SYS_futex, (int *)addr, op, val, timeout, NULL, 0);
}

/* 생산자가 데이터 공개 후 호출: 소비자가 자고 있을 때만 시스템콜 */
static void wake_consumer(lf_logger *lg)
{
    /*
     * Dekker 패턴: 생산자 [seq store → fence → sleeping load]
     *              소비자 [sleeping store → fence → seq load]
     * 두 fence 덕분에 "소비자는 빈 큐를 봤고, 생산자는 sleeping=0 을 봤다"는
     * 상황이 동시에 일어날 수 없다 → 깨우기 유실 없음.
     */
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load_explicit(&lg->sleeping, memory_order_relaxed) &&
        atomic_exchange_explicit(&lg->sleeping, 0, memory_order_relaxed))
        futex(&lg->sleeping, FUTEX_WAKE_PRIVATE, 1, NULL);
}

/* 0: 성공, -1: 큐 가득 참 */
static int try_enqueue(lf_logger *lg, const char *buf, size_t n)
{
    size_t pos = atomic_load_explicit(&lg->enqueue_pos, memory_order_relaxed);
    lf_cell *cell;
    for (;;) {
        cell = &lg->cells[pos & lg->mask];
        size_t seq = atomic_load_explicit(&cell->seq, memory_order_acquire);
        intptr_t diff = (intptr_t)seq - (intptr_t)pos;
        if (diff == 0) {
            /* 비어 있음 → 이 슬롯을 차지하려고 시도. 실패하면 pos 가 갱신되어 재시도 */
            if (atomic_compare_exchange_weak_explicit(&lg->enqueue_pos, &pos, pos + 1,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed))
                break;
        } else if (diff < 0) {
            return -1; /* 소비자가 아직 한 바퀴 전 데이터를 안 가져감 → 가득 참 */
        } else {
            pos = atomic_load_explicit(&lg->enqueue_pos, memory_order_relaxed);
        }
    }
    memcpy(cell->data, buf, n);
    cell->len = (uint32_t)n;
    atomic_store_explicit(&cell->seq, pos + 1, memory_order_release); /* 공개 */
    return 0;
}

/* 소비자: 다음 슬롯이 채워져 있으면 out 에 붙이고 1, 아니면 0 */
static int try_dequeue(lf_logger *lg, char *out, size_t *out_len)
{
    size_t pos = lg->dequeue_pos;
    lf_cell *cell = &lg->cells[pos & lg->mask];
    if (atomic_load_explicit(&cell->seq, memory_order_acquire) != pos + 1)
        return 0;
    memcpy(out + *out_len, cell->data, cell->len);
    *out_len += cell->len;
    /* 슬롯 반납: 한 바퀴 뒤의 생산자(pos + capacity)가 쓸 수 있게 */
    atomic_store_explicit(&cell->seq, pos + lg->mask + 1, memory_order_release);
    lg->dequeue_pos = pos + 1;
    return 1;
}

static int queue_has_data(lf_logger *lg)
{
    size_t pos = lg->dequeue_pos;
    return atomic_load_explicit(&lg->cells[pos & lg->mask].seq, memory_order_acquire) == pos + 1;
}

static void write_batch(lf_logger *lg, char *batch, size_t *len, unsigned long *last_dropped)
{
    unsigned long d = atomic_load_explicit(&lg->dropped, memory_order_relaxed);
    if (d != *last_dropped) {
        char note[128];
        int n = snprintf(note, sizeof(note),
                         "*** lf_logger: dropped %lu messages (queue full) ***\n",
                         d - *last_dropped);
        log_write_all(lg->fd, note, (size_t)n);
        *last_dropped = d;
    }
    if (*len > 0) {
        log_write_all(lg->fd, batch, *len);
        *len = 0;
    }
    atomic_store_explicit(&lg->written_pos, lg->dequeue_pos, memory_order_release);
}

static void *consumer_main(void *arg)
{
    lf_logger *lg = arg;
    char *batch = malloc(WRITE_BATCH);
    size_t len = 0;
    unsigned long last_dropped = 0;

    for (;;) {
        /* 1) 있는 만큼 모으고, 배치가 차면 쓴다 */
        int got = 0;
        while (len + LOG_LINE_MAX <= WRITE_BATCH && try_dequeue(lg, batch, &len))
            got = 1;
        if (len + LOG_LINE_MAX > WRITE_BATCH) {
            write_batch(lg, batch, &len, &last_dropped);
            continue;
        }
        if (got)
            continue; /* 방금 비웠지만 더 들어왔을 수도 있으니 한 번 더 */

        /* 2) 큐가 비었다 → 모은 것을 디스크로 (로그가 뜸해도 바로 내려감) */
        if (len > 0 || atomic_load_explicit(&lg->written_pos, memory_order_relaxed) != lg->dequeue_pos)
            write_batch(lg, batch, &len, &last_dropped);

        /* 3) 종료 요청 + 확보만 되고 아직 안 채워진 슬롯도 없음 → 끝 */
        if (atomic_load_explicit(&lg->stop, memory_order_acquire) &&
            atomic_load_explicit(&lg->enqueue_pos, memory_order_acquire) == lg->dequeue_pos) {
            write_batch(lg, batch, &len, &last_dropped); /* 남은 drop 알림 */
            break;
        }

        /* 4) 잠깐 스핀 (부하가 높을 때 futex 시스템콜을 피함) */
        int spun;
        for (spun = 0; spun < SPIN_BEFORE_SLEEP && !queue_has_data(lg); spun++)
            cpu_relax();
        if (spun < SPIN_BEFORE_SLEEP)
            continue;

        /* 5) 잠들기: sleeping=1 → fence → 큐 재확인 → futex_wait */
        atomic_store_explicit(&lg->sleeping, 1, memory_order_relaxed);
        atomic_thread_fence(memory_order_seq_cst);
        if (queue_has_data(lg) || atomic_load_explicit(&lg->stop, memory_order_relaxed)) {
            atomic_store_explicit(&lg->sleeping, 0, memory_order_relaxed);
            continue;
        }
        /* 타임아웃은 안전망 (확보만 되고 공개가 늦어지는 슬롯 등) */
        struct timespec ts = {0, 100 * 1000 * 1000};
        futex(&lg->sleeping, FUTEX_WAIT_PRIVATE, 1, &ts);
        atomic_store_explicit(&lg->sleeping, 0, memory_order_relaxed);
    }

    free(batch);
    return NULL;
}

static size_t round_up_pow2(size_t v)
{
    size_t p = 2;
    while (p < v)
        p <<= 1;
    return p;
}

lf_logger *lflog_open(const char *path, const lf_logger_opts *opts)
{
    lf_logger_opts o = opts ? *opts : (lf_logger_opts){LOG_INFO, 16384, 0};
    size_t cap = round_up_pow2(o.capacity < 2 ? 2 : o.capacity);

    lf_logger *lg = aligned_alloc(CACHE_LINE, sizeof(*lg));
    if (!lg)
        return NULL;
    memset(lg, 0, sizeof(*lg));

    lg->cells = aligned_alloc(CACHE_LINE, cap * sizeof(lf_cell));
    lg->fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (!lg->cells || lg->fd < 0)
        goto fail;

    lg->mask = cap - 1;
    lg->drop_on_full = o.drop_on_full;
    atomic_init(&lg->min_level, o.min_level);
    for (size_t i = 0; i < cap; i++)
        atomic_init(&lg->cells[i].seq, i);

    if (pthread_create(&lg->thread, NULL, consumer_main, lg) != 0)
        goto fail;
    return lg;

fail:
    if (lg->fd >= 0)
        close(lg->fd);
    free(lg->cells);
    free(lg);
    return NULL;
}

void lflog_close(lf_logger *lg)
{
    if (!lg)
        return;
    atomic_store_explicit(&lg->stop, 1, memory_order_release);
    atomic_store_explicit(&lg->sleeping, 0, memory_order_relaxed);
    futex(&lg->sleeping, FUTEX_WAKE_PRIVATE, 1, NULL);
    pthread_join(lg->thread, NULL);

    close(lg->fd);
    free(lg->cells);
    free(lg);
}

void lflog_write(lf_logger *lg, log_level lv, const char *file, int line,
                 const char *fmt, ...)
{
    if (!lflog_enabled(lg, lv))
        return;

    /* 슬롯을 잡기 전에 포맷팅 → 슬롯을 "확보했지만 미공개"인 시간을 최소화 */
    char buf[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    size_t n = log_format_line(buf, sizeof(buf), lv, file, line, fmt, ap);
    va_end(ap);

    while (try_enqueue(lg, buf, n) != 0) {
        if (lg->drop_on_full || atomic_load_explicit(&lg->stop, memory_order_relaxed)) {
            atomic_fetch_add_explicit(&lg->dropped, 1, memory_order_relaxed);
            return;
        }
        wake_consumer(lg);
        sched_yield();
    }
    wake_consumer(lg);
}

void lflog_flush(lf_logger *lg)
{
    size_t target = atomic_load_explicit(&lg->enqueue_pos, memory_order_acquire);
    wake_consumer(lg);
    while (atomic_load_explicit(&lg->written_pos, memory_order_acquire) - target > SIZE_MAX / 2) {
        /* written_pos < target (랩어라운드 고려). flush 는 드문 호출이라 폴링으로 충분 */
        struct timespec ts = {0, 50 * 1000};
        nanosleep(&ts, NULL);
    }
}

int lflog_enabled(const lf_logger *lg, log_level lv)
{
    return (int)lv >= atomic_load_explicit(&((lf_logger *)lg)->min_level, memory_order_relaxed);
}

void lflog_set_level(lf_logger *lg, log_level lv)
{
    atomic_store_explicit(&lg->min_level, lv, memory_order_relaxed);
}

unsigned long lflog_dropped(lf_logger *lg)
{
    return atomic_load_explicit(&lg->dropped, memory_order_relaxed);
}
