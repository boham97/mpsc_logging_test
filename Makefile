CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=c11
LDFLAGS += -pthread
CFLAGS  += -pthread

LIB_SRC = logger/log_common.c logger/async_logger.c logger/lockfree_logger.c logger/sync_logger.c
LIB_OBJ = $(LIB_SRC:.c=.o)

all: liblogx.a example bench fmt_test

liblogx.a: $(LIB_OBJ)
	ar rcs $@ $^

example: examples/example.c liblogx.a
	$(CC) $(CFLAGS) -o $@ $< liblogx.a $(LDFLAGS)

bench: examples/bench.c liblogx.a
	$(CC) $(CFLAGS) -o $@ $< liblogx.a $(LDFLAGS)

fmt_test: examples/fmt_test.c liblogx.a
	$(CC) $(CFLAGS) -o $@ $< liblogx.a $(LDFLAGS)

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
	rm -f $(LIB_OBJ) liblogx.a example bench fmt_test *.log

.PHONY: all clean tsan test
