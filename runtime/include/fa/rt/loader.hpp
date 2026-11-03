#pragma once
#include <cstdint>
#include <vector>
#include "fa/rt/kernel.hpp"

namespace fa::rt {

struct LoadedImage {
    uint32_t base = 0, size = 0, entry = 0;
    uint32_t stack_size = 0x80000;
    TlsInfo tls;
};

// Installs the recompiled function table (PPCFuncMappings) after the image in guest memory.
bool install_function_table();

// Maps an already-decoded image (guest layout) at `base` and commits the function table.
bool map_image(const uint8_t* data, uint32_t base, uint32_t size);

// Decrypts/decompresses a raw default.xex, maps it, links kernel variable imports, and
// reads the TLS and stack headers. Requires GuestMemory::init() first.
bool load_xex(const std::vector<uint8_t>& xex_file, LoadedImage& out);

}  // namespace fa::rt
