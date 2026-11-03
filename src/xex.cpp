#include "fa/xex.hpp"

#include <cstring>

namespace fa {

static bool be32(const std::vector<uint8_t>& d, size_t o, uint32_t& v) {
    if (o + 4 > d.size()) return false;
    v = uint32_t(d[o]) << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3];
    return true;
}

bool parse_xex(const std::vector<uint8_t>& d, XexInfo& x) {
    if (d.size() < 24 || std::memcmp(d.data(), "XEX2", 4) != 0) return false;
    uint32_t count = 0;
    if (!be32(d, 4, x.module_flags) || !be32(d, 8, x.pe_offset) ||
        !be32(d, 16, x.security_offset) || !be32(d, 20, count) || count > 512)
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t id, val;
        if (!be32(d, 24 + i * 8, id) || !be32(d, 28 + i * 8, val)) return false;
        x.optional_headers[id] = val;
    }
    auto get = [&](uint32_t id, uint32_t& dst) {
        auto it = x.optional_headers.find(id);
        if (it != x.optional_headers.end()) dst = it->second;
    };
    get(0x10100, x.entry_point);
    get(0x10201, x.base_address);
    be32(d, x.security_offset + 4, x.image_size);
    auto ex = x.optional_headers.find(0x40006);  // execution info: offset into file
    if (ex != x.optional_headers.end()) {
        be32(d, ex->second, x.media_id);
        be32(d, ex->second + 4, x.version);
        be32(d, ex->second + 12, x.title_id);
    }
    return true;
}

}  // namespace fa
