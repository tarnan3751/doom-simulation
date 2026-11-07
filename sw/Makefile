# sw/Makefile - all target software except Doom (sw/doom has its own Makefile).
#
#   make -C sw all        libc, bsp, hello, testpattern, coremark (+ quick), libctest -> build/sw/
#   make -C sw test       host unit tests of the C library (host libc as oracle) and of
#                         Doom's SoC-only arithmetic (sw/doom: make test)
#   make -C sw run-iss    run every program on build/iss/iss-run and check exit codes
#   make -C sw clean
SW_LIB_BUILD := 1
include $(dir $(lastword $(MAKEFILE_LIST)))common.mk

PROGRAM_DIRS := hello testpattern coremark libc/tests
ISS := $(BUILD)/iss/iss-run

.PHONY: all libs programs test run-iss clean $(PROGRAM_DIRS)
all: programs

libs:
	@$(MAKE) -s --no-print-directory -C $(SW_DIR)/libc lib
	@$(MAKE) -s --no-print-directory -C $(SW_DIR)/bsp lib

programs: libs
	@for d in $(PROGRAM_DIRS); do $(MAKE) --no-print-directory -C $(SW_DIR)/$$d all || exit 1; done

test:
	@$(MAKE) --no-print-directory -C $(SW_DIR)/libc test
	@$(MAKE) --no-print-directory -C $(SW_DIR)/doom test

# Each program with its expected exit status.  Needs the ISS (sim/iss).
run-iss: programs
	@test -x $(ISS) || { echo "run-iss: $(ISS) not found (make -C sim/iss)"; exit 1; }
	@sh $(SW_DIR)/run-iss.sh $(ISS) $(SW_BUILD) $(DOOM_WAD)

clean:
	rm -rf $(SW_BUILD)
