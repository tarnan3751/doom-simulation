# sw/coremark/Makefile - CoreMark: upstream sources unmodified + the port core_portme.[ch].
#
#   make                  build/sw/coremark/coremark.elf (CM_ITERATIONS) and coremark-quick.elf (20)
#   make CM_ITERATIONS=N  another iteration count for the full run
#
# The default count keeps a run valid (>= 10 s at 50 MHz) even on the ISS, i.e. at IPC 1.
CM_ITERATIONS       ?= 2500
CM_QUICK_ITERATIONS ?= 20

# reportable flags (printed as "Compiler flags"): the set SiFive publishes for its in-order cores
CM_OPT := -O2 -fno-common -funroll-all-loops -finline-limit=600 -ftree-dominator-opts \
          -fno-if-conversion2 -fselective-scheduling -fno-code-hoisting -funroll-loops \
          -finline-functions -falign-functions=4 -falign-jumps=4 -falign-loops=4

CM_DIR  := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
VARIANT ?= full
PROG    := $(if $(filter quick,$(VARIANT)),coremark-quick,coremark)
CM_ITER := $(if $(filter quick,$(VARIANT)),$(CM_QUICK_ITERATIONS),$(CM_ITERATIONS))
SRCS     = $(addprefix $(COREMARK)/,core_list_join.c core_main.c core_matrix.c core_state.c core_util.c) \
           core_portme.c
OUT      = $(SW_BUILD)/coremark
OBJDIR   = $(OUT)/obj-$(PROG)
OPT     := $(CM_OPT)
PROG_CFLAGS = -I$(CM_DIR) -I$(COREMARK) -DPERFORMANCE_RUN=1 -DITERATIONS=$(CM_ITER) \
              -DFLAGS_STR='"$(strip $(CM_OPT)) $(SW_ARCH)"'
include $(CM_DIR)/../common.mk

ifeq ($(VARIANT),full)
all: quick
clean: clean-quick
.PHONY: quick clean-quick
quick:
	@$(MAKE) --no-print-directory -f $(CM_DIR)/Makefile VARIANT=quick all
clean-quick:
	@$(MAKE) --no-print-directory -f $(CM_DIR)/Makefile VARIANT=quick clean
endif
