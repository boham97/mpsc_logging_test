/*
 * 방식 3: 락프리 MPSC 링 버퍼 + 로깅 전용 스레드 + 지연 포맷팅(deferred formatting)
 *
 *  [작업 스레드들] 시각/TID 기록 + 인자를 {타입태그, 값}으로 직렬화
 *                  --> CAS 로 슬롯 번호 확보 --> 슬롯에 memcpy --> seq 공개(release)
 *                                                                     │
 *  [로깅 스레드]   <-- seq 확인(acquire) --> 포맷 문자열 해석 + snprintf --> 로컬 버퍼 --> write()
 *
 * 지연 포맷팅 (LFLOG_* 매크로)
 * - 호출 스레드는 문자열 포맷팅을 하지 않는다. 숫자→문자열 변환, 패딩 등은 전부 로깅 스레드가 한다.
 * - 인자 타입은 _Generic 으로 "컴파일 시점에" 태그를 붙인다 (런타임에 포맷 문자열 파싱 안 함).
 * - 포맷 문자열은 포인터만 넘긴다 → 반드시 문자열 리터럴이어야 한다 (매크로가 강제함).
 * - %s 인자는 호출 시점에 내용을 슬롯으로 복사한다 (호출 후 버퍼가 바뀌어도 안전).
 *   %p 등 포인터는 값(주소)만 기록한다.
 * - printf 형식 검사(-Wformat)는 컴파일 시점에 그대로 동작한다.
 * - 인자는 최대 16개. long double 은 double 로 저장된다.
 * - 대가: 포맷팅 비용이 로깅 스레드 하나에 몰리므로, 스레드가 많으면 소비자가 병목이 될 수 있다.
 *
 * 기존 방식(호출 스레드가 포맷팅)은 LFLOG_TEXT / lflog_write 로 계속 쓸 수 있다.
 * 런타임에 만든 포맷 문자열은 이쪽을 써야 한다.
 *
 * 큐: Dmitry Vyukov 의 bounded MPMC 큐를 소비자 1개로 단순화한 것.
 * - 슬롯마다 seq 카운터가 있어 "비었음/채워짐/한 바퀴 뒤"를 구분한다.
 * - 생산자끼리는 enqueue_pos 하나에 대한 CAS 로만 경쟁한다 (뮤텍스 없음).
 * - 소비자는 할 일이 없으면 잠깐 스핀하다가 futex 로 잔다. 생산자는 소비자가
 *   잠들어 있을 때만 futex_wake 시스템콜을 한다 (평소에는 원자 변수 읽기 한 번).
 * - 큐가 가득 차면: drop_on_full=1 이면 버리고 개수만 셈, 0 이면 yield 하며 재시도.
 *
 * 주의: 슬롯 번호를 확보한 생산자가 데이터를 다 쓰기 전에 선점되면, 소비자는
 *       그 슬롯에서 기다려야 한다 (뒤의 슬롯이 채워져 있어도 순서를 지키므로).
 *       생산자 쪽 진행은 락프리지만, 소비자 쪽은 엄밀히 말하면 락프리가 아니다.
 */
#ifndef LOCKFREE_LOGGER_H
#define LOCKFREE_LOGGER_H

#include "log_common.h"

typedef struct lf_logger lf_logger;

typedef struct {
    log_level min_level;
    size_t capacity;   /* 슬롯 수 (2의 거듭제곱으로 올림). 메모리 = capacity * ~1KB */
    int drop_on_full;  /* 1: 가득 차면 버림, 0: 빌 때까지 재시도 */
} lf_logger_opts;

/* opts == NULL 이면 기본값 (INFO, 16384 슬롯 ≈ 17MB, 재시도) */
lf_logger *lflog_open(const char *path, const lf_logger_opts *opts);

/* 남은 로그를 모두 쓰고 스레드 종료 후 해제 */
void lflog_close(lf_logger *lg);

/* 지금까지 넣은 로그가 디스크에 써질 때까지 대기 */
void lflog_flush(lf_logger *lg);

int lflog_enabled(const lf_logger *lg, log_level lv);
void lflog_set_level(lf_logger *lg, log_level lv);
unsigned long lflog_dropped(lf_logger *lg);

/* ---- 지연 포맷팅: 타입 태그가 붙은 인자 ---- */

typedef enum { LF_ARG_I64, LF_ARG_U64, LF_ARG_F64, LF_ARG_STR, LF_ARG_PTR } lf_arg_type;

typedef struct {
    lf_arg_type type;
    union {
        long long i;
        unsigned long long u;
        double d;
        const char *s;
        const void *p;
    } v;
} lf_arg;

static inline lf_arg lf_arg_i(long long v) { return (lf_arg){LF_ARG_I64, {.i = v}}; }
static inline lf_arg lf_arg_u(unsigned long long v) { return (lf_arg){LF_ARG_U64, {.u = v}}; }
static inline lf_arg lf_arg_f(double v) { return (lf_arg){LF_ARG_F64, {.d = v}}; }
static inline lf_arg lf_arg_s(const char *v) { return (lf_arg){LF_ARG_STR, {.s = v}}; }
static inline lf_arg lf_arg_p(const void *v) { return (lf_arg){LF_ARG_PTR, {.p = v}}; }

