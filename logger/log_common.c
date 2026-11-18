#define _GNU_SOURCE
#include "log_common.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

const char *log_level_name(log_level lv)
{
    static const char *names[] = {"TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"};
    if ((unsigned)lv > LOG_FATAL)
        return "?????";
    return names[lv];
}

/* 커널 스레드 ID는 스레드마다 한 번만 조회 */
_Thread_local int log_tid_cache = 0;

int log_current_tid_slow(void)
{
    log_tid_cache = (int)syscall(SYS_gettid);
    return log_tid_cache;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* "00" "01" ... "99": 두 자리씩 변환해서 나눗셈 횟수를 반으로 */
static const char digits2[201] =
    "0001020304050607080910111213141516171819"
    "2021222324252627282930313233343536373839"
    "4041424344454647484950515253545556575859"
    "6061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

/* 부호 없는 32비트 정수 → 10진수. 반환값: 다음 쓸 위치 */
static char *put_u32(char *p, uint32_t v)
{
    char tmp[10];
    char *q = tmp + sizeof(tmp);
    while (v >= 100) {
        uint32_t r = v % 100;
        v /= 100;
        q -= 2;
        memcpy(q, digits2 + r * 2, 2);
    }
    if (v >= 10) {
        q -= 2;
        memcpy(q, digits2 + v * 2, 2);
    } else {
        *--q = (char)('0' + v);
    }
    size_t n = (size_t)(tmp + sizeof(tmp) - q);
    memcpy(p, q, n);
    return p + n;
}

/* %d 와 같은 결과 (INT_MIN 포함) */
static char *put_int(char *p, int v)
{
    if (v < 0) {
        *p++ = '-';
        return put_u32(p, 0u - (uint32_t)v);
    }
    return put_u32(p, (uint32_t)v);
}

/* ======================== %g ======================== */

typedef unsigned __int128 u128;

#define P10_19 10000000000000000000ULL
static const u128 pow10_128[23] = {
    1ULL, 10ULL, 100ULL, 1000ULL, 10000ULL, 100000ULL, 1000000ULL, 10000000ULL,
    100000000ULL, 1000000000ULL, 10000000000ULL, 100000000000ULL, 1000000000000ULL,
    10000000000000ULL, 100000000000000ULL, 1000000000000000ULL, 10000000000000000ULL,
    100000000000000000ULL, 1000000000000000000ULL, P10_19,
    (u128)P10_19 * 10, (u128)P10_19 * 100, (u128)P10_19 * 1000,
};

/* num / den 을 가장 가까운 정수로 (동률이면 짝수) */
static uint64_t div_round_even(u128 num, u128 den)
{
    u128 q, r;
    if ((num >> 64) == 0 && (den >> 64) == 0) { /* 흔한 경우: 64비트 나눗셈 */
        uint64_t n = (uint64_t)num, d = (uint64_t)den;
        q = n / d;
        r = n % d;
    } else {
        q = num / den;
        r = num % den;
    }
    if (2 * r > den || (2 * r == den && (q & 1)))
        q++;
    return (uint64_t)q;
}

/*
 * v = m * 2^q 를 10^(e-5) 로 나눠 반올림한 값 (유효숫자 6자리 후보).
 * 호출 범위(b = q+52 가 [-56, 70])에서 모든 중간값이 128비트에 들어간다.
 */
static uint64_t scaled6(uint64_t m, int q, int e)
{
    int s = e - 5;
    if (s >= 0) {
        if (q >= 0) /* v >= 2^52: m<<q < 2^71, 10^s <= 10^17 */
            return div_round_even((u128)m << q, pow10_128[s]);
        return div_round_even(m, pow10_128[s] << -q); /* 10^10 * 2^52 < 2^86 */
    }
    /* s < 0 이면 v < 1e5 이므로 q < 0. m * 10^t < 2^53 * 10^22 < 2^127, 나누기는 2^-q (-q <= 108) */
    u128 num = (u128)m * pow10_128[-s];
    int sh = -q;
    u128 qt = num >> sh, r = num & (((u128)1 << sh) - 1), half = (u128)1 << (sh - 1);
    if (r > half || (r == half && (qt & 1)))
        qt++;
    return (uint64_t)qt;
}

size_t log_format_g(char *out, double v)
{
    uint64_t bits;
    memcpy(&bits, &v, 8);
    int neg = (int)(bits >> 63);
    int be = (int)((bits >> 52) & 0x7ff);
    uint64_t m = bits & ((1ULL << 52) - 1);
    char *p = out;

    if (be == 0 && m == 0) { /* +0, -0 */
        if (neg)
            *p++ = '-';
        *p++ = '0';
        return (size_t)(p - out);
    }
    int b = be - 1023; /* v 는 [2^b, 2^(b+1)) */
    if (be == 0 || be == 0x7ff || b < -56 || b > 70) {
        int n = snprintf(out, LOG_G_MAX, "%g", v); /* 비정규수, inf, nan, 범위 밖 */
        return n < 0 ? 0 : (size_t)n;
    }
    m |= 1ULL << 52;
    int q = be - 1075;

    /* 십진 지수 e: floor(b*log10(2)) 는 e 또는 e-1. 반올림으로 10^6 이 되면 한 자리 올린다 */
    int e = (b * 78913) >> 18;
    uint64_t n6 = scaled6(m, q, e);
    while (n6 >= 1000000) {
        e++;
        n6 = scaled6(m, q, e);
    }
    while (n6 < 100000) { /* 추정이 한 칸 높을 때 (이론상 안 생김, 안전장치) */
        e--;
        n6 = scaled6(m, q, e);
    }

    char d[6];
    for (int i = 5; i >= 0; i--) {
        d[i] = (char)('0' + n6 % 10);
        n6 /= 10;
    }
    int nd = 6; /* 끝의 0 은 %g 처럼 지운다 */
    while (nd > 1 && d[nd - 1] == '0')
        nd--;

    if (neg)
        *p++ = '-';
    if (e < -4 || e >= 6) { /* d.ddddde±XX */
        *p++ = d[0];
        if (nd > 1) {
            *p++ = '.';
            memcpy(p, d + 1, (size_t)nd - 1);
            p += nd - 1;
        }
        *p++ = 'e';
        int x = e;
        if (x < 0) {
            *p++ = '-';
            x = -x;
        } else {
            *p++ = '+';
        }
        memcpy(p, digits2 + x * 2, 2); /* |e| <= 22 */
        p += 2;
    } else if (e >= 0) { /* 정수부 e+1 자리 */
        memcpy(p, d, (size_t)e + 1);
        p += e + 1;
        if (nd > e + 1) {
            *p++ = '.';
            memcpy(p, d + e + 1, (size_t)(nd - e - 1));
            p += nd - e - 1;
        }
    } else { /* 0.000ddd */
        *p++ = '0';
        *p++ = '.';
        for (int i = 0; i < -e - 1; i++)
            *p++ = '0';
        memcpy(p, d, (size_t)nd);
        p += nd;
    }
    return (size_t)(p - out);
}

/*
 * 고정 부분(시각 19자 + 이 정도)보다 넉넉한 여유:
 * ".123456 [INFO ] " 16 + TID 11 + ' ' + ':' + 줄 11 + " | " 3 + NUL = 44
 */
#define PREFIX_FIXED_MAX 48

size_t log_format_prefix(char *out, size_t cap, log_level lv, const char *file, int line,
                         const struct timespec *ts, int tid)
{
    /* localtime_r/strftime 은 비싸므로 초 단위 문자열을 스레드별로 캐싱 */
    static _Thread_local time_t cached_sec = -1;
    static _Thread_local char cached_time[32];
    static _Thread_local size_t cached_len;

    if (ts->tv_sec != cached_sec) {
        struct tm tm;
        localtime_r(&ts->tv_sec, &tm);
        cached_len = strftime(cached_time, sizeof(cached_time), "%Y-%m-%d %H:%M:%S", &tm);
        cached_sec = ts->tv_sec;
    }

    const char *base = base_name(file);
    size_t blen = strlen(base);

    /*
     * 줄마다 불리는 함수라 snprintf 없이 직접 쓴다 (snprintf 대비 약 5배 빠름, examples/fmt_bench.c).
     * 잘릴 수 있는 경우(아주 긴 파일 이름, 작은 cap)나 범위 밖 nsec 은 snprintf 로 넘겨 결과를 같게 한다.
     */
    if (cap < cached_len + blen + PREFIX_FIXED_MAX || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000L) {
        int n = snprintf(out, cap, "%s.%06ld [%s] %d %s:%d | ",
                         cached_time, ts->tv_nsec / 1000, log_level_name(lv), tid, base, line);
        return (n < 0) ? 0 : ((size_t)n >= cap ? cap - 1 : (size_t)n);
    }

    char *p = out;
    memcpy(p, cached_time, cached_len);
    p += cached_len;
    *p++ = '.';
    uint32_t us = (uint32_t)(ts->tv_nsec / 1000); /* %06ld: 항상 6자리 */
    memcpy(p, digits2 + (us / 10000) * 2, 2);
    memcpy(p + 2, digits2 + (us / 100 % 100) * 2, 2);
    memcpy(p + 4, digits2 + (us % 100) * 2, 2);
    p += 6;
    memcpy(p, " [", 2);
    memcpy(p + 2, log_level_name(lv), 5); /* 레벨 이름은 모두 5자 */
    memcpy(p + 7, "] ", 2);
    p += 9;
    p = put_int(p, tid);
    *p++ = ' ';
    memcpy(p, base, blen);
    p += blen;
    *p++ = ':';
    p = put_int(p, line);
    memcpy(p, " | ", 3);
    p += 3;
    *p = '\0'; /* snprintf 처럼 NUL 로 끝냄 */
    return (size_t)(p - out);
}

size_t log_format_line(char *out, size_t cap, log_level lv,
                       const char *file, int line, const char *fmt, va_list ap)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    size_t limit = cap - 1; /* 마지막 '\n' 자리 확보 */
    size_t len = log_format_prefix(out, limit, lv, file, line, &ts, log_current_tid());

    int n = vsnprintf(out + len, limit - len, fmt, ap);
    if (n > 0)
        len += ((size_t)n >= limit - len) ? limit - len - 1 : (size_t)n;

    out[len++] = '\n';
    out[len] = '\0';
    return len;
}

int log_write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t w = write(fd, buf, len);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += w;
        len -= (size_t)w;
    }
    return 0;
}
