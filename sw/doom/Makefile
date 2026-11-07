# sw/doom/Makefile - Doom (doomgeneric): the RV32IM build for the SoC and the native reference.
#
#   make soc                 build/doom/soc/doom.elf (sources build/doom/src-soc, through sw/common.mk)
#   make native              build/doom/native/doom-ref, the headless native reference
#   make native-variants     also doom-ref-O0 and doom-ref-ubsan
#   make ref                 the reference dumps build/doom/ref/demo{1,2,3}.dfrm
#   make sheets              contact sheets of the reference dumps (needs tools/)
#   make check-determinism   two O2 runs and one O0 run must be byte-identical
#   make sanitize            demo1..3 under UBSan, logs in build/doom/san/
#   make test                host test of the SoC FixedDiv against upstream's expression
#   make src                 only create the patched source copies
#   make clean               remove build/doom
include $(dir $(lastword $(MAKEFILE_LIST)))../../config.mk

DOOM_DIR := $(ROOT)/sw/doom
OUT      := $(BUILD)/doom
SRC      := $(OUT)/src
SRC_SOC  := $(OUT)/src-soc
SOC_DIR  := $(OUT)/soc
WAD      := $(DOOM_WAD)
DEMOS    := demo1 demo2 demo3

# upstream's SRC_DOOM list without the X11 platform file
DOOM_SRCS := dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main \
  d_mode d_net f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick \
  i_scale i_sound i_system i_timer memio m_argv m_bbox m_cheat m_config m_controls \
  m_fixed m_menu m_misc m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map \
  p_maputl p_mobj p_plats p_pspr p_saveg p_setup p_sight p_spec p_switch p_telept p_tick \
  p_user r_bsp r_data r_draw r_main r_plane r_segs r_sky r_things sha1 sounds statdump \
  st_lib st_stuff s_sound tables v_video wi_stuff w_checksum w_file w_main w_wad z_zone \
  w_file_stdc i_input i_video doomgeneric

# Doom-affecting flags, the same for both builds (README.md: why each one).
DOOM_DEFS   := -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DNORMALUNIX
DOOM_CFLAGS := -std=gnu11 -fsigned-char -fwrapv -fno-strict-aliasing -ffp-contract=off -w
RV_OPT      ?= -O2 -mtune=size

