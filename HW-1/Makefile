CC     ?= gcc
CFLAGS ?= -std=c17 -O2 -Wall -Wextra -Wpedantic -Wshadow -D_POSIX_C_SOURCE=200809L
SRC    := $(wildcard src/*.c)

bin/assembly_line: $(SRC) $(wildcard src/*.h)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(SRC)

asan:
	@mkdir -p bin
	$(CC) -std=c17 -g -Wall -D_POSIX_C_SOURCE=200809L -fsanitize=address,undefined -o bin/assembly_line_asan $(SRC)

test: bin/assembly_line
	./tests/run_tests.sh

clean:
	rm -rf bin build tests/out *.log

.PHONY: asan test clean
