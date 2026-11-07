# Builds the official riscv-tests (rv32ui / rv32um / rv32mi, "p" environment) into
# $(BUILD)/isa/<suite>-p-<test>.elf using our own rules (upstream build scripts are not run).
include ../../config.mk

OUT := $(BUILD)/isa
RT  := $(RISCV_TESTS)

# Tests we do not target:
#   fence_i  - needs Zifencei (self-modifying code with I-cache flush)
#   ma_data  - needs hardware support for misaligned loads/stores
UI_SKIP := fence_i ma_data
MI_ONLY := csr mcsr scall sbreak shamt ma_addr ma_fetch zicntr

UI := $(filter-out $(UI_SKIP),$(basename $(notdir $(wildcard $(RT)/isa/rv32ui/*.S))))
UM := $(basename $(notdir $(wildcard $(RT)/isa/rv32um/*.S)))
MI := $(filter $(MI_ONLY),$(basename $(notdir $(wildcard $(RT)/isa/rv32mi/*.S))))

ELFS := $(addprefix $(OUT)/rv32ui-p-,$(addsuffix .elf,$(UI))) \
        $(addprefix $(OUT)/rv32um-p-,$(addsuffix .elf,$(UM))) \
        $(addprefix $(OUT)/rv32mi-p-,$(addsuffix .elf,$(MI)))

CFLAGS := $(RV_ARCH) -static -mcmodel=medany -fvisibility=hidden -nostdlib -nostartfiles \
          -I$(RT)/env/p -I$(RT)/isa/macros/scalar -T$(RT)/env/p/link.ld

.PHONY: all clean list
all: $(ELFS)
	@echo "built $(words $(ELFS)) riscv-tests into $(OUT)"

list:
	@for e in $(ELFS); do echo $$e; done

$(OUT)/rv32ui-p-%.elf: $(RT)/isa/rv32ui/%.S | $(OUT)
	$(RV_CC) $(CFLAGS) $< -o $@
$(OUT)/rv32um-p-%.elf: $(RT)/isa/rv32um/%.S | $(OUT)
	$(RV_CC) $(CFLAGS) $< -o $@
$(OUT)/rv32mi-p-%.elf: $(RT)/isa/rv32mi/%.S | $(OUT)
	$(RV_CC) $(CFLAGS) $< -o $@

$(OUT):
	mkdir -p $@

clean:
	rm -rf $(OUT)
