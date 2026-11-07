# tools/Makefile - host tools for the Doom frame dumps; products go to build/tools/
include $(dir $(lastword $(MAKEFILE_LIST)))../config.mk

OUT := $(BUILD)/tools
CXXFLAGS := -std=gnu++17 -O2 -Wall -Wextra -I$(ROOT)/sim/common

.PHONY: all clean
all: $(OUT)/framecmp $(OUT)/dfrm2ppm

$(OUT)/%: %.cpp $(ROOT)/sim/common/dfrm.h
	@mkdir -p $(OUT)
	$(HOST_CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f $(OUT)/framecmp $(OUT)/dfrm2ppm
