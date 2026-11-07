// loader.h - getting a program into a simulated machine: RV32 ELF executables, the WAD and the boot-info
// page (ARCHITECTURE.md section 9). The loaders never own the target memory: they hand (guest address, bytes,
// length) to a WriteFn, which copies them wherever the caller keeps its RAM (ISS, SDRAM model, Verilator
// array) and returns false if the range is not writable, which aborts the load.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace simc {

using WriteFn = std::function<bool(uint32_t addr, const uint8_t* data, size_t len)>;

struct ElfSegment { uint32_t vaddr, memsz, filesz, offset; };   // a PT_LOAD segment (p_paddr ignored: no MMU)

class Elf32 {
public:
    // The bytes are copied. Fails (err filled) for a truncated file, bad magic, not ELF32, not little-endian,
    // not EM_RISCV, not ET_EXEC, malformed program/section headers.
    bool parse(const uint8_t* data, size_t size, std::string& err);
    bool load_file(const std::string& path, std::string& err);
    uint32_t entry() const { return entry_; }
    const std::vector<ElfSegment>& segments() const { return segs_; }
    bool find_symbol(const std::string& name, uint32_t& addr) const;   // from .symtab
    // Every PT_LOAD segment through `write`: file bytes, then zero fill up to memsz.
    bool load(const WriteFn& write, std::string& err) const;

private:
    std::vector<uint8_t> img_;
    uint32_t entry_ = 0;
    std::vector<ElfSegment> segs_;
    std::vector<std::pair<std::string, uint32_t>> syms_;
};

constexpr size_t kBootinfoSize = 1040;
constexpr size_t kWadNameMax = 64;    // including NUL
constexpr size_t kCmdlineMax = 960;   // including NUL
constexpr uint32_t kBootinfoAddr = 0x807FF000u;
constexpr uint32_t kBootinfoMagic = 0xB007F00Du;
constexpr uint32_t kWadAddr = 0x80800000u;
constexpr uint32_t kWadMaxBytes = 0x82000000u - 0x80800000u;  // 24 MB

std::string path_basename(const std::string& path);   // final path component ('/' or '\\')

// The 1040-byte page. wad_path empty: no WAD (wad_addr = wad_size = 0). args are joined with single spaces.
// Fails if the WAD name does not fit 63 bytes + NUL, the command line 959 bytes + NUL, or the size kWadMaxBytes.
bool build_bootinfo(const std::string& wad_path, uint32_t wad_size, const std::vector<std::string>& args,
                    std::vector<uint8_t>& page, std::string& err);
bool place_bootinfo(const WriteFn& write, const std::string& wad_path, uint32_t wad_size,
                    const std::vector<std::string>& args, std::string& err);   // the page at kBootinfoAddr
bool load_wad(const WriteFn& write, const std::string& path, uint32_t& size, std::string& err);   // to kWadAddr

// What every runner does before reset: the ELF (entry out), the WAD if given and the boot-info page, or with
// isa_test (riscv-tests) no page but the address of the 'tohost' word.
bool load_program(const WriteFn& write, const std::string& elf, const std::string& wad,
                  const std::vector<std::string>& args, bool isa_test, uint32_t& entry, uint32_t& tohost,
                  std::string& err);

}  // namespace simc
