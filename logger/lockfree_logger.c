#define _GNU_SOURCE
#include "lockfree_logger.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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

/* ======================== 시각: TSC ======================== */

/*
 * 호출 스레드는 시각을 TSC 원값으로만 남기고(rdtsc ≈ 8ns, clock_gettime ≈ 22ns),
 * 실제 시각으로 바꾸는 일은 로깅 스레드가 한다.
 * TSC 를 믿을 수 없으면(x86-64 가 아님, invariant TSC 없음, 커널 clocksource 가 tsc 가 아님)
 * 또는 -DLF_NO_TSC 로 빌드하면 CLOCK_REALTIME 을 ns 로 저장한다.
 */
#if defined(__x86_64__) && !defined(LF_NO_TSC)
#include <cpuid.h>
#include <x86intrin.h>
#define LF_HAVE_TSC 1
#else
#define LF_HAVE_TSC 0
#endif

#define CLOCK_RESYNC_NS 1000000000LL /* 로깅 스레드가 이 간격마다 기준점을 다시 잡는다 */

/* 로깅 스레드만 쓰는 기준점: 시각 = base_ns + (stamp - base_tsc) × ns_per_tick */
typedef struct {
    uint64_t base_tsc;
    int64_t base_ns;
    double ns_per_tick;
    int64_t resync_ticks;
} lf_clock;

static int64_t realtime_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

#if LF_HAVE_TSC
/* 프로세스에서 한 번 정해짐 (pthread_once). 호출 스레드는 lflog_open 이 끝난 뒤에만 읽는다 */
static int use_tsc;
static double tsc_ns_per_tick;

/* (TSC, 실제 시각) 한 쌍. clock_gettime 앞뒤 TSC 의 가운데를 쓰고, 다섯 번 중 간격이 가장 짧은 것 */
static void tsc_pair(uint64_t *tsc, int64_t *ns)
{
    uint64_t best = UINT64_MAX, best_tsc = 0;
    int64_t best_ns = 0;
    for (int i = 0; i < 5; i++) {
        uint64_t t0 = __rdtsc();
        int64_t n = realtime_ns();
        uint64_t t1 = __rdtsc();
        if (t1 - t0 < best) {
            best = t1 - t0;
            best_tsc = t0 + (t1 - t0) / 2;
            best_ns = n;
        }
    }
    *tsc = best_tsc;
    *ns = best_ns;
}

static int tsc_reliable(void)
{
    unsigned a, b, c, d;
    if (!__get_cpuid(0x80000007, &a, &b, &c, &d) || !(d & (1u << 8))) /* invariant TSC */
        return 0;
    /* 커널이 TSC 를 불안정하다고 보고 다른 clocksource 로 바꿨으면 쓰지 않는다 */
    char cs[32] = "";
    FILE *fp = fopen("/sys/devices/system/clocksource/clocksource0/current_clocksource", "r");
    if (fp) {
        if (!fgets(cs, sizeof(cs), fp))
            cs[0] = '\0';
        fclose(fp);
    }
    return strcmp(cs, "tsc\n") == 0;
}

/* 주파수 첫 보정: 10ms 떨어진 두 쌍 (오차 수십 ppm). 이후는 로깅 스레드가 1초마다 다시 잰다 */
static void tsc_init(void)
{
    if (!tsc_reliable())
        return;
    uint64_t t0, t1;
    int64_t n0, n1;
    tsc_pair(&t0, &n0);
    struct timespec q = {0, 10 * 1000 * 1000};
    nanosleep(&q, NULL);
    tsc_pair(&t1, &n1);
    if (t1 <= t0 || n1 <= n0)
        return;
    tsc_ns_per_tick = (double)(n1 - n0) / (double)(t1 - t0);
    use_tsc = 1;
}

static pthread_once_t tsc_once = PTHREAD_ONCE_INIT;

