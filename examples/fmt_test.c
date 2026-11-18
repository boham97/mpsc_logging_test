/*
 * 지연 포맷팅(LFLOG_*) 결과가 snprintf 와 글자 하나까지 같은지 확인한다.
 * 각 케이스를 LFLOG_INFO 로 남기고, 같은 인자로 snprintf 한 기대값과 파일 내용을 비교한다.
 */
#include "../logger/lockfree_logger.h"

#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#define PATH "fmt_test.log"
#define MAX_CASES 200

static char expect[MAX_CASES][4096];
static int ncases;
static const char *volatile current_fmt = "";

/* 같은 호출 위치를 두 번: 첫 호출은 포맷 문자열을 분석해 캐시를 채우고, 두 번째는 캐시를 쓴다 */
#define CASE(fmt, ...)                                                          \
    do {                                                                        \
        current_fmt = fmt;                                                      \
        for (int rep_ = 0; rep_ < 2; rep_++) {                                  \
            snprintf(expect[ncases++], sizeof(expect[0]), fmt __VA_OPT__(,) __VA_ARGS__); \
            LFLOG_INFO(lg, fmt __VA_OPT__(,) __VA_ARGS__);                      \
        }                                                                       \
    } while (0)

/* 범위 밖 읽기로 접근 금지 페이지를 건드리면 어느 케이스인지 알리고 실패 */
static void on_segv(int sig)
{
    (void)sig;
    static const char msg[] = "FAIL: 범위 밖 읽기 (SIGSEGV), 케이스: ";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    write(STDOUT_FILENO, (const char *)current_fmt, strlen((const char *)current_fmt));
    write(STDOUT_FILENO, "\n", 1);
    _exit(1);
}

/* 접근 금지 페이지 바로 앞에 data 를 NUL 없이 놓는다 → 끝을 넘어 읽으면 즉시 SIGSEGV */
static char *guarded(const char *data, size_t n)
{
    long pg = sysconf(_SC_PAGESIZE);
    char *m = mmap(NULL, 2 * (size_t)pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED || mprotect(m + pg, (size_t)pg, PROT_NONE) != 0) {
        perror("mmap");
        exit(1);
    }
    memcpy(m + pg - n, data, n);
    return m + pg - n;
}

enum color { RED, GREEN, BLUE };

/* ======================== log_format_g 단위 검증: snprintf("%g") 와 1:1 ======================== */

static long g_checked, g_fail;

static double from_bits(uint64_t b)
{
    double d;
    memcpy(&d, &b, 8);
    return d;
}

static uint64_t to_bits(double d)
{
    uint64_t b;
    memcpy(&b, &d, 8);
    return b;
}

static void check_g1(double v)
{
    char want[64], got[LOG_G_MAX + 8];
    snprintf(want, sizeof(want), "%g", v);
    memset(got, '#', sizeof(got));
    size_t n = log_format_g(got, v);
    g_checked++;
    if (n != strlen(want) || memcmp(got, want, n) != 0 || n > LOG_G_MAX) {
        if (g_fail++ < 20)
            printf("FAIL %%g bits=%016llx: want \"%s\", got \"%.*s\"\n",
                   (unsigned long long)to_bits(v), want, (int)n, got);
    }
}

/* v 와 그 앞뒤 ulp 몇 개, 그리고 부호를 바꾼 것 */
static void check_g_around(double v, int k)
{
    uint64_t b = to_bits(v) & ~(1ULL << 63);
    for (int d = -k; d <= k; d++) {
        if ((d < 0 && b < (uint64_t)-d) || b + (uint64_t)d >= 0x7ff0000000000000ULL)
            continue;
        double x = from_bits(b + (uint64_t)d);
        check_g1(x);
        check_g1(-x);
    }
}