ifeq ($(SOC_LINK),1)
# ---- second pass (make soc): the RISC-V program through sw/common.mk ----------------------
PROG         := doom
OUT          := $(SOC_DIR)
SRCS         := $(addprefix $(SRC_SOC)/,$(addsuffix .c,$(DOOM_SRCS))) $(DOOM_DIR)/doomgeneric_soc.c
OPT          := $(RV_OPT)
PROG_CFLAGS  := -I$(SRC_SOC) $(DOOM_DEFS) $(DOOM_CFLAGS)
# cache placement: README.md, "Cache placement"
PROG_LDFLAGS := -Wl,--section-ordering-file=$(DOOM_DIR)/soc-layout.ld -Wl,--section-start=.sdata=0x80073000
include $(ROOT)/sw/common.mk
$(ELF): $(DOOM_DIR)/soc-layout.ld
else
# ---- source copies: upstream + patches/ (both builds); + patches-soc/ (SoC only) -----------
PATCHES     := $(sort $(wildcard $(DOOM_DIR)/patches/*.patch))
SOC_PATCHES := $(sort $(wildcard $(DOOM_DIR)/patches-soc/*.patch))
UPSTREAM    := $(wildcard $(DOOMGENERIC)/*.c $(DOOMGENERIC)/*.h)

.PHONY: src soc native native-variants ref sheets check-determinism sanitize test clean
src: $(SRC)/.patched $(SRC_SOC)/.patched
$(addprefix $(SRC)/,$(addsuffix .c,$(DOOM_SRCS))): $(SRC)/.patched

$(SRC)/.patched: $(PATCHES) $(UPSTREAM)
	@echo "  SRC build copy + patches"
	@rm -rf $(SRC) && mkdir -p $(SRC)
	@cd $(DOOMGENERIC) && cp $(notdir $(filter-out $(DOOMGENERIC)/doomgeneric_%.c,$(wildcard $(DOOMGENERIC)/*.c))) *.h $(SRC)/
	@for p in $(PATCHES); do patch -p1 -s -d $(SRC) < $$p || exit 1; done
	@touch $@
$(SRC_SOC)/.patched: $(SRC)/.patched $(SOC_PATCHES)
	@echo "  SRC SoC copy + patches-soc"
	@rm -rf $(SRC_SOC) && cp -R $(SRC) $(SRC_SOC)
	@for p in $(SOC_PATCHES); do patch -p1 -s -d $(SRC_SOC) < $$p || exit 1; done
	@touch $@

soc: $(SRC_SOC)/.patched
	@$(MAKE) --no-print-directory -f $(DOOM_DIR)/Makefile SOC_LINK=1
	@$(RV_SIZE) $(SOC_DIR)/doom.elf

# ---- native reference builds: $(1) variant (flags NAT_FLAGS_$(1)), $(2) binary suffix --------
NATIVE_DIR      := $(OUT)/native
NAT_FLAGS_O2    := $(DOOM_CFLAGS) -O2
NAT_FLAGS_O0    := $(DOOM_CFLAGS) -O0
# -fwrapv would hide signed overflow from UBSan: the sanitizer build drops it
NAT_FLAGS_ubsan := $(filter-out -fwrapv,$(DOOM_CFLAGS)) -O1 -g -fno-omit-frame-pointer \
                   -fsanitize=undefined,float-divide-by-zero,float-cast-overflow
native_cc = @mkdir -p $(dir $@) && echo "  CC  $(notdir $<) [$(1)]" && \
  $(HOST_CC) $(NAT_FLAGS_$(1)) $(DOOM_DEFS) -I$(SRC) $(2) -c $< -o $@
define NATIVE_RULES
$$(NATIVE_DIR)/obj-$(1)/doomgeneric_ref.o: $$(DOOM_DIR)/doomgeneric_ref.c $$(SRC)/.patched
	$$(call native_cc,$(1),-Wall -Wextra)
$$(NATIVE_DIR)/obj-$(1)/%.o: $$(SRC)/%.c $$(SRC)/.patched
	$$(call native_cc,$(1))
$$(NATIVE_DIR)/doom-ref$(2): $$(addprefix $$(NATIVE_DIR)/obj-$(1)/,$$(addsuffix .o,$$(DOOM_SRCS) doomgeneric_ref))
	@echo "  LD  $$(notdir $$@)"
	@$$(HOST_CC) $$(NAT_FLAGS_$(1)) $$^ -lm -o $$@
endef
$(eval $(call NATIVE_RULES,O2,))
$(eval $(call NATIVE_RULES,O0,-O0))
$(eval $(call NATIVE_RULES,ubsan,-ubsan))
REF := $(NATIVE_DIR)/doom-ref
native: $(REF)
native-variants: $(REF) $(REF)-O0 $(REF)-ubsan

# ---- reference dumps, determinism and sanitizer runs ----------------------------------------
REFDIR := $(OUT)/ref
ref: $(addprefix $(REFDIR)/,$(addsuffix .dfrm,$(DEMOS)))
$(REFDIR)/%.dfrm: $(REF) $(WAD)
	@mkdir -p $(REFDIR)
	@$(REF) --dump-frames $@ -iwad $(WAD) -timedemo $* > $(REFDIR)/$*.log 2>&1; \
	  st=$$?; echo "exit status $$st" >> $(REFDIR)/$*.log; tail -n 3 $(REFDIR)/$*.log; test $$st -eq 0

# 24 frames per demo at half size, as PPM (PNG through macOS sips when available)
sheets: ref
	@$(MAKE) -s -C $(ROOT)/tools
	@for d in $(DEMOS); do \
	  n=$$($(BUILD)/tools/dfrm2ppm $(REFDIR)/$$d.dfrm --info | sed -n 's/.*: \([0-9]*\) frames.*/\1/p'); \
	  $(BUILD)/tools/dfrm2ppm $(REFDIR)/$$d.dfrm --every $$((n/23)) --sheet 6 --down 2 -o $(REFDIR)/$${d}_sheet.ppm | head -1; \
	  sips -s format png $(REFDIR)/$${d}_sheet.ppm --out $(REFDIR)/$${d}_sheet.png >/dev/null 2>&1 || true; done

check-determinism: $(REF) $(REF)-O0 $(WAD)
	@mkdir -p $(OUT)/det
	@set -e; for d in $(DEMOS); do \
	  for v in a:$(REF) b:$(REF) o0:$(REF)-O0; do \
	    $${v#*:} --dump-frames $(OUT)/det/$$d.$${v%%:*}.dfrm -iwad $(WAD) -timedemo $$d >/dev/null 2>&1; done; \
	  cmp $(OUT)/det/$$d.a.dfrm $(OUT)/det/$$d.b.dfrm  && echo "$$d: O2 run A == O2 run B"; \
	  cmp $(OUT)/det/$$d.a.dfrm $(OUT)/det/$$d.o0.dfrm && echo "$$d: O2 == O0"; \
	done

sanitize: $(REF)-ubsan $(WAD)
	@mkdir -p $(OUT)/san
	@for d in $(DEMOS); do UBSAN_OPTIONS=print_stacktrace=1 $(REF)-ubsan --dump-frames $(OUT)/san/$$d.ubsan.dfrm \
	  -iwad $(WAD) -timedemo $$d > $(OUT)/san/$$d.ubsan.log 2>&1; echo "$$d ubsan: exit $$?"; done

# ---- host test: the SoC copy's FixedDiv against upstream's expression, bit for bit ----------
FIXED_TEST := $(OUT)/test/fixed_test
$(FIXED_TEST): $(DOOM_DIR)/tests/fixed_test.c $(SRC_SOC)/.patched
	@mkdir -p $(dir $@)
	@echo "  HOSTCC fixed_test"
	@$(HOST_CC) -O2 $(DOOM_CFLAGS) -DFixedDiv=soc_FixedDiv -I$(SRC_SOC) $< $(SRC_SOC)/m_fixed.c -o $@
test: $(FIXED_TEST)
	@$(FIXED_TEST)

clean:
	rm -rf $(OUT)
endif