static void clock_resync(lf_clock *ck)
{
    uint64_t t;
    int64_t n;
    tsc_pair(&t, &n);
    /*
     * 지난 기준점부터의 실제 비율로 주파수를 다시 잰다 (NTP 보정 속도도 따라감).
     * 시계가 바뀌었으면(settimeofday 등) 비율이 엉뚱하게 나오므로 기준점만 옮긴다.
     */
    double r = (double)(n - ck->base_ns) / (double)(t - ck->base_tsc);
    if (r > tsc_ns_per_tick * 0.999 && r < tsc_ns_per_tick * 1.001)
        ck->ns_per_tick = r;
    ck->base_tsc = t;
    ck->base_ns = n;
}
#endif

static void clock_init(lf_clock *ck)
{
    memset(ck, 0, sizeof(*ck));
#if LF_HAVE_TSC
    pthread_once(&tsc_once, tsc_init);
    if (use_tsc) {
        tsc_pair(&ck->base_tsc, &ck->base_ns);
        ck->ns_per_tick = tsc_ns_per_tick;
        ck->resync_ticks = (int64_t)((double)CLOCK_RESYNC_NS / tsc_ns_per_tick);
    }
#endif
}

/* 호출 스레드가 남기는 시각 */
static inline uint64_t clock_stamp(void)
{
#if LF_HAVE_TSC
    if (use_tsc)
        return __rdtsc();
#endif
    return (uint64_t)realtime_ns();
}

/* 로깅 스레드: 남겨진 시각 → 실제 시각 */
static void clock_to_timespec(lf_clock *ck, uint64_t stamp, struct timespec *ts)
{
    int64_t ns = (int64_t)stamp;
#if LF_HAVE_TSC
    if (use_tsc) {
        if ((int64_t)(stamp - ck->base_tsc) > ck->resync_ticks)
            clock_resync(ck);
        ns = ck->base_ns + (int64_t)((double)(int64_t)(stamp - ck->base_tsc) * ck->ns_per_tick);
    }
#else
    (void)ck;
#endif
    ts->tv_sec = (time_t)(ns / 1000000000LL);
    ts->tv_nsec = (long)(ns % 1000000000LL);
}

enum { CELL_TEXT = 0, CELL_RECORD = 1 };

typedef struct {
    _Alignas(CACHE_LINE) _Atomic size_t seq;  /* == pos: 비어서 생산자 대기, == pos+1: 채워져서 소비자 대기 */
    uint32_t len;
    uint32_t kind;  /* CELL_TEXT: 완성된 한 줄, CELL_RECORD: 지연 포맷팅 레코드 */
    char data[LOG_LINE_MAX];
} lf_cell;

struct lf_logger {
    /* 생산자들이 경쟁하는 변수 → 자기만의 캐시라인 (false sharing 방지) */
    _Alignas(CACHE_LINE) _Atomic size_t enqueue_pos;

    /* 소비자만 쓰는 변수 */
    _Alignas(CACHE_LINE) size_t dequeue_pos;
    lf_clock clock;              /* 레코드의 시각 변환용 */
    _Atomic size_t written_pos;  /* 디스크까지 쓴 위치 (flush 대기용) */

    /* 생산자가 읽고 소비자가 쓰는 변수 */
    _Alignas(CACHE_LINE) _Atomic int sleeping;  /* futex 워드 */
    _Atomic int stop;
    int busy_poll;               /* 1: 소비자가 잠들지 않음 (sleeping 은 항상 0) */

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
static int try_enqueue(lf_logger *lg, const char *buf, size_t n, uint32_t kind)
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
    cell->kind = kind;
    atomic_store_explicit(&cell->seq, pos + 1, memory_order_release); /* 공개 */
    return 0;
}

/* ======================== 지연 포맷팅: 레코드 직렬화 / 렌더링 ======================== */

/*
 * 레코드 레이아웃 (슬롯 data 안):
 *   [rec_hdr][태그 1B][값] [태그 1B][값] ...
 *   값: 정수/실수/포인터 = 8B, 문자열 = 길이 2B + 내용 + '\0'
 * 슬롯이 모자라면 문자열은 잘리고, 그래도 안 들어가는 인자는 생략된다.
 */
typedef struct {
    const char *fmt;   /* 문자열 리터럴 → 정적 수명 */
    const char *file;  /* __FILE__ → 정적 수명 */
    uint64_t stamp;    /* clock_stamp(): TSC 원값 또는 CLOCK_REALTIME ns */
    int32_t line;
    int32_t tid;
    uint8_t level;
    uint8_t nargs;     /* 실제로 직렬화된 인자 수 */
} rec_hdr;

