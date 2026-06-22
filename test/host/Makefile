# ESP-IDF-free host test runner for main/lunar.c
#
# `lunar.c` is pure C (<stdio.h>/<string.h> only). This Makefile compiles it
# directly with the system C compiler — no ESP-IDF, no idf.py, no unity
# component install. It reuses the existing test cases from
# test/lunar/main/test_lunar.c via a thin compatibility shim.
#
# Usage (from repo root, with or without a uv venv):
#   make test-host           # build + run, exit code reflects pass/fail
#   make -C test/host run
#   make -C test/host clean
#
# Prereqs: a C compiler (cc/clang/gcc) — preinstalled on macOS/Linux.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wno-unused-parameter -O2 -I. -I../../main
SRCS    := test_lunar_host.c \
           ../../main/lunar.c
BIN     := build/lunar_host_test

.PHONY: all run clean

all: $(BIN)

$(BIN): $(SRCS) unity.h sdkconfig.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $(BIN) $(SRCS)

run: $(BIN)
	@printf '\033[36m[host] running %s (no ESP-IDF)...\033[0m\n' "$(BIN)"
	./$(BIN)

clean:
	rm -rf build