/* 함수 지정자를 고른 뒤 호출 → 선택되지 않은 분기의 타입 변환 오류가 생기지 않음 */
#define LF_ARG(x) _Generic((x),                                              \
    _Bool: lf_arg_u, char: lf_arg_i, signed char: lf_arg_i,                  \
    short: lf_arg_i, int: lf_arg_i, long: lf_arg_i, long long: lf_arg_i,     \
    unsigned char: lf_arg_u, unsigned short: lf_arg_u, unsigned int: lf_arg_u, \
    unsigned long: lf_arg_u, unsigned long long: lf_arg_u,                   \
    float: lf_arg_f, double: lf_arg_f, long double: lf_arg_f,                \
    char *: lf_arg_s, const char *: lf_arg_s,                                \
    default: lf_arg_p)(x)

/* 가변 인자 개수 세기 (0~16) */
#define LF_NARGS(...) LF_NARGS_(__VA_ARGS__ __VA_OPT__(,) \
    16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define LF_NARGS_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, N, ...) N

/* 각 인자에 LF_ARG 적용 */
#define LF_CAT(a, b) LF_CAT_(a, b)
#define LF_CAT_(a, b) a##b
#define LF_MAP(...) LF_CAT(LF_MAP_, LF_NARGS(__VA_ARGS__))(__VA_ARGS__)
#define LF_MAP_1(a) LF_ARG(a)
#define LF_MAP_2(a, ...) LF_ARG(a), LF_MAP_1(__VA_ARGS__)
#define LF_MAP_3(a, ...) LF_ARG(a), LF_MAP_2(__VA_ARGS__)
#define LF_MAP_4(a, ...) LF_ARG(a), LF_MAP_3(__VA_ARGS__)
#define LF_MAP_5(a, ...) LF_ARG(a), LF_MAP_4(__VA_ARGS__)
#define LF_MAP_6(a, ...) LF_ARG(a), LF_MAP_5(__VA_ARGS__)
#define LF_MAP_7(a, ...) LF_ARG(a), LF_MAP_6(__VA_ARGS__)
#define LF_MAP_8(a, ...) LF_ARG(a), LF_MAP_7(__VA_ARGS__)
#define LF_MAP_9(a, ...) LF_ARG(a), LF_MAP_8(__VA_ARGS__)
#define LF_MAP_10(a, ...) LF_ARG(a), LF_MAP_9(__VA_ARGS__)
#define LF_MAP_11(a, ...) LF_ARG(a), LF_MAP_10(__VA_ARGS__)
#define LF_MAP_12(a, ...) LF_ARG(a), LF_MAP_11(__VA_ARGS__)
#define LF_MAP_13(a, ...) LF_ARG(a), LF_MAP_12(__VA_ARGS__)
#define LF_MAP_14(a, ...) LF_ARG(a), LF_MAP_13(__VA_ARGS__)
#define LF_MAP_15(a, ...) LF_ARG(a), LF_MAP_14(__VA_ARGS__)
#define LF_MAP_16(a, ...) LF_ARG(a), LF_MAP_15(__VA_ARGS__)

/* 컴파일 시점 printf 형식 검사용 (호출되지 않음) */
static inline __attribute__((format(printf, 1, 2))) void lf_fmt_check(const char *fmt, ...)
{
    (void)fmt;
}

/* fmt 는 정적 수명이어야 한다(포인터만 저장). 매크로에서 리터럴만 받도록 강제한다 */
void lflog_record(lf_logger *lg, log_level lv, const char *file, int line,
                  const char *fmt, const lf_arg *args, int nargs);

#define LFLOG(lg, lv, fmt, ...)                                                    \
    do {                                                                           \
        if (0)                                                                     \
            lf_fmt_check(fmt __VA_OPT__(,) __VA_ARGS__);                           \
        if (lflog_enabled((lg), (lv)))                                             \
            lflog_record((lg), (lv), __FILE__, __LINE__, "" fmt "",                \
                         (const lf_arg[]){__VA_OPT__(LF_MAP(__VA_ARGS__), ){0}},   \
                         LF_NARGS(__VA_ARGS__));                                   \
    } while (0)
#define LFLOG_DEBUG(lg, ...) LFLOG(lg, LOG_DEBUG, __VA_ARGS__)
#define LFLOG_INFO(lg, ...)  LFLOG(lg, LOG_INFO, __VA_ARGS__)
#define LFLOG_WARN(lg, ...)  LFLOG(lg, LOG_WARN, __VA_ARGS__)
#define LFLOG_ERROR(lg, ...) LFLOG(lg, LOG_ERROR, __VA_ARGS__)

/* ---- 기존 방식: 호출 스레드가 vsnprintf 로 포맷팅 후 큐에 넣음 ---- */

void lflog_write(lf_logger *lg, log_level lv, const char *file, int line,
                 const char *fmt, ...) __attribute__((format(printf, 5, 6)));

#define LFLOG_TEXT(lg, lv, ...)                                    \
    do {                                                           \
        if (lflog_enabled((lg), (lv)))                             \
            lflog_write((lg), (lv), __FILE__, __LINE__, __VA_ARGS__); \
    } while (0)

#endif