_Static_assert(sizeof(((lf_arg *)0)->v) == 8, "lf_arg value must be 8 bytes");

enum { LM_NONE, LM_HH, LM_H, LM_L, LM_LL };

#define KNOWN_CONVS "diouxXcfFeEgGaAspn" /* 인자를 하나 소비하는 변환 문자 */

/*
 * printf 지정자 하나를 해석한 결과. 생산자(포인터 인자 분류)와 소비자(렌더링)가
 * 같은 함수를 써서, 어느 인자가 어느 지정자에 쓰이는지 해석이 어긋나지 않게 한다.
 */
typedef struct {
    const char *flags, *width, *prec;  /* 원문에서의 위치 */
    size_t nflags, nwidth, nprec;
    int width_star, has_prec, prec_star;
    int lm;
    char conv;        /* 0: 문자열이 지정자 중간에서 끝남 */
    const char *end;  /* 지정자 바로 다음 글자 */
} fmt_spec;

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* f 는 '%' 다음 글자를 가리킨다 ("%%" 는 호출자가 처리) */
static void parse_spec(const char *f, fmt_spec *s)
{
    s->flags = f;
    while (*f && strchr("-+ #0'", *f))
        f++;
    s->nflags = (size_t)(f - s->flags);

    s->width_star = (*f == '*');
    if (s->width_star)
        f++;
    s->width = f;
    while (!s->width_star && is_digit(*f))
        f++;
    s->nwidth = (size_t)(f - s->width);

    s->has_prec = (*f == '.');
    s->prec_star = 0;
    if (s->has_prec) {
        f++;
        s->prec_star = (*f == '*');
        if (s->prec_star)
            f++;
    }
    s->prec = f;
    while (s->has_prec && !s->prec_star && is_digit(*f))
        f++;
    s->nprec = (size_t)(f - s->prec);

    s->lm = LM_NONE;
    if (*f == 'h') {
        f++;
        s->lm = LM_H;
        if (*f == 'h') {
            f++;
            s->lm = LM_HH;
        }
    } else if (*f == 'l') {
        f++;
        s->lm = LM_L;
        if (*f == 'l') {
            f++;
            s->lm = LM_LL;
        }
    } else if (*f && strchr("jztLq", *f)) {
        f++;
        s->lm = LM_LL;
    }

    s->conv = *f;
    s->end = *f ? f + 1 : f;
}

static long long arg_as_i64(const lf_arg *a)
{
    switch (a->type) {
    case LF_ARG_I64: return a->v.i;
    case LF_ARG_U64: return (long long)a->v.u;
    case LF_ARG_F64: return (long long)a->v.d;
    default:         return (long long)(intptr_t)a->v.p;
    }
}

/*
 * 인자별 기록 방식 (plan). 포맷 문자열과 인자 타입에만 달려 있어서 호출 위치마다 항상 같다.
 *   PLAN_VALUE         : 값 그대로 (포인터는 주소만, 내용을 읽지 않음)
 *   PLAN_PREC_ARG(j)   : 문자열, 최대 길이는 j 번째 인자 값 (%.*s → 호출마다 다를 수 있음)
 *   0 이상             : 문자열, 그 길이까지만 읽음 (PLAN_NO_PREC: 제한 없음)
 */
#define PLAN_VALUE (-1)
#define PLAN_PREC_ARG(j) (-2 - (j))
#define PLAN_NO_PREC SHRT_MAX

_Static_assert(LOG_LINE_MAX * 10 < SHRT_MAX, "fixed precision must fit in short");

/*
 * 포인터 인자를 어떻게 기록할지 포맷 문자열을 보고 정한다 (printf 와 같은 의미가 되도록).
 * - %s 에 쓰이는 포인터: 내용을 복사하되 정밀도만큼만 읽는다 → NUL 없는 버퍼도 안전.
 *   char* 가 아닌 unsigned char* 등도 %s 면 문자열로 출력한다.
 * - 그 외 (%p 등): 포인터 값만 기록한다 → char* 라도 내용을 읽지 않는다.
 */
