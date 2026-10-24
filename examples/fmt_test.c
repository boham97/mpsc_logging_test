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
#define MAX_CASES 64

static char expect[MAX_CASES][4096];
static int ncases;
static const char *volatile current_fmt = "";

#define CASE(fmt, ...)                                                          \
    do {                                                                        \
        current_fmt = fmt;                                                      \
        snprintf(expect[ncases++], sizeof(expect[0]), fmt __VA_OPT__(,) __VA_ARGS__); \
        LFLOG_INFO(lg, fmt __VA_OPT__(,) __VA_ARGS__);                          \
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

int main(void)
{
    signal(SIGSEGV, on_segv);
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
    return fail ? 1 : 0;
}
