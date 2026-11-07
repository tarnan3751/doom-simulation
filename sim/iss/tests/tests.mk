# Included from sim/iss/Makefile.
TDIR    := $(CURDIR)/tests
PROGDIR := $(OUT)/prog
PROGS   := exit42 mmio video kbd bootinfo fault traps bench
PROG_ELFS := $(addprefix $(PROGDIR)/,$(addsuffix .elf,$(PROGS)))
RVFLAGS := $(RV_ARCH) -O2 -ffreestanding -nostdlib -nostartfiles -mno-relax -ffixed-x30 -ffixed-x31 \
           -Wall -Wextra -I$(ROOT)/sw/include -I$(TDIR) -T$(TDIR)/link.ld -static -Wl,--no-warn-rwx-segments

$(PROGDIR):
	mkdir -p $@

$(PROGDIR)/%.elf: $(TDIR)/%.c $(TDIR)/crt.S $(TDIR)/t.h $(TDIR)/link.ld $(TDIR)/bench_kernel.h | $(PROGDIR)
	$(RV_CC) $(RVFLAGS) $(TDIR)/crt.S $< -o $@

$(OUT)/test_api: $(TDIR)/test_api.cpp $(LIB_OBJS) $(TDIR)/bench_kernel.h
	$(HOST_CXX) $(CXXFLAGS) -I$(TDIR) -DPROGDIR='"$(PROGDIR)"' -DISADIR='"$(BUILD)/isa"' $< $(LIB_OBJS) -o $@

$(OUT)/test.wad: | $(OUT)
	yes ABCDEFGH | head -c 1001 > $@

test: $(OUT)/iss-run $(OUT)/test_api $(PROG_ELFS) $(OUT)/test.wad
	@$(MAKE) -s -C $(ROOT)/tests/isa
	$(OUT)/test_api
	sh $(TDIR)/test_cli.sh $(OUT)/iss-run $(PROGDIR) $(BUILD)/isa $(OUT)
	@echo '--- throughput benchmark (~200M instructions) ---'
	@$(OUT)/iss-run --stats $(PROGDIR)/bench.elf -- 4000000 || true   # exit status is the checksum, not a result

bench: $(OUT)/iss-run $(PROGDIR)/bench.elf
	$(OUT)/iss-run --stats $(PROGDIR)/bench.elf -- 20000000 || true   # exit status is the checksum, not a result
