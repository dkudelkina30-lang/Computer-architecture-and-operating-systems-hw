# make - собрать обе программы: bin/line_sem (семафоры) и bin/line_cond (условные переменные)
# make test - тесты обеих программ;  make tsan - сборка с ThreadSanitizer;  make clean
CC     ?= gcc
CFLAGS ?= -std=c17 -O2 -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=200809L
LIBS   := -pthread
COMMON := src/main.c src/line.c src/config.c src/log.c
HDRS   := $(wildcard src/*.h)

all: bin/line_sem bin/line_cond

bin/line_sem: $(COMMON) src/counter_sem.c $(HDRS)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(COMMON) src/counter_sem.c $(LIBS)

bin/line_cond: $(COMMON) src/counter_cond.c $(HDRS)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(COMMON) src/counter_cond.c $(LIBS)

tsan:
	@mkdir -p bin
	$(CC) -std=c17 -g -O1 -D_POSIX_C_SOURCE=200809L -fsanitize=thread -o bin/line_sem_tsan $(COMMON) src/counter_sem.c $(LIBS)
	$(CC) -std=c17 -g -O1 -D_POSIX_C_SOURCE=200809L -fsanitize=thread -o bin/line_cond_tsan $(COMMON) src/counter_cond.c $(LIBS)

test: all
	./tests/run_tests.sh bin/line_sem
	./tests/run_tests.sh bin/line_cond

clean:
	rm -rf bin tests/out *.log

.PHONY: all tsan test clean
