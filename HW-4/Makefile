CC     ?= gcc
CFLAGS ?= -std=c17 -O2 -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=200809L
S      := src
CLIENT := $(S)/client.c $(S)/net.c $(S)/log.c
SERVER := $(S)/server.c $(S)/line.c $(S)/config.c $(S)/net.c $(S)/log.c
HDRS   := $(wildcard $(S)/*.h)
PROGS  := bin/tcp_server bin/tcp_worker bin/tcp_observer bin/tcp_control

all: $(PROGS)

bin/tcp_server: $(SERVER) $(HDRS)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(SERVER)

bin/tcp_worker: $(S)/worker_main.c $(CLIENT) $(HDRS)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(S)/worker_main.c $(CLIENT)

bin/tcp_observer: $(S)/observer.c $(CLIENT) $(HDRS)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(S)/observer.c $(CLIENT)

bin/tcp_control: $(S)/control.c $(CLIENT) $(HDRS)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(S)/control.c $(CLIENT)

test: all
	timeout 65 bash tests/run_tests.sh

test-network: all
	timeout 35 bash tests/run_network.sh

clean:
	rm -rf bin tests/out *.log

.PHONY: all test test-network clean
