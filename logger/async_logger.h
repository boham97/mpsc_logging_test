/*
 * 방식 1: 로깅 전용 백그라운드 스레드 (비동기, 더블 버퍼)
 *
 *  [작업 스레드들] --포맷팅(락 없음)--> front 버퍼에 memcpy (락은 이 순간만)
 *                                           │ swap (포인터 교환, O(1))
 *  [로깅 스레드]   <------------------------ back 버퍼 --> write() (락 없음)
 *
 * - 호출 스레드는 디스크 I/O를 하지 않는다 → 지연시간이 짧고 일정
 * - 로깅 스레드는 flush_ms 주기 / 버퍼 절반 참 / ERROR 이상 발생 시 깨어나 몰아서 쓴다
 * - 버퍼가 가득 차면: drop_on_full=1 이면 버리고 개수만 셈, 0 이면 호출자가 대기
 */
#ifndef ASYNC_LOGGER_H
#define ASYNC_LOGGER_H

#include "log_common.h"

typedef struct async_logger async_logger;

typedef struct {
    log_level min_level;  /* 이보다 낮은 레벨은 무시 */
    size_t buf_size;      /* front/back 버퍼 각각의 크기 (바이트) */
    int flush_ms;         /* 최대 이 시간 안에 디스크로 내려감 */
    int drop_on_full;     /* 1: 버퍼 가득 차면 버림, 0: 빌 때까지 대기 */
} async_logger_opts;

/* opts == NULL 이면 기본값 (INFO, 4MB, 200ms, 대기) */
async_logger *alog_open(const char *path, const async_logger_opts *opts);

/* 남은 로그를 모두 쓰고 스레드 종료 후 해제 */
void alog_close(async_logger *lg);

void alog_write(async_logger *lg, log_level lv, const char *file, int line,
                const char *fmt, ...) __attribute__((format(printf, 5, 6)));

/* 지금까지 제출된 로그가 디스크에 써질 때까지 대기 */
void alog_flush(async_logger *lg);

int alog_enabled(const async_logger *lg, log_level lv);
void alog_set_level(async_logger *lg, log_level lv);
unsigned long alog_dropped(async_logger *lg);

#define ALOG(lg, lv, ...)                                         \
    do {                                                          \
        if (alog_enabled((lg), (lv)))                             \
            alog_write((lg), (lv), __FILE__, __LINE__, __VA_ARGS__); \
    } while (0)
#define ALOG_DEBUG(lg, ...) ALOG(lg, LOG_DEBUG, __VA_ARGS__)
#define ALOG_INFO(lg, ...)  ALOG(lg, LOG_INFO, __VA_ARGS__)
#define ALOG_WARN(lg, ...)  ALOG(lg, LOG_WARN, __VA_ARGS__)
#define ALOG_ERROR(lg, ...) ALOG(lg, LOG_ERROR, __VA_ARGS__)

#endif
