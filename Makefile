# Host build: unit tests for the portable C core (firmware/core).
#
#   make test    build and run every test suite
#   make clean   remove build output
#
# On Windows, run with MSYS2's make (C:\msys64\usr\bin\make.exe) or from the
# "MSYS2 UCRT64" shell. The MSYS2 UCRT64 gcc is picked up automatically.

CC     := gcc
CFLAGS := -std=c99 -Wall -Wextra -Werror -pedantic -Wconversion -g -O0

# Windows + MSYS2: make sees C:\msys64 as "/", so the UCRT64 gcc is at
# /ucrt64/bin. (MSYS2's make hides $(OS), so detect by path instead.)
ifneq ($(wildcard /ucrt64/bin/gcc.exe),)
  CC := /ucrt64/bin/gcc
  # /usr/bin supplies sh, rm and mkdir when make is started from PowerShell
  export PATH := /ucrt64/bin:/usr/bin:$(PATH)
  # gcc needs a writable temp dir; MSYS2 make may not pass TMP through
  export TMP  := $(CURDIR)/build
  export TEMP := $(CURDIR)/build
  EXE := .exe
  # link the simulator statically so it runs without MSYS2's DLLs on PATH
  SIM_LDFLAGS := -static
endif

CORE   := firmware/core
TEST   := firmware/test
FAKES  := $(TEST)/fakes
UNITY  := third_party/unity/src
BUILD  := build

INCS   := -I$(CORE) -I$(FAKES) -I$(UNITY)
COMMON := $(UNITY)/unity.c $(FAKES)/fake_hal.c

# test name -> core sources it needs
SRC_test_ring_buffer := $(CORE)/ring_buffer.c
SRC_test_cmd_parser  := $(CORE)/cmd_parser.c
SRC_test_mpu6050     := $(CORE)/mpu6050.c
SRC_test_logger      := $(CORE)/logger.c $(CORE)/publisher.c $(CORE)/ring_buffer.c $(CORE)/cmd_parser.c $(CORE)/mpu6050.c
SRC_test_logger_nohooks := $(SRC_test_logger)
SRC_test_publisher   := $(CORE)/publisher.c $(CORE)/ring_buffer.c $(FAKES)/fake_net.c
SRC_test_logger_net  := $(SRC_test_logger) $(FAKES)/fake_net.c

# test name -> extra defines. test_logger_nohooks is the production build.
DEFS_test_logger := -DLOGGER_TEST_HOOKS
DEFS_test_logger_net := -DLOGGER_TEST_HOOKS

SUITES := test_ring_buffer test_cmd_parser test_mpu6050 test_logger test_logger_nohooks \
          test_publisher test_logger_net
BINS   := $(addprefix $(BUILD)/,$(addsuffix $(EXE),$(SUITES)))

.DEFAULT_GOAL := test
.PHONY: test sim clean
.SECONDEXPANSION:

# ---- host simulator (Project 1) ------------------------------------------
# The firmware core + sim/host_main.c. build/sim has the test hooks,
# build/sim_prod is the production build (FAULT -> ERR UNKNOWN).
CORE_SRC  := $(CORE)/logger.c $(CORE)/publisher.c $(CORE)/ring_buffer.c $(CORE)/cmd_parser.c $(CORE)/mpu6050.c
SIM_FLAGS := -std=c99 -Wall -Wextra -Werror -pedantic -O2 -pthread -I$(CORE)

sim: $(BUILD)/sim$(EXE) $(BUILD)/sim_prod$(EXE)

$(BUILD)/sim$(EXE): sim/host_main.c $(CORE_SRC) $(wildcard $(CORE)/*.h) | $(BUILD)
	$(CC) $(SIM_FLAGS) -DLOGGER_TEST_HOOKS -o $@ sim/host_main.c $(CORE_SRC) $(SIM_LDFLAGS)

$(BUILD)/sim_prod$(EXE): sim/host_main.c $(CORE_SRC) $(wildcard $(CORE)/*.h) | $(BUILD)
	$(CC) $(SIM_FLAGS) -o $@ sim/host_main.c $(CORE_SRC) $(SIM_LDFLAGS)

test: $(BINS)
	@set -e; for t in $(BINS); do echo "== $$t"; ./$$t; done
	@echo "All test suites passed."

# Unity itself is not -Wconversion clean, so it is compiled without that flag.
$(BUILD)/unity.o: $(UNITY)/unity.c | $(BUILD)
	$(CC) -std=c99 -Wall -Wextra -g -c $< -o $@

$(BUILD)/%$(EXE): $(TEST)/%.c $$(SRC_$$*) $(FAKES)/fake_hal.c $(BUILD)/unity.o $(wildcard $(CORE)/*.h $(FAKES)/*.h) | $(BUILD)
	$(CC) $(CFLAGS) $(DEFS_$*) $(INCS) -o $@ $(TEST)/$*.c $(SRC_$*) $(FAKES)/fake_hal.c $(BUILD)/unity.o

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
