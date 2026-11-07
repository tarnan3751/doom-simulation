# sw/libc/tests/Makefile -> build/sw/libctest/libctest.elf (on-target libc self-test)
# The host unit tests are run from the parent directory: make -C sw/libc test
PROG := libctest
SRCS := libctest.c
OUT  := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../../../build/sw/libctest)
# keep the reference loops loops (no builtins, no loop -> memcpy rewriting)
# (the printf table deliberately contains cases GCC's format checker frowns upon)
PROG_CFLAGS := -fno-builtin -fno-tree-loop-distribute-patterns -Wno-format -Wno-format-overflow -Wno-format-truncation
include $(dir $(lastword $(MAKEFILE_LIST)))../../common.mk
