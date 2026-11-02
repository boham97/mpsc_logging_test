/*
 * 로깅 스레드의 포맷팅 비용: snprintf vs 직접 변환.
 *  1) 변환 하나씩: %d, %lld, %u, %x, %s, %5d
 *  2) 한 줄 전체: 지금의 render_record (지정자마다 snprintf) vs 직접 변환 고속 경로
 *     - 고속 경로는 플래그/폭/정밀도 없는 %d %i %u %x %s 만 직접 변환하고,
 *       그 외 지정자가 하나라도 있으면 지금의 render_message 로 넘긴다.
 * 측정 전에 직접 변환 결과가 snprintf / render_record 와 글자 하나까지 같은지 먼저 확인한다.
 *
 * render_record 등 static 함수를 그대로 재려고 lockfree_logger.c 를 직접 include 한다
 * (그래서 liblogx.a 대신 log_common.o 만 링크한다).
 *
 * 사용법: ./fmt_bench [반복 횟수=2000000]
 */
#include "../logger/lockfree_logger.c"

#define NVALS 1024 /* 입력을 돌려 써서 자릿수가 매번 달라지게 (분기 예측이 한 값에 맞춰지지 않게) */
#define REPS 5     /* 반복해서 가장 빠른 값을 쓴다 (다른 프로세스 간섭 제거) */

/* 함수 포인터로 불러서 컴파일러가 snprintf("%s") 등을 strcpy 로 바꿔 버리지 않게 한다 */
static int (*volatile snprintf_p)(char *, size_t, const char *, ...) = snprintf;

static volatile size_t sink; /* 결과를 써서 계산이 지워지지 않게 */
static lf_clock bench_clock; /* 레코드 시각 변환 (로깅 스레드의 lg->clock 역할) */

/* ======================== 직접 변환 ======================== */

static char digits2[200]; /* "00" "01" ... "99": 두 자리씩 변환해서 나눗셈 횟수를 반으로 */

static void init_digits(void)
{
    for (int i = 0; i < 100; i++) {
        digits2[2 * i] = (char)('0' + i / 10);
        digits2[2 * i + 1] = (char)('0' + i % 10);
    }
}

static size_t fmt_u64(char *out, unsigned long long v)
{
    char tmp[20];
    char *p = tmp + sizeof(tmp);
    while (v >= 100) {
        unsigned r = (unsigned)(v % 100);
        v /= 100;
        p -= 2;
        memcpy(p, digits2 + r * 2, 2);
    }
    if (v >= 10) {
        p -= 2;
        memcpy(p, digits2 + v * 2, 2);
    } else {
        *--p = (char)('0' + v);
    }
    size_t n = (size_t)(tmp + sizeof(tmp) - p);
    memcpy(out, p, n);
    return n;
}

static size_t fmt_i64(char *out, long long v)
{
    if (v < 0) {
        *out = '-';
        return 1 + fmt_u64(out + 1, 0ULL - (unsigned long long)v); /* LLONG_MIN 도 안전 */
    }
    return fmt_u64(out, (unsigned long long)v);
}

static size_t fmt_hex(char *out, unsigned long long v)
{
    char tmp[16];
    char *p = tmp + sizeof(tmp);
    do {
        *--p = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v);
    size_t n = (size_t)(tmp + sizeof(tmp) - p);
    memcpy(out, p, n);
    return n;
}

/* %5d: 오른쪽 정렬, 공백 채움 */
static size_t fmt_i64_width(char *out, long long v, int width)
{
    char tmp[24];
    size_t n = fmt_i64(tmp, v);
    size_t pad = (size_t)width > n ? (size_t)width - n : 0;
    memset(out, ' ', pad);
    memcpy(out + pad, tmp, n);
    return pad + n;
}

static size_t fmt_str(char *out, const char *s)
{
    size_t n = strlen(s);
    memcpy(out, s, n);
    return n;
}

/* ======================== 직접 변환으로 만든 render_record ======================== */

/*
 * 비교 기준: 직접 변환으로 바꾸기 전의 log_format_prefix (snprintf 한 번).
 * 지금의 log_format_prefix 가 이것과 글자 단위로 같은지 check_correctness 에서 확인한다.
 */
