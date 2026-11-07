# sw/libc/Makefile - the C library.
#
#   make            build/sw/libc/libc.a (RV32IM)
#   make test       host unit tests against the host C library (fails on any mismatch)
#   make clean
SW_LIB_BUILD := 1
include $(dir $(lastword $(MAKEFILE_LIST)))../common.mk

LIBC_OUT  := $(SW_BUILD)/libc
LIBC_SRCS := $(sort $(wildcard $(LIBC_DIR)/src/*.c))
LIBC_HDRS := $(wildcard $(LIBC_DIR)/include/*.h $(LIBC_DIR)/include/sys/*.h $(LIBC_DIR)/src/*.h)
LIBC_OBJS := $(patsubst $(LIBC_DIR)/src/%.c,$(LIBC_OUT)/obj/%.o,$(LIBC_SRCS))

# no builtins and no loop-to-memcpy/memset rewriting (it would turn memcpy's loop into a call to memcpy)
LIBC_CFLAGS := -std=gnu11 -O2 -g -ffreestanding -fno-builtin -Wall -Wextra -isystem $(LIBC_DIR)/include

.PHONY: all lib test clean
all: lib
lib: $(LIBC_A)

$(LIBC_A): $(LIBC_OBJS)
	@echo "  AR      $(notdir $@)"
	@rm -f $@
	@$(RV_AR) rcs $@ $(LIBC_OBJS)

$(LIBC_OUT)/obj/%.o: $(LIBC_DIR)/src/%.c $(LIBC_HDRS)
	@mkdir -p $(dir $@)
	@echo "  CC      libc/$(notdir $<)"
	@$(RV_CC) $(SW_ARCH) $(LIBC_CFLAGS) -fno-tree-loop-distribute-patterns -ffunction-sections -fdata-sections -c $< -o $@

# ---- host unit tests: the library compiled natively with OUR headers, every public name
# renamed to tl_<name> so it can live next to the host libc (the oracle) -----------------
HOST_RENAME := memcpy memmove memset memcmp memchr strlen strnlen strncpy strcmp strncmp strcasecmp \
  strncasecmp strchr strrchr strstr strdup malloc calloc realloc free malloc_get_stats malloc_check \
  atoi strtol strtoul atexit exit abort __assert_fail fopen fclose fflush fread fwrite fseek ftell \
  fputc putchar puts remove rename printf fprintf snprintf vfprintf vsnprintf sscanf vsscanf \
  memfile_register memfile_lookup
HOST_OUT  := $(LIBC_OUT)/host
HOST_OBJS := $(patsubst $(LIBC_DIR)/src/%.c,$(HOST_OUT)/lib/%.o,$(LIBC_SRCS))
TEST_BIN  := $(HOST_OUT)/test_host

$(HOST_OUT)/lib/%.o: $(LIBC_DIR)/src/%.c $(LIBC_HDRS) $(LIBC_DIR)/Makefile
	@mkdir -p $(dir $@)
	@echo "  HOSTCC  libc/$(notdir $<)"
	@$(HOST_CC) $(LIBC_CFLAGS) -fno-stack-protector -nostdlibinc $(foreach n,$(HOST_RENAME),-D$(n)=tl_$(n)) -c $< -o $@
$(HOST_OUT)/%.o: $(LIBC_DIR)/tests/%.c $(wildcard $(LIBC_DIR)/tests/*.h)
	@mkdir -p $(dir $@)
	@echo "  HOSTCC  tests/$(notdir $<)"
	@$(HOST_CC) -std=gnu11 -O1 -g -Wall -Wextra -Wno-format-extra-args -c $< -o $@
$(TEST_BIN): $(HOST_OBJS) $(HOST_OUT)/test_host.o $(HOST_OUT)/host_harness.o
	@echo "  HOSTLD  $(notdir $@)"
	@$(HOST_CC) $^ -o $@

test: $(TEST_BIN)
	@# every global the library defines must carry the tl_ prefix (or be internal)
	@bad=$$(nm -g -U $(HOST_OBJS) | awk 'NF==3 {print $$3}' | grep -v -E '^_(tl_|__libc_)' | sort -u); \
	 if [ -n "$$bad" ]; then echo "un-renamed library symbols (add them to HOST_RENAME):"; echo "$$bad"; exit 1; fi
	@$(TEST_BIN)

clean:
	rm -rf $(LIBC_OUT)
