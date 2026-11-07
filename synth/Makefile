# Tang Nano 20K (GW2AR-LV18QN88C8/I7) implementation flow:
# Yosys (slang frontend, synth_gowin) -> nextpnr-himbaechel -> report. Targets, configurations,
# variables (SEEDS, AUX_SEEDS, OUT, RTL) and the reasons for the flow: synth/README.md.
#   make -C synth -j3            everything (about 45 min; 25 with -j6 on 10 cores)
#   make -C synth synth | pnr-<cfg> | compare | probes | tb | bitstream | report | clean
include ../config.mk

OUT       := $(BUILD)/synth
PYTHON    ?= python3
SEEDS     ?= 1 2 3 4 5
AUX_SEEDS ?= 1 2 3
DEVICE    := GW2AR-LV18QN88C8/I7
FAMILY    := GW2A-18C
FREQ      := 50.4
# I/O registers into the IOBs (SDRAM pins). This build needs oreg_in_iob for the
# output registers; ioreg_in_iob alone only moves the input registers.
IOREG     := --vopt ireg_in_iob --vopt oreg_in_iob --vopt ioreg_in_iob
SDC       := $(ROOT)/synth/tangnano20k.sdc
CST       := $(ROOT)/rtl/fpga/tangnano20k.cst
CORE_CST  := $(ROOT)/synth/wrappers/core_tcm_top.cst
SCRIPTS   := $(ROOT)/synth/scripts
BIT_CFG   := main

CONFIGS         := main nodvi core pll1 prior
COMPARE_CONFIGS := syncbht
ALL_CONFIGS     := $(CONFIGS) $(COMPARE_CONFIGS)

prior_PATCHES   := video_bsram,btb_rwcheck,bht_sync,bht_bsram
syncbht_PATCHES := bht_sync

main_SEEDS    := $(SEEDS)
nodvi_SEEDS   := $(AUX_SEEDS)
core_SEEDS    := $(AUX_SEEDS)
pll1_SEEDS    := 1
prior_SEEDS   := 1
syncbht_SEEDS := $(AUX_SEEDS)

prior_VIEWS   := pnr

nodvi_GEN := -G DVI=0
pll1_GEN  := -G PLL_CASCADE=0

