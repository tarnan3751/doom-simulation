#include "loader.h"

#include <cstdio>
#include <cstring>

namespace simc {
namespace {

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
void put32(std::vector<uint8_t>& v, size_t o, uint32_t x) { for (int i = 0; i < 4; i++) v[o + i] = uint8_t(x >> (8 * i)); }
bool in_range(uint64_t off, uint64_t len, uint64_t total) { return off <= total && len <= total - off; }

}  // namespace

bool Elf32::parse(const uint8_t* d, size_t n, std::string& err) {
    img_.assign(d, d + n);
    segs_.clear();
    syms_.clear();
    entry_ = 0;
    if (n < 52) { err = "file too short for an ELF header (truncated?)"; return false; }
    if (memcmp(d, "\x7f""ELF", 4) != 0) { err = "not an ELF file (bad magic)"; return false; }
    if (d[4] != 1) { err = "not a 32-bit ELF (EI_CLASS != ELFCLASS32)"; return false; }
    if (d[5] != 1) { err = "not little-endian (EI_DATA != ELFDATA2LSB)"; return false; }
    if (rd16(d + 16) != 2) { err = "not an executable (e_type != ET_EXEC)"; return false; }
    if (rd16(d + 18) != 243) { err = "not a RISC-V ELF (e_machine != 243)"; return false; }
    entry_ = rd32(d + 24);
    uint32_t phoff = rd32(d + 28), shoff = rd32(d + 32);
    uint16_t phentsize = rd16(d + 42), phnum = rd16(d + 44), shentsize = rd16(d + 46), shnum = rd16(d + 48);
    if (phnum) {
        if (phentsize < 32) { err = "bad e_phentsize"; return false; }
        if (!in_range(phoff, uint64_t(phnum) * phentsize, n)) { err = "program header table outside file (truncated?)"; return false; }
        for (unsigned i = 0; i < phnum; i++) {
            const uint8_t* p = d + phoff + size_t(i) * phentsize;
            if (rd32(p) != 1) continue;  // PT_LOAD only
            ElfSegment s{rd32(p + 8), rd32(p + 20), rd32(p + 16), rd32(p + 4)};
            if (s.memsz < s.filesz) { err = "segment with p_memsz < p_filesz"; return false; }
            if (!in_range(s.offset, s.filesz, n)) { err = "segment data outside file (truncated?)"; return false; }
            if (uint64_t(s.vaddr) + s.memsz > 0x100000000ull) { err = "segment wraps the 32-bit address space"; return false; }
            segs_.push_back(s);
        }
    }
    if (shoff && shnum) {   // symbols (optional)
        if (shentsize < 40 || !in_range(shoff, uint64_t(shnum) * shentsize, n)) { err = "section header table outside file (truncated?)"; return false; }
        for (unsigned i = 0; i < shnum; i++) {
            const uint8_t* sh = d + shoff + size_t(i) * shentsize;
            if (rd32(sh + 4) != 2) continue;  // SHT_SYMTAB
            uint32_t off = rd32(sh + 16), size = rd32(sh + 20), link = rd32(sh + 24), entsz = rd32(sh + 36);
            if (entsz < 16 || link >= shnum || !in_range(off, size, n)) { err = "bad symbol table"; return false; }
            const uint8_t* str = d + shoff + size_t(link) * shentsize;
            uint32_t stroff = rd32(str + 16), strsize = rd32(str + 20);
            if (!in_range(stroff, strsize, n)) { err = "bad string table"; return false; }
            for (uint32_t o = 0; o + entsz <= size; o += entsz) {
                uint32_t nm = rd32(d + off + o);
                if (nm >= strsize) continue;
                const char* s = reinterpret_cast<const char*>(d + stroff + nm);
                size_t len = strnlen(s, strsize - nm);
                if (len < strsize - nm) syms_.push_back({std::string(s, len), rd32(d + off + o + 4)});   // skip unterminated
            }
        }
    }
    return true;
}

bool Elf32::load_file(const std::string& path, std::string& err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open '" + path + "'"; return false; }
    std::vector<uint8_t> buf;
    uint8_t tmp[65536];
    for (size_t r; (r = fread(tmp, 1, sizeof tmp, f)) > 0;) buf.insert(buf.end(), tmp, tmp + r);
    fclose(f);
    if (!parse(buf.data(), buf.size(), err)) { err = path + ": " + err; return false; }
    return true;
}

bool Elf32::find_symbol(const std::string& name, uint32_t& addr) const {
    for (const auto& s : syms_)
        if (s.first == name) { addr = s.second; return true; }
    return false;
}

bool Elf32::load(const WriteFn& write, std::string& err) const {
    static const uint8_t zeros[4096] = {0};
    char msg[96];
    for (const ElfSegment& s : segs_) {
        if (s.filesz && !write(s.vaddr, img_.data() + s.offset, s.filesz)) {
            snprintf(msg, sizeof msg, "cannot write segment at 0x%08x (+0x%x)", s.vaddr, s.filesz);
            err = msg;
            return false;
        }
        for (uint32_t o = s.filesz, chunk; o < s.memsz; o += chunk) {
            chunk = s.memsz - o < sizeof zeros ? s.memsz - o : uint32_t(sizeof zeros);
            if (!write(s.vaddr + o, zeros, chunk)) {
                snprintf(msg, sizeof msg, "cannot zero-fill segment at 0x%08x (+0x%x)", s.vaddr + o, chunk);
                err = msg;
                return false;
            }
        }
    }
    return true;
}

std::string path_basename(const std::string& p) {
    size_t i = p.find_last_of("/\\");
    return i == std::string::npos ? p : p.substr(i + 1);
}

bool build_bootinfo(const std::string& wad_path, uint32_t wad_size, const std::vector<std::string>& args,
                    std::vector<uint8_t>& page, std::string& err) {
    page.assign(kBootinfoSize, 0);
    std::string name = path_basename(wad_path), cmd;
    if (name.size() >= kWadNameMax) { err = "WAD file name too long for boot-info (max 63 bytes)"; return false; }
    if (wad_size > kWadMaxBytes) { err = "WAD too large for the 24 MB WAD region"; return false; }
    for (size_t i = 0; i < args.size(); i++) cmd += (i ? " " : "") + args[i];
    if (cmd.size() >= kCmdlineMax) { err = "command line too long for boot-info (max 959 bytes)"; return false; }
    bool have_wad = !wad_path.empty();
    put32(page, 0, kBootinfoMagic);
    put32(page, 4, have_wad ? kWadAddr : 0);
    put32(page, 8, have_wad ? wad_size : 0);
    if (have_wad) memcpy(&page[16], name.data(), name.size());
    memcpy(&page[16 + kWadNameMax], cmd.data(), cmd.size());
    return true;
}

bool place_bootinfo(const WriteFn& write, const std::string& wad_path, uint32_t wad_size,
                    const std::vector<std::string>& args, std::string& err) {
    std::vector<uint8_t> page;
    if (!build_bootinfo(wad_path, wad_size, args, page, err)) return false;
    if (!write(kBootinfoAddr, page.data(), page.size())) { err = "cannot write boot-info page"; return false; }
    return true;
}

bool load_wad(const WriteFn& write, const std::string& path, uint32_t& size, std::string& err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open WAD '" + path + "'"; return false; }
    std::vector<uint8_t> buf(1 << 20);
    uint64_t total = 0;
    for (size_t r; (r = fread(buf.data(), 1, buf.size(), f)) > 0; total += r) {
        if (total + r > kWadMaxBytes) { fclose(f); err = "WAD '" + path + "' larger than 24 MB"; return false; }
        if (!write(kWadAddr + uint32_t(total), buf.data(), r)) { fclose(f); err = "cannot write WAD to memory"; return false; }
    }
    bool bad = ferror(f);
    fclose(f);
    if (bad) { err = "read error on WAD '" + path + "'"; return false; }
    size = uint32_t(total);
    return true;
}

bool load_program(const WriteFn& write, const std::string& elf_path, const std::string& wad,
                  const std::vector<std::string>& args, bool isa_test, uint32_t& entry, uint32_t& tohost,
                  std::string& err) {
    Elf32 elf;
    uint32_t wad_size = 0;
    if (!elf.load_file(elf_path, err) || !elf.load(write, err)) return false;
    entry = elf.entry();
    if (!wad.empty() && !load_wad(write, wad, wad_size, err)) return false;
    if (isa_test && !elf.find_symbol("tohost", tohost)) { err = "--isa-test: no 'tohost' symbol in " + elf_path; return false; }
    return isa_test || place_bootinfo(write, wad, wad_size, args, err);
}

}  // namespace simc
