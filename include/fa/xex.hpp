#pragma once
// Minimal XEX2 header parser (big-endian). Enough to identify a title.
#include <cstdint>
#include <map>
#include <vector>

namespace fa {

struct XexInfo {
    uint32_t module_flags = 0;
    uint32_t pe_offset = 0;
    uint32_t security_offset = 0;
    uint32_t image_size = 0;      // from security info, 0 if out of range
    uint32_t entry_point = 0;     // optional header 0x10100
    uint32_t base_address = 0;    // optional header 0x10201
    uint32_t title_id = 0;        // execution info (0x40006)
    uint32_t media_id = 0;
    uint32_t version = 0;
    int encryption = -1;          // file format info (0x3FF): 0 none, 1 normal; -1 absent
    int compression = -1;         // 0 none, 1 basic, 2 normal (LZX), 3 delta; -1 absent
    std::map<uint32_t, uint32_t> optional_headers;  // id -> value/offset
};

// Returns false if data is not a valid XEX2 header.
bool parse_xex(const std::vector<uint8_t>& data, XexInfo& out);

}  // namespace fa