# ---------------------------------------------------------------- sources
RTL      := $(ROOT)/rtl
CORE_F   := rv32_pkg.sv $(filter-out rv32_pkg.sv,$(sort $(notdir $(wildcard $(RTL)/core/*.sv))))
MEM_F    := $(filter-out mem_ideal.sv,$(sort $(notdir $(wildcard $(RTL)/mem/*.sv))))
PERIPH_F := $(sort $(notdir $(wildcard $(RTL)/periph/*.sv)))
FPGA_SRC := $(ROOT)/synth/gowin_prims.sv $(sort $(wildcard $(RTL)/fpga/*.sv))
SHARED   := $(wildcard $(RTL)/core/*.sv $(RTL)/mem/*.sv $(RTL)/periph/*.sv $(RTL)/soc/*.sv)

patches_of = $(or $($(1)_PATCHES),none)
views_of   = $(or $($(1)_VIEWS),pnr swap)
rtlroot  = $(if $(filter none,$(call patches_of,$(1))),$(RTL),$(OUT)/$(1)/rtl)
top_of   = $(if $(filter core,$(1)),core_tcm_top,tangnano20k_top)
src_of   = $(if $(filter core,$(1)),\
             $(addprefix $(call rtlroot,$(1))/core/,$(CORE_F)) $(ROOT)/synth/wrappers/core_tcm_top.sv,\
             $(addprefix $(call rtlroot,$(1))/core/,$(CORE_F)) $(addprefix $(call rtlroot,$(1))/mem/,$(MEM_F)) \
             $(addprefix $(call rtlroot,$(1))/periph/,$(PERIPH_F)) $(call rtlroot,$(1))/soc/soc_top.sv $(FPGA_SRC))
cst_of   = $(if $(filter core,$(1)),$(CORE_CST),$(CST))
sdc_of   = $(if $(filter core,$(1)),,--sdc $(SDC))
slang_of = plugin -i slang; read_slang -DSYNTHESIS $($(1)_GEN) --top $(call top_of,$(1)) $(strip $(call src_of,$(1)))

.PHONY: all synth pnr compare probes tb lint bitstream report clean $(addprefix pnr-,$(ALL_CONFIGS))
all: tb probes pnr bitstream
	@$(MAKE) --no-print-directory report

# ---------------------------------------------------------------- synthesis
define SYNTH_rule
$(OUT)/$(1)/synth.json: $(SHARED) $(FPGA_SRC) $(SCRIPTS)/variant.py $(SCRIPTS)/bsram_swap.py \
                        $(ROOT)/synth/wrappers/core_tcm_top.sv Makefile
	@mkdir -p $(OUT)/$(1)
	@echo "[synth] $(1)"
	@$(if $(filter none,$(call patches_of,$(1))),true,$(PYTHON) $(SCRIPTS)/variant.py $(OUT)/$(1) $(call patches_of,$(1)))
	@printf 'patches=%s\ntop=%s\nparams=%s\nseeds=%s\nviews=%s\n' '$(call patches_of,$(1))' '$(call top_of,$(1))' \
	    '$($(1)_GEN)' '$($(1)_SEEDS)' '$(call views_of,$(1))' > $(OUT)/$(1)/config.txt
	@$(YOSYS) -q -l $(OUT)/$(1)/yosys.log -p "$(call slang_of,$(1)); \
	    synth_gowin -family gw2a -top $(call top_of,$(1)) -json $(OUT)/$(1)/synth.json; \
	    tee -q -o $(OUT)/$(1)/stat.txt stat" > /dev/null 2> $(OUT)/$(1)/yosys.stderr \
	    || { cat $(OUT)/$(1)/yosys.stderr; exit 1; }
	@$(YOSYS) -q -l $(OUT)/$(1)/libmap.log -p "$(call slang_of,$(1)); \
	    synth_gowin -family gw2a -top $(call top_of,$(1)) -run begin:map_ram; \
	    debug memory_libmap -lib +/gowin/lutrams.txt -lib +/gowin/brams.txt" > /dev/null 2>&1
	@$(PYTHON) $(SCRIPTS)/bsram_swap.py $(OUT)/$(1)/synth.json $(OUT)/$(1)/swap.json > /dev/null
$(if $(filter $(1),$(CONFIGS)),synth: $(OUT)/$(1)/synth.json)
endef
$(foreach c,$(ALL_CONFIGS),$(eval $(call SYNTH_rule,$(c))))

# ---------------------------------------------------------------- place and route
# $(1) config, $(2) view (pnr | swap), $(3) seed. A failing run (e.g. `prior`,
# which does not fit) is recorded (rc file + log), not fatal.
define PNR_rule
$(OUT)/$(1)/$(2)_s$(3)/done: $(OUT)/$(1)/synth.json $(call cst_of,$(1)) $(SDC) $(SCRIPTS)/postroute_dsp.py
	@mkdir -p $$(@D)
	@echo "[pnr] $(1) $(2) seed $(3)"
	@cd $$(@D) && DSP_ROUTES_JSON=$$(@D)/dsp_routes.json $(NEXTPNR) \
	    --json $(OUT)/$(1)/$(if $(filter swap,$(2)),swap,synth).json \
	    --device $(DEVICE) --vopt family=$(FAMILY) --vopt cst=$(call cst_of,$(1)) $(IOREG) \
	    $(call sdc_of,$(1)) --freq $(FREQ) --timing-allow-fail --seed $(3) \
	    --report $$(@D)/report.json --detailed-timing-report \
	    --post-route $(SCRIPTS)/postroute_dsp.py \
	    $(if $(and $(filter $(BIT_CFG),$(1)),$(filter pnr,$(2)),$(filter $(firstword $(SEEDS)),$(3))),--write $$(@D)/routed.json) \
	    -l $$(@D)/pnr.log > /dev/null 2>&1; echo $$$$? > $$(@D)/rc
	@touch $$@
pnr-$(1): $(OUT)/$(1)/$(2)_s$(3)/done
$(if $(filter $(1),$(CONFIGS)),pnr,compare): $(OUT)/$(1)/$(2)_s$(3)/done
endef
$(foreach c,$(ALL_CONFIGS),$(foreach v,$(call views_of,$(c)),$(foreach s,$($(c)_SEEDS),$(eval $(call PNR_rule,$(c),$(v),$(s))))))

# ---------------------------------------------------------------- bitstream
BIT_RUN := $(OUT)/$(BIT_CFG)/pnr_s$(firstword $(SEEDS))
bitstream: $(OUT)/tangnano20k.fs
$(OUT)/tangnano20k.fs: $(BIT_RUN)/done
	@echo "[pack] $(BIT_CFG) seed $(firstword $(SEEDS)) -> $@"
	@$(GOWIN_PACK) -d $(FAMILY) -o $@ $(BIT_RUN)/routed.json > $(OUT)/gowin_pack.log 2>&1

# ---------------------------------------------------------------- probes
probes: $(OUT)/probes/probes.json
$(OUT)/probes/probes.json: $(ROOT)/synth/probes/timing_probe.sv $(SCRIPTS)/probes.py
	@echo "[probes] BSRAM / DSP timing coverage"
	@mkdir -p $(OUT)/probes
	@$(PYTHON) $(SCRIPTS)/probes.py $(OUT)/probes $(YOSYS) $(NEXTPNR) | tee $(OUT)/probes/probes.txt

# ---------------------------------------------------------------- tests of the FPGA-only RTL
TB      := $(OUT)/tb
TB_FLAGS = --cc --exe --build -Wall -O3 -CFLAGS "$(VERILATOR_CXXFLAGS) -I$(ROOT)/synth/tb"
LINT_RTL := $(RTL)/fpga/tn20k_tmds_enc.sv $(RTL)/fpga/tn20k_vid_cdc.sv $(RTL)/fpga/tn20k_dvi_enc.sv \
            $(RTL)/fpga/tn20k_reset.sv

lint:
	@mkdir -p $(TB)
	@for m in tn20k_tmds_enc tn20k_vid_cdc tn20k_dvi_enc tn20k_reset; do \
	    $(VERILATOR) --lint-only -Wall --top-module $$m $(LINT_RTL) || exit 1; done
	@echo "[lint] rtl/fpga (pure-RTL modules): clean"
	@# board top with the whole SoC; open output pins are intentional (PINCONNECTEMPTY), and
	@# -DSYNTHESIS leaves rv32_core's simulation-only COVERAGE parameter unused (UNUSEDPARAM)
	@$(VERILATOR) --lint-only -Wall -Wno-PINCONNECTEMPTY -Wno-UNUSEDPARAM -DSYNTHESIS --top-module tangnano20k_top \
	    $(call src_of,main) > $(TB)/lint_top.log 2>&1 || { cat $(TB)/lint_top.log; exit 1; }
	@echo "[lint] tangnano20k_top + soc_top RTL (-Wall): clean"

$(TB)/obj_tmds/tb_tmds_enc: $(RTL)/fpga/tn20k_tmds_enc.sv tb/tb_tmds_enc.cpp tb/tmds_ref.h
	@mkdir -p $(TB)
	@$(VERILATOR) $(TB_FLAGS) --top-module tn20k_tmds_enc --Mdir $(TB)/obj_tmds -o tb_tmds_enc \
	    $(RTL)/fpga/tn20k_tmds_enc.sv $(abspath tb/tb_tmds_enc.cpp) > $(TB)/build_tmds.log 2>&1
$(TB)/obj_dvi/tb_dvi_enc: $(LINT_RTL) tb/tb_dvi_enc.cpp tb/tmds_ref.h
	@mkdir -p $(TB)
	@$(VERILATOR) $(TB_FLAGS) --top-module tn20k_dvi_enc --Mdir $(TB)/obj_dvi -o tb_dvi_enc \
	    $(RTL)/fpga/tn20k_dvi_enc.sv $(RTL)/fpga/tn20k_vid_cdc.sv $(RTL)/fpga/tn20k_tmds_enc.sv \
	    $(abspath tb/tb_dvi_enc.cpp) > $(TB)/build_dvi.log 2>&1

tb: lint $(TB)/obj_tmds/tb_tmds_enc $(TB)/obj_dvi/tb_dvi_enc
	@$(TB)/obj_tmds/tb_tmds_enc 3000000 1 | tee $(TB)/tb.log
	@$(TB)/obj_tmds/tb_tmds_enc 3000000 2 | tail -1 | tee -a $(TB)/tb.log
	@$(TB)/obj_dvi/tb_dvi_enc | tee -a $(TB)/tb.log
	@! grep -q FAIL $(TB)/tb.log

# ---------------------------------------------------------------- report
report:
	@$(PYTHON) $(SCRIPTS)/report.py $(OUT) $(CONFIGS) $(foreach c,$(COMPARE_CONFIGS),$(if $(wildcard $(OUT)/$(c)/synth.json),$(c)))
	@echo "[report] $(OUT)/report.md"

clean:
	rm -rf $(addprefix $(OUT)/,$(ALL_CONFIGS)) $(OUT)/probes $(OUT)/tb $(OUT)/report.md $(OUT)/tangnano20k.fs \
	    $(OUT)/gowin_pack.log
