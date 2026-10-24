CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=c11
LDFLAGS += -pthread
CFLAGS  += -pthread

LIB_SRC = logger/log_common.c logger/async_logger.c logger/lockfree_logger.c logger/sync_logger.c
LIB_OBJ = $(LIB_SRC:.c=.o)

all: liblogx.a example bench fmt_test fmt_bench

liblogx.a: $(LIB_OBJ)
	ar rcs $@ $^

example: examples/example.c liblogx.a
	$(CC) $(CFLAGS) -o $@ $< liblogx.a $(LDFLAGS)

bench: examples/bench.c liblogx.a
	$(CC) $(CFLAGS) -o $@ $< liblogx.a $(LDFLAGS)

fmt_test: examples/fmt_test.c liblogx.a
	$(CC) $(CFLAGS) -o $@ $< liblogx.a $(LDFLAGS)

# 포맷팅 비용 측정: lockfree_logger.c 의 static 함수를 직접 재려고 소스를 include 하므로 log_common.o 만 링크
fmt_bench: examples/fmt_bench.c logger/lockfree_logger.c logger/log_common.o
	$(CC) $(CFLAGS) -o $@ $< logger/log_common.o $(LDFLAGS)

test: fmt_test bench
	./fmt_test
	./bench 4 50000

# ThreadSanitizer 로 데이터 레이스 검사
tsan: CFLAGS = -O1 -g -Wall -Wextra -std=c11 -pthread -fsanitize=thread
tsan: LDFLAGS += -fsanitize=thread
tsan: clean bench
	./bench 4 20000

%.o: %.c logger/*.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(LIB_OBJ) liblogx.a example bench fmt_test fmt_bench *.log

.PHONY: all clean tsan test
