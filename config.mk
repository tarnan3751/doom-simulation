# Shared tool/flag configuration. Include from any Makefile as:
#   include $(dir $(lastword $(MAKEFILE_LIST)))<relative path>/config.mk
# ROOT is the absolute path of the repository root.
ROOT := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
BUILD := $(ROOT)/build
THIRD_PARTY := $(ROOT)/third_party

# --- RISC-V bare-metal toolchain (Homebrew riscv64-elf-gcc, has rv32im/ilp32 libgcc; NO libc) ---
RV_PREFIX  ?= riscv64-elf-
RV_CC      := $(RV_PREFIX)gcc
RV_AS      := $(RV_PREFIX)gcc
RV_LD      := $(RV_PREFIX)gcc
RV_AR      := $(RV_PREFIX)ar
RV_OBJCOPY := $(RV_PREFIX)objcopy
RV_OBJDUMP := $(RV_PREFIX)objdump
RV_SIZE    := $(RV_PREFIX)size
RV_NM      := $(RV_PREFIX)nm
RV_ARCH    := -march=rv32im_zicsr -mabi=ilp32

# --- Verilator (Homebrew). Apple clang 15 needs an explicit C++ standard. ---
VERILATOR      ?= verilator
VERILATOR_CXXFLAGS := -std=gnu++17 -O3 -Wno-unknown-warning-option
HOST_CXX       ?= c++
HOST_CXXFLAGS  ?= -std=gnu++17 -O2 -Wall
HOST_CC        ?= cc

# --- SDL2 (Homebrew) ---
SDL2_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null)
SDL2_LIBS   := $(shell sdl2-config --libs 2>/dev/null)

# --- FPGA flow (OSS CAD Suite, project-local) ---
OSS_CAD  := $(ROOT)/.tools/oss-cad-suite
YOSYS    := $(OSS_CAD)/bin/yosys
NEXTPNR  := $(OSS_CAD)/bin/nextpnr-himbaechel
GOWIN_PACK := $(OSS_CAD)/bin/gowin_pack

# --- Third-party sources (fetched by scripts/fetch-third-party.sh) ---
RISCV_TESTS := $(THIRD_PARTY)/riscv-tests
DOOMGENERIC := $(THIRD_PARTY)/doomgeneric/doomgeneric
COREMARK    := $(THIRD_PARTY)/coremark
DOOM_WAD    := $(THIRD_PARTY)/wad/doom1.wad
