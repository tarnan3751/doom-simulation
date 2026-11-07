# Included from sim/vboard/Makefile.
TDIR    := $(CURDIR)/tests
PROGDIR := $(OUT)/prog
PROGS   := hello args spin video videoloop kbd bench fault corners
RVFLAGS := $(RV_ARCH) -O2 -ffreestanding -nostdlib -nostartfiles -mno-relax -Wall -Wextra \
           -I$(ROOT)/sw/include -I$(TDIR) -T$(TDIR)/link.ld -static -Wl,--no-warn-rwx-segments
PROG_ELFS := $(addprefix $(PROGDIR)/,$(addsuffix .elf,$(PROGS))) $(PROGDIR)/tohost_pass.elf $(PROGDIR)/tohost_fail.elf $(PROGDIR)/video_hold.elf
TDEPS   := $(TDIR)/crt.S $(TDIR)/t.h $(TDIR)/link.ld

$(PROGDIR):
	mkdir -p $@
$(PROGDIR)/tohost_pass.elf: $(TDIR)/tohost.c $(TDEPS) | $(PROGDIR)
	$(RV_CC) $(RVFLAGS) -DVAL=1 $(TDIR)/crt.S $< -o $@
$(PROGDIR)/tohost_fail.elf: $(TDIR)/tohost.c $(TDEPS) | $(PROGDIR)
	$(RV_CC) $(RVFLAGS) -DVAL=5 $(TDIR)/crt.S $< -o $@
$(PROGDIR)/video_hold.elf: $(TDIR)/video.c $(TDEPS) | $(PROGDIR)
	$(RV_CC) $(RVFLAGS) -DHOLD=4000000 $(TDIR)/crt.S $< -o $@
$(PROGDIR)/%.elf: $(TDIR)/%.c $(TDEPS) | $(PROGDIR)
	$(RV_CC) $(RVFLAGS) $(TDIR)/crt.S $< -o $@

$(OUT)/test_host: $(TDIR)/test_host.cpp $(BOARD_OBJS) $(ISS_OBJS) $(VL_LIBS)
	$(HOST_CXX) $(CXXFLAGS) $^ $(SDL2_LIBS) $(VL_LDOPT) -o $@

$(OUT)/test.wad: | $(OUT)
	yes ABCDEFGH | head -c 1001 > $@

.PHONY: test
test: $(OUT)/vboard $(OUT)/test_host $(PROG_ELFS) $(OUT)/test.wad
	@$(OUT)/test_host unit
	sh $(TDIR)/test_cli.sh $(OUT)/vboard $(OUT)/test_host $(PROGDIR) $(BUILD)/isa $(OUT)
	sh $(TDIR)/test_rtl.sh $(OUT)/vboard $(OUT)/test_host $(PROGDIR) $(BUILD)/isa $(OUT) short

# longer runs (several minutes): more Doom frames under cosim, full CoreMark
test-long: $(OUT)/vboard $(OUT)/test_host $(PROG_ELFS)
	sh $(TDIR)/test_rtl.sh $(OUT)/vboard $(OUT)/test_host $(PROGDIR) $(BUILD)/isa $(OUT) long
