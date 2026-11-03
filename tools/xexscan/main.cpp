// xexscan: load a game's default.xex (decrypt + decompress via XenonUtils), list its
// sections, and find the compiler's register save/restore helper functions that
// XenonRecomp's TOML config needs.
//
//   xexscan default.xex [dump-image.bin]
//   xexscan default.xex --disasm 0xSTART 0xEND     (print instructions in a range)
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

#include <image.h>
#include <disasm.h>

struct Pattern {
    const char* key;
    std::vector<uint8_t> bytes;
};

static const Pattern kPatterns[] = {
    {"restgprlr_14_address", {0xe9, 0xc1, 0xff, 0x68}},
    {"savegprlr_14_address", {0xf9, 0xc1, 0xff, 0x68}},
    {"restfpr_14_address",   {0xc9, 0xcc, 0xff, 0x70}},
    {"savefpr_14_address",   {0xd9, 0xcc, 0xff, 0x70}},
    {"restvmx_14_address",   {0x39, 0x60, 0xfe, 0xe0, 0x7d, 0xcb, 0x60, 0xce}},
    {"savevmx_14_address",   {0x39, 0x60, 0xfe, 0xe0, 0x7d, 0xcb, 0x61, 0xce}},
    {"restvmx_64_address",   {0x39, 0x60, 0xfc, 0x00, 0x10, 0x0b, 0x60, 0xcb}},
    {"savevmx_64_address",   {0x39, 0x60, 0xfc, 0x00, 0x10, 0x0b, 0x61, 0xcb}},
};

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: xexscan default.xex [dump.bin]\n"); return 2; }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), {});

    Image img = Image::ParseImage(file.data(), file.size());
    if (!img.data) { std::fprintf(stderr, "failed to parse image\n"); return 1; }

    if (argc == 5 && std::strcmp(argv[2], "--disasm") == 0) {
        size_t a = std::strtoull(argv[3], nullptr, 16), b = std::strtoull(argv[4], nullptr, 16);
        for (size_t addr = a & ~size_t(3); addr < b; addr += 4) {
            const uint8_t* p = static_cast<const uint8_t*>(img.Find(addr));
            if (!p) { std::printf("%08zX: <unmapped>\n", addr); continue; }
            ppc_insn insn{};
            ppc::Disassemble(p, addr, insn);
            std::printf("%08zX: %02X%02X%02X%02X  %s\n", addr, p[0], p[1], p[2], p[3],
                        insn.opcode ? insn.opcode->name : ".long (data?)");
        }
        return 0;
    }

    std::printf("# base=0x%zX size=0x%X entry=0x%zX\n", img.base, img.size, img.entry_point);
    for (const auto& s : img.sections)
        std::printf("# section %-10s base=0x%zX size=0x%X %s\n", s.name.c_str(), s.base, s.size,
                    (s.flags & SectionFlags_Code) ? "code" : "data");

    if (argc > 2) {
        std::ofstream out(argv[2], std::ios::binary);
        out.write(reinterpret_cast<const char*>(img.data.get()), img.size);
        std::printf("# wrote image to %s\n", argv[2]);
    }

    for (const auto& p : kPatterns) {
        int hits = 0;
        for (const auto& s : img.sections) {
            if (!(s.flags & SectionFlags_Code)) continue;
            for (uint32_t off = 0; off + p.bytes.size() <= s.size; off += 4) {
                if (std::memcmp(s.data + off, p.bytes.data(), p.bytes.size()) == 0) {
                    std::printf("%s = 0x%zX%s\n", p.key, s.base + off, hits ? "  # extra match" : "");
                    ++hits;
                }
            }
        }
        if (!hits) std::printf("# %s: NOT FOUND\n", p.key);
    }
    return 0;
}
