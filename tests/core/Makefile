# tests/core - verification of rtl/core (rv32_core) on its own; see rtl/core/README.md.
#
#   make -C tests/core            build the testbench (build/core/obj/tb_core)
#   make -C tests/core quick      < 1 minute: lint + riscv-tests (2 modes) + short random programs
#   make -C tests/core test       everything: lint, riscv-tests in all latency modes x seeds,
#                                 random programs (short and long) in all modes, benchmarks
#   make -C tests/core lint       verilator --lint-only -Wall on rtl/core
#   make -C tests/core bench      pipeline measurements with zero-latency memory
#   make -C tests/core mutate     mutation testing: injected RTL bugs must be caught (~5 min)
#
# Knobs: JOBS (parallel runs), SEEDS, RANDOM_COUNT, LONG_COUNT; RTLDIR, OUT (other RTL / build dir).
include ../../config.mk

RTLDIR  ?= $(ROOT)/rtl/core
OUT     ?= $(BUILD)/core
RTL     := $(addprefix $(RTLDIR)/,rv32_pkg.sv rv32_decode.sv rv32_alu.sv rv32_regfile.sv \
                                  rv32_muldiv.sv rv32_csr.sv rv32_bpred.sv rv32_core.sv)
TBDIR   := $(CURDIR)/tb
TBSRC   := $(TBDIR)/tb_main.cpp $(TBDIR)/rand_prog.cpp
ISS     := $(ROOT)/sim/iss/iss.cpp $(ROOT)/sim/common/loader.cpp      # the lockstep reference
ISS_INC := $(ROOT)/sim/iss $(ROOT)/sim/common $(ROOT)/sw/include
DEPS    := $(RTL) $(TBSRC) $(TBDIR)/rand_prog.h $(ISS) $(ROOT)/sim/iss/iss.h $(ROOT)/sim/common/loader.h Makefile
SIM     := $(OUT)/obj/tb_core
ISA_DIR := $(BUILD)/isa
BENCH_DIR  := $(CURDIR)/bench
BENCH_OUT  := $(OUT)/bench
BENCH_ELFS := $(patsubst $(BENCH_DIR)/%,$(BENCH_OUT)/%.elf,$(basename $(filter-out %/crt.S,$(wildcard $(BENCH_DIR)/*.S $(BENCH_DIR)/*.c))))

JOBS         ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
SEEDS        ?= 4
RANDOM_COUNT ?= 1000
LONG_COUNT   ?= 80
VL_JOBS      ?= 0
VL_WARN      ?= -Wall

TB := $(SIM) -j $(JOBS)

.PHONY: all sim lint quick test isa-check bench mutate mutant-build mutant clean
all: sim
sim: $(SIM)

# The same testbench is built twice: with the default predictor parameters and
# with tiny ones (heavy BTB/BHT aliasing, a 3-entry RAS that wraps around), to
# show that predictor size/state never changes architectural results.
SMALL_PARAMS := -GBTB_ENTRIES=16 -GBHT_ENTRIES=32 -GRAS_DEPTH=3
SIM_SMALL    := $(OUT)/obj_small/tb_core
VL_ROOT := $(shell $(VERILATOR) --getenv VERILATOR_ROOT 2>/dev/null)
# $(1) object dir, $(2) extra Verilator options
VL_BUILD = mkdir -p $(1) && echo "verilator: building $(1)/tb_core $(2)" && \
	$(VERILATOR) --cc --exe --build -j $(VL_JOBS) --trace $(VL_WARN) --top-module rv32_core \
	    --x-assign unique --x-initial unique -Mdir $(1) -o tb_core $(2) \
	    -CFLAGS "$(VERILATOR_CXXFLAGS) $(addprefix -I,$(TBDIR) $(ISS_INC))" -LDFLAGS "-pthread" \
	    $(RTL) $(TBSRC) $(ISS) > $(1)/build.log 2>&1 || { tail -40 $(1)/build.log; exit 1; }

$(SIM): $(DEPS)
	@$(call VL_BUILD,$(@D),)
	@# our own C++ is held to -Wall -Wextra -Werror (Verilator's code and the ISS are not ours here)
	@$(HOST_CXX) -std=gnu++17 -Wall -Wextra -Werror -fsyntax-only -I$(TBDIR) $(addprefix -isystem ,$(ISS_INC)) \
	    -isystem $(VL_ROOT)/include -isystem $(VL_ROOT)/include/vltstd -isystem $(@D) $(TBSRC) \
	    && echo "testbench sources: -Wall -Wextra clean"

$(SIM_SMALL): $(DEPS)
	@$(call VL_BUILD,$(@D),$(SMALL_PARAMS))

lint:
	$(VERILATOR) --lint-only -Wall --top-module rv32_core $(RTL)
	@echo "PASS   lint: verilator --lint-only -Wall rtl/core clean"

isa-check:
	@test -n "$$(ls $(ISA_DIR)/*.elf 2>/dev/null)" || $(MAKE) -C ../isa

quick: $(SIM) isa-check lint
	@status=0; \
	$(TB) --suite isa --elf-dir $(ISA_DIR) --modes fast,both --seeds 1 || status=1; \
	$(TB) --suite random --count 40 --items 250 --modes all --require-coverage --label "random: 40 short programs" || status=1; \
	if [ $$status -ne 0 ]; then echo "QUICK: FAILED"; else echo "QUICK: all passed"; fi; exit $$status

test: $(SIM) $(SIM_SMALL) isa-check lint $(BENCH_ELFS)
	@status=0; \
	$(TB) --suite isa --elf-dir $(ISA_DIR) --modes all --seeds $(SEEDS) || status=1; \
	$(TB) --suite random --count $(RANDOM_COUNT) --items 400 --modes all --require-coverage \
	      --label "random: $(RANDOM_COUNT) programs x 400 items" || status=1; \
	$(TB) --suite random --count $(LONG_COUNT) --items 4000 --seed-base 100000 --modes all \
	      --label "random: $(LONG_COUNT) long programs x 4000 items" || status=1; \
	$(TB) --suite isa --elf-dir $(BENCH_OUT) --modes all --seeds 2 --label "benchmarks (lockstep)" || status=1; \
	$(SIM_SMALL) -j $(JOBS) --suite isa --elf-dir $(ISA_DIR) --modes all --seeds 1 \
	      --label "isa, small predictor $(SMALL_PARAMS)" || status=1; \
	$(SIM_SMALL) -j $(JOBS) --suite random --count 100 --items 400 --seed-base 50000 --modes all \
	      --label "random, small predictor" || status=1; \
	if [ $$status -ne 0 ]; then echo "TEST: FAILED"; else echo "TEST: all passed"; fi; exit $$status

# ---------------------------------------------------------------- benchmarks
$(BENCH_OUT)/%.elf: $(BENCH_DIR)/%.S $(BENCH_DIR)/bench.h $(BENCH_DIR)/link.ld
	@mkdir -p $(@D)
	$(RV_CC) $(RV_ARCH) -nostdlib -nostartfiles -static -T$(BENCH_DIR)/link.ld -I$(BENCH_DIR) $< -o $@
$(BENCH_OUT)/%.elf: $(BENCH_DIR)/%.c $(BENCH_DIR)/crt.S $(BENCH_DIR)/bench.h $(BENCH_DIR)/link.ld
	@mkdir -p $(@D)
	$(RV_CC) $(RV_ARCH) -O2 -ffreestanding -fno-builtin -nostdlib -nostartfiles -static -T$(BENCH_DIR)/link.ld \
	    -I$(BENCH_DIR) $(BENCH_DIR)/crt.S $< -lgcc -o $@

bench: $(SIM) $(BENCH_ELFS)
	@for e in $(BENCH_ELFS); do $(SIM) --elf $$e --mode fast --bench || exit 1; done

# ---------------------------------------------------------------- mutation testing
# mutate.py copies the RTL, plants one bug of mutants.txt and calls `mutant-build`
# and `mutant` here with RTLDIR=<mutated copy> OUT=<its build dir>.
MUT_JOBS ?= 5
mutate: isa-check
	@python3 $(CURDIR)/mutate.py $(CURDIR)/mutants.txt $(RTLDIR) $(OUT)/mut d=default -j $(MUT_JOBS) -- \
	    $(MAKE) -s -C $(CURDIR) JOBS=2 VL_JOBS=2 VL_WARN=-Wno-fatal
mutant-build: $(SIM)
mutant: mutant-build
	@$(TB) --suite isa --elf-dir $(ISA_DIR) --modes fast,both --seeds 1 && \
	 $(TB) --suite random --count 40 --items 300 --modes all

clean:
	rm -rf $(OUT)
