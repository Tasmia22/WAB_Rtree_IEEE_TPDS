# ================================
#   FAIR CPU BASELINE MAKEFILE
#   (Portable debug; optimized release uses native CPU tuning)
# ================================

# -------- Config --------
MODE ?= release

# -------- Tools ---------
CC := gcc

# -------- Sources -------
SRCS  := $(wildcard *.c)
OBJS  := $(SRCS:.c=.o)
DEPS  := rtree.h
TARGET := rtree_cpu_baseline
ROOFLINE_DIR := roofline
ROOFLINE_OUTPUT_DIR := $(ROOFLINE_DIR)/output
ROOFLINE_LOG_DIR := $(ROOFLINE_OUTPUT_DIR)/logs
ROOFLINE_LOG := $(ROOFLINE_LOG_DIR)/roofline_datasets_10pct.txt
ROOFLINE_PLOT := $(ROOFLINE_OUTPUT_DIR)/roofline_datasets_10pct.png
# Empirical upper envelope for the 12-thread memory path.  The search counters
# model logical bytes rather than hardware-counter DRAM traffic, so use the
# Conservative effective-bandwidth envelope for the logical-byte model.  It
# includes modest run-to-run headroom above the observed 120.44 GB/s maximum;
# it is not a hardware-counter DRAM-bandwidth measurement.
ROOFLINE_BANDWIDTH := 1.25e11
STREAM_TARGET := $(ROOFLINE_DIR)/stream
STREAM_OBJ := $(ROOFLINE_DIR)/stream.o
PEAK_TARGET := $(ROOFLINE_DIR)/overlap_peak
PEAK_OBJ := $(ROOFLINE_DIR)/overlap_peak.o
BANDWIDTH_TARGET := $(ROOFLINE_DIR)/pthread_bandwidth

# -------- Flags ---------
CSTD    := -std=c11
WARN    := -Wall -Wextra -Wshadow -Wundef
OPT     := -O3                     # optimized release build
DEBUG   := -g
THREADS := -pthread

# Use native CPU tuning for the release build to match host performance
CPUFLAGS := -march=native

# No LTO, no unroll, no -march, no strict alias
EXTRA :=                           # intentionally empty

# Release vs Debug mode
ifeq ($(MODE),debug)
  CFLAGS := $(CSTD) $(WARN) $(DEBUG) $(CPUFLAGS) $(THREADS) $(EXTRA)
else
  CFLAGS := $(CSTD) $(WARN) $(OPT) $(CPUFLAGS) $(THREADS) $(EXTRA)
endif

# Link libs
LDFLAGS := $(THREADS) -lm

# -------- Build Rules --------
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

$(STREAM_TARGET): $(ROOFLINE_DIR)/stream.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(PEAK_TARGET): $(PEAK_OBJ) rtreefunction.o
	$(CC) -o $@ $^ $(LDFLAGS)

$(BANDWIDTH_TARGET): $(ROOFLINE_DIR)/pthread_bandwidth.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c $< -o $@

$(PEAK_OBJ): $(ROOFLINE_DIR)/overlap_peak.c $(DEPS)
	$(CC) $(CFLAGS) -I. -c $< -o $@

test: $(TARGET) plot

plot: plot-datasets-10pct

plot-datasets-10pct: $(TARGET) $(PEAK_TARGET)
	mkdir -p $(ROOFLINE_LOG_DIR)
	python3 $(ROOFLINE_DIR)/run_datasets_roofline.py --log-file $(ROOFLINE_LOG)
	python3 $(ROOFLINE_DIR)/roofline_plot_from_log.py --log-file $(ROOFLINE_LOG) --bandwidth $(ROOFLINE_BANDWIDTH) --peak-bin ./$(PEAK_TARGET) --mode parallel --all --labels 'Sports 999K,Parks 9.9M,Lakes 8.4M,Uniform 16M,Buildings 57M' --output $(ROOFLINE_PLOT) --title 'CPU Roofline: R-tree Search (10% Queries)'

clean:
	rm -f $(TARGET) $(STREAM_TARGET) $(STREAM_OBJ) $(PEAK_TARGET) $(PEAK_OBJ) $(BANDWIDTH_TARGET) *.o $(ROOFLINE_PLOT) $(ROOFLINE_LOG)

print-flags:
	@echo "CFLAGS  = $(CFLAGS)"
	@echo "LDFLAGS = $(LDFLAGS)"

.PHONY: all clean print-flags plot plot-datasets-10pct
