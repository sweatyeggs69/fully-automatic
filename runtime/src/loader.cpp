#include "fa/rt/loader.hpp"
#include "fa/rt/memory.hpp"
#include "fa/rt/vars.hpp"
#include "fa/xex.hpp"

#include <cstdio>
#include <cstring>
#include <unordered_map>

extern PPCFuncMapping PPCFuncMappings[];

namespace fa::rt {

bool install_function_table() {
    uint32_t table = uint32_t(PPC_IMAGE_BASE + PPC_IMAGE_SIZE);
    uint32_t bytes = uint32_t(PPC_CODE_SIZE * 2);
    if (!g_mem.commit_fixed(table, bytes)) {
        std::perror("commit function table");
        return false;
    }
    size_t n = 0;
    for (PPCFuncMapping* m = PPCFuncMappings; m->guest; ++m, ++n) {
        if (m->guest < PPC_CODE_BASE || m->guest >= PPC_CODE_BASE + PPC_CODE_SIZE) continue;
        PPC_LOOKUP_FUNC(g_base, m->guest) = m->host;
    }
    std::fprintf(stderr, "function table: %zu entries\n", n);
    return true;
}

bool map_image(const uint8_t* data, uint32_t base, uint32_t size) {
    if (!g_mem.commit_fixed(base, size)) {
        std::perror("commit image");
        return false;
    }
    std::memcpy(host(base), data, size);
    return install_function_table();
}

}  // namespace fa::rt

#ifdef FA_WITH_XENONUTILS
#include <image.h>

extern std::unordered_map<size_t, const char*> XamExports;
extern std::unordered_map<size_t, const char*> XboxKernelExports;

namespace fa::rt {

static uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

// Variable imports: slots that did not get the nop/blr thunk hold the (byte swapped) import
// descriptor, whose low 16 bits are the ordinal. Write the variable's guest address instead.
static void link_variables(const std::vector<uint8_t>& f, const XexInfo& info, uint32_t base) {
    auto it = info.optional_headers.find(0x103FF);
    if (it == info.optional_headers.end()) return;
    const uint8_t* hdr = f.data() + it->second;
    uint32_t str_size = be32(hdr + 4), num_libs = be32(hdr + 8);
    const uint8_t* strs = hdr + 12;
    std::vector<std::string> names;
    size_t off = 0;
    for (uint32_t i = 0; i < num_libs; ++i) {
        names.emplace_back(reinterpret_cast<const char*>(strs + off));
        off += ((names.back().size() + 1) + 3) & ~size_t(3);
    }
    const uint8_t* lib = hdr + 12 + str_size;
    size_t linked = 0;
    for (uint32_t i = 0; i < num_libs; ++i) {
        uint16_t count = be16(lib + 38);
        const uint8_t* desc = lib + 40;
        auto& table = names[i] == "xam.xex" ? XamExports : XboxKernelExports;
        for (uint16_t k = 0; k < count; ++k) {
            uint32_t slot = be32(desc + k * 4);
            if (slot < base) continue;
            uint8_t* p = host(slot);
            static const uint8_t thunk[16] = {0x60,0,0,0, 0x60,0,0,0, 0x60,0,0,0, 0x4E,0x80,0,0x20};
            if (std::memcmp(p, thunk, 16) == 0) continue;  // function import
            uint32_t swapped;
            std::memcpy(&swapped, p, 4);
            auto n = table.find(swapped & 0xFFFF);
            std::string name = n != table.end() ? n->second : "ordinal_" + std::to_string(swapped & 0xFFFF);
            st32(slot, kernel_variable(name));
            ++linked;
        }
        lib = desc + count * 4;
    }
    std::fprintf(stderr, "linked %zu variable imports\n", linked);
}

bool load_xex(const std::vector<uint8_t>& file, LoadedImage& out) {
    XexInfo info;
    if (!parse_xex(file, info)) {
        std::fprintf(stderr, "not a valid XEX\n");
        return false;
    }
    Image img = Image::ParseImage(file.data(), file.size());
    if (!img.data) {
        std::fprintf(stderr, "failed to decode XEX image\n");
        return false;
    }
    out.base = uint32_t(img.base);
    out.size = img.size;
    out.entry = uint32_t(img.entry_point);
    if (out.base != PPC_IMAGE_BASE || out.size != PPC_IMAGE_SIZE)
        std::fprintf(stderr, "warning: image 0x%X/0x%X differs from recompiled 0x%llX/0x%llX\n",
                     out.base, out.size, (unsigned long long)PPC_IMAGE_BASE, (unsigned long long)PPC_IMAGE_SIZE);
    if (!map_image(img.data.get(), out.base, out.size)) return false;

    link_variables(file, info, out.base);

    auto st = info.optional_headers.find(0x20200);
    if (st != info.optional_headers.end() && st->second) out.stack_size = st->second;
    auto tl = info.optional_headers.find(0x20104);
    if (tl != info.optional_headers.end() && tl->second + 16 <= file.size()) {
        const uint8_t* t = file.data() + tl->second;
        out.tls = {be32(t), be32(t + 4), be32(t + 8), be32(t + 12)};
    }
    return true;
}

}  // namespace fa::rt
#endif
