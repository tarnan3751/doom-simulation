# sw/common.mk - shared build rules for every RISC-V program (and the libraries).
# A program sets PROG and SRCS (and optionally OPT, PROG_CFLAGS, PROG_LDFLAGS, PROG_LIBS, STACK_SIZE,
# OUT, OBJDIR), then includes this file: see sw/README.md.  Targets: all, clean, dis, size.

SW_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
include $(SW_DIR)/../config.mk

SW_BUILD := $(BUILD)/sw
LIBC_DIR := $(SW_DIR)/libc
BSP_DIR  := $(SW_DIR)/bsp
LIBC_A   := $(SW_BUILD)/libc/libc.a
BSP_A    := $(SW_BUILD)/bsp/libbsp.a
CRT0     := $(SW_BUILD)/bsp/crt0.o
LDSCRIPT := $(BSP_DIR)/link.ld

# the C library, the board API (bsp.h), the register map (soc.h); the core traps on misaligned accesses
SW_INCLUDES := -isystem $(LIBC_DIR)/include -I$(BSP_DIR)/include -I$(SW_DIR)/include
SW_ARCH     := $(RV_ARCH) -mstrict-align
SW_CFLAGS   := $(SW_ARCH) -std=gnu11 -g -ffunction-sections -fdata-sections -Wall -Wextra $(SW_INCLUDES)
SW_ASFLAGS  := $(SW_ARCH) -g $(SW_INCLUDES)

STACK_SIZE ?= 262144
SW_LDFLAGS  = $(RV_ARCH) -nostdlib -nostartfiles -T $(LDSCRIPT) -Wl,--gc-sections \
              -Wl,--defsym=__stack_size=$(STACK_SIZE) -Wl,--no-warn-rwx-segments
SW_LIBS     = -Wl,--start-group $(LIBC_A) $(BSP_A) -lgcc -Wl,--end-group

.DEFAULT_GOAL := all

ifdef PROG
OPT    ?= -O2
OUT    ?= $(SW_BUILD)/$(PROG)
OBJDIR ?= $(OUT)/obj
ELF    := $(OUT)/$(PROG).elf

PROG_CFLAGS_ALL = $(SW_CFLAGS) $(OPT) $(PROG_CFLAGS)

# relative source paths are relative to the program's Makefile
PROG_DIR := $(patsubst %/,%,$(dir $(abspath $(firstword $(MAKEFILE_LIST)))))
SRCS_ABS := $(foreach s,$(SRCS),$(if $(filter /%,$(s)),$(s),$(PROG_DIR)/$(s)))

obj_of = $(OBJDIR)/$(basename $(notdir $(1))).o
OBJS := $(foreach s,$(SRCS_ABS),$(call obj_of,$(s)))
ifneq ($(words $(OBJS)),$(words $(sort $(OBJS))))
$(error $(PROG): two sources share a base name; objects are named after it)
endif

define compile_rule
$(call obj_of,$(1)): $(1)
	@mkdir -p $$(dir $$@)
	@echo "  CC      $$(notdir $$<)"
	@$$(RV_CC) $$(if $$(filter %.S %.s,$$<),$$(SW_ASFLAGS) $$(PROG_CFLAGS),$$(PROG_CFLAGS_ALL)) -MD -MP -c $$< -o $$@
endef
$(foreach s,$(SRCS_ABS),$(eval $(call compile_rule,$(s))))
-include $(OBJS:.o=.d)

# objects depend on the compiler flags through this stamp (a flag change rebuilds them)
$(OBJS): $(OBJDIR)/.flags
$(OBJDIR)/.flags: FORCE
	@mkdir -p $(OBJDIR)
	@echo "$(PROG_CFLAGS_ALL)" | cmp -s - $@ || echo "$(PROG_CFLAGS_ALL)" > $@

$(ELF): $(OBJS) $(CRT0) $(LIBC_A) $(BSP_A) $(LDSCRIPT) $(PROG_LIBS)
	@echo "  LD      $(notdir $@)"
	@$(RV_LD) $(SW_LDFLAGS) $(PROG_LDFLAGS) -Wl,-Map,$(OUT)/$(PROG).map $(CRT0) $(OBJS) $(PROG_LIBS) $(SW_LIBS) -o $@
	@$(RV_SIZE) $@ | tail -1 | awk '{printf "  SIZE    %s: text %d  data %d  bss %d\n", "$(notdir $@)", $$1, $$2, $$3}'

.PHONY: all clean dis size
all: $(ELF)
dis: $(ELF)
	$(RV_OBJDUMP) -d -S -C $(ELF) > $(OUT)/$(PROG).dis
size: $(ELF)
	$(RV_SIZE) -A $(ELF)
clean:
	rm -rf $(OBJDIR) $(ELF) $(OUT)/$(PROG).map $(OUT)/$(PROG).dis
endif

# The libraries are built by their own Makefiles; programs ask for them on
# every build (cheap when nothing changed) and relink when they changed.
.PHONY: FORCE
ifneq ($(SW_LIB_BUILD),1)
$(LIBC_A): FORCE
	@$(MAKE) -s --no-print-directory -C $(LIBC_DIR) lib
$(BSP_A): FORCE
	@$(MAKE) -s --no-print-directory -C $(BSP_DIR) lib
$(CRT0): $(BSP_A) ;
endif
FORCE:
