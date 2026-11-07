# ISS, iss-run and tests. Products under build/iss/.
include ../../config.mk

OUT     := $(BUILD)/iss
COMMON  := $(ROOT)/sim/common
CXXFLAGS := -std=gnu++17 -O2 -Wall -Wextra -I$(ROOT)/sw/include -I$(COMMON) -I.

LIB_OBJS := $(OUT)/iss.o $(OUT)/loader.o

.PHONY: all test bench clean
all: $(OUT)/iss-run

$(OUT):
	mkdir -p $@

$(OUT)/iss.o: iss.cpp iss.h | $(OUT)
	$(HOST_CXX) $(CXXFLAGS) -O3 -c $< -o $@
$(OUT)/%.o: $(COMMON)/%.cpp $(wildcard $(COMMON)/*.h) | $(OUT)
	$(HOST_CXX) $(CXXFLAGS) -c $< -o $@
$(OUT)/iss_run.o: iss_run.cpp iss.h $(wildcard $(COMMON)/*.h) | $(OUT)
	$(HOST_CXX) $(CXXFLAGS) -c $< -o $@
$(OUT)/iss-run: $(OUT)/iss_run.o $(LIB_OBJS)
	$(HOST_CXX) $(CXXFLAGS) $^ -o $@

include tests/tests.mk

clean:
	rm -rf $(OUT)
