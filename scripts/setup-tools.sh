#!/bin/sh
# Installs the tools this project needs on macOS (Apple silicon).
#
#   Homebrew:        verilator, riscv64-elf-gcc (bare-metal RISC-V GCC with rv32im libgcc), sdl2
#   OSS CAD Suite:   yosys (+ slang SystemVerilog frontend), nextpnr-himbaechel (Gowin), gowin_pack
#                    unpacked into .tools/ inside this repository, nothing system-wide
#
# No C library is installed for the RISC-V target: the project carries its own (sw/libc).
set -eu
cd "$(dirname "$0")/.."

brew install verilator riscv64-elf-gcc sdl2

OSS_CAD_DATE=2026-11-06
OSS_CAD_FILE=oss-cad-suite-darwin-arm64-$(echo "$OSS_CAD_DATE" | tr -d -).tgz
if [ ! -x .tools/oss-cad-suite/bin/yosys ]; then
    mkdir -p .tools
    curl -sL -o ".tools/$OSS_CAD_FILE" \
        "https://github.com/YosysHQ/oss-cad-suite-build/releases/download/$OSS_CAD_DATE/$OSS_CAD_FILE"
    tar -xzf ".tools/$OSS_CAD_FILE" -C .tools
    rm ".tools/$OSS_CAD_FILE"
fi

verilator --version
riscv64-elf-gcc --version | head -1
.tools/oss-cad-suite/bin/yosys -V