static void build_plan(const char *fmt, const lf_arg *args, int nargs, short *plan)
{
    int has_ptr = 0;
    for (int i = 0; i < nargs; i++) {
        plan[i] = PLAN_VALUE;
        has_ptr |= args[i].type == LF_ARG_STR || args[i].type == LF_ARG_PTR;
    }
    if (!has_ptr) /* 숫자만 있으면 포맷 문자열을 훑을 필요 없음 */
        return;

    int i = 0;
    const char *f = fmt;
    while (i < nargs && (f = strchr(f, '%')) != NULL) {
        if (f[1] == '%') {
            f += 2;
            continue;
        }
        fmt_spec s;
        parse_spec(f + 1, &s);
        if (!s.conv)
            break;
        f = s.end;
        if (!strchr(KNOWN_CONVS, s.conv))
            continue;

        if (s.width_star)
            i++;
        short str_plan = PLAN_NO_PREC;
        if (s.prec_star) {
            str_plan = (short)PLAN_PREC_ARG(i);
            i++;
        } else if (s.has_prec) {
            int prec = 0; /* "%.s" 는 정밀도 0 */
            for (size_t k = 0; k < s.nprec && prec < LOG_LINE_MAX; k++)
                prec = prec * 10 + (s.prec[k] - '0');
            str_plan = (short)prec;
        }
        if (i >= nargs)
            break;
        if (s.conv == 's' && (args[i].type == LF_ARG_STR || args[i].type == LF_ARG_PTR))
            plan[i] = str_plan;
        i++;
    }
}

/*
 * 캐시가 차 있으면 그대로 쓰고, 아니면 계산한다. 처음 계산한 스레드 하나만 캐시를 채운다
 * (동시에 처음 호출한 다른 스레드들은 각자 계산한 local 을 쓴다).
 */
static const short *get_plan(const char *fmt, const lf_arg *args, int nargs,
                             lf_fmt_cache *cache, short *local)
{
    if (cache && atomic_load_explicit(&cache->state, memory_order_acquire) == LF_CACHE_READY)
        return cache->plan;
    build_plan(fmt, args, nargs, local);
    int expect = LF_CACHE_EMPTY;
    if (cache && atomic_compare_exchange_strong_explicit(&cache->state, &expect, LF_CACHE_FILLING,
                                                         memory_order_relaxed,
                                                         memory_order_relaxed)) {
        memcpy(cache->plan, local, sizeof(short) * (size_t)nargs);
        atomic_store_explicit(&cache->state, LF_CACHE_READY, memory_order_release);
    }
    return local;
}

/* 생산자 쪽: 숫자→문자열 변환 없이 값만 복사 */
static size_t encode_record(char *buf, size_t cap, log_level lv, const char *file, int line,
                            const char *fmt, const lf_arg *args, int nargs, lf_fmt_cache *cache)
{
    rec_hdr h = {fmt, file, clock_stamp(), line, log_current_tid(), (uint8_t)lv, 0};

    if (nargs > LF_MAX_ARGS)
        nargs = LF_MAX_ARGS;
    short local[LF_MAX_ARGS];
    const short *plan = get_plan(fmt, args, nargs, cache, local);

    size_t off = sizeof(h);
    for (int i = 0; i < nargs; i++) {
        const lf_arg *a = &args[i];
        if (plan[i] != PLAN_VALUE) {
            if (off + 4 > cap) /* 태그 + 길이 + '\0' */
                break;
            int str_max = plan[i];
            if (str_max < 0) { /* %.*s: 정밀도는 이번 호출의 인자 값. 음수면 생략과 같음 (printf 와 동일) */
                int j = -2 - str_max;
                str_max = j < nargs ? (int)arg_as_i64(&args[j]) : -1;
                if (str_max < 0)
                    str_max = INT_MAX;
            }
            /* NULL: glibc 처럼 정밀도가 6 이상일 때만 "(null)" */
            const char *str = a->v.s ? a->v.s : (str_max >= 6 ? "(null)" : "");
            size_t room = cap - off - 4;
            size_t n = strnlen(str, (size_t)str_max < room ? (size_t)str_max : room);
            uint16_t n16 = (uint16_t)n;
            buf[off] = (char)LF_ARG_STR;
            memcpy(buf + off + 1, &n16, 2);
            memcpy(buf + off + 3, str, n);
            buf[off + 3 + n] = '\0';
            off += 4 + n;
        } else {
            if (off + 9 > cap)
                break;
            /* %s 가 아닌 곳에 쓰인 char* 는 포인터 값으로 기록 (v.s 와 v.p 는 같은 자리) */
            buf[off] = (char)(a->type == LF_ARG_STR ? LF_ARG_PTR : a->type);
            memcpy(buf + off + 1, &a->v, 8);
            off += 9;
        }
        h.nargs++;
    }
    memcpy(buf, &h, sizeof(h));
    return off;
}

