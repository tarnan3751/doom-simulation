# Doom on a self-designed RISC-V CPU, in simulation.
#
# Everything is driven from here; each directory also builds on its own.
# `make help` lists the targets. Products land under build/.
include config.mk

VBOARD := $(BUILD)/vboard/vboard
WAD    := $(DOOM_WAD)
DEMO   ?= demo3
ENGINE ?= accurate

.DEFAULT_GOAL := help
.PHONY: help
help:
	@echo "Setup"
	@echo "  make setup          install tools (Homebrew + project-local OSS CAD Suite) and fetch third-party sources"
	@echo "Build"
	@echo "  make all            build everything: riscv-tests, ISS, software, Doom (native + RISC-V), tools, virtual board"
	@echo "Verify"
	@echo "  make test           every regression: core, memory system, peripherals, ISS, libc, virtual board (minutes)"
	@echo "  make test-long      + 400 frames of Doom per engine in lockstep, full CoreMark"
	@echo "Run (ENGINE=iss|fast|accurate|model, default accurate)"
	@echo "  make isa            the official riscv-tests on the RTL (both engines, lockstep against the ISS)"
	@echo "  make hello          serial hello world"
	@echo "  make coremark       CoreMark"
	@echo "  make testpattern    video test pattern in the board window"
	@echo "  make doom           play Doom in the board window        (ENGINE=model: real time, the accurate machine's exact timing)"
	@echo "  make timedemo       DEMO=demo1|demo2|demo3: run the demo headless, dump every frame,"
	@echo "                      compare pixel for pixel with native doomgeneric   (results in build/results/)"
	@echo "  make reference      build native doomgeneric and produce the three reference frame dumps"
	@echo "FPGA study"
	@echo "  make synth          Yosys + nextpnr for the Tang Nano 20K: utilisation and Fmax (build/synth/report.md)"

.PHONY: setup
setup:
	scripts/setup-tools.sh
	scripts/fetch-third-party.sh

# ------------------------------------------------------------------ build
.PHONY: all isa-build iss sw doom-build tools vboard reference
all: isa-build iss sw doom-build tools vboard

isa-build:
	$(MAKE) -C tests/isa
iss:
	$(MAKE) -C sim/iss
sw:
	$(MAKE) -C sw all
tools:
	$(MAKE) -C tools
doom-build: sw tools
	$(MAKE) -C sw/doom native soc
reference: tools
	$(MAKE) -C sw/doom native ref
vboard: iss doom-build      # after Doom: the board build trains its PGO profile on it
	$(MAKE) -C sim/vboard

# ------------------------------------------------------------------ verify
.PHONY: test test-long
test: all
	$(MAKE) -C tests/core test
	$(MAKE) -C tests/mem test
	$(MAKE) -C tests/periph test
	$(MAKE) -C sim/iss test
	$(MAKE) -C sw test
	$(MAKE) -C sw run-iss
	$(MAKE) -C sim/vboard test
	@echo "ALL REGRESSIONS PASSED"

test-long: test
	$(MAKE) -C sim/vboard test-long

# ------------------------------------------------------------------ run
.PHONY: isa hello coremark testpattern doom timedemo
isa: all
	@fail=0; for e in $(BUILD)/isa/*.elf; do \
	    for eng in fast accurate; do \
	        $(VBOARD) --engine $$eng --headless --isa-test --cosim --quiet-stats $$e >/dev/null 2>&1 \
	            && r=pass || { r=FAIL; fail=1; }; \
	        printf "%-28s %-9s %s\n" $$(basename $$e .elf) $$eng $$r; \
	    done; \
	done; [ $$fail -eq 0 ] && echo "riscv-tests: all passed on both engines"

hello: all
	$(VBOARD) --engine $(ENGINE) --headless $(BUILD)/sw/hello/hello.elf -- one two three

coremark: all
	$(VBOARD) --engine $(ENGINE) --headless $(BUILD)/sw/coremark/coremark.elf

testpattern: all
	$(VBOARD) --engine $(ENGINE) $(BUILD)/sw/testpattern/testpattern.elf -- 600

doom: all
	$(VBOARD) --engine $(ENGINE) --realtime --wad $(WAD) $(BUILD)/doom/soc/doom.elf

timedemo: all
	@test -f $(BUILD)/doom/ref/$(DEMO).dfrm || $(MAKE) reference
	scripts/timedemo.sh $(ENGINE) $(DEMO)

# ------------------------------------------------------------------ FPGA
.PHONY: synth
synth:
	$(MAKE) -C synth

.PHONY: clean
clean:
	rm -rf $(BUILD)