static size_t prefix_snprintf(char *out, size_t cap, log_level lv, const char *file, int line,
                              const struct timespec *ts, int tid)
{
    static _Thread_local time_t cached_sec = -1;
    static _Thread_local char cached_time[32];

    if (ts->tv_sec != cached_sec) {
        struct tm tm;
        localtime_r(&ts->tv_sec, &tm);
        strftime(cached_time, sizeof(cached_time), "%Y-%m-%d %H:%M:%S", &tm);
        cached_sec = ts->tv_sec;
    }
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    int n = snprintf_p(out, cap, "%s.%06ld [%s] %d %s:%d | ",
                       cached_time, ts->tv_nsec / 1000, log_level_name(lv), tid, base, line);
    return (n < 0) ? 0 : ((size_t)n >= cap ? cap - 1 : (size_t)n);
}

/* 플래그/폭/정밀도 없는 %d %i %u %x %s 만 있는가 (실제로 넣을 때는 호출 위치별로 캐싱하면 됨) */
static int all_simple(const char *fmt)
{
    for (const char *f = fmt; (f = strchr(f, '%')) != NULL;) {
        if (f[1] == '%') {
            f += 2;
            continue;
        }
        fmt_spec s;
        parse_spec(f + 1, &s);
        if (!s.conv || s.nflags || s.nwidth || s.width_star || s.has_prec || !strchr("diuxs", s.conv))
            return 0;
        f = s.end;
    }
    return 1;
}

/* %d %i %u %x %s 하나를 직접 변환 (render_message 와 같은 규칙: 길이 수식자 폭으로 자르기, 타입이 안 맞으면 타입대로) */
static void put_arg_fast(outbuf *o, char conv, int lm, arg_reader *r)
{
    lf_arg a;
    if (!next_arg(r, &a)) {
        out_putn(o, "<?>", 3);
        return;
    }
    char tmp[24];
    size_t n;
    switch (conv) {
    case 'd': case 'i': {
        if (a.type == LF_ARG_STR) { out_arg_default(o, &a); return; }
        long long v = arg_as_i64(&a);
        if (lm == LM_HH) v = (signed char)v;
        else if (lm == LM_H) v = (short)v;
        else if (lm == LM_NONE) v = (int)v;
        else if (lm == LM_L) v = (long)v;
        n = fmt_i64(tmp, v);
        break;
    }
    case 'u': case 'x': {
        if (a.type == LF_ARG_STR) { out_arg_default(o, &a); return; }
        unsigned long long v = (unsigned long long)arg_as_i64(&a);
        if (lm == LM_HH) v = (unsigned char)v;
        else if (lm == LM_H) v = (unsigned short)v;
        else if (lm == LM_NONE) v = (unsigned int)v;
        else if (lm == LM_L) v = (unsigned long)v;
        n = conv == 'u' ? fmt_u64(tmp, v) : fmt_hex(tmp, v);
        break;
    }
    default: /* 's' */
        if (a.type != LF_ARG_STR) { out_arg_default(o, &a); return; }
        out_putn(o, a.v.s, strlen(a.v.s));
        return;
    }
    out_putn(o, tmp, n);
}

/* 직접 변환, 포맷 문자열은 매번 해석 */
static void render_message_fast(outbuf *o, const char *fmt, arg_reader *r)
{
    if (!all_simple(fmt)) {
        render_message(o, fmt, r);
        return;
    }
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
        fmt_spec s;
        parse_spec(f + 1, &s);
        f = s.end;
        put_arg_fast(o, s.conv, s.lm, r);
    }
}

/*
 * 직접 변환 + 포맷 문자열 해석 결과 캐싱: "리터럴 조각 + 지정자" 목록으로 한 번만 바꿔 둔다.
 * 측정용이라 fmt 포인터로 찾는 단순한 전역 표를 쓴다 (단일 스레드).
 * 실제로 넣는다면 생산자의 lf_fmt_cache 처럼 호출 위치별 캐시에 둘 수 있다.
 */
#define PROG_MAX_OPS 24
typedef struct {
    const char *lit; /* 앞의 리터럴 조각 */
    uint16_t nlit;
    char conv;       /* 0: 리터럴만 (끝 또는 "%%") */
    uint8_t lm;
} fmt_op;

typedef struct {
    const char *fmt;
    int simple; /* 0: 고속 경로 대상이 아님 → render_message */
    int nops;
    fmt_op ops[PROG_MAX_OPS];
} fmt_prog;

static fmt_prog progs[64];