typedef struct {
    const char *p;
    int left;
} arg_reader;

static int next_arg(arg_reader *r, lf_arg *a)
{
    if (r->left <= 0)
        return 0;
    r->left--;
    a->type = (lf_arg_type)(unsigned char)*r->p++;
    if (a->type == LF_ARG_STR) {
        uint16_t n;
        memcpy(&n, r->p, 2);
        a->v.s = r->p + 2;
        r->p += 2 + n + 1;
    } else {
        memcpy(&a->v, r->p, 8);
        r->p += 8;
    }
    return 1;
}

typedef struct {
    char *buf;
    size_t len;
    size_t cap;  /* len 은 항상 cap-1 이하 */
} outbuf;

static void out_putn(outbuf *o, const char *s, size_t n)
{
    size_t room = o->cap - 1 - o->len;
    if (n > room)
        n = room;
    memcpy(o->buf + o->len, s, n);
    o->len += n;
}

static void out_printf(outbuf *o, const char *spec, ...)
{
    size_t room = o->cap - o->len;
    if (room <= 1)
        return;
    va_list ap;
    va_start(ap, spec);
    int n = vsnprintf(o->buf + o->len, room, spec, ap);
    va_end(ap);
    if (n > 0)
        o->len += ((size_t)n >= room) ? room - 1 : (size_t)n;
}

/* src 를 spec 끝에 붙이되 spec 길이가 limit 를 넘지 않게 한다 */
static size_t spec_append(char *spec, size_t sl, const char *src, size_t n, size_t limit)
{
    for (size_t k = 0; k < n && sl < limit; k++)
        spec[sl++] = src[k];
    return sl;
}

static double arg_as_f64(const lf_arg *a)
{
    switch (a->type) {
    case LF_ARG_F64: return a->v.d;
    case LF_ARG_I64: return (double)a->v.i;
    case LF_ARG_U64: return (double)a->v.u;
    default:         return 0.0;
    }
}

/* 지정자와 타입이 안 맞을 때: 타입대로 출력 */
static void out_arg_default(outbuf *o, const lf_arg *a)
{
    switch (a->type) {
    case LF_ARG_I64: out_printf(o, "%lld", a->v.i); break;
    case LF_ARG_U64: out_printf(o, "%llu", a->v.u); break;
    case LF_ARG_F64: out_printf(o, "%g", a->v.d); break;
    case LF_ARG_STR: out_printf(o, "%s", a->v.s); break;
    case LF_ARG_PTR: out_printf(o, "%p", a->v.p); break;
    }
}

/*
 * 소비자 쪽: printf 포맷 문자열을 해석하면서 지정자 하나당 snprintf 한 번.
 * 길이 수식자(hh/h/l/ll/z/j/t/L)는 떼어 내고, 저장된 타입(64비트 정수/double)에 맞는
 * 수식자로 바꿔 끼운다. 단 hh/h/없음/l 은 printf 와 같은 결과가 나오도록 먼저 그 폭으로 잘라 준다.
 */
