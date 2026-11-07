# Virtual board. Products under build/vboard/ (or OUT=...). See README.md ("Build", "Speed").
include ../../config.mk

OUT     ?= $(BUILD)/vboard
RTL_DIR ?= $(ROOT)/rtl
COMMON  := $(ROOT)/sim/common
INCS    := -I. -I$(ROOT)/sim/iss -I$(ROOT)/sim/model -I$(COMMON) -I$(ROOT)/sim/models -I$(ROOT)/sw/include $(SDL2_CFLAGS)
CXXFLAGS := -std=gnu++17 -O2 -Wall -Wextra $(INCS)
RTL_SRCS := $(RTL_DIR)/core/rv32_pkg.sv $(filter-out %/rv32_pkg.sv,$(wildcard $(RTL_DIR)/core/*.sv $(RTL_DIR)/mem/*.sv $(RTL_DIR)/periph/*.sv)) $(RTL_DIR)/soc/soc_top.sv
HDRS    := $(wildcard *.h $(COMMON)/*.h $(ROOT)/sim/iss/*.h $(ROOT)/sim/model/*.h $(ROOT)/sim/models/*.h)

# -fno-dfg -fno-split: without Verilator's data-flow optimiser and always-block splitting this design's code
# is about 20% shorter. Thin LTO lets the scheduling code and the device models inline into the cycle loop (the ISS
# stays out of it: with LTO its run loop executes about 6% more host instructions).
VL_OPT    ?= -O3 --x-assign fast --x-initial fast --noassert -fno-dfg -fno-split
VL_CXXOPT ?= -O3 -flto=thin
VL_LDOPT  ?= -O3 -flto=thin
VL_GEXTRA ?=   # extra Verilator arguments for variant builds, e.g. VL_GEXTRA="-GICACHE_BYTES=4096" OUT=build/sweep/x
# PGO: $(OUT)/pgo.profdata is trained on Doom by an instrumented copy of this build ($(OUT)/pgo-gen) and redone
# when that copy changes; skipped without PGO_ELF or the WAD. PGO=0: off; PGO_DATA=FILE: a given profile.
PGO        ?= 1
PGO_ELF    ?= $(BUILD)/doom/soc/doom.elf
PGO_CYCLES ?= 60000000
PGO_DATA   ?= $(OUT)/pgo.profdata
ifeq ($(PGO)|$(PGO_DATA),1|$(OUT)/pgo.profdata)
PGO_DEP := $(if $(and $(wildcard $(PGO_ELF)),$(wildcard $(DOOM_WAD))),$(PGO_DATA))
endif
ifeq ($(PGO),1)
VL_PGO ?= $(if $(PGO_DEP)$(wildcard $(PGO_DATA)),-fprofile-instr-use=$(PGO_DATA) -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled)
endif
VL_FAST := $(VL_CXXOPT) $(VL_PGO)
VL_ROOT := $(shell $(VERILATOR) --getenv VERILATOR_ROOT)
VL_DEFS := -DVERILATOR=1 -DVM_COVERAGE=0 -DVM_SC=0 -DVM_TIMING=0 -DVM_TRACE=0 -DVM_TRACE_FST=0 -DVM_TRACE_VCD=0 -DVM_TRACE_SAIF=0 -DVM_VPI=0 \
    -faligned-new -isystem $(VL_ROOT)/include -isystem $(VL_ROOT)/include/vltstd $(VB_DEFS)

BOARD_OBJS := $(addprefix $(OUT)/,engine_iss.o engine_fast.o engine_acc.o engine_model.o gfx.o hud.o keymap.o cosim.o)
ISS_OBJS   := $(OUT)/iss.o $(OUT)/model.o $(OUT)/loader.o $(OUT)/sdram_model.o
VL_LIBS    := $(OUT)/vl_fast/libVsoc_fast.a $(OUT)/vl_acc/libVsoc_acc.a $(OUT)/vl_fast/libverilated.a

.PHONY: all test test-long pgo clean FORCE
all: $(OUT)/vboard
FORCE:
$(OUT):
	mkdir -p $@
$(OUT)/vl_flags: FORCE | $(OUT)   # rebuild the models when the Verilator / C++ flags change
	@echo "$(VL_OPT) | $(VL_FAST) | $(VL_DEFS) | $(VL_GEXTRA)" > $@.tmp; cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

# one verilated model and its engine: $(1) = suffix, $(2) = prefix, $(3) = FAST, $(4) = UART_DIV, $(5) = engine name
define VERILATE
$(OUT)/vl_$(1)/lib$(2).a: $(RTL_SRCS) $(MAKEFILE_LIST) vl_check.py $(OUT)/vl_flags $(PGO_DEP) | $(OUT)
	rm -rf $(OUT)/vl_$(1)
	$(VERILATOR) --cc $(VL_OPT) -Wno-fatal -GFAST=$(3) -GUART_DIV=$(4) $(VL_GEXTRA) --top-module soc_top --prefix $(2) \
	    -Mdir $(OUT)/vl_$(1) -CFLAGS -std=gnu++17 $(RTL_SRCS) > $(OUT)/vl_$(1).log 2>&1 || { cat $(OUT)/vl_$(1).log; exit 1; }
	python3 vl_check.py $(OUT)/vl_$(1)/$(2)___024root__0.cpp $(OUT)/vl_$(1)/vb_model_check.h
	$(MAKE) -C $(OUT)/vl_$(1) -f $(2).mk -j8 OPT_FAST="$(VL_FAST)" OPT_SLOW="-O1" OPT_GLOBAL="$(VL_FAST)" > $(OUT)/vl_$(1).build.log 2>&1 || { tail -30 $(OUT)/vl_$(1).build.log; exit 1; }
$(OUT)/engine_$(1).o: engine_rtl.cpp $(HDRS) $(OUT)/vl_$(1)/lib$(2).a
	$(HOST_CXX) $(CXXFLAGS) $(VL_FAST) $(VL_DEFS) -isystem $(OUT)/vl_$(1) -DVB_MODEL=$(2) -DVB_ENGINE=$(5) -DVB_ACC=$(if $(filter 1,$(3)),0,1) \
	    -DVB_UART_DIV=$(4) -include $(2).h -include $(2)___024root.h -c $$< -o $$@
endef
$(eval $(call VERILATE,fast,Vsoc_fast,1,4,fast))
$(eval $(call VERILATE,acc,Vsoc_acc,0,434,accurate))
$(OUT)/vl_fast/libverilated.a: $(OUT)/vl_fast/libVsoc_fast.a

$(OUT)/iss.o: $(ROOT)/sim/iss/iss.cpp $(HDRS) | $(OUT)
	$(HOST_CXX) $(CXXFLAGS) -O3 -c $< -o $@
$(OUT)/model.o: $(ROOT)/sim/model/model.cpp $(HDRS) $(OUT)/vl_flags $(PGO_DEP) | $(OUT)
	$(HOST_CXX) $(CXXFLAGS) $(VL_FAST) -c $< -o $@
$(OUT)/sdram_model.o: $(ROOT)/sim/models/sdram_model.cpp $(HDRS) $(OUT)/vl_flags $(PGO_DEP) | $(OUT)
	$(HOST_CXX) $(CXXFLAGS) $(VL_FAST) -c $< -o $@
$(OUT)/%.o: $(COMMON)/%.cpp $(HDRS) | $(OUT)
	$(HOST_CXX) $(CXXFLAGS) -c $< -o $@
$(OUT)/%.o: %.cpp $(HDRS) | $(OUT)
	$(HOST_CXX) $(CXXFLAGS) -c $< -o $@

# PGO: the instrumented build, a training run on each RTL engine and the model, the merged profile
$(OUT)/pgo-gen/vboard: FORCE
	@$(MAKE) --no-print-directory OUT=$(OUT)/pgo-gen PGO=0 VL_PGO=-fprofile-instr-generate VL_LDFLAGS="$(VL_LDFLAGS) -fprofile-instr-generate" $@
$(OUT)/pgo.profdata: $(OUT)/pgo-gen/vboard
	for e in fast accurate model; do LLVM_PROFILE_FILE=$(OUT)/pgo-gen/$$e.profraw $< --engine $$e --headless --wad $(DOOM_WAD) \
	    --max-cycles $(PGO_CYCLES) --quiet-stats $(PGO_ELF) -- -timedemo demo3 > /dev/null 2>&1 & done; wait
	xcrun llvm-profdata merge -o $@ $(OUT)/pgo-gen/fast.profraw $(OUT)/pgo-gen/accurate.profraw $(OUT)/pgo-gen/model.profraw
pgo:
	@test -f $(PGO_ELF) -a -f $(DOOM_WAD) || { echo "make pgo: needs $(PGO_ELF) and $(DOOM_WAD)"; exit 1; }
	rm -f $(OUT)/pgo.profdata
	$(MAKE) all

$(OUT)/vboard: $(OUT)/main.o $(BOARD_OBJS) $(ISS_OBJS) $(VL_LIBS)
	$(HOST_CXX) $(CXXFLAGS) $^ $(SDL2_LIBS) $(VL_LDOPT) $(VL_LDFLAGS) -o $@

include tests/tests.mk

clean:
	rm -rf $(OUT)
