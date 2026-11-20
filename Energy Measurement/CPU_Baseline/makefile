# ================================
#   FAIR CPU BASELINE MAKEFILE
# ================================

MODE ?= release

CC := gcc
PYTHON ?= python3

SOURCE_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PROJECT_DIR := $(abspath $(SOURCE_DIR)/..)
TOOLS_DIR := $(SOURCE_DIR)/tools
COMMON_SRCS := $(SOURCE_DIR)/rtree.c $(SOURCE_DIR)/rtreefunction.c
COMMON_DEPS := $(SOURCE_DIR)/rtree.h
BASE_TARGET := rtree_cpu_baseline
ROOFLINE_TARGET := rtree_cpu_roofline
ROOFLINE_ARGS ?=
ROOFLINE_OUTPUT_DIR ?= roofline_outputs
ROOFLINE_PLOT ?= $(ROOFLINE_OUTPUT_DIR)/roofline.png
ROOFLINE_CSV ?= $(ROOFLINE_OUTPUT_DIR)/roofline_points.csv
ROOFLINE_HOST_PLOT ?= $(ROOFLINE_OUTPUT_DIR)/roofline_host.png
ROOFLINE_HOST_CSV ?= $(ROOFLINE_OUTPUT_DIR)/roofline_host.csv
HOST_PEAK_BW_GBS ?= 100
HOST_PEAK_MTESTS ?= 5000

CSTD    := -std=c11
POSIX   := -D_POSIX_C_SOURCE=200809L
WARN    := -Wall -Wextra -Wshadow -Wundef
OPT     := -O3
DEBUG   := -g
THREADS := -pthread
CPUFLAGS := -march=native -flto -funroll-loops -fomit-frame-pointer
EXTRA :=
PLATFORM_FLAGS :=

# On this macOS installation the default SDK cannot be parsed by the linker.
# Prefer the compatible SDK when present; Linux builds leave SDKROOT unset.
ifeq ($(strip $(SDKROOT)),)
  ifneq ($(wildcard /Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk),)
    SDKROOT := /Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk
    MACOSX_DEPLOYMENT_TARGET ?= 15.0
  endif
endif

ifneq ($(strip $(SDKROOT)),)
  PLATFORM_FLAGS += -isysroot $(SDKROOT)
endif

ifneq ($(strip $(MACOSX_DEPLOYMENT_TARGET)),)
  PLATFORM_FLAGS += -mmacosx-version-min=$(MACOSX_DEPLOYMENT_TARGET)
endif

ifeq ($(MODE),debug)
  CFLAGS_COMMON := $(CSTD) $(POSIX) $(WARN) $(DEBUG) $(CPUFLAGS) $(THREADS) $(PLATFORM_FLAGS) $(EXTRA)
else
  CFLAGS_COMMON := $(CSTD) $(POSIX) $(WARN) $(OPT) -DNDEBUG $(CPUFLAGS) $(THREADS) $(PLATFORM_FLAGS) $(EXTRA)
endif

CFLAGS_BASE := $(CFLAGS_COMMON)
CFLAGS_ROOFLINE := $(CFLAGS_COMMON) -DENABLE_ROOFLINE=1
LDFLAGS := $(THREADS) -lm $(PLATFORM_FLAGS)

BASE_OBJS := rtree.base.o rtreefunction.base.o
ROOFLINE_OBJS := rtree.roofline.o rtreefunction.roofline.o

all: $(BASE_TARGET)

$(BASE_TARGET): $(BASE_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

$(ROOFLINE_TARGET): $(ROOFLINE_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

rtree.base.o: $(SOURCE_DIR)/rtree.c $(COMMON_DEPS)
	$(CC) $(CFLAGS_BASE) -c $< -o $@

rtreefunction.base.o: $(SOURCE_DIR)/rtreefunction.c $(COMMON_DEPS)
	$(CC) $(CFLAGS_BASE) -c $< -o $@

rtree.roofline.o: $(SOURCE_DIR)/rtree.c $(COMMON_DEPS)
	$(CC) $(CFLAGS_ROOFLINE) -c $< -o $@

rtreefunction.roofline.o: $(SOURCE_DIR)/rtreefunction.c $(COMMON_DEPS)
	$(CC) $(CFLAGS_ROOFLINE) -c $< -o $@

test: $(BASE_TARGET)
	cd $(PROJECT_DIR) && $(SOURCE_DIR)/$(BASE_TARGET)

test-roofline: $(ROOFLINE_TARGET)
	./$(ROOFLINE_TARGET)

print-flags:
	@echo "CFLAGS_BASE     = $(CFLAGS_BASE)"
	@echo "CFLAGS_ROOFLINE = $(CFLAGS_ROOFLINE)"
	@echo "LDFLAGS         = $(LDFLAGS)"

roofline:
	$(PYTHON) $(TOOLS_DIR)/plot_roofline.py --output $(ROOFLINE_PLOT) --csv $(ROOFLINE_CSV) $(ROOFLINE_ARGS)

roofline-host:
	$(PYTHON) $(TOOLS_DIR)/plot_roofline.py --output $(ROOFLINE_HOST_PLOT) --csv $(ROOFLINE_HOST_CSV) --peak-bandwidth-gbs $(HOST_PEAK_BW_GBS) --peak-throughput-mtests $(HOST_PEAK_MTESTS) $(ROOFLINE_ARGS)

clean:
	rm -f *.o $(BASE_TARGET) $(ROOFLINE_TARGET)

.PHONY: all clean print-flags roofline roofline-host test test-roofline
