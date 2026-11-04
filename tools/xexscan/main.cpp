// xexscan: load a game's default.xex (decrypt + decompress via XenonUtils), list its
// sections, and find the compiler's register save/restore helper functions that
// XenonRecomp's TOML config needs.
//
//   xexscan default.xex [dump-image.bin]
//   xexscan default.xex --dump16 0xADDR COUNT     (print big-endian halfwords)
//   xexscan default.xex --disasm 0xSTART 0xEND     (print instructions in a range)
//   xexscan default.xex --check-switches switch_tables.toml
//       compares each [[switch]] with the table in the image: label mismatches, and further
//       code-address words right after the last label (a table recorded too short)
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <string>
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

    if (argc == 4 && std::strcmp(argv[2], "--check-switches") == 0) {
        std::ifstream tf(argv[3]);
        if (!tf) { std::fprintf(stderr, "cannot open %s\n", argv[3]); return 1; }
        std::string text((std::istreambuf_iterator<char>(tf)), {});
        auto be32 = [&](size_t a, uint32_t& v) {
            const uint8_t* p = static_cast<const uint8_t*>(img.Find(a));
            if (!p) return false;
            v = uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
            return true;
        };
        auto is_code = [&](uint32_t a) {
            for (const auto& s : img.sections)
                if ((s.flags & SectionFlags_Code) && a >= s.base && a < s.base + s.size && !(a & 3)) return true;
            return false;
        };
        std::regex re_switch(R"(\[\[switch\]\][^\[]*?base\s*=\s*(0x[0-9A-Fa-f]+)[^\[]*?labels\s*=\s*\[([^\]]*)\])");
        int bad = 0, total = 0;
        for (std::sregex_iterator it(text.begin(), text.end(), re_switch), end; it != end; ++it) {
            size_t base = std::strtoull((*it)[1].str().c_str(), nullptr, 16);
            std::vector<uint32_t> labels;
            std::string body = (*it)[2].str();
            static const std::regex re_hex("0x[0-9A-Fa-f]+");
            for (std::sregex_iterator l(body.begin(), body.end(), re_hex), e2; l != e2; ++l)
                labels.push_back(uint32_t(std::strtoull(l->str().c_str(), nullptr, 16)));
            ++total;
            // Where the table is. XenonAnalyse entries put `base` at the lis of the lis/addi pair
            // that loads the table address (a few instructions before the bctr); hand written
            // entries put it at the bctr, with the table right behind it (base + 4).
            size_t table = base + 4;
            for (size_t a = base; a <= base + 0x20; a += 4) {
                uint32_t i1, i2;
                if (!be32(a, i1) || !be32(a + 4, i2)) break;
                if ((i1 >> 26) == 15 && ((i1 >> 16) & 0x1F) == 0 && (i2 >> 26) == 14 &&
                    ((i2 >> 21) & 0x1F) == ((i1 >> 21) & 0x1F) && ((i2 >> 16) & 0x1F) == ((i1 >> 21) & 0x1F)) {
                    table = uint32_t((i1 & 0xFFFF) << 16) + int32_t(int16_t(i2 & 0xFFFF));
                    break;
                }
            }
            bool ok = true;
            for (size_t i = 0; i < labels.size(); i++) {
                uint32_t w;
                if (!be32(table + i * 4, w) || w != labels[i]) {
                    std::printf("MISMATCH switch 0x%zX (table 0x%zX) entry %zu: toml %08X image %08X\n", base, table, i,
                                labels[i], be32(table + i * 4, w) ? w : 0);
                    ok = false;
                }
            }
            // More words that point into the same area as the labels: the table may be longer.
            uint32_t lo = ~0u, hi = 0;
            for (uint32_t a : labels) { lo = std::min(lo, a); hi = std::max(hi, a); }
            size_t extra = 0;
            for (size_t i = labels.size(); i < labels.size() + 32; i++) {
                uint32_t w;
                if (!be32(table + i * 4, w) || !is_code(w) || w + 0x1000 < lo || w > hi + 0x1000) break;
                std::printf("LONGER? switch 0x%zX (table 0x%zX): word %zu after the labels is %08X (near the other labels)\n", base, table, i, w);
                ++extra;
            }
            if (extra || !ok) ++bad;
        }
        std::printf("# %d switches checked, %d suspicious\n", total, bad);
        return bad ? 3 : 0;
    }
    if (argc == 5 && std::strcmp(argv[2], "--dump16") == 0) {
        size_t a = std::strtoull(argv[3], nullptr, 16), n = std::strtoull(argv[4], nullptr, 10);
        const uint8_t* p = static_cast<const uint8_t*>(img.Find(a));
        if (!p) { std::printf("unmapped\n"); return 1; }
        for (size_t i = 0; i < n; ++i)
            std::printf("%zu: %04X\n", i, p[i * 2] << 8 | p[i * 2 + 1]);
        return 0;
    }
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
