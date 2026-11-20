# Makefile for CPU plus CUDA R tree project
#salloc -A general -p gpu -t 02:00:00 --gres=gpu:1 --cpus-per-task=4 --mem=32G
#compile and run: module load cuda-toolkit/12.6
#nvcc -O3 -std=c++17 host_main.cu rtree_gpu.cu rtreefunction.c -I . -o build/rtree_run


CUDA_PATH ?= /usr/local/cuda
NVCC := $(CUDA_PATH)/bin/nvcc
CC := gcc
CXX := g++

ARCH ?= native

CFLAGS   := -O3 -std=c11
CXXFLAGS := -O3 -std=c++17
NVFLAGS  := -O3 -std=c++17 -arch=$(ARCH)

SRC_DIR := src
OBJ_DIR := build
INCLUDES := -I$(SRC_DIR)

C_SRCS   := $(SRC_DIR)/rtreefunction.c
CU_SRCS  := $(SRC_DIR)/rtree_gpu.cu
CPP_SRCS := $(wildcard $(SRC_DIR)/host_main.cpp) $(wildcard $(SRC_DIR)/main.cpp) $(wildcard $(SRC_DIR)/host_main.cu)

OBJS_C    := $(patsubst $(SRC_DIR)/%.c,   $(OBJ_DIR)/%.o, $(C_SRCS))
OBJS_CU   := $(patsubst $(SRC_DIR)/%.cu,  $(OBJ_DIR)/%.o, $(CU_SRCS))
OBJS_CPP  := $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(filter %.cpp,$(CPP_SRCS)))
OBJS_CUDA := $(patsubst $(SRC_DIR)/%.cu,  $(OBJ_DIR)/%.o, $(filter %.cu,$(CPP_SRCS)))
OBJS := $(OBJS_C) $(OBJS_CU) $(OBJS_CPP) $(OBJS_CUDA)

TARGET := rtree_search
ENERGY_TARGET := rtree_energy
ENERGY_HOST_OBJ := $(OBJ_DIR)/host_main_energy.o

.PHONY: all energy clean dirs

all: dirs $(TARGET)

energy: dirs $(ENERGY_TARGET)

dirs:
	@mkdir -p $(OBJ_DIR)

$(TARGET): $(OBJS)
	$(NVCC) $(NVFLAGS) $(INCLUDES) -o $@ $^

$(ENERGY_TARGET): $(OBJS_C) $(OBJS_CU) $(ENERGY_HOST_OBJ)
	$(NVCC) $(NVFLAGS) $(INCLUDES) -o $@ $^

$(ENERGY_HOST_OBJ): $(SRC_DIR)/host_main.cu
	$(NVCC) $(NVFLAGS) $(INCLUDES) -DRTREE_ENERGY_MEASUREMENT -c $< -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cu
	$(NVCC) $(NVFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR) $(TARGET) $(ENERGY_TARGET)

help:
	@echo "make ARCH=native for the current GPU, or set ARCH=sm_90 / sm_70 for specific hardware"
	@echo "make energy ARCH=native builds rtree_energy for repeated PN150 wall-energy measurements"
