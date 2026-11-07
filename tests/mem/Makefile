# tests/mem - memory-system tests: Verilator benches around memsys, SDRAM chip-model unit test;
# see rtl/mem/README.md.
#
#   make -C tests/mem quick     lint + model test + short randomised runs (< 1 min)
#   make -C tests/mem test      everything (all configurations, many seeds, millions of ops)
#   make -C tests/mem lint      verilator --lint-only -Wall on rtl/mem (several parameter sets)
#   make -C tests/mem synth     Yosys/Gowin synthesis of memsys: block-RAM budget, every memory mapped
#   make -C tests/mem penalty   miss-penalty / refresh measurements (both backends)
#   make -C tests/mem mutate    plant known RTL bugs and check the bench catches them (~7 min)
#   make -C tests/mem build     build all bench binaries
# RTLDIR, OUT: other RTL / build directory.
include ../../config.mk

RTLDIR ?= $(ROOT)/rtl/mem
OUT    ?= $(BUILD)/mem
RTL    := $(wildcard $(RTLDIR)/*.sv)
MODELS := $(ROOT)/sim/models
TB     := $(CURDIR)/tb_memsys.cpp

# configuration: FAST ICACHE_BYTES DCACHE_BYTES SDRAM_ROW_BITS CLK_HZ
CFGS            := sdram_def sdram_tiny sdram_row11 sdram_100m ideal_def ideal_tiny
cfg_sdram_def   := 0 8192 16384 13 50000000
cfg_sdram_tiny  := 0 256  128   13 50000000
cfg_sdram_row11 := 0 8192 16384 11 50000000
cfg_sdram_100m  := 0 1024 2048  13 100000000
cfg_ideal_def   := 1 8192 16384 13 50000000
cfg_ideal_tiny  := 1 128  256   13 50000000
cfgw  = $(word $(2),$(cfg_$(1)))
gen   = -GFAST=$(call cfgw,$(1),1) -GICACHE_BYTES=$(call cfgw,$(1),2) -GDCACHE_BYTES=$(call cfgw,$(1),3) \
        -GSDRAM_ROW_BITS=$(call cfgw,$(1),4) -GCLK_HZ=$(call cfgw,$(1),5)
tbdef = -DTB_FAST=$(call cfgw,$(1),1) -DTB_ICACHE_BYTES=$(call cfgw,$(1),2) -DTB_DCACHE_BYTES=$(call cfgw,$(1),3) \
        -DTB_ROW_BITS=$(call cfgw,$(1),4) -DTB_CLK_HZ=$(call cfgw,$(1),5)

VROOT := $(shell $(VERILATOR) --getenv VERILATOR_ROOT)
BINS  := $(foreach c,$(CFGS),$(OUT)/$(c)/Vmemsys)

.PHONY: all build quick test lint synth penalty mutate mutant-build mutant clean
all: build
build: $(BINS) $(OUT)/sdram_model_test

$(OUT)/%/Vmemsys: $(RTL) $(TB) $(MODELS)/sdram_model.cpp $(MODELS)/sdram_model.h Makefile
	@mkdir -p $(@D)
	@echo "  VERILATE $*"
	@$(VERILATOR) --cc --exe --build -j 0 -O3 --x-assign unique --x-initial unique --top-module memsys -Wno-fatal \
	    -Mdir $(@D) -o Vmemsys $(call gen,$*) -CFLAGS "$(VERILATOR_CXXFLAGS) -I$(MODELS) $(call tbdef,$*) -DTB_CFG='\"$*\"'" \
	    $(RTL) $(TB) $(MODELS)/sdram_model.cpp > $(@D)/build.log 2>&1 || { tail -30 $(@D)/build.log; exit 1; }
	@# our own C++ must be warning-free (Verilator's runtime/generated code is not ours)
	@$(HOST_CXX) -std=gnu++17 -fsyntax-only -Wall -Wextra -Werror -isystem $(VROOT)/include \
	    -isystem $(VROOT)/include/vltstd -isystem $(@D) -I$(MODELS) $(call tbdef,$*) -DTB_CFG='"$*"' $(TB) $(MODELS)/sdram_model.cpp

$(OUT)/sdram_model_test: sdram_model_test.cpp $(MODELS)/sdram_model.cpp $(MODELS)/sdram_model.h Makefile
	@mkdir -p $(OUT)
	@echo "  CXX      sdram_model_test"
	@$(HOST_CXX) $(HOST_CXXFLAGS) -Wextra -I$(MODELS) sdram_model_test.cpp $(MODELS)/sdram_model.cpp -o $@

lint:
	@for g in "-GFAST=0" "-GFAST=1" "-GSDRAM_ROW_BITS=11" "-GICACHE_BYTES=128 -GDCACHE_BYTES=128" \
	          "-GFAST=1 -GICACHE_BYTES=256 -GDCACHE_BYTES=256 -GSDRAM_ROW_BITS=11"; do \
	    $(VERILATOR) --lint-only -Wall $$g --top-module memsys $(RTL) > /dev/null || exit 1; \
	done; echo "lint: verilator --lint-only -Wall clean (5 parameter sets)"

quick test: build
	@./run.sh $(if $(filter quick,$@),quick,full) $(OUT)

penalty: build
	@$(OUT)/sdram_def/Vmemsys --penalty
	@$(OUT)/ideal_def/Vmemsys --penalty

# Gowin GW2AR-18 synthesis of the board configuration (FAST=0, 8 MB SDRAM,
# default cache sizes). memsys is an internal block, so no IO pads.
# Fails if the block-RAM budget (13) is exceeded or a memory was not mapped.
BRAM_BUDGET := 13
synth:
	@mkdir -p $(OUT)/synth
	@$(YOSYS) -q -l $(OUT)/synth/yosys.log -p "plugin -i slang; \
	    read_slang -DSYNTHESIS --top memsys -G FAST=0 -G SDRAM_ROW_BITS=11 $(RTL); \
	    synth_gowin -top memsys -noiopads -json $(OUT)/synth/memsys.json; \
	    tee -o $(OUT)/synth/stat.txt stat" > /dev/null 2> $(OUT)/synth/stderr.txt \
	    || { cat $(OUT)/synth/stderr.txt; exit 1; }
	@# the only warnings are Yosys' own Gowin BRAM map file (narrow port widths)
	@! grep -v "share/yosys/gowin/brams_map.v" $(OUT)/synth/stderr.txt | grep -i warning
	@sed -n '/=== memsys ===/,$$p' $(OUT)/synth/stat.txt | grep -E '^ +[0-9]+ +[A-Z]'
	@grep -E "mapping memory" $(OUT)/synth/yosys.log | sed 's/^/  /'
	@awk -v budget=$(BRAM_BUDGET) ' \
	    $$2 ~ /^(DPB|DPX9B|SDPB|SDPX9B|SP|SPX9|pROM|pROMX9)$$/ { bram += $$1 } \
	    $$2 ~ /^LUT[1-4]$$/ { lut += $$1 } $$2 ~ /^ALU$$/ { alu += $$1 } \
	    $$2 ~ /^DFF/ { ff += $$1 } $$2 ~ /^RAM16/ { lram += $$1 } $$2 ~ /^\$$mem/ { unmapped += $$1 } \
	    END { printf "BRAM %d (budget %d), LUT %d + ALU %d, FF %d, RAM16SDP4 %d, unmapped memories %d: %s\n", \
	          bram, budget, lut, alu, ff, lram, unmapped, (bram <= budget && unmapped == 0) ? "PASS" : "FAIL"; \
	          exit !(bram <= budget && unmapped == 0) }' $(OUT)/synth/stat.txt

# ../core/mutate.py copies the RTL, plants one bug of mutants.txt and calls `mutant-build`
# and `mutant` here with RTLDIR=<mutated copy> OUT=<its build dir> CFG=<configuration>.
mutate:
	@python3 ../core/mutate.py $(CURDIR)/mutants.txt $(RTLDIR) $(OUT)/mutate s=sdram_def,f=ideal_tiny -j 2 -- \
	    $(MAKE) -s -C $(CURDIR)
mutant-build: $(OUT)/$(CFG)/Vmemsys
mutant: mutant-build
	@$(OUT)/$(CFG)/Vmemsys --seeds 1-2 --ops 60000

clean:
	rm -rf $(OUT)
