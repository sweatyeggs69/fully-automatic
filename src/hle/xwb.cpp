#include "fa/hle/xwb.hpp"

#include <cstring>
#include <fstream>

namespace fa::hle {

namespace {
constexpr uint32_t kXact1_1 = 3, kXact2_0 = 21, kXact2_1 = 22, kXact2_2 = 34, kXact2_3 = 38, kXact2_4 = 41;
constexpr uint32_t kFlagCompact = 0x00020000;
}  // namespace

const char* codec_name(XwbEntry::Codec c) {
    switch (c) {
        case XwbEntry::Codec::Pcm: return "PCM";
        case XwbEntry::Codec::Xma: return "XMA";
        case XwbEntry::Codec::Adpcm: return "ADPCM";
        case XwbEntry::Codec::Wma: return "WMA";
        default: return "?";
    }
}

bool parse_xwb(const std::vector<uint8_t>& d, XwbBank& bank, std::string& err) {
    if (d.size() < 0x30) { err = "file too small"; return false; }
    if (std::memcmp(d.data(), "WBND", 4) == 0) bank.big_endian = false;
    else if (std::memcmp(d.data(), "DNBW", 4) == 0) bank.big_endian = true;
    else { err = "not a wave bank (no WBND signature)"; return false; }

    bool be = bank.big_endian;
    auto u32 = [&](size_t o) -> uint32_t {
        if (o + 4 > d.size()) return 0;
        const uint8_t* p = &d[o];
        return be ? (uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3])
                  : (uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]);
    };
    auto str = [&](size_t o, size_t n) {
        std::string s;
        for (size_t i = 0; i < n && o + i < d.size() && d[o + i]; ++i) s += char(d[o + i]);
        return s;
    };

    uint32_t v = u32(4);
    if (v == 0x87) v = kXact2_4;  // Crackdown
    bank.version = v;
    if (v <= kXact1_1) { err = "XACT 1 (original Xbox) banks are not supported"; return false; }

    size_t seg = v <= kXact2_4 ? 0x08 : 0x0C;
    uint32_t base_off = u32(seg), entry_off = u32(seg + 8), names_off = 0;
    size_t sub;
    if (v <= kXact2_0) {
        names_off = u32(seg + 0x10);
        sub = 8;
    } else if (v <= kXact2_1) {
        names_off = u32(seg + 0x10);
        sub = base_off - 0x20;
    } else if (v <= kXact2_3) {
        names_off = u32(seg + 0x10);
        sub = 16;
    } else {
        names_off = u32(seg + 0x18);
        sub = 16;
    }
    uint32_t data_off = u32(seg + 0x10 + sub);
    (void)data_off;
    if (!base_off || base_off >= d.size()) { err = "bad bank header"; return false; }

    bank.flags = u32(base_off);
    uint32_t count = u32(base_off + 4);
    size_t name_len = v <= kXact2_0 ? 0x10 : 0x40;
    bank.name = str(base_off + 8, name_len);
    size_t bs = base_off + 8 + name_len;
    uint32_t elem = u32(bs);
    uint32_t align = u32(bs + 8);
    if (count > 100000 || elem == 0 || entry_off + uint64_t(count) * elem > d.size()) {
        err = "bad entry table";
        return false;
    }
    if (bank.flags & kFlagCompact) {
        err = "compact wave banks are not supported yet";
        return false;
    }
    (void)align;

    for (uint32_t i = 0; i < count; ++i) {
        size_t o = entry_off + size_t(i) * elem;
        XwbEntry e;
        uint32_t info = u32(o);
        e.flags = info & 0xF;
        e.num_samples = info >> 4;
        uint32_t fmt = u32(o + 4);
        e.offset = data_off + u32(o + 8);
        e.size = u32(o + 12);
        e.loop_start = u32(o + 16);
        e.loop_end = u32(o + 20);
        int tag;
        if (v <= kXact2_2) {
            e.bits_per_sample = (fmt >> 31) & 1 ? 16 : 8;
            e.block_align = int((fmt >> 24) & 0xFF);
            e.sample_rate = int((fmt >> 4) & 0x7FFFF);
            e.channels = int((fmt >> 1) & 7);
            tag = int(fmt & 1);
        } else {
            e.bits_per_sample = (fmt >> 31) & 1 ? 16 : 8;
            e.block_align = int((fmt >> 23) & 0xFF);
            e.sample_rate = int((fmt >> 5) & 0x3FFFF);
            e.channels = int((fmt >> 2) & 7);
            tag = int(fmt & 3);
        }
        switch (tag) {
            case 0: e.codec = XwbEntry::Codec::Pcm; break;
            case 1: e.codec = XwbEntry::Codec::Xma; break;
            case 2: e.codec = XwbEntry::Codec::Adpcm; break;
            case 3: e.codec = XwbEntry::Codec::Wma; break;
        }
        if (names_off && names_off + (i + 1) * 0x40ull <= d.size()) e.name = str(names_off + i * 0x40ull, 0x40);
        bank.entries.push_back(e);
    }
    return true;
}

bool write_wav(const std::string& path, const std::vector<int16_t>& pcm, int channels, int rate) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    auto w32 = [&](uint32_t x) { f.write(reinterpret_cast<const char*>(&x), 4); };
    auto w16 = [&](uint16_t x) { f.write(reinterpret_cast<const char*>(&x), 2); };
    uint32_t bytes = uint32_t(pcm.size() * 2);
    f.write("RIFF", 4); w32(36 + bytes); f.write("WAVEfmt ", 8);
    w32(16); w16(1); w16(uint16_t(channels)); w32(uint32_t(rate));
    w32(uint32_t(rate * channels * 2)); w16(uint16_t(channels * 2)); w16(16);
    f.write("data", 4); w32(bytes);
    f.write(reinterpret_cast<const char*>(pcm.data()), bytes);
    return bool(f);
}

}  // namespace fa::hle