static const fmt_prog *get_prog(const char *fmt)
{
    fmt_prog *p = &progs[((uintptr_t)fmt >> 4) & 63];
    if (p->fmt == fmt)
        return p;
    p->fmt = fmt;
    p->nops = 0;
    p->simple = all_simple(fmt);
    const char *f = fmt;
    while (p->simple) {
        if (p->nops == PROG_MAX_OPS) {
            p->simple = 0;
            break;
        }
        fmt_op *op = &p->ops[p->nops++];
        const char *pct = strchr(f, '%');
        if (!pct) {
            *op = (fmt_op){f, (uint16_t)strlen(f), 0, 0};
            break;
        }
        if (pct[1] == '%') { /* 리터럴에 '%' 하나까지 포함하고 두 번째 '%' 는 건너뜀 */
            *op = (fmt_op){f, (uint16_t)(pct + 1 - f), 0, 0};
            f = pct + 2;
            continue;
        }
        fmt_spec s;
        parse_spec(pct + 1, &s);
        *op = (fmt_op){f, (uint16_t)(pct - f), s.conv, (uint8_t)s.lm};
        f = s.end;
    }
    return p;
}

static void render_message_cached(outbuf *o, const char *fmt, arg_reader *r)
{
    const fmt_prog *p = get_prog(fmt);
    if (!p->simple) {
        render_message(o, fmt, r);
        return;
    }
    for (int i = 0; i < p->nops; i++) {
        const fmt_op *op = &p->ops[i];
        out_putn(o, op->lit, op->nlit);
        if (op->conv)
            put_arg_fast(o, op->conv, op->lm, r);
    }
}

static size_t render_record_with(char *out, size_t cap, const char *data,
                                 void (*msg)(outbuf *, const char *, arg_reader *))
{
    rec_hdr h;
    memcpy(&h, data, sizeof(h));
    struct timespec ts; clock_to_timespec(&bench_clock, h.stamp, &ts);

    outbuf o = {out, 0, cap - 1};
    o.len = log_format_prefix(out, o.cap, (log_level)h.level, h.file, h.line, &ts, h.tid);
    arg_reader r = {data + sizeof(h), h.nargs};
    msg(&o, h.fmt, &r);
    out[o.len++] = '\n';
    return o.len;
}

static size_t render_record_fast(char *out, size_t cap, const char *data)
{
    return render_record_with(out, cap, data, render_message_fast);
}

static size_t render_record_cached(char *out, size_t cap, const char *data)
{
    return render_record_with(out, cap, data, render_message_cached);
}

/* ======================== 입력 ======================== */

static unsigned long long rng = 88172645463325252ULL;

