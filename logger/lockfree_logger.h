/*
 * 방식 3: 락프리 MPSC 링 버퍼 + 로깅 전용 스레드
 *
 *  [작업 스레드들] --포맷팅--> CAS 로 슬롯 번호 확보 --> 슬롯에 memcpy --> seq 공개(release)
 *                                                                          │
 *  [로깅 스레드]   <-- seq 확인(acquire) 후 슬롯을 로컬 버퍼에 모음 --> write() (몰아서)
 *
 * - 큐는 Dmitry Vyukov 의 bounded MPMC 큐를 소비자 1개로 단순화한 것.
 *   슬롯마다 seq 카운터가 있어 "비었음/채워짐/한 바퀴 뒤"를 구분한다.
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

void lflog_write(lf_logger *lg, log_level lv, const char *file, int line,
                 const char *fmt, ...) __attribute__((format(printf, 5, 6)));

/* 지금까지 넣은 로그가 디스크에 써질 때까지 대기 */
void lflog_flush(lf_logger *lg);

int lflog_enabled(const lf_logger *lg, log_level lv);
void lflog_set_level(lf_logger *lg, log_level lv);
unsigned long lflog_dropped(lf_logger *lg);

#define LFLOG(lg, lv, ...)                                         \
    do {                                                           \
        if (lflog_enabled((lg), (lv)))                             \
            lflog_write((lg), (lv), __FILE__, __LINE__, __VA_ARGS__); \
    } while (0)
#define LFLOG_DEBUG(lg, ...) LFLOG(lg, LOG_DEBUG, __VA_ARGS__)
#define LFLOG_INFO(lg, ...)  LFLOG(lg, LOG_INFO, __VA_ARGS__)
#define LFLOG_WARN(lg, ...)  LFLOG(lg, LOG_WARN, __VA_ARGS__)
#define LFLOG_ERROR(lg, ...) LFLOG(lg, LOG_ERROR, __VA_ARGS__)

#endif
