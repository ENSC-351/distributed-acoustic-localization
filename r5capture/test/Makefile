# Unit tests for r5capture, run on the PC (course VM).
#   make test        build and run every test
#   make arm-check   check that all firmware code also compiles for ARM (aarch64)
#   make board       cross-compile the tests for the BeagleY-AI into BOARD_DIR (default: the NFS share)
#   make clean
#
# Adding a test: add its name to TESTS and a rule like the one for test_mcp3208.

CC     := gcc
CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Wmissing-prototypes -Werror
INC    := -I../fw -I../include -I../linux
OUT    := ../../build/r5capture-test

TESTS  := $(OUT)/test_mcp3208 $(OUT)/test_hal_sim $(OUT)/test_capture $(OUT)/test_ring

CORE   := ../fw/capture.c ../fw/mcp3208.c hal_sim.c
HDRS   := $(wildcard ../fw/*.h ../include/*.h ../linux/*.h *.h)

BOARD_DIR ?= $(HOME)/ensc351/public/myApps/r5capture
ARM_CC    := aarch64-linux-gnu-gcc

.PHONY: test arm-check board clean

test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; $$t; done

$(OUT)/test_mcp3208: test_mcp3208.c ../fw/mcp3208.c ../fw/mcp3208.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INC) test_mcp3208.c ../fw/mcp3208.c -o $@

# hal_sim.c is the fake hardware (test/ only, never part of the firmware)
$(OUT)/test_hal_sim: test_hal_sim.c hal_sim.c hal_sim.h ../fw/mcp3208.c ../fw/mcp3208.h ../fw/r5_hal.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INC) test_hal_sim.c hal_sim.c ../fw/mcp3208.c -o $@

$(OUT)/test_capture: test_capture.c $(CORE) $(HDRS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INC) test_capture.c $(CORE) -o $@

$(OUT)/test_ring: test_ring.c $(CORE) ../linux/r5cap_reader.c $(HDRS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INC) -pthread test_ring.c $(CORE) ../linux/r5cap_reader.c -o $@

arm-check:
	@set -e; for f in ../fw/*.c ../linux/*.c; do \
		aarch64-linux-gnu-gcc $(CFLAGS) $(INC) -c $$f -o /dev/null; \
		echo "aarch64 OK: $$f"; \
	done

board:
	@mkdir -p $(BOARD_DIR)
	$(ARM_CC) $(CFLAGS) $(INC) test_mcp3208.c ../fw/mcp3208.c -o $(BOARD_DIR)/test_mcp3208
	$(ARM_CC) $(CFLAGS) $(INC) test_hal_sim.c hal_sim.c ../fw/mcp3208.c -o $(BOARD_DIR)/test_hal_sim
	$(ARM_CC) $(CFLAGS) $(INC) test_capture.c $(CORE) -o $(BOARD_DIR)/test_capture
	$(ARM_CC) $(CFLAGS) $(INC) -pthread test_ring.c $(CORE) ../linux/r5cap_reader.c -o $(BOARD_DIR)/test_ring
	cp ../board/*.sh $(BOARD_DIR)/
	@echo "On the board: cd /mnt/remote/myApps/r5capture && ./run_tests.sh"

clean:
	rm -rf $(OUT)
