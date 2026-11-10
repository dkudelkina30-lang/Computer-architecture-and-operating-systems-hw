CC     ?= gcc
CFLAGS ?= -std=c17 -O2 -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=200809L
LIBS   := -pthread -lrt
S      := src
CORE   := $(S)/line.c $(S)/log.c $(S)/worker.c
MGR    := $(CORE) $(S)/config.c $(S)/app.c
HDRS   := $(wildcard $(S)/*.h)
PROGS  := bin/line_unnamed bin/line_named bin/shm_manager bin/shm_worker bin/mq_manager bin/mq_worker

all: $(PROGS)

bin/line_unnamed: $(S)/main_shm.c $(MGR) $(S)/call_shm.c $(S)/sync_unnamed.c $(HDRS)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(S)/main_shm.c $(MGR) $(S)/call_shm.c $(S)/sync_unnamed.c $(LIBS)

bin/line_named: $(S)/main_shm.c $(MGR) $(S)/call_shm.c $(S)/sync_named.c $(HDRS)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(S)/main_shm.c $(MGR) $(S)/call_shm.c $(S)/sync_named.c $(LIBS)

bin/shm_manager: $(S)/main_shm.c $(MGR) $(S)/call_shm.c $(S)/sync_named.c $(HDRS)
	@mkdir -p bin
	$(CC) $(CFLAGS) -DINDEPENDENT -o $@ $(S)/main_shm.c $(MGR) $(S)/call_shm.c $(S)/sync_named.c $(LIBS)

bin/shm_worker: $(S)/worker_main.c $(CORE) $(S)/call_shm.c $(S)/sync_named.c $(HDRS)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(S)/worker_main.c $(CORE) $(S)/call_shm.c $(S)/sync_named.c $(LIBS)

bin/mq_manager: $(S)/manager_mq.c $(S)/line.c $(S)/log.c $(S)/config.c $(S)/app.c $(S)/call_mq.c $(S)/sync_named.c $(HDRS)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(S)/manager_mq.c $(S)/line.c $(S)/log.c $(S)/config.c $(S)/app.c $(S)/call_mq.c $(S)/sync_named.c $(LIBS)

bin/mq_worker: $(S)/worker_main.c $(CORE) $(S)/call_mq.c $(S)/sync_named.c $(HDRS)
	@mkdir -p bin
	$(CC) $(CFLAGS) -DUSE_MQ -o $@ $(S)/worker_main.c $(CORE) $(S)/call_mq.c $(S)/sync_named.c $(LIBS)

test: all
	./tests/run_tests.sh

clean:
	rm -rf bin tests/out *.log

.PHONY: all test clean