static unsigned long long xorshift(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

/* 자릿수가 1 ~ max_digits 사이에서 고르게 나오도록 */
static unsigned long long rand_digits(int max_digits)
{
    int d = 1 + (int)(xorshift() % (unsigned)max_digits);
    unsigned long long lim = 1;
    for (int i = 0; i < d; i++)
        lim *= 10;
    return xorshift() % lim;
}

static int ival[NVALS];
static long long llval[NVALS];
static unsigned uval[NVALS];
static const char *payload = "the quick brown fox jumps over the lazy dog";

/* 한 줄 전체 측정용 레코드 (로깅 스레드가 슬롯에서 읽는 것과 같은 바이트) */
typedef struct {
    const char *name;
    char rec[NVALS][LOG_LINE_MAX];
} line_case;

static line_case lines[2];

static void make_records(void)
{
    lines[0].name = "\"T%d #%d payload=%s\" (bench 와 같음)";
    lines[1].name = "\"req=%u status=%d bytes=%d us=%d id=%x\"";
    for (int k = 0; k < NVALS; k++) {
        lf_arg a0[] = {LF_ARG(k % 16), LF_ARG(ival[k] < 0 ? -ival[k] : ival[k]), LF_ARG(payload)};
        encode_record(lines[0].rec[k], LOG_LINE_MAX, LOG_INFO, __FILE__, __LINE__,
                      "T%d #%d payload=%s", a0, 3, NULL);
        lf_arg a1[] = {LF_ARG(uval[k]), LF_ARG(200 + k % 300), LF_ARG(ival[k]),
                       LF_ARG((int)(uval[k] % 100000)), LF_ARG(uval[NVALS - 1 - k])};
        encode_record(lines[1].rec[k], LOG_LINE_MAX, LOG_INFO, __FILE__, __LINE__,
                      "req=%u status=%d bytes=%d us=%d id=%x", a1, 5, NULL);
    }
}

/* ======================== 정확성 확인 ======================== */

static int fails;

static void expect_same(const char *what, const char *got, size_t n, const char *want)
{
    if (n != strlen(want) || memcmp(got, want, n) != 0) {
        if (fails++ < 10)
            printf("MISMATCH %s: got \"%.*s\", want \"%s\"\n", what, (int)n, got, want);
    }
}

/* log_format_prefix(직접 변환)가 snprintf 판과 같은지: 레벨, 정수 경계, nsec 경계, 파일 이름, 작은 cap(잘림) */
static void check_prefix(void)
{
    static char longname[400];
    memset(longname, 'x', sizeof(longname) - 3);
    memcpy(longname + sizeof(longname) - 3, ".c", 3);
    const char *files[] = {"main.c", "/a/b/c.c", "dir/", "", "noslash", longname};
    const int ints[] = {0, 1, -1, 9, 10, 99, 100, 12345, INT_MAX, INT_MIN};
    const long nsecs[] = {0, 999, 1000, 123456789, 999999999, -1, 1000000000};
    const time_t secs[] = {1761900000, 1761900001, 0};
    const size_t caps[] = {1, 8, 20, 40, 64, 70, 100, 1024};
    char want[1100], got[1100];
    long checked = 0;

    for (size_t si = 0; si < sizeof(secs) / sizeof(secs[0]); si++)
    for (size_t ni = 0; ni < sizeof(nsecs) / sizeof(nsecs[0]); ni++)
    for (int lv = 0; lv <= LOG_FATAL + 1; lv++)
    for (size_t fi = 0; fi < sizeof(files) / sizeof(files[0]); fi++)
    for (size_t ti = 0; ti < sizeof(ints) / sizeof(ints[0]); ti++)
    for (size_t ci = 0; ci < sizeof(caps) / sizeof(caps[0]); ci++) {
        struct timespec ts = {secs[si], nsecs[ni]};
        int v = ints[ti], line = ints[(ti + 3) % (sizeof(ints) / sizeof(ints[0]))];
        size_t cap = caps[ci];
        memset(want, '#', sizeof(want));
        memset(got, '#', sizeof(got));
        size_t nw = prefix_snprintf(want, cap, (log_level)lv, files[fi], line, &ts, v);
        size_t ng = log_format_prefix(got, cap, (log_level)lv, files[fi], line, &ts, v);
        /* 길이, 내용, NUL, 그 뒤를 건드리지 않았는지까지 같아야 한다 */
        if (ng != nw || memcmp(got, want, cap) != 0 || memcmp(got + cap, want + cap, 8) != 0) {
            if (fails++ < 10)
                printf("MISMATCH prefix lv=%d file=%.20s tid=%d line=%d nsec=%ld cap=%zu:\n"
                       "  got  %zu \"%.*s\"\n  want %zu \"%.*s\"\n",
                       lv, files[fi], v, line, nsecs[ni], cap, ng, (int)ng, got, nw, (int)nw, want);
        }
        checked++;
    }
    printf("접두어: %ld 조합이 snprintf 판과 같음 확인\n", checked - fails);
}

static void check_correctness(void)
{
    char a[64], b[1100], c[1100];
    check_prefix();
    static const long long edge[] = {0, 1, -1, 9, 10, 99, 100, INT_MAX, INT_MIN, LLONG_MAX, LLONG_MIN};
    for (size_t i = 0; i < sizeof(edge) / sizeof(edge[0]); i++) {
        snprintf(b, sizeof(b), "%lld", edge[i]);
        expect_same("%lld", a, fmt_i64(a, edge[i]), b);
        snprintf(b, sizeof(b), "%llu", (unsigned long long)edge[i]);
        expect_same("%llu", a, fmt_u64(a, (unsigned long long)edge[i]), b);
        snprintf(b, sizeof(b), "%llx", (unsigned long long)edge[i]);
        expect_same("%llx", a, fmt_hex(a, (unsigned long long)edge[i]), b);
    }
    for (int k = 0; k < NVALS; k++) {
        snprintf(b, sizeof(b), "%d", ival[k]);
        expect_same("%d", a, fmt_i64(a, ival[k]), b);
        snprintf(b, sizeof(b), "%lld", llval[k]);
        expect_same("%lld", a, fmt_i64(a, llval[k]), b);
        snprintf(b, sizeof(b), "%u", uval[k]);
        expect_same("%u", a, fmt_u64(a, uval[k]), b);
        snprintf(b, sizeof(b), "%x", uval[k]);
        expect_same("%x", a, fmt_hex(a, uval[k]), b);
        snprintf(b, sizeof(b), "%5d", ival[k] % 100000);
        expect_same("%5d", a, fmt_i64_width(a, ival[k] % 100000, 5), b);
        for (int l = 0; l < 2; l++) {
            size_t nb = render_record(b, LOG_LINE_MAX, lines[l].rec[k], &bench_clock);
            b[nb] = '\0';
            expect_same("render_record_fast", c, render_record_fast(c, LOG_LINE_MAX, lines[l].rec[k]), b);
            expect_same("render_record_cached", c, render_record_cached(c, LOG_LINE_MAX, lines[l].rec[k]), b);
        }
    }

    /* 고속 경로의 경계: %%, 길이 수식자, 타입 불일치, 고속 경로가 아닌 지정자(→ render_message) */
    static char rec[LOG_LINE_MAX];
    struct { const char *fmt; lf_arg args[4]; int n; } more[] = {
        {"100%% done %d%%", {LF_ARG(5)}, 1},
        {"%hhd %hu %lx %llu", {LF_ARG(300), LF_ARG(70000), LF_ARG(-1L), LF_ARG(ULLONG_MAX)}, 4},
        {"%d %s", {LF_ARG("str"), LF_ARG(42)}, 2},
        {"%5d|%-3s|%.2f", {LF_ARG(7), LF_ARG("a"), LF_ARG(3.14159)}, 3},
        {"tail %d", {{0}}, 0},
    };
    for (size_t i = 0; i < sizeof(more) / sizeof(more[0]); i++) {
        encode_record(rec, LOG_LINE_MAX, LOG_INFO, __FILE__, __LINE__, more[i].fmt, more[i].args, more[i].n, NULL);
        size_t nb = render_record(b, LOG_LINE_MAX, rec, &bench_clock);
        b[nb] = '\0';
        for (int rep = 0; rep < 2; rep++) { /* 두 번째는 캐시된 해석 결과 사용 */
            expect_same(more[i].fmt, c, render_record_fast(c, LOG_LINE_MAX, rec), b);
            expect_same(more[i].fmt, c, render_record_cached(c, LOG_LINE_MAX, rec), b);
        }
    }
    if (fails) {
        printf("직접 변환 결과가 snprintf 와 다름 (%d건) → 측정 중단\n", fails);
        exit(1);
    }
}

/* ======================== 측정 ======================== */

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static long iters;

/* body 를 iters 번 돌린 ns/회. REPS 번 중 가장 빠른 값. body 안에서 k(입력 번호)와 acc 를 쓴다 */
#define BENCH(...)                                                       \
    ({                                                                   \
        double best_ = 1e30;                                             \
        for (int rep_ = 0; rep_ < REPS; rep_++) {                        \
            size_t acc = 0;                                              \
            double t0_ = now_ns();                                       \
            for (long it_ = 0; it_ < iters; it_++) {                     \
                size_t k = (size_t)it_ & (NVALS - 1);                    \
                __VA_ARGS__;                                                \
            }                                                            \
            double dt_ = (now_ns() - t0_) / (double)iters;               \
            sink += acc;                                                 \
            if (dt_ < best_)                                             \
                best_ = dt_;                                             \
        }                                                                \
        best_;                                                           \
    })

static void row(const char *name, double slow, double fast)
{
    printf("  %-44s %8.1f %8.1f %7.1fx\n", name, slow, fast, slow / fast);
}

int main(int argc, char **argv)
{
    iters = argc > 1 ? atol(argv[1]) : 2000000;
    init_digits();
    clock_init(&bench_clock);
    for (int k = 0; k < NVALS; k++) {
        ival[k] = (int)rand_digits(9) * ((xorshift() & 1) ? 1 : -1);
        llval[k] = (long long)rand_digits(18) * ((xorshift() & 1) ? 1 : -1);
        uval[k] = (unsigned)rand_digits(9);
    }
    make_records();
    check_correctness();

    char buf[LOG_LINE_MAX];
    printf("반복 %ld회 × %d번 중 최솟값, 단위 ns/회 (출력이 snprintf 와 같음을 먼저 확인함)\n\n", iters, REPS);
    printf("  %-44s %8s %8s %8s\n", "[변환 하나]", "snprintf", "직접", "배수");
    row("%d   (1~9자리, 부호 섞임)",
        BENCH(acc += (size_t)snprintf_p(buf, sizeof(buf), "%d", ival[k])),
        BENCH(acc += fmt_i64(buf, ival[k])));
    row("%lld (1~18자리)",
        BENCH(acc += (size_t)snprintf_p(buf, sizeof(buf), "%lld", llval[k])),
        BENCH(acc += fmt_i64(buf, llval[k])));
    row("%u",
        BENCH(acc += (size_t)snprintf_p(buf, sizeof(buf), "%u", uval[k])),
        BENCH(acc += fmt_u64(buf, uval[k])));
    row("%x",
        BENCH(acc += (size_t)snprintf_p(buf, sizeof(buf), "%x", uval[k])),
        BENCH(acc += fmt_hex(buf, uval[k])));
    row("%s   (43자)",
        BENCH(acc += (size_t)snprintf_p(buf, sizeof(buf), "%s", payload); buf[0] ^= (char)k),
        BENCH(acc += fmt_str(buf, payload); buf[0] ^= (char)k));
    row("%5d",
        BENCH(acc += (size_t)snprintf_p(buf, sizeof(buf), "%5d", ival[k] % 100000)),
        BENCH(acc += fmt_i64_width(buf, ival[k] % 100000, 5)));

#define MSG_BENCH(fn)                                                       \
    BENCH(rec_hdr h; memcpy(&h, lines[l].rec[k], sizeof(h));                \
          outbuf o = {buf, 0, sizeof(buf) - 1};                             \
          arg_reader r = {lines[l].rec[k] + sizeof(h), h.nargs};            \
          fn(&o, h.fmt, &r); acc += o.len)

    printf("\n  [한 줄 전체: 슬롯의 레코드 → 완성된 줄]\n");
    printf("  %-44s %8s %8s %8s %8s\n", "", "현재", "직접", "직접+캐시", "배수");
    for (int l = 0; l < 2; l++) {
        printf("  %s\n", lines[l].name);
        double ps = BENCH(rec_hdr h; memcpy(&h, lines[l].rec[k], sizeof(h));
                          struct timespec ts; clock_to_timespec(&bench_clock, h.stamp, &ts);
                          acc += prefix_snprintf(buf, sizeof(buf), (log_level)h.level, h.file, h.line, &ts, h.tid));
        double pf = BENCH(rec_hdr h; memcpy(&h, lines[l].rec[k], sizeof(h));
                          struct timespec ts; clock_to_timespec(&bench_clock, h.stamp, &ts);
                          acc += log_format_prefix(buf, sizeof(buf), (log_level)h.level, h.file, h.line, &ts, h.tid));
        printf("  %-44s %8.1f %8.1f %8s %7.1fx\n", "  접두어: snprintf(기존) → 현재 (직접 변환)", ps, pf, "-", ps / pf);
        double ms = MSG_BENCH(render_message);
        double mf = MSG_BENCH(render_message_fast);
        double mc = MSG_BENCH(render_message_cached);
        printf("  %-44s %8.1f %8.1f %8.1f %7.1fx\n", "  메시지", ms, mf, mc, ms / mc);
        double ts_ = BENCH(acc += render_record(buf, LOG_LINE_MAX, lines[l].rec[k], &bench_clock));
        double tf = BENCH(acc += render_record_fast(buf, LOG_LINE_MAX, lines[l].rec[k]));
        double tc = BENCH(acc += render_record_cached(buf, LOG_LINE_MAX, lines[l].rec[k]));
        printf("  %-44s %8.1f %8.1f %8.1f %7.1fx\n", "  합계 (render_record)", ts_, tf, tc, ts_ / tc);
        printf("  %-44s %8.2f %8.2f %8.2f  M줄/s (포맷팅만, 쓰기 제외)\n", "  → 로깅 스레드 하나의 상한",
               1e3 / ts_, 1e3 / tf, 1e3 / tc);
    }
    return 0;
}
