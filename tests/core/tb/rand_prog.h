// rand_prog.h - constrained-random RV32IM program generator.
//
// Produces a self-contained memory image (code at the reset PC, a trap handler,
// a scratch data region) that always terminates by storing 1 to `tohost`,
// whatever the data values and timing are:
//   - loops use dedicated counter registers that random code never writes,
//   - every other branch/jump goes forward or to a fixed structural target,
//   - calls use dedicated link registers (x1 for level-1, x5 for leaf calls),
//   - every trapping instruction is skipped by the handler (mepc + 4).
// Register usage:
//   x3 (gp) scratch base, x4/x7 loop counters, x1/x5 link registers,
//   x6 structural temp (computed addresses / jump targets),
//   x30/x31 trap handler, everything else (x2, x8..x29) random data.
#pragma once
#include <cstdint>
#include <vector>

struct RandProgram {
    uint32_t tohost = 0;
    std::vector<std::pair<uint32_t, uint32_t>> words;    // (address, instruction)
    std::vector<std::pair<uint32_t, uint8_t>>  bytes;    // initialised data
};

struct RandOptions {
    unsigned items = 400;          // top-level items in the main body
    unsigned functions = 6;        // level-1 functions
    unsigned leaves = 4;           // level-2 (leaf) functions
};

RandProgram gen_random_program(uint64_t seed, const RandOptions& opt, uint32_t base);