static uint64_t g_rng = 0x9e3779b97f4a7c15ULL;
static uint64_t g_next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static int check_g(void)
{
    /* 특수값과 직접 변환 범위 밖 (snprintf 로 넘어가는 쪽) */
    const double specials[] = {0.0, -0.0, 1.0 / 0.0, -1.0 / 0.0, __builtin_nan(""), -__builtin_nan(""),
                               2.2250738585072014e-308, 4.9e-324, 1.7976931348623157e308,
                               1e-300, 1e300, 1.0, 0.1, 0.5, 2.5, 123456.5, 999999.5, 0.0001, 0.00009999995};
    for (size_t i = 0; i < sizeof(specials) / sizeof(specials[0]); i++)
        check_g_around(specials[i], 3);

    /* 십진 경계: 10^e, 9.999995*10^e(6자리 반올림이 다음 자리로 넘어가는 점), 5*10^e 근처 ulp 들 */
    double p = 1e-25;
    for (int e = -25; e <= 25; e++, p *= 10) {
        check_g_around(p, 40);
        check_g_around(9.999995 * p, 40);
        check_g_around(9.9999949999 * p, 4);
        check_g_around(1.000005 * p, 40);
        check_g_around(5 * p, 4);
    }

    /* 2^b 경계마다 (지수 추정이 b 에서 정해짐), 직접 변환 범위 [-56,70] 과 그 바깥 */
    for (int b = -80; b <= 90; b++) {
        double x = 1.0;
        for (int i = 0; i < (b < 0 ? -b : b); i++)
            x = b < 0 ? x / 2 : x * 2;
        check_g_around(x, 20);
    }

    /* 동률(정확히 반): 1000000~9999999 의 모든 정수 (끝자리 5 가 동률), 그리고 n+0.5 (n=100000~999999) */
    for (long n = 1000000; n < 10000000; n++)
        check_g1((double)n);
    for (long n = 100000; n < 1000000; n++)
        check_g1((double)n + 0.5);
    /* cmp_bench 처리량·지연 값 */
    for (long i = 0; i < 4000000; i += 7)
        check_g1((double)i / 2);
    for (long i = 0; i < 10000; i++)
        check_g1((double)i + 0.1 * (double)i);

    /* 무작위 비트: 직접 변환 범위 근처(2^-60 ~ 2^74) 2천만 개, 전체 범위 1백만 개 */
    for (long i = 0; i < 20000000; i++) {
        uint64_t r = g_next();
        uint64_t be = 1023 - 60 + (r >> 52) % 135;
        check_g1(from_bits((r & ((1ULL << 52) - 1)) | (be << 52) | (g_next() & (1ULL << 63))));
    }
    for (long i = 0; i < 1000000; i++)
        check_g1(from_bits(g_next()));
    /* 무작위 십진수: 정수 / 10^k (사람이 쓰는 값) */
    for (long i = 0; i < 3000000; i++) {
        double x = (double)(g_next() % 100000000);
        for (int k = (int)(g_next() % 20); k > 0; k--)
            x /= 10;
        check_g1(x);
    }

    printf("log_format_g: %ld/%ld 개가 snprintf(\"%%g\") 와 같음\n", g_checked - g_fail, g_checked);
    return g_fail ? 1 : 0;
}

