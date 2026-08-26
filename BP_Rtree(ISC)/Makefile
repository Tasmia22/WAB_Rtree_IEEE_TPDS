export UPMEM_NO_OS_WARNING = 1
DPU_DIR := dpu
HOST_DIR := host
BUILDDIR ?= build
NR_TASKLETS ?= 11
NR_DPUS ?= 2540

define conf_filename
	${BUILDDIR}/.NR_DPUS_$(1)_NR_TASKLETS_$(2).conf
endef
CONF := $(call conf_filename,${NR_DPUS},${NR_TASKLETS})

HOST_TARGET := ${BUILDDIR}/host
DPU_TARGET := ${BUILDDIR}/dpu

COMMON_INCLUDES := common
HOST_SOURCES := $(wildcard ${HOST_DIR}/*.c)
DPU_SOURCES := $(wildcard ${DPU_DIR}/*.c)

.PHONY: all clean test

__dirs := $(shell mkdir -p ${BUILDDIR})
__logdir := $(shell mkdir -p logs)


# Common flags for both host and DPU
COMMON_FLAGS := -Wall -Wextra -O3 -DNDEBUG -I${COMMON_INCLUDES}


HOST_FLAGS := ${COMMON_FLAGS} -march=native -flto -funroll-loops -fomit-frame-pointer \
              -D_POSIX_C_SOURCE=200809L -std=c11 \
              `dpu-pkg-config --cflags --libs dpu` \
              -DNR_TASKLETS=${NR_TASKLETS} -DNR_DPUS=${NR_DPUS} -lm

# DPU flags (must use v1A or v1B, NOT native)
DPU_FLAGS := ${COMMON_FLAGS} -mcpu=v1B -DNR_TASKLETS=${NR_TASKLETS}


all: ${HOST_TARGET} ${DPU_TARGET}

${CONF}:
	$(RM) $(call conf_filename,*,*)
	touch ${CONF}

${HOST_TARGET}: ${HOST_SOURCES} ${COMMON_INCLUDES} ${CONF}
	$(CC) -o $@ ${HOST_SOURCES} ${HOST_FLAGS}

${DPU_TARGET}: ${DPU_SOURCES} ${COMMON_INCLUDES} ${CONF}
	dpu-upmem-dpurte-clang ${DPU_FLAGS} -o $@ ${DPU_SOURCES}

clean:
	$(RM) -r $(BUILDDIR)

test_c: ${HOST_TARGET} ${DPU_TARGET}
	./${HOST_TARGET}

test: test_c 

