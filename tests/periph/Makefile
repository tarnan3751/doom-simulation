# Peripheral / SoC-top tests. `make -C tests/periph test` runs everything.
# PERIPH, SOCSRC and OUT may be overridden (the mutation checks point them at a mutated copy).
include $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../../config.mk)

HERE   := $(ROOT)/tests/periph
OUT    := $(BUILD)/periph
PERIPH := $(ROOT)/rtl/periph
RTLSRC := $(addprefix $(PERIPH)/,uart_tx.sv sysctl.sv kbd.sv vid_timing.sv video.sv mmio.sv)
SOCSRC := $(ROOT)/rtl/soc/soc_top.sv
# soc_top is linted with the real core and memory system; tb_soc swaps in scripted stand-ins.
REALSRC := $(ROOT)/rtl/core/rv32_pkg.sv $(filter-out %/rv32_pkg.sv,$(sort $(wildcard $(ROOT)/rtl/core/*.sv))) \
           $(sort $(wildcard $(ROOT)/rtl/mem/*.sv))
BSTUBS := $(HERE)/stubs/behav/rv32_core.sv $(HERE)/stubs/behav/memsys.sv

VFLAGS := -Wall -Wno-fatal --cc --exe --build -j 4 -O2 --no-timing \
          -CFLAGS "$(VERILATOR_CXXFLAGS) -I$(ROOT)/sim/models -I$(HERE)" -LDFLAGS "-lm"
UART_DIV ?= 16
CLK_HZ_T ?= 33333333

BENCHES := tb_mmio0 tb_mmio1 tb_video tb_soc0 tb_soc1
.PHONY: test lint synth clean $(BENCHES)

test: lint
	@mkdir -p $(OUT)/logs; fail=0; \
	for t in $(BENCHES); do \
	  if $(MAKE) --no-print-directory -s $$t > $(OUT)/logs/$$t.log 2>&1; then :; else fail=1; cat $(OUT)/logs/$$t.log; fi; \
	done; \
	echo "---- summary ----"; \
	for t in $(BENCHES); do printf "%-9s " $$t; grep -E "^tb_[a-z]+: (PASS|FAIL)" $(OUT)/logs/$$t.log || echo "no result"; done; \
	if [ $$fail -ne 0 ]; then echo "PERIPH TESTS: FAILED"; exit 1; fi; echo "PERIPH TESTS: ALL PASSED"

lint:
	@mkdir -p $(OUT); for f in 0 1; do \
	  $(VERILATOR) --lint-only -Wall --top-module mmio -GFAST=$$f $(RTLSRC) > $(OUT)/lint.log 2>&1 || { cat $(OUT)/lint.log; exit 1; }; \
	  $(VERILATOR) --lint-only -Wall --top-module soc_top -GFAST=$$f $(RTLSRC) $(SOCSRC) $(REALSRC) > $(OUT)/lint.log 2>&1 || { cat $(OUT)/lint.log; exit 1; }; \
	done; echo "lint: clean (mmio, soc_top with the real core and memsys; FAST=0/1)"

# $(1) bench, $(2) top module, $(3) model prefix (= executable name), $(4) sources, $(5) extra verilator args
define BENCH
$(1):
	@mkdir -p $(OUT)/$(1)
	@$(VERILATOR) $(VFLAGS) --top-module $(2) --prefix $(3) -Mdir $(OUT)/$(1) $(5) $(4) > $(OUT)/$(1).build.log 2>&1 \
	   || { tail -40 $(OUT)/$(1).build.log; exit 1; }
	@$(OUT)/$(1)/$(3)
endef
MMIO_ARGS = -GFAST=$(1) -GUART_DIV=$(UART_DIV) -GCLK_HZ=$(CLK_HZ_T) \
            -CFLAGS "-DTB_FAST=$(1) -DTB_UART_DIV=$(UART_DIV) -DTB_CLK_HZ=$(CLK_HZ_T)"
SOC_ARGS  = -GFAST=$(1) -GUART_DIV=$(UART_DIV) --public-flat-rw -CFLAGS "-DTB_FAST=$(1)"
$(foreach f,0 1,$(eval $(call BENCH,tb_mmio$(f),mmio,Vmmio,$(RTLSRC) $(HERE)/tb_mmio.cpp,$(call MMIO_ARGS,$(f)))))
$(eval $(call BENCH,tb_video,mmio,Vmmio,$(RTLSRC) $(HERE)/tb_video.cpp,-GFAST=0))
$(foreach f,0 1,$(eval $(call BENCH,tb_soc$(f),soc_top,Vsoc,$(RTLSRC) $(SOCSRC) $(BSTUBS) $(HERE)/tb_soc.cpp,$(call SOC_ARGS,$(f)))))

# Yosys synth_gowin of mmio (FAST=0): cell counts
synth:
	@mkdir -p $(OUT)
	@printf 'plugin -i slang\nread_slang --top mmio -GFAST=0 $(RTLSRC)\nsynth_gowin -top mmio\nstat\n' > $(OUT)/synth.ys
	@$(YOSYS) -q -l $(OUT)/yosys.log $(OUT)/synth.ys > /dev/null 2>&1; \
	  awk '/^=== mmio ===/{f=1} f' $(OUT)/yosys.log | grep -E "cells|LUT|DFF|ALU|DPB|SDP|RAM|MUX2" ; echo "(full log: $(OUT)/yosys.log)"

clean:
	rm -rf $(OUT)
