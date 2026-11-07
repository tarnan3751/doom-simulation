# sw/bsp/Makefile - board support package.
#
#   make     build/sw/bsp/crt0.o and build/sw/bsp/libbsp.a
#   make clean
SW_LIB_BUILD := 1
include $(dir $(lastword $(MAKEFILE_LIST)))../common.mk

BSP_OUT  := $(SW_BUILD)/bsp
BSP_SRCS := $(BSP_DIR)/bsp.c $(BSP_DIR)/trap.c
BSP_OBJS := $(patsubst $(BSP_DIR)/%.c,$(BSP_OUT)/%.o,$(BSP_SRCS))
BSP_HDRS := $(BSP_DIR)/include/bsp.h $(SW_DIR)/include/soc.h $(wildcard $(LIBC_DIR)/include/*.h)

.PHONY: all lib clean
all: lib
lib: $(CRT0) $(BSP_A)

$(CRT0): $(BSP_DIR)/crt0.S
	@mkdir -p $(dir $@)
	@echo "  AS      bsp/crt0.S"
	@$(RV_CC) $(SW_ASFLAGS) -c $< -o $@

$(BSP_OUT)/%.o: $(BSP_DIR)/%.c $(BSP_HDRS)
	@mkdir -p $(dir $@)
	@echo "  CC      bsp/$(notdir $<)"
	@$(RV_CC) $(SW_CFLAGS) -O2 -c $< -o $@

$(BSP_A): $(BSP_OBJS)
	@echo "  AR      $(notdir $@)"
	@rm -f $@
	@$(RV_AR) rcs $@ $(BSP_OBJS)

clean:
	rm -rf $(BSP_OUT)