int main(void)
{
    signal(SIGSEGV, on_segv);
    int g_bad = check_g();
    unlink(PATH);
    lf_logger *lg = lflog_open(PATH, NULL);
    if (!lg) {
        perror("lflog_open");
        return 1;
    }

    CASE("plain text, no args");
    CASE("%d %i %d", -42, 7, INT_MIN);
    CASE("%5d|%-5d|%05d|%+d|% d", 42, 42, 42, 42, 42);
    CASE("%u %x %X %#x %o %#o", 4000000000u, 255u, 255u, 255u, 8u, 8u);
    CASE("%ld %lu %lld %llu", -1234567890123L, ULONG_MAX, LLONG_MIN, ULLONG_MAX);
    CASE("%hhd %hd %hhu %hu", 300, 70000, 300, 70000);  /* printf 처럼 잘려야 함 */
    CASE("%zu %zd %jd %td", sizeof(int) * 1000, (ssize_t)-5, (intmax_t)-9, (ptrdiff_t)-3);
    CASE("%c%c%c", 'a', 'B', '!');
    CASE("%f %.2f %10.3f %-10.1f| %+.0f", 3.14159, 2.71828, -1.5, 9.99, 2.5);
    CASE("%e %E %g %G %a", 12345.678, 0.000123, 1e-10, 1e20, 1.0);
    CASE("%.3e %Lf %f", 6.02e23, 1.5L, 1.25f);
    /* 꾸밈 없는 %g 는 로깅 스레드에서 log_format_g 로 직접 변환한다 */
    CASE("%g|%g|%g|%g|%g|%g", 0.0, -0.0, 1.0 / 0.0, -1.0 / 0.0, __builtin_nan(""), 1e-320);
    CASE("%g %g %g %g %g", 1234567.5, 999999.5, 0.0001, 0.00001234565, 123456.5);
    CASE("%g %g %g %Lg %g", 1e21, 3e22, 1e-17, 2.5L, 1.25f);
    CASE("%5g|%-8g|%.3g|%+g|%#g|%G", 1.5, 2.5, 3.14159, 1.0, 1.0, 1e-10); /* 꾸밈 있으면 snprintf */
    CASE("Iteration: %zu int: %zu double: %g", (size_t)2469135, (size_t)4938270, 1234567.5);
    CASE("Logging iteration: %zu, message: %zu, double: %g", (size_t)9999, (size_t)0, 10998.9);
    CASE("%s|%10s|%-10s|%.3s|%.*s|%*s|", "hello", "hi", "hi", "abcdef", 2, "xyz", 6, "r");
    CASE("%*d|%-*d|%.*f|%*.*f", 6, 42, 6, 42, 2, 3.14159, 8, 3, 2.5);
    CASE("%p %p", (void *)0x1234, (void *)0);
    CASE("100%% done, %d%%", 5);
    CASE("bool=%d enum=%d char=%d", (_Bool)1, BLUE, (char)-3);
    CASE("한글 %s %5.2f원", "로그", 1.5);
    CASE("%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);

    /* %s 는 호출 시점에 복사되어야 한다 */
    char buf[16] = "before";
    CASE("buf=%s", buf);
    strcpy(buf, "after!");

    /* 긴 문자열: 한 줄 최대 길이에서 잘려야 한다 */
    static char big[3000];
    memset(big, 'x', sizeof(big) - 1);
    CASE("big=%s", big);

    /*
     * 정밀도 있는 %s: printf 처럼 정밀도만큼만 읽어야 한다 (NUL 없는 버퍼).
     * 버퍼 바로 뒤가 접근 금지 페이지라서, 더 읽으면 여기서 SIGSEGV 로 실패한다.
     */
    char *pkt = guarded("ABCDE", 5);
    CASE("pkt=%.*s n=%d", 5, pkt, 42);          /* 뒤 인자가 <?> 로 밀리지 않아야 함 */
    CASE("%.3s|%.5s|%8.2s|%-6.1s|", pkt + 2, pkt, pkt, pkt);
    CASE("%*.*s|%d|%.*s|%s", 8, 3, pkt, 7, 2, pkt + 3, "tail");
    CASE("[%.0s][%.s][%.*s]", pkt, pkt, 0, pkt);
    /* 같은 호출 위치에서 %.*s 정밀도가 매번 달라도 그 호출의 값을 따라야 한다 (캐시는 값이 아니라 "몇 번째 인자"를 기억) */
    for (int k = 0; k <= 5; k++)                /* 첫 호출의 정밀도(0)를 계속 쓰면 덜 복사해서 틀림 */
        CASE("up k=%d [%.*s]", k, k, pkt);
    for (int k = 5; k >= 0; k--)                /* 첫 호출의 정밀도(5)를 계속 쓰면 접근 금지 페이지를 읽음 */
        CASE("down k=%d [%.*s]", k, k, pkt + 5 - k);
    char *pkt0 = guarded("ABCDE", 6);           /* NUL 이 접근 금지 페이지 바로 앞 */
    CASE("[%.*s]", -1, pkt0);                   /* 음수 정밀도 = 정밀도 없음 → NUL 에서 멈춰야 함 */
    CASE("%.*s|", -1, "neg prec = no limit");   /* 음수 정밀도는 생략과 같음 */
    CASE("%.1000s|", "short");                  /* 정밀도가 문자열보다 길 때 */
    char *empty = guarded("", 0);               /* 첫 바이트부터 접근 금지 */
    CASE("[%.0s][%.*s]", empty, 0, empty);

    /* %p 에 넘긴 char* 는 내용을 읽지 않고 호출자 주소를 그대로 찍어야 한다 */
    char cbuf[] = "hello";
    const char *ccp = cbuf;
    CASE("%p %p", cbuf, ccp);
    CASE("%p|%s", empty, "after");              /* 읽을 수 없는 메모리를 가리키는 char* */
    CASE("%p %.2s %p", pkt, pkt, pkt + 1);

    /* char* 가 아닌 포인터도 %s 면 문자열로 (printf 와 같음) */
    unsigned char ubuf[] = "unsigned";
    const uint8_t *u8 = (const uint8_t *)"uint8";
    CASE("%s %s %.4s", ubuf, u8, (unsigned char *)guarded("WXYZ", 4));

    /* NULL: glibc 는 정밀도가 6 미만이면 빈 문자열 */
    char *volatile null_v = NULL;
    char *np = null_v;
    CASE("[%s][%.3s][%.6s][%.*s]", np, np, np, 10, np);

    lflog_close(lg);

    /* 검증 */
    FILE *fp = fopen(PATH, "r");
    char line[8192];
    int i = 0, fail = 0;
    while (fgets(line, sizeof(line), fp) && i < ncases) {
        size_t llen = strlen(line);
        if (llen && line[llen - 1] == '\n')
            line[--llen] = '\0';
        const char *msg = strstr(line, "| ");
        msg = msg ? msg + 2 : "";

        int ok = strcmp(msg, expect[i]) == 0;
        /* 잘린 줄: 기대값의 앞부분이고 줄 전체가 최대 길이면 OK */
        if (!ok && llen + 1 == LOG_LINE_MAX - 1 && strncmp(msg, expect[i], strlen(msg)) == 0)
            ok = 1;
        if (!ok) {
            fail++;
            printf("FAIL #%d\n  expect: %.200s\n  got   : %.200s\n", i, expect[i], msg);
        }
        i++;
    }
    fclose(fp);
    if (i != ncases) {
        printf("FAIL: %d lines in file, %d cases\n", i, ncases);
        fail++;
    }
    printf("fmt_test: %d/%d passed\n", ncases - fail, ncases);
    return (fail || g_bad) ? 1 : 0;
}