static void render_message(outbuf *o, const char *fmt, arg_reader *r)
{
    const char *f = fmt;
    while (*f) {
        if (*f != '%') {
            const char *pct = strchr(f, '%');
            size_t n = pct ? (size_t)(pct - f) : strlen(f);
            out_putn(o, f, n);
            f += n;
            continue;
        }
        if (f[1] == '%') {
            out_putn(o, "%", 1);
            f += 2;
            continue;
        }

        const char *start = f;
        fmt_spec s;
        parse_spec(f + 1, &s);
        f = s.end;
        int lm = s.lm;
        char conv = s.conv;
        if (!conv) { /* 문자열이 지정자 중간에서 끝남 → 그대로 출력 */
            out_putn(o, start, strlen(start));
            break;
        }
        if (!strchr(KNOWN_CONVS, conv)) { /* %m 등 모르는 지정자 → 원문 그대로 */
            out_putn(o, start, (size_t)(f - start));
            continue;
        }

        /* 플래그/폭/정밀도를 다시 조립. 길이 수식자와 변환 문자는 아래에서 붙인다 */
        char spec[64];
        size_t sl = 0;
        lf_arg a;
        spec[sl++] = '%';
        sl = spec_append(spec, sl, s.flags, s.nflags, 10);
        if (s.width_star) {
            if (next_arg(r, &a))
                sl += (size_t)snprintf(spec + sl, sizeof(spec) - sl, "%d", (int)arg_as_i64(&a));
        } else {
            sl = spec_append(spec, sl, s.width, s.nwidth, 24);
        }
        if (s.prec_star) {
            if (next_arg(r, &a)) {
                int prec = (int)arg_as_i64(&a);
                if (prec >= 0) /* 음수 정밀도는 생략한 것으로 취급 (printf 와 동일) */
                    sl += (size_t)snprintf(spec + sl, sizeof(spec) - sl, ".%d", prec);
            }
        } else if (s.has_prec) {
            spec[sl++] = '.';
            sl = spec_append(spec, sl, s.prec, s.nprec, 40);
        }

        if (!next_arg(r, &a)) {
            out_putn(o, "<?>", 3);
            continue;
        }

        switch (conv) {
        case 'd': case 'i': {
            if (a.type == LF_ARG_STR) { out_arg_default(o, &a); break; }
            long long v = arg_as_i64(&a);
            if (lm == LM_HH) v = (signed char)v;
            else if (lm == LM_H) v = (short)v;
            else if (lm == LM_NONE) v = (int)v;
            else if (lm == LM_L) v = (long)v;
            spec[sl] = 'l'; spec[sl + 1] = 'l'; spec[sl + 2] = conv; spec[sl + 3] = '\0';
            out_printf(o, spec, v);
            break;
        }
        case 'o': case 'u': case 'x': case 'X': {
            if (a.type == LF_ARG_STR) { out_arg_default(o, &a); break; }
            unsigned long long v = (unsigned long long)arg_as_i64(&a);
            if (lm == LM_HH) v = (unsigned char)v;
            else if (lm == LM_H) v = (unsigned short)v;
            else if (lm == LM_NONE) v = (unsigned int)v;
            else if (lm == LM_L) v = (unsigned long)v;
            spec[sl] = 'l'; spec[sl + 1] = 'l'; spec[sl + 2] = conv; spec[sl + 3] = '\0';
            out_printf(o, spec, v);
            break;
        }
        case 'c':
            if (a.type == LF_ARG_STR) { out_arg_default(o, &a); break; }
            spec[sl] = 'c'; spec[sl + 1] = '\0';
            out_printf(o, spec, (int)arg_as_i64(&a));
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            if (a.type == LF_ARG_STR || a.type == LF_ARG_PTR) { out_arg_default(o, &a); break; }
            spec[sl] = conv; spec[sl + 1] = '\0';
            out_printf(o, spec, arg_as_f64(&a));
            break;
        case 's':
            if (a.type != LF_ARG_STR) { out_arg_default(o, &a); break; }
            spec[sl] = 's'; spec[sl + 1] = '\0';
            out_printf(o, spec, a.v.s);
            break;
        case 'p':
            spec[sl] = 'p'; spec[sl + 1] = '\0';
            out_printf(o, spec, a.type == LF_ARG_PTR ? a.v.p : (const void *)(uintptr_t)a.v.u);
            break;
        case 'n': /* 의미 없음 (호출자 메모리에 쓸 수 없음) → 인자만 소비 */
            break;
        }
    }
}

