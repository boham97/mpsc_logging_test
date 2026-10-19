/*
 * 방식 2: 로깅 전용 스레드 없음 (호출 스레드가 직접 쓴다)
 *
 * 두 가지 모드를 제공한다.
 *
 * (a) buf_size == 0 : 락 없는 직접 쓰기
 *     O_APPEND 로 연 fd 에 "한 줄 = write() 한 번". 리눅스 일반 파일에서 O_APPEND write 는
 *     오프셋 이동+쓰기가 원자적이라 스레드끼리 줄이 섞이거나 덮어쓰지 않는다.
 *     매 줄 시스템콜이지만 크래시가 나도 이미 호출한 로그는 커널에 넘어가 있다.
 *
 * (b) buf_size > 0 : 공유 버퍼 + 뮤텍스
 *     락 잡고 memcpy, 버퍼가 차면 "그 순간 락을 쥔 스레드"가 대신 write() 한다.
 *     시스템콜이 줄지만 (1) 버퍼를 비우는 스레드는 그동안 다른 스레드를 막고
 *     (2) 백그라운드 타이머가 없으므로 로그가 뜸하면 버퍼에 오래 머문다.
 *     → ERROR 이상은 즉시 flush, 그 외에는 slog_flush()/slog_close() 로 비운다.
 */
#ifndef SYNC_LOGGER_H
#define SYNC_LOGGER_H

#include "log_common.h"

typedef struct sync_logger sync_logger;

/* buf_size: 0 이면 (a) 모드, 그 외 (b) 모드 */
sync_logger *slog_open(const char *path, log_level min_level, size_t buf_size);
void slog_close(sync_logger *lg);

void slog_write(sync_logger *lg, log_level lv, const char *file, int line,
                const char *fmt, ...) __attribute__((format(printf, 5, 6)));
void slog_flush(sync_logger *lg);

int slog_enabled(const sync_logger *lg, log_level lv);
void slog_set_level(sync_logger *lg, log_level lv);

#define SLOG(lg, lv, ...)                                         \
    do {                                                          \
        if (slog_enabled((lg), (lv)))                             \
            slog_write((lg), (lv), __FILE__, __LINE__, __VA_ARGS__); \
    } while (0)
#define SLOG_DEBUG(lg, ...) SLOG(lg, LOG_DEBUG, __VA_ARGS__)
#define SLOG_INFO(lg, ...)  SLOG(lg, LOG_INFO, __VA_ARGS__)
#define SLOG_WARN(lg, ...)  SLOG(lg, LOG_WARN, __VA_ARGS__)
#define SLOG_ERROR(lg, ...) SLOG(lg, LOG_ERROR, __VA_ARGS__)

#endif