/* 레코드 → 완성된 한 줄. 반환값: 쓴 바이트 수 ('\n' 포함, cap-1 이하) */
static size_t render_record(char *out, size_t cap, const char *data, lf_clock *ck)
{
    rec_hdr h;
    memcpy(&h, data, sizeof(h));
    struct timespec ts;
    clock_to_timespec(ck, h.stamp, &ts);

    outbuf o = {out, 0, cap - 1}; /* 마지막 '\n' 자리 확보 */
    o.len = log_format_prefix(out, o.cap, (log_level)h.level, h.file, h.line, &ts, h.tid);
    arg_reader r = {data + sizeof(h), h.nargs};
    render_message(&o, h.fmt, &r);
    out[o.len++] = '\n';
    return o.len;
}

/* ===================================================================================== */

/* 소비자: 다음 슬롯이 채워져 있으면 out 에 붙이고 1, 아니면 0 */
static int try_dequeue(lf_logger *lg, char *out, size_t *out_len)
{
    size_t pos = lg->dequeue_pos;
    lf_cell *cell = &lg->cells[pos & lg->mask];
    if (atomic_load_explicit(&cell->seq, memory_order_acquire) != pos + 1)
        return 0;
    if (cell->kind == CELL_TEXT) {
        memcpy(out + *out_len, cell->data, cell->len);
        *out_len += cell->len;
    } else {
        /* 포맷팅은 여기서 (호출자는 이미 다른 일 하는 중) */
        *out_len += render_record(out + *out_len, LOG_LINE_MAX, cell->data, &lg->clock);
    }
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
        if (spun < SPIN_BEFORE_SLEEP || lg->busy_poll)
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
    lf_logger_opts o = opts ? *opts : (lf_logger_opts){.min_level = LOG_INFO, .capacity = 16384};
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
    lg->busy_poll = o.busy_poll;
    atomic_init(&lg->min_level, o.min_level);
    for (size_t i = 0; i < cap; i++)
        atomic_init(&lg->cells[i].seq, i);
    clock_init(&lg->clock);

    /* 고정은 스레드를 만들 때 한다 (처음부터 그 CPU 에서만 돎) */
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0)
        goto fail;
    int err = 0;
    if (o.pin_consumer) {
        cpu_set_t set;
        CPU_ZERO(&set);
        if (o.consumer_cpu < 0 || o.consumer_cpu >= CPU_SETSIZE)
            err = EINVAL;
        else {
            CPU_SET(o.consumer_cpu, &set);
            err = pthread_attr_setaffinity_np(&attr, sizeof(set), &set);
        }
    }
    if (!err)
        err = pthread_create(&lg->thread, &attr, consumer_main, lg);
    pthread_attr_destroy(&attr);
    if (err) {
        errno = err;
        goto fail;
    }
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

/* 큐에 넣기. 가득 차면 옵션에 따라 버리거나 소비자를 깨우고 재시도 */
static void enqueue_or_wait(lf_logger *lg, const char *buf, size_t n, uint32_t kind)
{
    while (try_enqueue(lg, buf, n, kind) != 0) {
        if (lg->drop_on_full || atomic_load_explicit(&lg->stop, memory_order_relaxed)) {
            atomic_fetch_add_explicit(&lg->dropped, 1, memory_order_relaxed);
            return;
        }
        wake_consumer(lg);
        sched_yield();
    }
    wake_consumer(lg);
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

    enqueue_or_wait(lg, buf, n, CELL_TEXT);
}

void lflog_record(lf_logger *lg, log_level lv, const char *file, int line,
                  const char *fmt, const lf_arg *args, int nargs, lf_fmt_cache *cache)
{
    if (!lflog_enabled(lg, lv))
        return;

    /* 포맷팅 없이 값만 직렬화 (역시 슬롯을 잡기 전에) */
    char buf[LOG_LINE_MAX];
    size_t n = encode_record(buf, sizeof(buf), lv, file, line, fmt, args, nargs, cache);

    enqueue_or_wait(lg, buf, n, CELL_RECORD);
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
