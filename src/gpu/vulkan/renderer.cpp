// Xenos -> Vulkan renderer. See include/fa/gpu/renderer.hpp for the overall design.
#include "fa/gpu/renderer.hpp"

#include <vulkan/vulkan.h>
#include <shaderc/shaderc.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <cmath>
#include <cstdio>
#include <array>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "fa/gpu/gpu_profile.hpp"
#include "fa/gpu/pm4.hpp"
#include "fa/gpu/png.hpp"
#include "fa/gpu/primitive.hpp"
#include "fa/gpu/shader.hpp"
#include "fa/gpu/texture.hpp"
#include "fa/gpu/xenos.hpp"

namespace fa::gpu {

namespace {

constexpr VkDeviceSize kRingSize = 64ull << 20;       // per-batch upload memory
constexpr uint32_t kMaxTextureSets = 4096;            // texture descriptor sets per batch
constexpr uint32_t kEdramBytes = 10u << 20;

const char* VkErr(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        default: return "VkResult(other)";
    }
}

// Splits row ranges across a few worker threads (resolve conversion). Run() blocks until done.
class RowPool {
public:
    RowPool() {
        unsigned n = std::thread::hardware_concurrency();
        n = n > 2 ? std::min(n - 1, 6u) : 0;  // leave cores for the game's own threads
        if (const char* e = std::getenv("FA_RESOLVE_THREADS")) n = unsigned(std::max(0, std::atoi(e)));
        for (unsigned i = 0; i < n; i++) workers_.emplace_back([this] { Loop(); });
    }
    ~RowPool() {
        {
            std::lock_guard lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }
    // fn(y0, y1) for disjoint bands covering [0, rows).
    void Run(uint32_t rows, const std::function<void(uint32_t, uint32_t)>& fn) {
        const uint32_t parts = std::min<uint32_t>(uint32_t(workers_.size()) + 1, (rows + 15) / 16);
        if (parts <= 1) { fn(0, rows); return; }
        // Bands start on 8-row boundaries so an endian-swap unit never straddles two bands.
        const uint32_t band = ((rows + parts - 1) / parts + 7) & ~7u;
        {
            std::lock_guard lk(mu_);
            fn_ = &fn;
            band_ = band;
            rows_ = rows;
            next_ = 1;  // band 0 runs on the calling thread
            pending_ = parts - 1;
            parts_ = parts;
            gen_++;
        }
        cv_.notify_all();
        fn(0, std::min(band, rows));
        std::unique_lock lk(mu_);
        done_.wait(lk, [&] { return pending_ == 0; });
        fn_ = nullptr;
    }

private:
    void Loop() {
        uint64_t seen = 0;
        std::unique_lock lk(mu_);
        for (;;) {
            cv_.wait(lk, [&] { return stop_ || (gen_ != seen && next_ < parts_); });
            if (stop_) return;
            uint32_t part = next_++;
            if (next_ >= parts_) seen = gen_;
            const auto* fn = fn_;
            uint32_t y0 = part * band_, y1 = std::min(rows_, y0 + band_);
            lk.unlock();
            if (y0 < y1) (*fn)(y0, y1);
            lk.lock();
            if (--pending_ == 0) done_.notify_one();
        }
    }
    std::vector<std::thread> workers_;
    std::mutex mu_;
    std::condition_variable cv_, done_;
    const std::function<void(uint32_t, uint32_t)>* fn_ = nullptr;
    uint32_t band_ = 0, rows_ = 0, next_ = 0, parts_ = 0, pending_ = 0;
    uint64_t gen_ = 0;
    bool stop_ = false;
};

uint64_t HashBytes(const void* data, size_t n, uint64_t h = 0xCBF29CE484222325ull) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t i = 0;
    if (n >= 64) {
        // Four independent lanes so the multiplies overlap (texture checks hash megabytes per frame).
        uint64_t l[4] = {h, h ^ 0x9E3779B97F4A7C15ull, h ^ 0xC2B2AE3D27D4EB4Full, h ^ 0x165667B19E3779F9ull};
        for (; i + 32 <= n; i += 32) {
            for (int k = 0; k < 4; k++) {
                uint64_t v;
                std::memcpy(&v, p + i + 8 * k, 8);
                l[k] = (l[k] ^ v) * 0x100000001B3ull;
                l[k] ^= l[k] >> 29;
            }
        }
        h = l[0] ^ (l[1] * 0x9E3779B97F4A7C15ull) ^ (l[2] * 0xC2B2AE3D27D4EB4Full) ^ (l[3] * 0x165667B19E3779F9ull);
        h ^= h >> 32;
    }
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        std::memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 31;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

float HalfToFloat(uint16_t h) {
    uint32_t sign = uint32_t(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, bits;
    if (e == 0) {
        if (!m) bits = sign;
        else {
            e = 113;
            while (!(m & 1024)) { m <<= 1; e--; }
            bits = sign | (e << 23) | ((m & 1023) << 13);
        }
    } else if (e == 31) bits = sign | 0x7F800000u | (m << 13);
    else bits = sign | ((e + 112) << 23) | (m << 13);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
uint16_t FloatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000;
    int32_t e = int32_t((x >> 23) & 0xFF) - 112;
    uint32_t m = x & 0x7FFFFF;
    if (((x >> 23) & 0xFF) == 0xFF) return uint16_t(sign | 0x7C00 | (m ? 0x200 : 0));
    if (e >= 31) return uint16_t(sign | 0x7C00);
    if (e <= 0) return uint16_t(sign);
    return uint16_t(sign | (uint32_t(e) << 10) | (m >> 13));
}

// The fixed-point 16_16 render targets (k_16_16, k_16_16_16_16) hold values in [-32, 32): the shader
// output is stored as value / 32 in an int16. The host target keeps the real value in a float, so a
// resolve into a 16-bit texture has to divide by 32 to put the same bits in guest memory.
// Off by default: the shadow map the game resolves from a k_16_16 target is fetched as unsigned
// normalised with no exponent adjust, and its fill writes 1.0 expecting 1.0 back. FA_RT1616=1
// turns the divide by 32 on again.
bool Rt1616Scaled() {
    static const bool on = [] { const char* e = std::getenv("FA_RT1616"); return e && e[0] == '1'; }();
    return on;
}

VkFormat ColorRtVkFormat(uint32_t f) {
    switch (f) {
        case xe::kRt8888: case xe::kRt8888Gamma: return VK_FORMAT_R8G8B8A8_UNORM;
        case xe::kRt2_10_10_10: case xe::kRt2_10_10_10As10: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case xe::kRt16_16: case xe::kRt16_16Float: return VK_FORMAT_R16G16_SFLOAT;
        case xe::kRt32Float: return VK_FORMAT_R32_SFLOAT;
        case xe::kRt32_32Float: return VK_FORMAT_R32G32_SFLOAT;
        default: return VK_FORMAT_R16G16B16A16_SFLOAT;  // 16_16_16_16 (+float), 2_10_10_10_FLOAT
    }
}
uint32_t VkFormatBytes(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: return 8;
        default: return 4;
    }
}
bool IsRt64bpp(uint32_t f) { return f == xe::kRt16_16_16_16 || f == xe::kRt16_16_16_16Float || f == xe::kRt32_32Float; }

// Host image texel -> RGBA float.
void ReadbackToFloat(VkFormat f, const uint8_t* p, float out[4]) {
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM:
            for (int c = 0; c < 4; c++) out[c] = p[c] / 255.0f;
            break;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
            uint32_t v;
            std::memcpy(&v, p, 4);
            out[0] = (v & 1023) / 1023.0f;
            out[1] = ((v >> 10) & 1023) / 1023.0f;
            out[2] = ((v >> 20) & 1023) / 1023.0f;
            out[3] = (v >> 30) / 3.0f;
            break;
        }
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            for (int c = 0; c < 4; c++) { uint16_t h; std::memcpy(&h, p + 2 * c, 2); out[c] = HalfToFloat(h); }
            break;
        case VK_FORMAT_R16G16_SFLOAT:
            for (int c = 0; c < 2; c++) { uint16_t h; std::memcpy(&h, p + 2 * c, 2); out[c] = HalfToFloat(h); }
            break;
        case VK_FORMAT_R32_SFLOAT: std::memcpy(out, p, 4); break;
        case VK_FORMAT_R32G32_SFLOAT: std::memcpy(out, p, 8); break;
        default: break;
    }
}

// RB_COLOR_CLEAR (+ _LO for 64bpp) in the render target's format -> float RGBA.
void ClearColorFloat(uint32_t rtFormat, uint32_t hi, uint32_t lo, float out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    switch (rtFormat) {
        case xe::kRt8888: case xe::kRt8888Gamma:
            for (int c = 0; c < 4; c++) out[c] = ((hi >> (8 * c)) & 0xFF) / 255.0f;
            break;
        case xe::kRt2_10_10_10: case xe::kRt2_10_10_10As10:
            out[0] = (hi & 1023) / 1023.0f; out[1] = ((hi >> 10) & 1023) / 1023.0f;
            out[2] = ((hi >> 20) & 1023) / 1023.0f; out[3] = (hi >> 30) / 3.0f;
            break;
        case xe::kRt16_16Float:
            out[0] = HalfToFloat(uint16_t(hi)); out[1] = HalfToFloat(uint16_t(hi >> 16));
            break;
        case xe::kRt16_16:
            out[0] = float(int16_t(hi)) / 32767.0f; out[1] = float(int16_t(hi >> 16)) / 32767.0f;
            if (Rt1616Scaled()) { out[0] *= 32.0f; out[1] *= 32.0f; }
            break;
        case xe::kRt16_16_16_16Float:
            out[0] = HalfToFloat(uint16_t(lo)); out[1] = HalfToFloat(uint16_t(lo >> 16));
            out[2] = HalfToFloat(uint16_t(hi)); out[3] = HalfToFloat(uint16_t(hi >> 16));
            break;
        case xe::kRt16_16_16_16:
            out[0] = float(int16_t(lo)) / 32767.0f; out[1] = float(int16_t(lo >> 16)) / 32767.0f;
            out[2] = float(int16_t(hi)) / 32767.0f; out[3] = float(int16_t(hi >> 16)) / 32767.0f;
            if (Rt1616Scaled()) for (int c = 0; c < 4; c++) out[c] *= 32.0f;
            break;
        case xe::kRt32Float: out[0] = xe::AsFloat(hi); break;
        case xe::kRt32_32Float: out[0] = xe::AsFloat(lo); out[1] = xe::AsFloat(hi); break;
        default:
            for (int c = 0; c < 4; c++) out[c] = ((hi >> (8 * c)) & 0xFF) / 255.0f;
            break;
    }
}

VkBlendFactor BlendFactor(uint32_t f) {
    switch (f) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 4: return VK_BLEND_FACTOR_SRC_COLOR;
        case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 8: return VK_BLEND_FACTOR_DST_COLOR;
        case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 10: return VK_BLEND_FACTOR_DST_ALPHA;
        case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        default: return VK_BLEND_FACTOR_ONE;
    }
}
VkBlendOp BlendOp(uint32_t op) {
    switch (op) {
        case 1: return VK_BLEND_OP_SUBTRACT;
        case 2: return VK_BLEND_OP_MIN;
        case 3: return VK_BLEND_OP_MAX;
        case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
        default: return VK_BLEND_OP_ADD;
    }
}
VkSamplerAddressMode AddressMode(uint32_t m, bool mirrorClampEdge) {
    switch (m) {
        case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 2: case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 3: return mirrorClampEdge ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 6: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    }
}

// Rectangle lists: each triangle is three corners of a rectangle; the fourth is emitted here.
// Interpolators are separate variables per location (matching the translated shaders).
std::string RectGsSource() {
    std::string g = "#version 450\nlayout(triangles) in;\nlayout(triangle_strip, max_vertices = 4) out;\n";
    for (int k = 0; k < 16; k++) {
        g += "layout(location = " + std::to_string(k) + ") in vec4 xe_in" + std::to_string(k) + "[];\n";
        g += "layout(location = " + std::to_string(k) + ") out vec4 xe_out" + std::to_string(k) + ";\n";
    }
    g += "void Put(int i) {\n  gl_Position = gl_in[i].gl_Position;\n";
    for (int k = 0; k < 16; k++) g += "  xe_out" + std::to_string(k) + " = xe_in" + std::to_string(k) + "[i];\n";
    g += "  EmitVertex();\n}\n";
    g += R"(void main() {
  // The longest edge is the diagonal; the corner opposite it is the right angle.
  vec2 p0 = gl_in[0].gl_Position.xy / gl_in[0].gl_Position.w;
  vec2 p1 = gl_in[1].gl_Position.xy / gl_in[1].gl_Position.w;
  vec2 p2 = gl_in[2].gl_Position.xy / gl_in[2].gl_Position.w;
  float d01 = dot(p1 - p0, p1 - p0), d12 = dot(p2 - p1, p2 - p1), d20 = dot(p0 - p2, p0 - p2);
  int c = 2, a = 0, b = 1;
  if (d12 >= d01 && d12 >= d20) { c = 0; a = 1; b = 2; }
  else if (d20 >= d01) { c = 1; a = 2; b = 0; }
  Put(c); Put(a); Put(b);
  gl_Position = gl_in[a].gl_Position + gl_in[b].gl_Position - gl_in[c].gl_Position;
)";
    for (int k = 0; k < 16; k++) {
        std::string n = std::to_string(k);
        g += "  xe_out" + n + " = xe_in" + n + "[a] + xe_in" + n + "[b] - xe_in" + n + "[c];\n";
    }
    g += "  EmitVertex();\n}\n";
    return g;
}

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* map = nullptr;
    VkDeviceSize size = 0;
};

struct Image {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize memOff = 0, memSize = 0;  // suballocation in a pooled block (memSize 0: a dedicated allocation)
    uint32_t memType = 0;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0, layers = 1, levels = 1;
};

struct RenderTarget {
    Image image;
    bool depth = false;
    uint32_t xeFormat = 0;
    uint32_t base = 0;  // EDRAM tile
};

struct Shader {
    TranslatedShader info;
    VkShaderModule module = VK_NULL_HANDLE;
    bool ok = false;
};

struct Texture {
    Image image;
    uint64_t contentHash = 0;
    uint64_t validatedGen = ~0ull;
    uint32_t guestBase = 0, guestSize = 0;
    bool ok = false;
};

#pragma pack(push, 1)
struct PipelineKey {
    uint64_t vs = 0, ps = 0;
    uint32_t prim = 0;
    uint32_t colorFormats[4] = {};
    uint32_t depthFormat = 0;
    uint32_t blend[4] = {};
    uint32_t colorMask = 0;
    uint32_t depthControl = 0;  // masked to the used bits
    uint32_t cull = 0, frontFace = 0, depthBias = 0;
};
#pragma pack(pop)

}  // namespace

// =============================================================================================
struct VulkanRenderer::Impl {
    GuestMemory& mem;
    Backend* next;
    Options opt;
    Stats stats;
    bool ready = false;
    bool broken = false;

    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties memProps{};
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t qfam = 0;
    bool haveGs = false, independentBlend = false, mirrorClampEdge = false;
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkFormat floatTexFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool recording = false;

    Buffer ring;
    VkDeviceSize ringPos = 0;
    Buffer readback;

    VkDescriptorSetLayout set0Layout = VK_NULL_HANDLE, set1Layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkDescriptorPool set0Pool = VK_NULL_HANDLE, texPool = VK_NULL_HANDLE;
    VkDescriptorSet set0 = VK_NULL_HANDLE;
    uint32_t texSetsUsed = 0;
    VkPipelineCache pipeCache = VK_NULL_HANDLE;

    shaderc_compiler_t compiler = nullptr;
    std::unordered_map<uint64_t, Shader> shaders;
    VkShaderModule rectGs = VK_NULL_HANDLE;
    std::unordered_map<std::string, VkPipeline> pipelines;
    struct TexKey { uint32_t binding; VkSampler smp; VkImageView view; };
    VkDescriptorSet lastTexSet = VK_NULL_HANDLE;  // texture set of the previous draw in this batch
    std::vector<TexKey> lastTexKey;
    std::unordered_map<uint64_t, VkDescriptorSet> texSetCache;  // every texture set built in this batch, by hash of its (binding, sampler, view) list
    HostDraw hostDraw;  // reused across draws so its index buffer keeps its capacity
    std::map<uint64_t, RenderTarget> rts;
    std::unordered_map<uint64_t, Texture> textures;
    std::unordered_map<uint64_t, VkSampler> samplers;
    Image dummy2D, dummyCube;
    std::vector<Image> garbageImages;  // freed after the batch completes

    // Rendering state.
    bool renderingActive = false;
    RenderTarget* curColor[4] = {};
    RenderTarget* curDepth = nullptr;
    uint32_t curW = 0, curH = 0;
    VkPipeline curPipeline = VK_NULL_HANDLE;
    // FA_WATCH_ADDR=<hex physical address>: log the guest bytes at that surface after every resolve into it
    // and every time a texture fetch binds it (with whether the cached copy was reused).
    uint32_t watchAddr = [] { const char* e = std::getenv("FA_WATCH_ADDR"); return e ? uint32_t(std::strtoul(e, nullptr, 16) & 0x1FFFFFFFu) : 0u; }();
    void WatchBytes(const char* what, uint32_t base, uint32_t size, const char* extra = "") {
        if (!watchAddr || (base & 0x1FFFFFFFu) != watchAddr) return;
        static const uint64_t fromFrame = [] { const char* e = std::getenv("FA_WATCH_FROM_FRAME"); return e ? std::strtoull(e, nullptr, 10) : 0ull; }();
        static const int maxEvents = [] { const char* e = std::getenv("FA_WATCH_MAX"); return e ? std::atoi(e) : 400; }();
        if (frame < fromFrame) return;
        static int logged = 0;
        if (logged++ >= maxEvents) return;
        const uint8_t* p = mem.HostPtr(base, size);
        if (!p) { std::fprintf(stderr, "[watch] frame %llu draws %llu %s %08x: not mapped (%u bytes) %s\n", (unsigned long long)frame, (unsigned long long)stats.draws.load(), what, base, size, extra); return; }
        std::fprintf(stderr, "[watch] frame %llu draws %llu %s %08x size %u hash %016llx %s\n  @0:", (unsigned long long)frame,
                     (unsigned long long)stats.draws.load(), what, base, size, (unsigned long long)HashBytes(p, size), extra);
        const uint32_t offs[3] = {0, (size / 2) & ~15u, ((size / 4) * 3) & ~15u};
        for (int k = 0; k < 3; k++) {
            if (k) std::fprintf(stderr, "\n  @%x:", offs[k]);
            for (int i = 0; i < 16; i++) std::fprintf(stderr, "%s%02x", i % 4 ? "" : " ", p[offs[k] + i]);
        }
        std::fprintf(stderr, "\n");
    }
    // FA_SNAPLOG=1: log (once per base and reason) why a resolve was not kept as a GPU snapshot and why a
    // texture fetch of a snapshotted surface could not use it. These are the per-frame render target re-uploads.
    static void SnapLog(const char* kind, const char* why, uint32_t base, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) {
        static const bool on = [] { const char* v = std::getenv("FA_SNAPLOG"); return v && v[0] == '1'; }();
        if (!on) return;
        static std::set<uint64_t> seen;
        static int n = 0;
        uint64_t k = (uint64_t(base) << 24) ^ (uint64_t(why[0]) << 16) ^ (uint64_t(why[1]) << 8) ^ uint64_t(kind[0]) ^ (uint64_t(a) << 48);
        if (n < 400 && seen.insert(k).second) {
            n++;
            std::fprintf(stderr, "[snaplog] %s %s base %08x: %u %u %u %u %u %u\n", kind, why, base, a, b, c, d, e, f);
        }
    }
    uint64_t generation = 1;  // bumped once per frame: textures are rechecked against guest memory

    // A resolve wrote [base, base + size): textures overlapping it are rechecked on next use.
    // Other textures keep their check from earlier in the frame (the CPU writes them between frames).
    void InvalidateTextures(uint32_t base, uint32_t size) {
        const uint64_t lo = base & 0x1FFFFFFFu, hi = lo + size;
        for (auto& [k, e] : snaps)
            if (e.valid && uint64_t(e.base) < hi && lo < uint64_t(e.base) + e.size) e.valid = false;
        for (auto& [k, t] : textures) {
            const uint64_t tlo = t.guestBase & 0x1FFFFFFFu, thi = tlo + t.guestSize;
            if (tlo < hi && lo < thi) t.validatedGen = ~0ull;
        }
    }

    // ---- resolved surfaces kept on the GPU ---------------------------------------------------------
    // A resolve of an 8888 target to guest memory also copies the pixels into an image of its own. A
    // texture fetch that reads exactly that surface samples the image instead of hashing, decoding and
    // uploading the guest bytes again (FA_RESOLVE_GPU=0 turns this off).
    // Guest texture format classes a snapshot can stand in for: 8888, 2_10_10_10, 32_FLOAT, 16_16_FLOAT (all 4 bytes per texel).
    static int SnapClass(uint32_t f) {
        return (f == 6 || f == 14 || f == 50) ? 1 : (f == 7 || f == 54) ? 2 : f == 36 ? 3 : f == 31 ? 4 : 0;
    }
    // Host render target formats whose resolve to guest format `f` can be kept as a snapshot (ResolveRaw converts on writeback).
    static bool SnapResolvable(VkFormat src, uint32_t f) {
        switch (SnapClass(f)) {
            case 1: case 2: return src == VK_FORMAT_R8G8B8A8_UNORM || src == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            case 3: return src == VK_FORMAT_R32_SFLOAT;
            case 4: return src == VK_FORMAT_R16G16_SFLOAT;
        }
        return false;
    }
    struct Snap {
        Image img;
        std::unordered_map<uint32_t, VkImageView> views;  // by fetch swizzle
        // Textures a few texels smaller than the (8-aligned) resolve rectangle sample a cropped copy.
        struct Crop {
            Image img;
            uint64_t version = 0;
            std::unordered_map<uint32_t, VkImageView> views;
        };
        std::unordered_map<uint32_t, Crop> crops;  // by width << 16 | height
        uint64_t version = 0;                       // bumped whenever the snapshot is rewritten
        uint64_t checkedGen = 0, checkedVer = ~0ull;  // frame and version at which the guest memory samples last matched
        bool valid = false;
        uint32_t base = 0, size = 0, pitch = 0, endian = 0, w = 0, h = 0;
        bool swapRB = false;
        bool pending = false;       // guest memory not written yet (see FlushPending)
        ResolveDest dest{};         // how to write it when it is
        uint32_t samples[64] = {};  // dwords of the guest surface when it was resolved
    };
    std::unordered_map<uint32_t, Snap> snaps;  // by destination base (masked to 512 MB)
    std::vector<VkImageView> garbageViews;
    const bool snapEnabled = [] { const char* e = std::getenv("FA_RESOLVE_GPU"); return !(e && e[0] == '0'); }();

    static bool SnapSample(const uint8_t* p, uint32_t size, uint32_t out[64]) {
        const uint32_t words = size / 4;
        if (words < 64) return false;
        for (uint32_t i = 0; i < 64; i++) std::memcpy(&out[i], p + size_t(i) * (words / 64) * 4, 4);
        return true;
    }
    void ReleaseSnapImage(Snap& e) {
        for (auto& [k, v] : e.views) garbageViews.push_back(v);
        e.views.clear();
        for (auto& [k, c] : e.crops) {
            for (auto& [sw, v] : c.views) garbageViews.push_back(v);
            if (c.img.img) garbageImages.push_back(c.img);
        }
        e.crops.clear();
        if (e.img.img) garbageImages.push_back(e.img);
        e.img = Image{};
        e.valid = false;
    }
    // Lazy writeback: snapshotted resolves do not read their pixels back right away. The snapshot
    // images hold the data and the guest memory is written in one batch (one GPU wait) when something
    // can observe it: a swap, a fence / interrupt / memory write from the command stream, a texture
    // fetch that cannot use a snapshot, or a resolve that overlaps a pending one.
    // Snapshotted resolves are written into guest memory in batches at sync points; FA_RESOLVE_LAZY=0 writes each one right away.
    const bool lazyWriteback = [] { const char* e = std::getenv("FA_RESOLVE_LAZY"); return !(e && e[0] == '0'); }();
    std::vector<uint32_t> pendingKeys;
    uint64_t pendingBytes = 0;
    bool PendingOverlaps(uint32_t base, uint32_t size) {
        const uint64_t lo = base & 0x1FFFFFFFu, hi = lo + size;
        for (uint32_t k : pendingKeys) {
            const Snap& e = snaps[k];
            if (uint64_t(e.base) < hi && lo < uint64_t(e.base) + e.size) return true;
        }
        return false;
    }
    // Copies the resolved rectangle of `src` into the snapshot of `key` and marks it pending.
    bool SnapStore(RenderTarget* src, const ResolveDest& d, uint32_t destBytes, uint32_t key, uint32_t w, uint32_t h) {
        Snap& e = snaps[key];
        const VkFormat fmt = src->image.format;
        // A pending surface of another base overlapping this one is written first (order matters).
        for (uint32_t k : pendingKeys) {
            const Snap& o = snaps[k];
            if (k != key && uint64_t(o.base) < uint64_t(key) + destBytes && uint64_t(key) < uint64_t(o.base) + o.size) { FlushPending(prof::kFlushOverlap); break; }
        }
        if (e.img.img && (e.img.w != w || e.img.h != h || e.img.format != fmt)) {
            if (e.pending) FlushPending(prof::kFlushOverlap);
            ReleaseSnapImage(e);
        }
        if (!e.img.img) {
            if (!CreateImage(e.img, fmt, w, h, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT, false, VK_IMAGE_ASPECT_COLOR_BIT)) {
                ReleaseSnapImage(e);
                return false;
            }
            ImageToGeneral(e.img.img, VK_IMAGE_ASPECT_COLOR_BIT, 1);
        }
        const uint8_t* gp = mem.HostPtr(d.base, destBytes);
        uint32_t samples[64];
        if (!gp || !SnapSample(gp, destBytes, samples)) return false;
        InvalidateTextures(d.base, destBytes);  // other textures over this memory are stale (may drop a pending write of e)
        Barrier();
        VkImageCopy ic{};
        ic.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        ic.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        ic.extent = {w, h, 1};
        vkCmdCopyImage(cmd, src->image.img, VK_IMAGE_LAYOUT_GENERAL, e.img.img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
        Barrier();
        if (!e.pending) {
            e.pending = true;
            pendingKeys.push_back(key);
        }
        pendingBytes += uint64_t(w) * h * 4;
        e.base = key;
        e.size = destBytes;
        e.pitch = (std::max(d.pitch, 32u) + 31) & ~31u;
        e.endian = d.endian;
        e.swapRB = d.swapRB;
        e.w = w;
        e.h = h;
        e.dest = d;
        std::memcpy(e.samples, samples, sizeof samples);
        e.version++;
        e.valid = true;
        if (pendingBytes > (96u << 20)) FlushPending(prof::kFlushCap);
        return true;
    }
    void FlushPending(prof::Stat why = prof::kFlushSync) {
        if (pendingKeys.empty()) return;
        prof::Add(why, 1);
        std::vector<uint32_t> keys;
        keys.swap(pendingKeys);
        pendingBytes = 0;
        VkDeviceSize total = 0;
        std::vector<VkDeviceSize> offs;
        for (uint32_t k : keys) {
            offs.push_back(total);
            total += (VkDeviceSize(snaps[k].w) * snaps[k].h * 4 + 15) & ~VkDeviceSize(15);
        }
        EndRendering();
        if (total > readback.size) {
            SubmitAndWait();
            Begin();
            DestroyBuffer(readback);
            CreateBuffer(readback, total, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        }
        Begin();
        Barrier();
        for (size_t i = 0; i < keys.size(); i++) {
            Snap& e = snaps[keys[i]];
            VkBufferImageCopy bic{};
            bic.bufferOffset = offs[i];
            bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            bic.imageExtent = {e.w, e.h, 1};
            vkCmdCopyImageToBuffer(cmd, e.img.img, VK_IMAGE_LAYOUT_GENERAL, readback.buf, 1, &bic);
        }
        Barrier();
        {
            prof::Scope pw_(prof::kResolveWait);
            SubmitAndWait();
        }
        Begin();
        prof::Scope pc_(prof::kResolveCpu);
        const uint8_t* rb = static_cast<const uint8_t*>(readback.map);
        for (size_t i = 0; i < keys.size(); i++) {
            Snap& e = snaps[keys[i]];
            e.pending = false;
            if (!e.valid) continue;  // overwritten since: nothing to write
            const uint32_t destBytes = ResolveDestByteSize(e.dest);
            if (ResolveRaw(e.dest, e.img.format, false, 0, 0, e.w, e.h, rb + offs[i], destBytes)) {
                const bool keep = e.valid;
                InvalidateTextures(e.dest.base, destBytes);  // other textures over this memory are stale now
                e.valid = keep;
                if (const uint8_t* gp = mem.HostPtr(e.dest.base, destBytes)) SnapSample(gp, destBytes, e.samples);
            } else {
                e.valid = false;
            }
        }
    }

    // View of a still valid snapshot that matches this texture fetch, or null.
    VkImageView SnapView(const TextureFetch& t) {
        auto it = snaps.find(t.base & 0x1FFFFFFFu);
        if (it == snaps.end()) return VK_NULL_HANDLE;
        if (!it->second.valid) { SnapLog("fetch", "snapshot invalid", t.base & 0x1FFFFFFFu, t.format, t.width, t.height, 0, 0, 0); return VK_NULL_HANDLE; }
        Snap& e = it->second;
        const bool fmtOk = SnapClass(t.format) && SnapClass(t.format) == SnapClass(e.dest.format);
        auto rej = [&](const char* why) { SnapLog("fetch", why, t.base & 0x1FFFFFFFu, t.format, t.width, t.height, e.w, e.h, t.pitch); return VK_NULL_HANDLE; };
        if (!t.tiled || t.dimension != 1 || !fmtOk) return rej("tile/dim/fmt");
        if (t.endian != e.endian || t.expAdjust != 0) return rej("endian/exp");
        // The resolve rectangle is rounded up to 8 texels; the texture may be a little smaller.
        if (t.width > e.w || t.height > e.h || e.w - t.width >= 32 || e.h - t.height >= 32) return rej("size");
        for (int c = 0; c < 4; c++) if (t.sign[c]) return rej("sign");
        if (((std::max(t.pitch ? t.pitch : t.width, 32u) + 31) & ~31u) != e.pitch) return rej("pitch");
        const uint8_t* src = mem.HostPtr(t.base, e.size);
        if (!src) return VK_NULL_HANDLE;
        // Sampling 64 dwords spread over the surface costs a cache miss each: once per frame and snapshot version
        // is enough (the CPU writes textures between frames, as for ordinary textures).
        if (e.checkedGen != generation || e.checkedVer != e.version) {
            uint32_t cur[64];
            if (!SnapSample(src, e.size, cur) || std::memcmp(cur, e.samples, sizeof cur)) { e.valid = false; return rej("samples changed"); }
            e.checkedGen = generation;
            e.checkedVer = e.version;
        }
        VkImage image = e.img.img;
        std::unordered_map<uint32_t, VkImageView>* views = &e.views;
        if (t.width != e.w || t.height != e.h) {
            Snap::Crop& c = e.crops[(t.width << 16) | t.height];
            Begin();
            if (!c.img.img) {
                if (!CreateImage(c.img, e.img.format, t.width, t.height, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 false, VK_IMAGE_ASPECT_COLOR_BIT)) {
                    DestroyImage(c.img);
                    e.crops.erase((t.width << 16) | t.height);
                    return VK_NULL_HANDLE;
                }
                EndRendering();
                ImageToGeneral(c.img.img, VK_IMAGE_ASPECT_COLOR_BIT, 1);
            }
            if (c.version != e.version) {
                EndRendering();
                Barrier();
                VkImageCopy ic{};
                ic.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                ic.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                ic.extent = {t.width, t.height, 1};
                vkCmdCopyImage(cmd, e.img.img, VK_IMAGE_LAYOUT_GENERAL, c.img.img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
                Barrier();
                c.version = e.version;
            }
            image = c.img.img;
            views = &c.views;
        }
        auto v = views->find(t.swizzle);
        if (v != views->end()) return v->second;
        VkComponentMapping cm;
        VkComponentSwizzle* comps[4] = {&cm.r, &cm.g, &cm.b, &cm.a};
        for (int c = 0; c < 4; c++) {
            uint32_t sw = (t.swizzle >> (3 * c)) & 7;
            if (e.swapRB && sw < 4 && sw != 1 && sw != 3) sw = sw == 0 ? 2 : 0;  // the resolve swapped R and B
            *comps[c] = sw < 4 ? VkComponentSwizzle(VK_COMPONENT_SWIZZLE_R + sw)
                      : sw == 5 ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_ZERO;
        }
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = e.img.format;
        vi.components = cm;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(dev, &vi, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
        (*views)[t.swizzle] = view;
        return view;
    }

    std::vector<float> resolvePx;  // scratch for resolves that go through floats
    RowPool rowPool;

    // Per-batch upload dedup.
    std::unordered_map<uint64_t, VkDeviceSize> vbCache;
    std::vector<uint32_t> lastConsts;
    VkDeviceSize lastConstsOff = ~0ull;
    int drawsTraced = 0;
    uint64_t frame = 0;  // swaps seen
    // FA_RENDER_DUMP=<dir>: every 30th frame (12 of them) gets all its draws logged and all its
    // resolves dumped, so the dumps line up with FrameBackend's front buffer dumps.
    const char* dumpDir = nullptr;
    bool dbgNoAtest = false, dbgNoBlend = false;
    // FA_RENDER_PROBE=frame[-last],x,y[,x,y...]: in those frames, after every draw, read back those screen pixels and log
    // the draws that changed it, with their full trace ("who painted this pixel").
    bool probeOn = false;
    bool probeOnTrigger = false;  // probe the frame the dump trigger fires on
    uint64_t probeFrame = 0, probeFrameEnd = 0;  // inclusive range
    std::vector<std::pair<int, int>> probePts;
    std::vector<std::array<float, 4>> probeLast;   // per point: value after the latest draw covering it
    std::vector<uint64_t> probeLastDraw;           // per point: that draw's number
    std::set<std::pair<uint64_t, uint64_t>> probeTraced;  // (vs, ps) pairs whose full trace was already printed
    uint64_t dumpCount = 12;  // FA_RENDER_DUMP_COUNT: how many 30-frame steps get dumped
    // FA_RENDER_DUMP_FRAME=N dumps swap N (all resolves, textures and targets), restricted to draws
    // A..B of that frame with FA_RENDER_DUMP_DRAWS=A-B (draw numbers count from 0 in each frame).
    // FA_RENDER_DUMP=dir is still required. This reaches the 3D menu without dumping GBs.
    int64_t dumpFrameSel = -1;
    uint64_t dumpDrawLo = 0, dumpDrawHi = ~0ull, drawInFrame = 0;
    std::set<uint32_t> dumpedTexBases;
    // FA_RENDER_DUMP_TRIGGER=<file>: when that file appears (touch it), the next frame is dumped like
    // FA_RENDER_DUMP_FRAME and the file is removed. FA_RENDER_DUMP_AFTER=<seconds>: the same, once,
    // that many seconds after start. Both need FA_RENDER_DUMP=<dir>.
    const char* dumpTrigger = nullptr;
    double dumpAfterSec = -1.0;
    std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    bool DumpFrame() const {
        if (!dumpDir) return false;
        if (dumpFrameSel >= 0) return uint64_t(dumpFrameSel) == frame;
        return frame < 6 || (frame % 30 == 0 && frame / 30 < dumpCount);
    }
    bool DumpDraw() const {
        if (!DumpFrame()) return false;
        if (dumpFrameSel >= 0) return drawInFrame >= dumpDrawLo && drawInFrame <= dumpDrawHi;
        return true;
    }

    Impl(GuestMemory& m, Backend* n) : mem(m), next(n) {
        const char* d = std::getenv("FA_RENDER_DUMP");
        if (d && *d) dumpDir = d;
        if (const char* c = std::getenv("FA_RENDER_DUMP_COUNT")) dumpCount = std::strtoull(c, nullptr, 10);
        if (const char* c = std::getenv("FA_RENDER_DUMP_TRIGGER")) if (*c) dumpTrigger = c;
        if (const char* c = std::getenv("FA_RENDER_DUMP_AFTER")) dumpAfterSec = std::atof(c);
        if (const char* c = std::getenv("FA_RENDER_DUMP_FRAME")) dumpFrameSel = std::strtoll(c, nullptr, 10);
        if (const char* c = std::getenv("FA_RENDER_DUMP_DRAWS")) {
            char* e = nullptr;
            dumpDrawLo = std::strtoull(c, &e, 10);
            dumpDrawHi = (e && *e == '-') ? std::strtoull(e + 1, nullptr, 10) : dumpDrawLo;
        }
        // FA_DBG_RENDER=noatest,noblend: debugging switches that override the guest's alpha test / blending.
        if (const char* c = std::getenv("FA_RENDER_PROBE")) {
            char* e = nullptr;
            if (std::strncmp(c, "trigger", 7) == 0) {  // FA_RENDER_PROBE=trigger,x,y,...: armed by FA_RENDER_DUMP_TRIGGER
                probeOnTrigger = true;
                probeFrame = probeFrameEnd = ~0ull;
                e = const_cast<char*>(c + 7);
            } else {
                probeFrame = std::strtoull(c, &e, 10);
                probeFrameEnd = probeFrame;
                if (e && *e == '-') probeFrameEnd = std::strtoull(e + 1, &e, 10);
            }
            while (e && *e == ',') {
                long x = std::strtol(e + 1, &e, 10);
                if (!e || *e != ',') break;
                long y = std::strtol(e + 1, &e, 10);
                probePts.push_back({int(x), int(y)});
            }
            probeOn = !probePts.empty();
            probeLast.assign(probePts.size(), {-1.0f, -1.0f, -1.0f, -1.0f});
            probeLastDraw.assign(probePts.size(), 0);
        }
        if (const char* c = std::getenv("FA_DBG_RENDER")) {
            dbgNoAtest = std::strstr(c, "noatest") != nullptr;
            dbgNoBlend = std::strstr(c, "noblend") != nullptr;
        }
    }

    bool Fail(std::string& err, const char* what, VkResult r) {
        char b[256];
        std::snprintf(b, sizeof b, "%s failed: %s (%d)", what, VkErr(r), int(r));
        err = b;
        return false;
    }
    void Check(VkResult r, const char* what) {
        if (r != VK_SUCCESS && !broken) {
            std::fprintf(stderr, "[render] %s failed: %s (%d); renderer disabled\n", what, VkErr(r), int(r));
            broken = true;
        }
    }

    int FindMemory(uint32_t bits, VkMemoryPropertyFlags want) {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & want) == want) return int(i);
        return -1;
    }

    bool CreateBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = usage;
        if (vkCreateBuffer(dev, &bi, nullptr, &b.buf) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, b.buf, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        int mt = -1;
        if (hostVisible) {
            mt = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                    VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
            if (mt < 0) mt = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        } else {
            mt = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        }
        if (mt < 0) return false;
        ai.memoryTypeIndex = uint32_t(mt);
        if (vkAllocateMemory(dev, &ai, nullptr, &b.mem) != VK_SUCCESS) return false;
        vkBindBufferMemory(dev, b.buf, b.mem, 0);
        if (hostVisible && vkMapMemory(dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map) != VK_SUCCESS) return false;
        b.size = size;
        return true;
    }
    void DestroyBuffer(Buffer& b) {
        if (b.buf) vkDestroyBuffer(dev, b.buf, nullptr);
        if (b.mem) vkFreeMemory(dev, b.mem, nullptr);
        b = Buffer{};
    }

    // Image memory comes from big device-local blocks, carved into size classes whose freed chunks are
    // reused. One vkAllocateMemory per texture made a first-use burst of ~700 textures take seconds.
    struct PoolBlock { VkDeviceMemory mem; VkDeviceSize size, used; uint32_t type; };
    std::vector<PoolBlock> poolBlocks;
    struct PoolChunk { VkDeviceMemory mem; VkDeviceSize off; };
    std::map<uint64_t, std::vector<PoolChunk>> poolFree;
    static uint64_t PoolKey(uint32_t type, VkDeviceSize cls) { return (uint64_t(type) << 48) ^ cls; }
    bool PoolAlloc(Image& im, const VkMemoryRequirements& req, uint32_t type) {
        constexpr VkDeviceSize kBlock = 64ull << 20, kMaxChunk = 16ull << 20, kGrain = 65536;
        if (req.alignment > kGrain || req.size > kMaxChunk) return false;
        // Classes: four steps per power of two, in multiples of 64 KB.
        VkDeviceSize cls = kGrain;
        while (cls < req.size) cls += std::max(kGrain, VkDeviceSize(1) << (63 - __builtin_clzll(cls) - 2));
        cls = (cls + kGrain - 1) / kGrain * kGrain;
        auto& fl = poolFree[PoolKey(type, cls)];
        if (!fl.empty()) {
            PoolChunk c = fl.back();
            fl.pop_back();
            im.mem = c.mem; im.memOff = c.off;
        } else {
            PoolBlock* b = nullptr;
            for (auto& pb : poolBlocks) if (pb.type == type && pb.size - pb.used >= cls) { b = &pb; break; }
            if (!b) {
                VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                ai.allocationSize = kBlock;
                ai.memoryTypeIndex = type;
                VkDeviceMemory m;
                if (vkAllocateMemory(dev, &ai, nullptr, &m) != VK_SUCCESS) return false;
                poolBlocks.push_back({m, kBlock, 0, type});
                b = &poolBlocks.back();
            }
            im.mem = b->mem; im.memOff = b->used;
            b->used += cls;
        }
        im.memSize = cls;
        im.memType = type;
        return true;
    }

    bool CreateImage(Image& im, VkFormat fmt, uint32_t w, uint32_t h, uint32_t layers, VkImageUsageFlags usage, bool cube,
                     VkImageAspectFlags aspect, const VkComponentMapping* swz = nullptr, uint32_t levels = 1) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = fmt;
        ci.extent = {w, h, 1};
        ci.mipLevels = levels;
        ci.arrayLayers = layers;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(dev, &ci, nullptr, &im.img) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev, im.img, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        int mt = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt < 0) mt = FindMemory(req.memoryTypeBits, 0);
        ai.memoryTypeIndex = uint32_t(mt);
        if (!PoolAlloc(im, req, uint32_t(mt))) {
            if (vkAllocateMemory(dev, &ai, nullptr, &im.mem) != VK_SUCCESS) return false;
            im.memOff = 0;
            im.memSize = 0;
        }
        vkBindImageMemory(dev, im.img, im.mem, im.memOff);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = im.img;
        vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        if (swz) vi.components = *swz;
        vi.subresourceRange = {aspect, 0, levels, 0, layers};
        if (vkCreateImageView(dev, &vi, nullptr, &im.view) != VK_SUCCESS) return false;
        im.format = fmt;
        im.w = w;
        im.h = h;
        im.layers = layers;
        im.levels = levels;
        return true;
    }
    void DestroyImage(Image& im) {
        if (im.view) vkDestroyImageView(dev, im.view, nullptr);
        if (im.img) vkDestroyImage(dev, im.img, nullptr);
        if (im.mem) {
            if (im.memSize) poolFree[PoolKey(im.memType, im.memSize)].push_back({im.mem, im.memOff});
            else vkFreeMemory(dev, im.mem, nullptr);
        }
        im = Image{};
    }

    // ---- command recording / batches ------------------------------------------------------
    void Begin() {
        if (recording) return;
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
        recording = true;
        VkDescriptorSet s = set0;
        (void)s;
    }
    void Barrier() {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                             nullptr, 0, nullptr);
    }
    void ImageToGeneral(VkImage img, VkImageAspectFlags aspect, uint32_t layers, uint32_t levels = 1) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {aspect, 0, levels, 0, layers};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
    }
    void EndRendering() {
        if (!renderingActive) return;
        vkCmdEndRendering(cmd);
        renderingActive = false;
        curPipeline = VK_NULL_HANDLE;
        Barrier();
    }
    // Submits everything recorded so far and waits for it. All per-batch resources are reset.
    void SubmitAndWait() {
        if (!recording) return;
        EndRendering();
        Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
        recording = false;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        Check(vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit");
        Check(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        vkResetFences(dev, 1, &fence);
        vkResetCommandPool(dev, cmdPool, 0);
        vkResetDescriptorPool(dev, texPool, 0);
        texSetsUsed = 0;
        lastTexSet = VK_NULL_HANDLE;
        texSetCache.clear();
        curPipeline = VK_NULL_HANDLE;  // bindings do not carry over to the next command buffer
        ringPos = 0;
        vbCache.clear();
        lastConstsOff = ~0ull;
        for (auto& im : garbageImages) DestroyImage(im);
        garbageImages.clear();
        for (auto v : garbageViews) vkDestroyImageView(dev, v, nullptr);
        garbageViews.clear();
        stats.submits++;
        prof::Add(prof::kSubmits, 1);
    }

    // Linear allocation from the upload ring; flushes the batch when it runs out.
    VkDeviceSize RingAlloc(VkDeviceSize size, VkDeviceSize align, bool& flushed) {
        flushed = false;
        align = std::max<VkDeviceSize>(align, 16);
        VkDeviceSize at = (ringPos + align - 1) / align * align;
        if (at + size > ring.size) {
            SubmitAndWait();
            Begin();
            flushed = true;
            at = 0;
        }
        ringPos = at + size;
        return at;
    }

    // ---- init ------------------------------------------------------------------------------
    bool Init(const Options& o, std::string& err) {
        opt = o;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Full Auto renderer";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        const char* layer = "VK_LAYER_KHRONOS_validation";
        if (opt.validation) {
            uint32_t n = 0;
            vkEnumerateInstanceLayerProperties(&n, nullptr);
            std::vector<VkLayerProperties> lp(n);
            vkEnumerateInstanceLayerProperties(&n, lp.data());
            for (auto& l : lp)
                if (!std::strcmp(l.layerName, layer)) { ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = &layer; }
        }
        VkResult r = vkCreateInstance(&ici, nullptr, &inst);
        if (r != VK_SUCCESS) return Fail(err, "vkCreateInstance", r);

        uint32_t n = 0;
        vkEnumeratePhysicalDevices(inst, &n, nullptr);
        std::vector<VkPhysicalDevice> devs(n);
        vkEnumeratePhysicalDevices(inst, &n, devs.data());
        int best = -1, bestScore = -1;
        for (uint32_t i = 0; i < n; i++) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(devs[i], &p);
            if (p.apiVersion < VK_API_VERSION_1_3) continue;
            int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                      : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                      : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? 0 : 1;
            if (opt.deviceIndex == int(i)) score = 100;
            if (score > bestScore) { bestScore = score; best = int(i); }
        }
        if (best < 0) { err = "no Vulkan 1.3 device"; return false; }
        phys = devs[size_t(best)];
        vkGetPhysicalDeviceProperties(phys, &props);
        vkGetPhysicalDeviceMemoryProperties(phys, &memProps);

        vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, nullptr);
        std::vector<VkQueueFamilyProperties> qf(n);
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, qf.data());
        bool foundQ = false;
        for (uint32_t i = 0; i < n; i++)
            if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfam = i; foundQ = true; break; }
        if (!foundQ) { err = "no graphics queue"; return false; }

        VkPhysicalDeviceVulkan12Features have12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features have13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        have12.pNext = &have13;
        VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        have.pNext = &have12;
        vkGetPhysicalDeviceFeatures2(phys, &have);
        if (!have13.dynamicRendering) { err = "device lacks dynamic rendering"; return false; }

        VkPhysicalDeviceVulkan12Features use12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        use12.samplerMirrorClampToEdge = have12.samplerMirrorClampToEdge;
        mirrorClampEdge = have12.samplerMirrorClampToEdge;
        VkPhysicalDeviceVulkan13Features use13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        use13.dynamicRendering = VK_TRUE;
        use12.pNext = &use13;
        VkPhysicalDeviceFeatures2 use{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        use.pNext = &use12;
        use.features.geometryShader = have.features.geometryShader;
        use.features.independentBlend = have.features.independentBlend;
        haveGs = have.features.geometryShader;
        independentBlend = have.features.independentBlend;

        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = qfam;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext = &use;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        r = vkCreateDevice(phys, &dci, nullptr, &dev);
        if (r != VK_SUCCESS) return Fail(err, "vkCreateDevice", r);
        vkGetDeviceQueue(dev, qfam, 0, &queue);

        // Depth format: D32S8 where available (AMD has no D24S8), else D24S8.
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_D32_SFLOAT_S8_UINT, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) depthFormat = VK_FORMAT_D24_UNORM_S8_UINT;
        vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_R32G32B32A32_SFLOAT, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) floatTexFormat = VK_FORMAT_R32G32B32A32_SFLOAT;

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = qfam;
        r = vkCreateCommandPool(dev, &pci, nullptr, &cmdPool);
        if (r != VK_SUCCESS) return Fail(err, "vkCreateCommandPool", r);
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = cmdPool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev, &cai, &cmd);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(dev, &fci, nullptr, &fence);

        if (!CreateBuffer(ring, kRingSize,
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          true)) {
            err = "cannot allocate the upload buffer";
            return false;
        }
        if (!CreateBuffer(readback, 16u << 20, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) {
            err = "cannot allocate the readback buffer";
            return false;
        }

        // Descriptor layouts.
        VkDescriptorSetLayoutBinding b0[4] = {};
        for (uint32_t i = 0; i < 4; i++) {
            b0[i].binding = i;
            b0[i].descriptorType = i == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            b0[i].descriptorCount = 1;
            b0[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 4;
        lci.pBindings = b0;
        vkCreateDescriptorSetLayout(dev, &lci, nullptr, &set0Layout);
        std::vector<VkDescriptorSetLayoutBinding> b1(64);
        for (uint32_t i = 0; i < 64; i++) {
            b1[i].binding = i;
            b1[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b1[i].descriptorCount = 1;
            b1[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        lci.bindingCount = 64;
        lci.pBindings = b1.data();
        vkCreateDescriptorSetLayout(dev, &lci, nullptr, &set1Layout);
        VkDescriptorSetLayout layouts[2] = {set0Layout, set1Layout};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 2;
        plci.pSetLayouts = layouts;
        vkCreatePipelineLayout(dev, &plci, nullptr, &pipeLayout);

        VkDescriptorPoolSize ps0[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets = 1;
        dpci.poolSizeCount = 2;
        dpci.pPoolSizes = ps0;
        vkCreateDescriptorPool(dev, &dpci, nullptr, &set0Pool);
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = set0Pool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &set0Layout;
        vkAllocateDescriptorSets(dev, &dsai, &set0);
        VkDescriptorBufferInfo bufInfo[4] = {{ring.buf, 0, 8192}, {ring.buf, 0, 160}, {ring.buf, 0, sizeof(XeSystemConstants)},
                                             {ring.buf, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet w[4] = {};
        for (uint32_t i = 0; i < 4; i++) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = set0;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = b0[i].descriptorType;
            w[i].pBufferInfo = &bufInfo[i];
        }
        vkUpdateDescriptorSets(dev, 4, w, 0, nullptr);
        VkDescriptorPoolSize ps1 = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextureSets * 8};
        dpci.maxSets = kMaxTextureSets;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes = &ps1;
        vkCreateDescriptorPool(dev, &dpci, nullptr, &texPool);

        VkPipelineCacheCreateInfo pcci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        vkCreatePipelineCache(dev, &pcci, nullptr, &pipeCache);

        compiler = shaderc_compiler_initialize();
        if (!compiler) { err = "shaderc init failed"; return false; }
        if (haveGs) {
            std::vector<uint32_t> spv;
            std::string e;
            if (!CompileGlsl(RectGsSource(), shaderc_geometry_shader, "rect.geom", spv, e)) {
                std::fprintf(stderr, "[render] rectangle geometry shader: %s\n", e.c_str());
                haveGs = false;
            } else {
                rectGs = MakeModule(spv);
            }
        }

        Begin();
        // Dummy textures for slots whose fetch constant is unusable.
        const VkImageUsageFlags texUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        CreateImage(dummy2D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, texUsage, false, VK_IMAGE_ASPECT_COLOR_BIT);
        CreateImage(dummyCube, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 6, texUsage, true, VK_IMAGE_ASPECT_COLOR_BIT);
        ImageToGeneral(dummy2D.img, VK_IMAGE_ASPECT_COLOR_BIT, 1);
        ImageToGeneral(dummyCube.img, VK_IMAGE_ASPECT_COLOR_BIT, 6);
        VkClearColorValue black{};
        VkImageSubresourceRange rr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
        vkCmdClearColorImage(cmd, dummy2D.img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &rr);
        vkCmdClearColorImage(cmd, dummyCube.img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &rr);
        SubmitAndWait();
        Begin();
        ready = true;
        std::fprintf(stderr, "[render] Vulkan renderer on %s (geometry shaders %s, depth %s)\n", props.deviceName,
                     haveGs ? "yes" : "no", depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT ? "D32S8" : "D24S8");
        return true;
    }

    ~Impl() {
        if (dev) {
            vkDeviceWaitIdle(dev);
            for (auto& [k, s] : shaders) if (s.module) vkDestroyShaderModule(dev, s.module, nullptr);
            if (rectGs) vkDestroyShaderModule(dev, rectGs, nullptr);
            for (auto& [k, p] : pipelines) vkDestroyPipeline(dev, p, nullptr);
            for (auto& [k, rt] : rts) DestroyImage(rt.image);
            for (auto& [k, t] : textures) DestroyImage(t.image);
            for (auto& [k, s] : samplers) vkDestroySampler(dev, s, nullptr);
            for (auto& im : garbageImages) DestroyImage(im);
            for (auto v : garbageViews) vkDestroyImageView(dev, v, nullptr);
            for (auto& [k, e] : snaps) {
                for (auto& [ck, c] : e.crops) {
                    for (auto& [sw, v] : c.views) vkDestroyImageView(dev, v, nullptr);
                    DestroyImage(c.img);
                }
                for (auto& [sw, v] : e.views) vkDestroyImageView(dev, v, nullptr);
                DestroyImage(e.img);
            }
            DestroyImage(dummy2D);
            DestroyImage(dummyCube);
            DestroyBuffer(ring);
            DestroyBuffer(readback);
            if (pipeCache) vkDestroyPipelineCache(dev, pipeCache, nullptr);
            if (texPool) vkDestroyDescriptorPool(dev, texPool, nullptr);
            if (set0Pool) vkDestroyDescriptorPool(dev, set0Pool, nullptr);
            if (pipeLayout) vkDestroyPipelineLayout(dev, pipeLayout, nullptr);
            if (set0Layout) vkDestroyDescriptorSetLayout(dev, set0Layout, nullptr);
            if (set1Layout) vkDestroyDescriptorSetLayout(dev, set1Layout, nullptr);
            if (fence) vkDestroyFence(dev, fence, nullptr);
            if (cmdPool) vkDestroyCommandPool(dev, cmdPool, nullptr);
            for (auto& pb : poolBlocks) vkFreeMemory(dev, pb.mem, nullptr);
            poolBlocks.clear();
            poolFree.clear();
            vkDestroyDevice(dev, nullptr);
        }
        if (inst) vkDestroyInstance(inst, nullptr);
        if (compiler) shaderc_compiler_release(compiler);
    }

    // ---- shaders ---------------------------------------------------------------------------
    bool CompileGlsl(const std::string& src, shaderc_shader_kind kind, const char* name, std::vector<uint32_t>& spv,
                     std::string& err) {
        shaderc_compile_options_t o = shaderc_compile_options_initialize();
        shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
        shaderc_compile_options_set_optimization_level(o, shaderc_optimization_level_performance);
        shaderc_compilation_result_t r = shaderc_compile_into_spv(compiler, src.data(), src.size(), kind, name, "main", o);
        bool ok = shaderc_result_get_compilation_status(r) == shaderc_compilation_status_success;
        if (ok) {
            const uint32_t* w = reinterpret_cast<const uint32_t*>(shaderc_result_get_bytes(r));
            spv.assign(w, w + shaderc_result_get_length(r) / 4);
        } else {
            err = shaderc_result_get_error_message(r);
        }
        shaderc_result_release(r);
        shaderc_compile_options_release(o);
        return ok;
    }
    VkShaderModule MakeModule(const std::vector<uint32_t>& spv) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = spv.size() * 4;
        ci.pCode = spv.data();
        VkShaderModule m = VK_NULL_HANDLE;
        Check(vkCreateShaderModule(dev, &ci, nullptr, &m), "vkCreateShaderModule");
        return m;
    }
    Shader* GetShader(ShaderStage stage, const uint32_t* ucode, uint32_t dwords, uint64_t hash) {
        auto it = shaders.find(hash);
        if (it != shaders.end()) return &it->second;
        prof::Scope ps_(prof::kShader);
        Shader& s = shaders[hash];
        s.info = TranslateShader(stage, ucode, dwords);
        std::string err = s.info.error;
        if (s.info.ok) {
            std::vector<uint32_t> spv;
            char name[64];
            std::snprintf(name, sizeof name, "%s_%016llx", stage == ShaderStage::Vertex ? "vs" : "ps",
                          (unsigned long long)hash);
            std::string cerr;
            if (CompileGlsl(s.info.glsl, stage == ShaderStage::Vertex ? shaderc_vertex_shader : shaderc_fragment_shader,
                            name, spv, cerr)) {
                s.module = MakeModule(spv);
                s.ok = s.module != VK_NULL_HANDLE;
            } else {
                err += cerr;
            }
        }
        if (!opt.dumpShaders.empty()) {
            char path[512];
            std::snprintf(path, sizeof path, "%s/%s_%016llx.glsl", opt.dumpShaders.c_str(),
                          stage == ShaderStage::Vertex ? "vs" : "ps", (unsigned long long)hash);
            if (FILE* f = std::fopen(path, "w")) {
                std::fprintf(f, "/*\n%s%s%s*/\n%s", s.info.disasm.c_str(), err.empty() ? "" : "ERROR: ", err.c_str(),
                             s.info.glsl.c_str());
                std::fclose(f);
            }
        }
        if (s.ok) {
            stats.shadersCompiled++;
        } else {
            stats.shaderErrors++;
            if (stats.shaderErrors < 20)
                std::fprintf(stderr, "[render] %s shader %016llx failed: %.300s\n", stage == ShaderStage::Vertex ? "vertex" : "pixel",
                             (unsigned long long)hash, err.c_str());
        }
        return &s;
    }

    // ---- render targets ----------------------------------------------------------------------
    RenderTarget* GetRt(uint32_t base, uint32_t xeFormat, uint32_t pitch, uint32_t msaa, bool depth) {
        pitch = std::clamp<uint32_t>(pitch ? pitch : 1280, 16, 8192);
        // Formats with the same bits share one image, so a pass that writes k_2_10_10_10 and a
        // later one that reads the tile as k_2_10_10_10_AS_10_10_10_10 see the same pixels.
        if (!depth && xeFormat == xe::kRt2_10_10_10As10) xeFormat = xe::kRt2_10_10_10;
        if (!depth && xeFormat == xe::kRt2_10_10_10FloatAs16) xeFormat = xe::kRt2_10_10_10Float;
        // Targets are single sampled, and MSAA does not change the key: the game's 4x tiled
        // scene passes and the 1x passes that follow on the same EDRAM tile share pixels.
        (void)msaa;
        uint64_t key = uint64_t(base & 0xFFF) | (uint64_t(xeFormat & 0xF) << 12) | (uint64_t(pitch) << 16) |
                       (uint64_t(depth) << 34);
        auto it = rts.find(key);
        if (it != rts.end()) return &it->second;
        RenderTarget& rt = rts[key];
        rt.depth = depth;
        rt.xeFormat = xeFormat;
        rt.base = base & 0xFFF;
        uint32_t bpp = depth ? 4 : (IsRt64bpp(xeFormat) ? 8 : 4);
        uint32_t rows = std::clamp<uint32_t>(kEdramBytes / (pitch * bpp), 16, 2048);
        rows = (rows + 15) & ~15u;
        VkFormat fmt = depth ? depthFormat : ColorRtVkFormat(xeFormat);
        VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                  (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        VkImageAspectFlags aspect = depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
        if (!CreateImage(rt.image, fmt, pitch, rows, 1, usage, false, aspect)) {
            std::fprintf(stderr, "[render] cannot create a %ux%u render target\n", pitch, rows);
            rts.erase(key);
            return nullptr;
        }
        EndRendering();
        ImageToGeneral(rt.image.img, aspect, 1);
        VkImageSubresourceRange rr{aspect, 0, 1, 0, 1};
        if (depth) {
            VkClearDepthStencilValue z{1.0f, 0};
            vkCmdClearDepthStencilImage(cmd, rt.image.img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rr);
        } else {
            VkClearColorValue c{};
            vkCmdClearColorImage(cmd, rt.image.img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &rr);
        }
        Barrier();
        static int logged = 0;
        if (logged++ < 32)
            std::fprintf(stderr, "[render] new %s target: EDRAM tile %u, format %u, %ux%u\n", depth ? "depth" : "color",
                         base & 0xFFF, xeFormat, pitch, rows);
        return &rt;
    }

    void BeginRendering(RenderTarget* const color[4], RenderTarget* depth, VkRect2D area) {
        VkRenderingAttachmentInfo ca[4] = {};
        for (int i = 0; i < 4; i++) {
            ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            ca[i].imageView = color[i] ? color[i]->image.view : VK_NULL_HANDLE;
            ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        da.imageView = depth ? depth->image.view : VK_NULL_HANDLE;
        da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = area;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 4;
        ri.pColorAttachments = ca;
        if (depth) {
            ri.pDepthAttachment = &da;
            ri.pStencilAttachment = &da;
        }
        vkCmdBeginRendering(cmd, &ri);
        renderingActive = true;
        curPipeline = VK_NULL_HANDLE;
    }

    // ---- textures ----------------------------------------------------------------------------
    // The fetch constant's MIN_MIP_LEVEL is how the game streams a texture: levels sharper than it are
    // not resident yet, so sampling must not go below it (FA_MIPCLAMP=0 disables). The image clamps
    // minLod to its last decoded level, so a range past the decoded levels is safe.
    static uint32_t mipClampMin(const TextureFetch& t) {
        static const bool off = [] { const char* e = std::getenv("FA_MIPCLAMP"); return e && e[0] == '0'; }();
        return (off || t.mipFilter == 2) ? 0u : uint32_t(t.mipMin & 15);
    }

    VkSampler GetSampler(const TextureFetch& t) {
        uint64_t key = uint64_t(t.magFilter & 1) | (uint64_t(t.minFilter & 1) << 1) | (uint64_t(t.clamp[0]) << 2) |
                       (uint64_t(t.clamp[1]) << 5) | (uint64_t(t.clamp[2]) << 8) | (uint64_t(t.borderColor) << 11) |
                       (uint64_t(t.mipFilter & 3) << 14) | (uint64_t(mipClampMin(t)) << 16);
        auto it = samplers.find(key);
        if (it != samplers.end()) return it->second;
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = (t.magFilter & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.minFilter = (t.minFilter & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        // Mip filter 0 point, 1 linear, 2 base map only. Images only hold the levels that were decoded,
        // so the LOD clamps to their last level.
        si.mipmapMode = t.mipFilter == 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = AddressMode(t.clamp[0], mirrorClampEdge);
        si.addressModeV = AddressMode(t.clamp[1], mirrorClampEdge);
        si.addressModeW = AddressMode(t.clamp[2], mirrorClampEdge);
        si.borderColor = t.borderColor == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        si.minLod = float(mipClampMin(t));
        si.maxLod = t.mipFilter == 2 ? 0.0f : 16.0f;
        VkSampler s = VK_NULL_HANDLE;
        Check(vkCreateSampler(dev, &si, nullptr, &s), "vkCreateSampler");
        samplers[key] = s;
        return s;
    }

    // Returns the image view for texture fetch slot `slot` (uploading it if its bytes changed).
    VkImageView GetTexture(const RegisterFile& regs, uint32_t slot, bool cube, VkSampler& sampler) {
        uint32_t d[6];
        for (int k = 0; k < 6; k++) d[k] = regs[pm4::kFetchConstBase + slot * 6 + k];
        TextureFetch t = ParseTextureFetch(d);
        sampler = GetSampler(t);
        Image& fallback = cube ? dummyCube : dummy2D;
        // Every fall back to the black dummy texture is logged once per (reason, format, size),
        // since a black building or shadow means a texture that never made it to the GPU.
        auto fell = [&](const char* why) -> VkImageView {
            static std::set<uint64_t> seen;
            static int logged = 0;
            uint64_t k = (uint64_t(t.format) << 40) ^ (uint64_t(t.width) << 20) ^ t.height ^ (uint64_t(t.dimension) << 56) ^
                         (uint64_t(why[0]) << 48) ^ (uint64_t(why[1]) << 32);
            if (logged < 80 && seen.insert(k).second) {
                logged++;
                std::fprintf(stderr, "[render] texture slot %u falls back to black (%s): type %u fmt %u %ux%u dim %u base %08x %s | %08x %08x %08x %08x %08x %08x\n",
                             slot, why, t.type, t.format, t.width, t.height, t.dimension, t.base, t.tiled ? "tiled" : "linear",
                             d[0], d[1], d[2], d[3], d[4], d[5]);
            }
            return fallback.view;
        };
        if (!t.IsTexture()) return fell("not a texture fetch");
        if (!t.base) return fell("no base address");
        if (!GetHostFormatInfo(t.format).supported) return fell("unsupported format");
        if (cube != (t.dimension == 3)) {
            // Shader and fetch constant disagree (e.g. a 3D / stacked texture sampled as 2D).
            if (cube || t.dimension == 3) return fell("dimension mismatch");
        }
        if (watchAddr && (t.base & 0x1FFFFFFFu) == watchAddr) {
            auto wit = textures.find(HashBytes(d, 6 * sizeof(uint32_t)) ^ (uint64_t(t.dimension) << 62));
            const bool reuse = wit != textures.end() && wit->second.validatedGen == generation && wit->second.ok;
            if (!reuse) WatchBytes("texture bind", t.base, HostTextureByteSize(t), "(will recheck memory)");  // reused binds are not logged
        }
        if (snapEnabled && !cube && !snaps.empty()) {
            prof::Scope pn_(prof::kDtSnap);
            if (VkImageView sv = SnapView(t)) return sv;
        }
        uint64_t key = HashBytes(d, 6 * sizeof(uint32_t));  // bytes of the whole fetch constant (HashBytes counts bytes): format, base, size, swizzle, mip range and address
        key ^= uint64_t(t.dimension) << 62;
        Texture& tex = textures[key];
        uint32_t size = HostTextureByteSize(t);
        if (tex.validatedGen == generation && tex.ok) return tex.image.view;
        tex.guestBase = t.base;
        tex.guestSize = size;
        if (!pendingKeys.empty() && PendingOverlaps(t.base, size)) FlushPending(prof::kFlushTex);
        const uint8_t* src = mem.HostPtr(t.base, size);
        std::vector<uint8_t> padded;
        if (!src) {
            // The padded tile footprint can run past the end of the allocation holding the
            // texture: use what is mapped and zero fill the rest.
            for (uint32_t part = size; part > 4096 && !src;) {
                part = (part / 2) & ~4095u;
                if (!part) break;
                const uint8_t* q = mem.HostPtr(t.base, part);
                if (q) {
                    // Grow back toward the full size to find the largest mapped prefix.
                    uint32_t lo = part, hi = size;
                    while (hi - lo > 4096) {
                        uint32_t mid = (lo + (hi - lo) / 2) & ~4095u;
                        if (mid == lo) break;
                        if (mem.HostPtr(t.base, mid)) lo = mid; else hi = mid;
                    }
                    padded.assign(size, 0);
                    std::memcpy(padded.data(), mem.HostPtr(t.base, lo), lo);
                    src = padded.data();
                }
            }
        }
        if (!src) {
            tex.validatedGen = generation;
            return tex.ok ? tex.image.view : fell("guest memory not mapped");
        }
        uint64_t h;
        {
            prof::Scope ps_(prof::kTexCheck);
            h = HashBytes(src, size);
        }
        // Mip levels 1.. live at the mip address. They are optional: when that memory is not
        // readable the texture is uploaded with its base level only.
        const uint32_t mipBytes = HostMipByteSize(t);
        const uint8_t* mipSrc = mipBytes ? mem.HostPtr(t.mipBase, mipBytes) : nullptr;
        if (mipSrc) h = h * 0x9E3779B97F4A7C15ull + HashBytes(mipSrc, mipBytes) + 1;
        tex.validatedGen = generation;
        if (tex.ok && h == tex.contentHash) return tex.image.view;
        prof::Scope ps_(prof::kTexUpload);
        {   // FA_TEXLOG=1: one line per upload, to see when the game's streaming changes each texture
            static const bool texLog = [] { const char* e = std::getenv("FA_TEXLOG"); return e && e[0] == '1'; }();
            static int texLogged = 0;
            if (texLog && texLogged < 20000) {
                texLogged++;
                std::fprintf(stderr, "[texlog] frame %llu base %08x fmt %u %ux%u mipbase %08x mipmin %u mipmax %u %s\n", (unsigned long long)frame,
                             t.base, t.format, t.width, t.height, t.mipBase, t.mipMin, t.mipMax, tex.ok ? "CHANGED" : "new");
            }
        }

        const uint64_t upT0 = prof::g_enabled ? prof::NowNs() : 0;
        HostTexture ht;
        if (!DecodeTextureHost(t, src, ht, mipSrc)) return fell("decode failed");
        // FA_TEX_FORCE=<hex phys addr>:r,g,b,a replaces that texture's texels with one constant colour (0..1,
        // before the fetch swizzle): a debug aid to find what value a shader needs from a map.
        {
            static float force[4];
            static const uint32_t forceAddr = [] {
                const char* e = std::getenv("FA_TEX_FORCE");
                if (!e) return 0u;
                char* c = nullptr;
                uint32_t a = uint32_t(std::strtoul(e, &c, 16) & 0x1FFFFFFFu);
                for (int i = 0; i < 4; i++) {
                    if (!c || *c != (i ? ',' : ':')) return 0u;
                    force[i] = std::strtof(c + 1, &c);
                }
                return a;
            }();
            if (forceAddr && (t.base & 0x1FFFFFFFu) == forceAddr) {
                const size_t n = ht.data.size() / (ht.isFloat ? 16 : 4);
                for (size_t i = 0; i < n; i++) {
                    if (ht.isFloat) std::memcpy(&ht.data[i * 16], force, 16);
                    else for (int c = 0; c < 4; c++) ht.data[i * 4 + c] = uint8_t(std::clamp(force[c], 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            }
        }
        if (prof::g_enabled) {
            static uint64_t n = 0;
            static uint64_t decNs[64] = {};
            // Aggregate decode time per texture format and report every 400 uploads.
            decNs[t.format & 63] += prof::NowNs() - upT0;
            if (++n % 400 == 0) {
                std::fprintf(stderr, "[texup] decode ms by format over 400 uploads:");
                for (int f = 0; f < 64; f++) if (decNs[f]) { std::fprintf(stderr, " fmt%d=%.1f", f, double(decNs[f]) / 1e6); decNs[f] = 0; }
                std::fprintf(stderr, " | last: fmt %u %ux%u float %d tiled %d\n", t.format, t.width, t.height, ht.isFloat ? 1 : 0, t.tiled ? 1 : 0);
            }
        }
        VkFormat fmt = ht.isFloat ? floatTexFormat : VK_FORMAT_R8G8B8A8_UNORM;
        bool needNew = !tex.ok || tex.image.w != ht.width || tex.image.h != ht.height || tex.image.format != fmt ||
                       tex.image.layers != ht.layers || tex.image.levels != ht.levels;
        EndRendering();
        if (needNew) {
            if (tex.ok) garbageImages.push_back(tex.image);
            tex.image = Image{};
            tex.ok = false;
            VkComponentMapping cm;
            VkComponentSwizzle* comps[4] = {&cm.r, &cm.g, &cm.b, &cm.a};
            for (int c = 0; c < 4; c++) {
                uint32_t s = (t.swizzle >> (3 * c)) & 7;
                *comps[c] = s < 4 ? VkComponentSwizzle(VK_COMPONENT_SWIZZLE_R + s)
                          : s == 5 ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_ZERO;
            }
            if (!CreateImage(tex.image, fmt, ht.width, ht.height, ht.layers,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, ht.layers == 6,
                             VK_IMAGE_ASPECT_COLOR_BIT, &cm, ht.levels))
                return fallback.view;
            ImageToGeneral(tex.image.img, VK_IMAGE_ASPECT_COLOR_BIT, ht.layers, ht.levels);
        }
        // Stage the texels (RGBA32F -> RGBA16F when that is the host format).
        size_t texels = ht.data.size() / (ht.isFloat ? 16 : 4);  // all layers and levels
        size_t bytes = texels * (fmt == VK_FORMAT_R8G8B8A8_UNORM ? 4 : fmt == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 16);
        if (bytes > ring.size / 2) return fallback.view;
        bool flushed;
        VkDeviceSize at = RingAlloc(bytes, 16, flushed);
        uint8_t* dst = static_cast<uint8_t*>(ring.map) + at;
        if (fmt == VK_FORMAT_R16G16B16A16_SFLOAT) {
            const float* f = reinterpret_cast<const float*>(ht.data.data());
            uint16_t* o = reinterpret_cast<uint16_t*>(dst);
            const size_t n = texels * 4, chunk = 4096;
            rowPool.Run(uint32_t((n + chunk - 1) / chunk), [&](uint32_t ca, uint32_t cb) {
                for (size_t i = size_t(ca) * chunk, e = std::min(n, size_t(cb) * chunk); i < e; i++) o[i] = FloatToHalf(f[i]);
            });
        } else {
            std::memcpy(dst, ht.data.data(), bytes);
        }
        Barrier();
        {
            const VkDeviceSize texelBytes = fmt == VK_FORMAT_R8G8B8A8_UNORM ? 4 : fmt == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 16;
            VkBufferImageCopy regions[16];
            uint32_t nr = 0;
            if (ht.levels <= 1) {
                regions[nr] = VkBufferImageCopy{};
                regions[nr].bufferOffset = at;
                regions[nr].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, ht.layers};
                regions[nr].imageExtent = {ht.width, ht.height, 1};
                nr++;
            } else {  // mip chains are 2D, one layer: levels are stored back to back
                VkDeviceSize off = at;
                for (uint32_t m = 0; m < ht.levels && nr < 16; m++) {
                    const uint32_t lw = std::max(1u, ht.width >> m), lh = std::max(1u, ht.height >> m);
                    regions[nr] = VkBufferImageCopy{};
                    regions[nr].bufferOffset = off;
                    regions[nr].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
                    regions[nr].imageExtent = {lw, lh, 1};
                    nr++;
                    off += VkDeviceSize(lw) * lh * texelBytes;
                }
            }
            vkCmdCopyBufferToImage(cmd, ring.buf, tex.image.img, VK_IMAGE_LAYOUT_GENERAL, nr, regions);
        }
        Barrier();
        tex.contentHash = h;
        tex.guestBase = t.base;
        tex.guestSize = size;
        tex.ok = true;
        stats.textureUploads++;
        return tex.image.view;
    }

    // ---- pipelines ---------------------------------------------------------------------------
    VkPipeline GetPipeline(const PipelineKey& key, Shader* vs, Shader* ps) {
        std::string k(reinterpret_cast<const char*>(&key), sizeof key);
        auto it = pipelines.find(k);
        if (it != pipelines.end()) return it->second;
        prof::Scope ps_(prof::kPipeline);

        VkPipelineShaderStageCreateInfo st[3] = {};
        uint32_t ns = 0;
        auto stage = [&](VkShaderStageFlagBits s, VkShaderModule m) {
            st[ns].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            st[ns].stage = s;
            st[ns].module = m;
            st[ns].pName = "main";
            ns++;
        };
        stage(VK_SHADER_STAGE_VERTEX_BIT, vs->module);
        if (key.prim == uint32_t(HostPrim::Rects) && haveGs) stage(VK_SHADER_STAGE_GEOMETRY_BIT, rectGs);
        stage(VK_SHADER_STAGE_FRAGMENT_BIT, ps->module);

        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = key.prim == uint32_t(HostPrim::Points) ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                    : key.prim == uint32_t(HostPrim::Lines) ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                    : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1;
        vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = key.cull;
        rs.frontFace = VkFrontFace(key.frontFace);
        rs.depthBiasEnable = key.depthBias;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        uint32_t dc = key.depthControl;
        ds.depthTestEnable = (dc >> 1) & 1;
        ds.depthWriteEnable = (dc >> 2) & 1;
        ds.depthCompareOp = VkCompareOp((dc >> 4) & 7);
        ds.stencilTestEnable = dc & 1;
        ds.front.compareOp = VkCompareOp((dc >> 8) & 7);
        ds.front.failOp = VkStencilOp((dc >> 11) & 7);
        ds.front.passOp = VkStencilOp((dc >> 14) & 7);
        ds.front.depthFailOp = VkStencilOp((dc >> 17) & 7);
        if ((dc >> 7) & 1) {
            ds.back.compareOp = VkCompareOp((dc >> 20) & 7);
            ds.back.failOp = VkStencilOp((dc >> 23) & 7);
            ds.back.passOp = VkStencilOp((dc >> 26) & 7);
            ds.back.depthFailOp = VkStencilOp((dc >> 29) & 7);
        } else {
            ds.back = ds.front;
        }
        VkPipelineColorBlendAttachmentState cba[4] = {};
        for (int i = 0; i < 4; i++) {
            uint32_t b = independentBlend ? key.blend[i] : key.blend[0];
            uint32_t m = independentBlend ? (key.colorMask >> (4 * i)) & 0xF : key.colorMask & 0xF;
            cba[i].colorWriteMask = m;
            cba[i].srcColorBlendFactor = BlendFactor(b & 31);
            cba[i].colorBlendOp = BlendOp((b >> 5) & 7);
            cba[i].dstColorBlendFactor = BlendFactor((b >> 8) & 31);
            cba[i].srcAlphaBlendFactor = BlendFactor((b >> 16) & 31);
            cba[i].alphaBlendOp = BlendOp((b >> 21) & 7);
            cba[i].dstAlphaBlendFactor = BlendFactor((b >> 24) & 31);
            bool noBlend = (b & 31) == 1 && ((b >> 5) & 7) == 0 && ((b >> 8) & 31) == 0 && ((b >> 16) & 31) == 1 &&
                           ((b >> 21) & 7) == 0 && ((b >> 24) & 31) == 0;
            cba[i].blendEnable = !noBlend;
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 4;
        cb.pAttachments = cba;
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = uint32_t(sizeof dyn / sizeof dyn[0]);
        dy.pDynamicStates = dyn;
        VkFormat cf[4];
        for (int i = 0; i < 4; i++) cf[i] = VkFormat(key.colorFormats[i]);
        VkPipelineRenderingCreateInfo pri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        pri.colorAttachmentCount = 4;
        pri.pColorAttachmentFormats = cf;
        pri.depthAttachmentFormat = VkFormat(key.depthFormat);
        pri.stencilAttachmentFormat = VkFormat(key.depthFormat);
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.pNext = &pri;
        gp.stageCount = ns;
        gp.pStages = st;
        gp.pVertexInputState = &vi;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pDepthStencilState = &ds;
        gp.pColorBlendState = &cb;
        gp.pDynamicState = &dy;
        gp.layout = pipeLayout;
        VkPipeline p = VK_NULL_HANDLE;
        VkResult r = vkCreateGraphicsPipelines(dev, pipeCache, 1, &gp, nullptr, &p);
        if (r != VK_SUCCESS) {
            std::fprintf(stderr, "[render] pipeline creation failed: %s\n", VkErr(r));
            p = VK_NULL_HANDLE;
        } else {
            stats.pipelines++;
        }
        pipelines[k] = p;
        return p;
    }

    // ---- draws -------------------------------------------------------------------------------
    void Draw(const DrawCall& dc, const RegisterFile& regs) {
        prof::Scope ps_(prof::kDraw);
        uint32_t mode = regs[xe::RB_MODECONTROL] & 7;
        if (mode != xe::kEdramColorDepth && mode != xe::kEdramDepthOnly) { stats.drawsSkipped++; return; }
        if (!dc.vsUcode || !dc.psUcode) { stats.drawsSkipped++; return; }
        Shader* vs = GetShader(ShaderStage::Vertex, dc.vsUcode, dc.vsDwords, dc.vsHash);
        Shader* ps = GetShader(ShaderStage::Pixel, dc.psUcode, dc.psDwords, dc.psHash);
        if (!vs->ok || !ps->ok) { stats.drawsSkipped++; return; }
        std::optional<prof::Scope> sec;
        sec.emplace(prof::kDrawBuild);
        if (!BuildHostDraw(dc, regs, mem, hostDraw)) { stats.drawsSkipped++; return; }
        HostDraw& hd = hostDraw;
        prof::Add(prof::kIdxCount, hd.indices.size());
        if (hd.prim == HostPrim::Rects && !haveGs) hd.prim = HostPrim::Triangles;
        Begin();

        // Render targets bound by this draw.
        const uint32_t surf = regs[xe::RB_SURFACE_INFO];
        const uint32_t pitch = surf & 0x3FFF, msaa = (surf >> 16) & 3;
        const uint32_t colorMaskReg = regs[xe::RB_COLOR_MASK];
        const uint32_t depthCtl = regs[xe::RB_DEPTHCONTROL];
        RenderTarget* color[4] = {};
        uint32_t colorMask = 0;
        if (mode == xe::kEdramColorDepth) {
            static const uint32_t infoRegs[4] = {xe::RB_COLOR_INFO, xe::RB_COLOR1_INFO, xe::RB_COLOR2_INFO, xe::RB_COLOR3_INFO};
            for (int i = 0; i < 4; i++) {
                uint32_t m = (colorMaskReg >> (4 * i)) & 0xF;
                if (!m || !(ps->info.colorWritten & (1u << i))) continue;
                uint32_t info = regs[infoRegs[i]];
                color[i] = GetRt(info & 0xFFF, (info >> 16) & 0xF, pitch, msaa, false);
                if (color[i]) colorMask |= m << (4 * i);
            }
        }
        RenderTarget* depth = nullptr;
        if (depthCtl & 0x3) {  // z or stencil enabled
            uint32_t info = regs[xe::RB_DEPTH_INFO];
            depth = GetRt(info & 0xFFF, (info >> 16) & 1, pitch, msaa, true);
        }
        if (!depth && !colorMask) { stats.drawsSkipped++; return; }

        // Textures (uploads happen outside rendering, so before BeginRendering).
        sec.emplace(prof::kDrawTex);
        uint32_t texUsed = vs->info.texUsed | ps->info.texUsed;
        VkDescriptorSet texSet = VK_NULL_HANDLE;
        if (texUsed) {
            // Views first: an upload can flush the batch, which resets the descriptor pool.
            VkDescriptorImageInfo ii[64];
            VkWriteDescriptorSet w[64];
            uint32_t nw = 0;
            for (uint32_t s = 0; s < 32; s++) {
                if (!(texUsed & (1u << s))) continue;
                bool cube = (vs->info.texUsed & (1u << s) ? vs->info.texDim[s] : ps->info.texDim[s]) == kTexDimCube;
                VkSampler smp;
                VkImageView view;
                {
                    prof::Scope pg_(prof::kDtGet);
                    view = GetTexture(regs, s, cube, smp);
                }
                ii[nw] = {smp, view, VK_IMAGE_LAYOUT_GENERAL};
                w[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w[nw].dstBinding = cube ? 32 + s : s;
                w[nw].descriptorCount = 1;
                w[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                nw++;
            }
            // Same samplers and views as the previous draw of this batch: reuse its descriptor set.
            bool sameSet = lastTexSet != VK_NULL_HANDLE && lastTexKey.size() == nw;
            for (uint32_t i = 0; i < nw && sameSet; i++)
                sameSet = lastTexKey[i].binding == w[i].dstBinding && lastTexKey[i].smp == ii[i].sampler && lastTexKey[i].view == ii[i].imageView;
            uint64_t setHash = 0;
            if (!sameSet) {
                setHash = 0xCBF29CE484222325ull ^ nw;
                for (uint32_t i = 0; i < nw; i++) {
                    setHash = (setHash ^ w[i].dstBinding) * 0x100000001B3ull;
                    setHash = (setHash ^ uint64_t(reinterpret_cast<uintptr_t>(ii[i].sampler))) * 0x100000001B3ull;
                    setHash = (setHash ^ uint64_t(reinterpret_cast<uintptr_t>(ii[i].imageView))) * 0x100000001B3ull;
                    setHash ^= setHash >> 29;
                }
                auto cached = texSetCache.find(setHash);
                if (cached != texSetCache.end()) {
                    // Built earlier in this batch for the same samplers and views: reuse it.
                    texSet = lastTexSet = cached->second;
                    lastTexKey.resize(nw);
                    for (uint32_t i = 0; i < nw; i++) lastTexKey[i] = {w[i].dstBinding, ii[i].sampler, ii[i].imageView};
                    sameSet = true;
                }
            }
            if (!sameSet) {
            prof::Scope pd_(prof::kDtDesc);
            if (texSetsUsed >= kMaxTextureSets) { SubmitAndWait(); Begin(); }
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = texPool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &set1Layout;
            if (vkAllocateDescriptorSets(dev, &ai, &texSet) != VK_SUCCESS) {
                SubmitAndWait();
                Begin();
                vkAllocateDescriptorSets(dev, &ai, &texSet);
            }
            texSetsUsed++;
            for (uint32_t i = 0; i < nw; i++) {
                w[i].dstSet = texSet;
                w[i].pImageInfo = &ii[i];
            }
            vkUpdateDescriptorSets(dev, nw, w, 0, nullptr);
            lastTexSet = texSet;
            texSetCache[setHash] = texSet;
            lastTexKey.resize(nw);
            for (uint32_t i = 0; i < nw; i++) lastTexKey[i] = {w[i].dstBinding, ii[i].sampler, ii[i].imageView};
            } else {
                texSet = lastTexSet;
            }
        }

        // Uniforms and vertex data.
        sec.emplace(prof::kDrawUpload);
        const VkDeviceSize uboAlign = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 16);
        bool flushed;
        VkDeviceSize constsOff;
        const uint32_t* consts = &regs.r[pm4::kAluConstBase];
        if (lastConstsOff != ~0ull && lastConsts.size() == 2048 && !std::memcmp(lastConsts.data(), consts, 8192)) {
            constsOff = lastConstsOff;
        } else {
            prof::Add(prof::kConstUploads, 1);
            constsOff = RingAlloc(8192, uboAlign, flushed);
            std::memcpy(static_cast<uint8_t*>(ring.map) + constsOff, consts, 8192);
            lastConsts.assign(consts, consts + 2048);
            lastConstsOff = constsOff;
        }
        VkDeviceSize boolOff = RingAlloc(160, uboAlign, flushed);
        if (flushed) { constsOff = RingAlloc(8192, uboAlign, flushed); std::memcpy(static_cast<uint8_t*>(ring.map) + constsOff, consts, 8192); lastConstsOff = constsOff; }
        std::memcpy(static_cast<uint8_t*>(ring.map) + boolOff, &regs.r[pm4::kBoolConstBase], 160);

        XeSystemConstants sys{};
        const uint32_t vte = regs[xe::PA_CL_VTE_CNTL];
        const float vs_[3] = {xe::AsFloat(regs[xe::PA_CL_VPORT_XSCALE]), xe::AsFloat(regs[xe::PA_CL_VPORT_YSCALE]),
                              xe::AsFloat(regs[xe::PA_CL_VPORT_ZSCALE])};
        const float vo_[3] = {xe::AsFloat(regs[xe::PA_CL_VPORT_XOFFSET]), xe::AsFloat(regs[xe::PA_CL_VPORT_YOFFSET]),
                              xe::AsFloat(regs[xe::PA_CL_VPORT_ZOFFSET])};
        for (int c = 0; c < 3; c++) {
            sys.vportScale[c] = (vte >> (2 * c)) & 1 ? vs_[c] : 1.0f;
            sys.vportOffset[c] = (vte >> (2 * c + 1)) & 1 ? vo_[c] : 0.0f;
        }
        sys.flags[0] = ((vte >> 8) & 1) | (((vte >> 9) & 1) << 1) | (((vte >> 10) & 1) << 2);
        const uint32_t sc = regs[xe::PA_SU_SC_MODE_CNTL];
        float wox = 0, woy = 0;
        if ((sc >> 16) & 1) {
            wox = float(xe::SBits(regs[xe::PA_SC_WINDOW_OFFSET], 0, 15));
            woy = float(xe::SBits(regs[xe::PA_SC_WINDOW_OFFSET], 16, 15));
        }
        const float half = (regs[xe::PA_SU_VTX_CNTL] & 1) ? 0.0f : 0.5f;  // D3D pixel centres

        // Render area = smallest bound target.
        uint32_t areaW = 1u << 30, areaH = 1u << 30;
        for (auto* c : color) if (c) { areaW = std::min(areaW, c->image.w); areaH = std::min(areaH, c->image.h); }
        if (depth) { areaW = std::min(areaW, depth->image.w); areaH = std::min(areaH, depth->image.h); }
        sys.ndcScale[0] = 2.0f / float(areaW);
        sys.ndcScale[1] = 2.0f / float(areaH);
        sys.ndcScale[2] = 1.0f;
        sys.ndcOffset[0] = (wox + half) * sys.ndcScale[0] - 1.0f;
        sys.ndcOffset[1] = (woy + half) * sys.ndcScale[1] - 1.0f;
        sys.ndcOffset[2] = 0.0f;
        const uint32_t colorCtl = regs[xe::RB_COLORCONTROL];
        sys.flags[1] = ((colorCtl >> 3) & 1) && !dbgNoAtest ? (colorCtl & 7) : 7;
        const uint32_t prog = regs[xe::SQ_PROGRAM_CNTL];
        sys.flags[2] = ((prog >> 18) & 1) ? ((regs[xe::SQ_CONTEXT_MISC] >> 8) & 0xFF) : 0xFFFFFFFFu;
        sys.flags[3] = 16;
        sys.misc[0] = xe::AsFloat(regs[xe::RB_ALPHA_REF]);
        // Alpha to mask (bit 4) turns alpha into MSAA coverage. Targets are single sampled here, so
        // approximate it with a cut at 0.5, which gives foliage cards their leaf-shaped edges.
        if (!((colorCtl >> 3) & 1) && ((colorCtl >> 4) & 1) && !dbgNoAtest && !std::getenv("FA_NO_A2M")) {
            sys.flags[1] = 6;
            sys.misc[0] = 0.5f;
        }
        sys.misc[1] = float((regs[xe::PA_SU_POINT_SIZE] >> 16) & 0xFFFF) / 8.0f;
        if (sys.misc[1] <= 0.0f) sys.misc[1] = 1.0f;

        // Vertex buffers used by the vertex shader.
        uint32_t maxIdx = ~0u;  // largest vertex index of this draw, computed on first use
        for (uint32_t s = 0; s < 96; s++) {
            if (!(vs->info.vfetchUsed[s / 32] & (1u << (s % 32)))) continue;
            xe::VertexFetch vf = xe::ParseVertexFetch(regs[pm4::kFetchConstBase + s * 2], regs[pm4::kFetchConstBase + s * 2 + 1]);
            uint32_t words = std::min<uint32_t>(vf.sizeWords, 4u << 20);
            if (vf.type != 3 || !words) continue;
            // Games often give the fetch constant a whole pool as its size; only the part the draw's
            // indices can reach needs copying (when the shader indexes straight from the vertex index).
            if (vs->info.vfetchStride[s]) {
                if (maxIdx == ~0u) {
                    maxIdx = 0;
                    for (uint32_t ix : hd.indices) maxIdx = std::max(maxIdx, ix);
                }
                uint64_t need = uint64_t(maxIdx) * vs->info.vfetchStride[s] + vs->info.vfetchEnd[s];
                words = uint32_t(std::min<uint64_t>(words, need));
            }
            if (prof::g_enabled) {
                static int logged = 0;
                static uint64_t seen = 0;
                if (logged < 60 && seen++ % 700 == 0) {
                    logged++;
                    std::fprintf(stderr, "[vbprof] vs %016llx slot %u: fetch words %u, copy %u, stride %u end %u, indices %zu max %d\n",
                                 (unsigned long long)dc.vsHash, s, vf.sizeWords, words, vs->info.vfetchStride[s],
                                 vs->info.vfetchEnd[s], hd.indices.size(), maxIdx == ~0u ? -1 : int(maxIdx));
                }
            }
            uint64_t key = (uint64_t(vf.address) << 32) ^ words;
            auto it = vbCache.find(key);
            VkDeviceSize at;
            if (it != vbCache.end()) {
                at = it->second;
            } else {
                const uint8_t* src = mem.HostPtr(vf.address, words * 4);
                if (!src) continue;
                at = RingAlloc(words * 4, 16, flushed);
                if (flushed) { RecoverConsts(consts, constsOff, boolOff, regs, uboAlign); }
                std::memcpy(static_cast<uint8_t*>(ring.map) + at, src, words * 4);
                prof::Add(prof::kVbBytes, uint64_t(words) * 4);
                vbCache[key] = at;
            }
            sys.vf[s][0] = uint32_t(at / 4);
            sys.vf[s][1] = vf.endian;
            sys.vf[s][2] = words;
        }
        VkDeviceSize sysOff = RingAlloc(sizeof sys, uboAlign, flushed);
        if (flushed) { RecoverConsts(consts, constsOff, boolOff, regs, uboAlign); ReuploadVbs(sys, vs, regs); sysOff = RingAlloc(sizeof sys, uboAlign, flushed); }
        std::memcpy(static_cast<uint8_t*>(ring.map) + sysOff, &sys, sizeof sys);
        VkDeviceSize idxOff = RingAlloc(hd.indices.size() * 4, 16, flushed);
        if (flushed) {
            // Batch restarted: everything above has to be uploaded again into the new batch.
            RecoverConsts(consts, constsOff, boolOff, regs, uboAlign);
            ReuploadVbs(sys, vs, regs);
            sysOff = RingAlloc(sizeof sys, uboAlign, flushed);
            std::memcpy(static_cast<uint8_t*>(ring.map) + sysOff, &sys, sizeof sys);
            idxOff = RingAlloc(hd.indices.size() * 4, 16, flushed);
        }
        std::memcpy(static_cast<uint8_t*>(ring.map) + idxOff, hd.indices.data(), hd.indices.size() * 4);

        // Pipeline.
        sec.emplace(prof::kDrawRecord);
        PipelineKey key{};
        key.vs = dc.vsHash;
        key.ps = dc.psHash;
        key.prim = uint32_t(hd.prim);
        for (int i = 0; i < 4; i++) key.colorFormats[i] = color[i] && ((colorMask >> (4 * i)) & 0xF) ? color[i]->image.format : VK_FORMAT_UNDEFINED;
        key.depthFormat = depth ? depthFormat : VK_FORMAT_UNDEFINED;
        static const uint32_t blendRegs[4] = {xe::RB_BLENDCONTROL0, xe::RB_BLENDCONTROL1, xe::RB_BLENDCONTROL2, xe::RB_BLENDCONTROL3};
        for (int i = 0; i < 4; i++) key.blend[i] = key.colorFormats[i] ? (dbgNoBlend ? 0x00010001u : regs[blendRegs[i]] & 0x1FFF1FFF) : 0;
        key.colorMask = colorMask;
        key.depthControl = depth ? depthCtl : 0;
        if (depth && !(depthCtl & 1)) key.depthControl &= 0xFF & ~0x80u;  // stencil off: ignore its fields
        bool tri = hd.prim == HostPrim::Triangles;
        key.cull = tri ? ((sc & 1) ? VK_CULL_MODE_FRONT_BIT : 0) | ((sc & 2) ? VK_CULL_MODE_BACK_BIT : 0) : 0;
        key.frontFace = (sc & 4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        key.depthBias = depth && ((sc >> 11) & 3) ? 1 : 0;
        VkPipeline pipe = GetPipeline(key, vs, ps);
        if (!pipe) { stats.drawsSkipped++; return; }

        // Rendering pass for this target set.
        bool same = renderingActive && curDepth == depth;
        for (int i = 0; i < 4 && same; i++) same = curColor[i] == (key.colorFormats[i] ? color[i] : nullptr);
        if (!same) {
            EndRendering();
            for (int i = 0; i < 4; i++) curColor[i] = key.colorFormats[i] ? color[i] : nullptr;
            curDepth = depth;
            curW = areaW;
            curH = areaH;
            BeginRendering(curColor, curDepth, VkRect2D{{0, 0}, {areaW, areaH}});
        }
        if (curPipeline != pipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            curPipeline = pipe;
        }
        uint32_t dyn[3] = {uint32_t(constsOff), uint32_t(boolOff), uint32_t(sysOff)};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout, 0, 1, &set0, 3, dyn);
        if (texSet) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout, 1, 1, &texSet, 0, nullptr);
        VkViewport vp{0.0f, 0.0f, float(areaW), float(areaH), 0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        VkRect2D scissor = Scissor(regs, areaW, areaH);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        float bc[4] = {xe::AsFloat(regs[xe::RB_BLEND_RED]), xe::AsFloat(regs[xe::RB_BLEND_GREEN]),
                       xe::AsFloat(regs[xe::RB_BLEND_BLUE]), xe::AsFloat(regs[xe::RB_BLEND_ALPHA])};
        vkCmdSetBlendConstants(cmd, bc);
        uint32_t srm = regs[xe::RB_STENCILREFMASK];
        uint32_t srmBf = ((depthCtl >> 7) & 1) ? regs[xe::RB_STENCILREFMASK_BF] : srm;
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, srm & 0xFF);
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (srm >> 8) & 0xFF);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (srm >> 16) & 0xFF);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, srmBf & 0xFF);
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, (srmBf >> 8) & 0xFF);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, (srmBf >> 16) & 0xFF);
        vkCmdSetDepthBias(cmd, xe::AsFloat(regs[xe::PA_SU_POLY_OFFSET_FRONT_OFFSET]), 0.0f,
                          xe::AsFloat(regs[xe::PA_SU_POLY_OFFSET_FRONT_SCALE]));
        vkCmdBindIndexBuffer(cmd, ring.buf, idxOff, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, uint32_t(hd.indices.size()), 1, 0, 0, 0);
        stats.draws++;
        drawInFrame++;

        if (drawsTraced < opt.traceDraws || DumpDraw()) {
            drawsTraced++;
            std::fprintf(stderr,
                         "[render] frame %llu draw %llu (#%llu in frame): prim %u -> %u, %zu indices, vs %016llx ps %016llx, color %03x/%03x mask %04x depth %s "
                         "pitch %u scissor %d,%d %ux%u wo %g,%g vte %03x\n",
                         (unsigned long long)frame, (unsigned long long)stats.draws.load(), (unsigned long long)(drawInFrame - 1), dc.primType, uint32_t(hd.prim), hd.indices.size(),
                         (unsigned long long)dc.vsHash, (unsigned long long)dc.psHash, regs[xe::RB_COLOR_INFO] & 0xFFF,
                         (regs[xe::RB_COLOR_INFO] >> 16) & 0xF, colorMask, depth ? "on" : "off", pitch, scissor.offset.x,
                         scissor.offset.y, scissor.extent.width, scissor.extent.height, wox, woy, vte);
            if (DumpDraw()) TraceInputs(regs, vs, ps, sys);
        }
        if (DumpDraw()) {
            for (int i = 0; i < 4; i++)
                if (curColor[i]) { DumpTarget(curColor[i], scissor, "draw"); break; }
        }
        if (probeOn && frame >= probeFrame && frame <= probeFrameEnd) {
            RenderTarget* rt = nullptr;
            for (int i = 0; i < 4 && !rt; i++) rt = curColor[i];
            std::vector<size_t> idx;
            std::vector<std::pair<int, int>> pos;
            for (size_t k = 0; rt && k < probePts.size(); k++) {
                const int tx = probePts[k].first + int(wox), ty = probePts[k].second + int(woy);
                if (tx < scissor.offset.x || ty < scissor.offset.y || tx >= scissor.offset.x + int(scissor.extent.width) ||
                    ty >= scissor.offset.y + int(scissor.extent.height) || tx >= int(rt->image.w) || ty >= int(rt->image.h))
                    continue;
                idx.push_back(k);
                pos.push_back({tx, ty});
            }
            if (!idx.empty()) {
                std::vector<std::array<float, 4>> vals;
                ProbePixels(rt, pos, vals);
                for (size_t j = 0; j < idx.size(); j++) {
                    const size_t k = idx[j];
                    const auto& f = vals[j];
                    probeLastDraw[k] = stats.draws.load();
                    if (!std::memcmp(probeLast[k].data(), f.data(), sizeof(float) * 4)) continue;
                    probeLast[k] = f;
                    std::fprintf(stderr, "[probe] frame %llu draw %llu pixel(%d,%d) -> %.3f %.3f %.3f %.3f | prim %u idx %zu vs %016llx ps %016llx blend %08x\n",
                                 (unsigned long long)frame, (unsigned long long)stats.draws.load(), probePts[k].first, probePts[k].second,
                                 f[0], f[1], f[2], f[3], uint32_t(hd.prim), hd.indices.size(), (unsigned long long)dc.vsHash,
                                 (unsigned long long)dc.psHash, regs[xe::RB_BLENDCONTROL0]);
                    if (probeTraced.insert({dc.vsHash, dc.psHash}).second) TraceInputs(regs, vs, ps, sys);
                }
            }
        }
    }

    // Reads several pixels of a colour target in one submit.
    void ProbePixels(RenderTarget* rt, const std::vector<std::pair<int, int>>& pts, std::vector<std::array<float, 4>>& out) {
        const uint32_t bpp = VkFormatBytes(rt->image.format);
        const VkDeviceSize stride = 32;
        const VkDeviceSize need = stride * pts.size();
        EndRendering();
        if (need > readback.size) {
            SubmitAndWait();
            Begin();
            DestroyBuffer(readback);
            CreateBuffer(readback, std::max<VkDeviceSize>(need, 4096), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        }
        Barrier();
        for (size_t i = 0; i < pts.size(); i++) {
            VkBufferImageCopy bic{};
            bic.bufferOffset = stride * i;
            bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            bic.imageOffset = {pts[i].first, pts[i].second, 0};
            bic.imageExtent = {1, 1, 1};
            vkCmdCopyImageToBuffer(cmd, rt->image.img, VK_IMAGE_LAYOUT_GENERAL, readback.buf, 1, &bic);
        }
        Barrier();
        SubmitAndWait();
        Begin();
        out.assign(pts.size(), {0, 0, 0, 0});
        for (size_t i = 0; i < pts.size(); i++)
            ReadbackToFloat(rt->image.format, static_cast<const uint8_t*>(readback.map) + stride * i, out[i].data());
        (void)bpp;
        for (auto& c : curColor) c = nullptr;
        curDepth = nullptr;
    }

    // Dump frames: the colour target right after a draw (scissor rectangle), as a PNG.
    void DumpTarget(RenderTarget* rt, VkRect2D r, const char* what) {
        static int count = 0;
        static const int maxPng = [] { const char* e = std::getenv("FA_RENDER_DUMP_MAXPNG"); return e ? std::atoi(e) : 300; }();
        if (count >= maxPng || !r.extent.width || !r.extent.height) return;
        uint32_t bpp = VkFormatBytes(rt->image.format);
        VkDeviceSize bytes = VkDeviceSize(r.extent.width) * r.extent.height * bpp;
        EndRendering();
        if (bytes > readback.size) {
            SubmitAndWait();
            Begin();
            DestroyBuffer(readback);
            CreateBuffer(readback, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        }
        Barrier();
        VkBufferImageCopy bic{};
        bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bic.imageOffset = {r.offset.x, r.offset.y, 0};
        bic.imageExtent = {r.extent.width, r.extent.height, 1};
        vkCmdCopyImageToBuffer(cmd, rt->image.img, VK_IMAGE_LAYOUT_GENERAL, readback.buf, 1, &bic);
        Barrier();
        SubmitAndWait();
        Begin();
        std::vector<uint8_t> rgba(size_t(r.extent.width) * r.extent.height * 4);
        const uint8_t* rb = static_cast<const uint8_t*>(readback.map);
        for (size_t i = 0; i < size_t(r.extent.width) * r.extent.height; i++) {
            float f[4];
            ReadbackToFloat(rt->image.format, rb + i * bpp, f);
            for (int c = 0; c < 3; c++) rgba[i * 4 + c] = uint8_t(std::lround(std::clamp(f[c], 0.0f, 1.0f) * 255.0f));
            rgba[i * 4 + 3] = 255;
        }
        char path[512];
        std::snprintf(path, sizeof path, "%s/f%04llu_%s%05llu_tile%03x_%ux%u.png", dumpDir, (unsigned long long)frame, what,
                      (unsigned long long)stats.draws.load(), rt->base & 0xFFF, r.extent.width, r.extent.height);
        WritePng(path, r.extent.width, r.extent.height, rgba.data());
        count++;
        for (auto& c : curColor) c = nullptr;
        curDepth = nullptr;
    }

    // Dump frames: what the draw reads (textures, vertex buffers, a few vertices).
    void TraceInputs(const RegisterFile& regs, Shader* vs, Shader* ps, const XeSystemConstants& sys) {
        uint32_t texUsed = vs->info.texUsed | ps->info.texUsed;
        for (uint32_t s = 0; s < 32; s++) {
            if (!(texUsed & (1u << s))) continue;
            uint32_t d[6];
            for (int k = 0; k < 6; k++) d[k] = regs[pm4::kFetchConstBase + s * 6 + k];
            TextureFetch t = ParseTextureFetch(d);
            std::fprintf(stderr,
                         "[render]   tex %u: type %u fmt %u %ux%u depth %u dim %u base %08x pitch %u %s endian %u swz %03x "
                         "clamp %u,%u filter %u/%u | %08x %08x %08x %08x %08x %08x\n",
                         s, t.type, t.format, t.width, t.height, t.depth, t.dimension, t.base, t.pitch,
                         t.tiled ? "tiled" : "linear", t.endian, t.swizzle, t.clamp[0], t.clamp[1], t.magFilter, t.minFilter,
                         d[0], d[1], d[2], d[3], d[4], d[5]);
            if (t.IsTexture() && t.mipMax) {
                const uint32_t levels = HostMipLevels(t), mb = HostMipByteSize(t);
                std::fprintf(stderr, "[render]     mips: filter %u min %u max %u packed %d mip addr %08x -> %u host levels, %u bytes", t.mipFilter,
                             t.mipMin, t.mipMax, t.packedMips ? 1 : 0, t.mipBase, levels, mb);
                const uint8_t* ms = mb ? mem.HostPtr(t.mipBase, mb) : nullptr;
                const uint8_t* bs = levels > 1 ? mem.HostPtr(t.base, HostTextureByteSize(t)) : nullptr;
                HostTexture ht;
                if (ms && bs && DecodeTextureHost(t, bs, ht, ms) && !ht.isFloat && ht.levels > 1) {
                    for (uint32_t m = 0, off = 0; m < ht.levels; m++) {
                        const uint32_t lw = std::max(1u, ht.width >> m), lh = std::max(1u, ht.height >> m);
                        double sum[4] = {};
                        for (size_t i = 0; i < size_t(lw) * lh; i++)
                            for (int c = 0; c < 4; c++) sum[c] += ht.data[off + i * 4 + c];
                        const double n = double(lw) * lh * 255.0;
                        std::fprintf(stderr, " | L%u %ux%u mean %.3f %.3f %.3f %.3f", m, lw, lh, sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n);
                        off += lw * lh * 4;
                    }
                } else if (mb && !ms) {
                    std::fprintf(stderr, " (mip memory not mapped)");
                }
                std::fprintf(stderr, "\n");
            }
            // The texture as the CPU decoder sees it in guest memory, plus whether each row of
            // blocks is made of identical blocks (a sign of degenerate source data).
            static int texDumps = 0;
            if (t.dimension == 1 && GetFormatInfo(t.format).supported) {
                const uint8_t* src = mem.HostPtr(t.base, TextureByteSize(t));
                std::vector<uint8_t> rgba;
                if (src && DecodeTexture(t, src, rgba)) {
                    double sum[4] = {};
                    for (size_t i = 0; i < rgba.size(); i++) sum[i & 3] += rgba[i];
                    const double n = double(rgba.size() / 4) * 255.0;
                    std::fprintf(stderr, "[render]     mean rgba %.3f %.3f %.3f %.3f\n", sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n);
                    char path[512];
                    std::snprintf(path, sizeof path, "%s/f%04llu_tex_%08x_fmt%u_%ux%u.png", dumpDir ? dumpDir : ".", (unsigned long long)frame,
                                  t.base, t.format, t.width, t.height);
                    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
                    if (dumpDir && texDumps < 400 && dumpedTexBases.insert(t.base).second) { texDumps++; WritePng(path, t.width, t.height, rgba.data()); }
                    uint32_t constRows = 0;
                    for (uint32_t y = 0; y < t.height; y++) {
                        const uint8_t* row = &rgba[size_t(y) * t.width * 4];
                        bool same = true;
                        for (uint32_t x = 1; x < t.width && same; x++) same = !std::memcmp(row, row + x * 4, 3);
                        constRows += same;
                    }
                    std::fprintf(stderr, "[render]     decoded: %u of %u rows are one colour; raw:", constRows, t.height);
                    for (int i = 0; i < 32; i++) std::fprintf(stderr, "%s%02x", i % 4 ? "" : " ", src[i]);
                    std::fprintf(stderr, "\n");
                }
            } else if (t.IsTexture() && t.base && GetHostFormatInfo(t.format).supported) {
                // Cube, 3D, stacked and the float / compressed-normal formats: mean of the host decode.
                const uint8_t* src = mem.HostPtr(t.base, HostTextureByteSize(t));
                HostTexture ht;
                if (!src) {
                    std::fprintf(stderr, "[render]     (guest memory not mapped for %u bytes)\n", HostTextureByteSize(t));
                } else if (DecodeTextureHost(t, src, ht)) {
                    double sum[4] = {};
                    const size_t n = size_t(ht.width) * ht.height * ht.layers;
                    for (size_t i = 0; i < n; i++)
                        for (int c = 0; c < 4; c++) {
                            if (ht.isFloat) { float f; std::memcpy(&f, &ht.data[(i * 4 + c) * 4], 4); sum[c] += f; }
                            else sum[c] += ht.data[i * 4 + c] / 255.0;
                        }
                    std::fprintf(stderr, "[render]     host mean rgba %.3f %.3f %.3f %.3f (%u layers%s)\n", sum[0] / n, sum[1] / n, sum[2] / n,
                                 sum[3] / n, ht.layers, ht.isFloat ? ", float" : "");
                }
            }
        }
        for (uint32_t s = 0; s < 96; s++) {
            if (!(vs->info.vfetchUsed[s / 32] & (1u << (s % 32)))) continue;
            xe::VertexFetch vf = xe::ParseVertexFetch(regs[pm4::kFetchConstBase + s * 2], regs[pm4::kFetchConstBase + s * 2 + 1]);
            std::fprintf(stderr, "[render]   vf %u: type %u addr %08x words %u endian %u | first words:", s, vf.type, vf.address,
                         vf.sizeWords, vf.endian);
            for (uint32_t i = 0; i < std::min<uint32_t>(vf.sizeWords, 24); i++) std::fprintf(stderr, " %08x", mem.Read32(vf.address + i * 4));
            std::fprintf(stderr, "\n");
        }
        auto c = [&](uint32_t i, int k) { return xe::AsFloat(regs[pm4::kAluConstBase + i * 4 + k]); };
        std::fprintf(stderr, "[render]   blend %08x colorctl %08x depthctl %08x | ps c0..c3", regs[xe::RB_BLENDCONTROL0],
                     regs[xe::RB_COLORCONTROL], regs[xe::RB_DEPTHCONTROL]);
        for (uint32_t i = 256; i < 260; i++) std::fprintf(stderr, " (%g %g %g %g)", c(i, 0), c(i, 1), c(i, 2), c(i, 3));
        std::fprintf(stderr, " | vs c255 (%g %g %g %g)\n", c(255, 0), c(255, 1), c(255, 2), c(255, 3));
        // Non-zero constants c0..c31 of each stage (colours, fades, colour matrices live here).
        std::fprintf(stderr, "[render]   alpha ref %g sc_mode %08x clip %08x\n", xe::AsFloat(regs[xe::RB_ALPHA_REF]),
                     regs[xe::PA_SU_SC_MODE_CNTL], regs[xe::PA_CL_CLIP_CNTL]);
        for (uint32_t base : {0u, 256u}) {
            std::fprintf(stderr, "[render]   %s consts:", base ? "ps" : "vs");
            for (uint32_t i = base; i < base + 256; i++) {
                if (i >= base + 32 && i < base + 224) continue;  // c0..c31 and c224..c255
                if (!regs[pm4::kAluConstBase + i * 4] && !regs[pm4::kAluConstBase + i * 4 + 1] &&
                    !regs[pm4::kAluConstBase + i * 4 + 2] && !regs[pm4::kAluConstBase + i * 4 + 3])
                    continue;
                std::fprintf(stderr, " c%u(%g %g %g %g)", i - base, c(i, 0), c(i, 1), c(i, 2), c(i, 3));
            }
            std::fprintf(stderr, "\n");
        }
        std::fprintf(stderr, "[render]   vport scale %g %g %g offset %g %g %g ndc scale %g %g offset %g %g flags %x %x %x\n",
                     sys.vportScale[0], sys.vportScale[1], sys.vportScale[2], sys.vportOffset[0], sys.vportOffset[1],
                     sys.vportOffset[2], sys.ndcScale[0], sys.ndcScale[1], sys.ndcOffset[0], sys.ndcOffset[1], sys.flags[0],
                     sys.flags[1], sys.flags[2]);
    }

    void RecoverConsts(const uint32_t* consts, VkDeviceSize& constsOff, VkDeviceSize& boolOff, const RegisterFile& regs,
                       VkDeviceSize uboAlign) {
        bool f;
        constsOff = RingAlloc(8192, uboAlign, f);
        std::memcpy(static_cast<uint8_t*>(ring.map) + constsOff, consts, 8192);
        lastConsts.assign(consts, consts + 2048);
        lastConstsOff = constsOff;
        boolOff = RingAlloc(160, uboAlign, f);
        std::memcpy(static_cast<uint8_t*>(ring.map) + boolOff, &regs.r[pm4::kBoolConstBase], 160);
    }
    void ReuploadVbs(XeSystemConstants& sys, Shader* vs, const RegisterFile& regs) {
        bool f;
        for (uint32_t s = 0; s < 96; s++) {
            if (!(vs->info.vfetchUsed[s / 32] & (1u << (s % 32))) || !sys.vf[s][2]) continue;
            xe::VertexFetch vf = xe::ParseVertexFetch(regs[pm4::kFetchConstBase + s * 2], regs[pm4::kFetchConstBase + s * 2 + 1]);
            const uint8_t* src = mem.HostPtr(vf.address, sys.vf[s][2] * 4);
            if (!src) { sys.vf[s][2] = 0; continue; }
            VkDeviceSize at = RingAlloc(sys.vf[s][2] * 4, 16, f);
            std::memcpy(static_cast<uint8_t*>(ring.map) + at, src, sys.vf[s][2] * 4);
            sys.vf[s][0] = uint32_t(at / 4);
            vbCache[(uint64_t(vf.address) << 32) ^ sys.vf[s][2]] = at;
        }
    }

    VkRect2D Scissor(const RegisterFile& regs, uint32_t w, uint32_t h) {
        uint32_t tl = regs[xe::PA_SC_WINDOW_SCISSOR_TL], br = regs[xe::PA_SC_WINDOW_SCISSOR_BR];
        int32_t x0 = int32_t(tl & 0x3FFF), y0 = int32_t((tl >> 16) & 0x3FFF);
        int32_t x1 = int32_t(br & 0x3FFF), y1 = int32_t((br >> 16) & 0x3FFF);
        if (x1 == 0 && y1 == 0) { x1 = int32_t(w); y1 = int32_t(h); }
        if (!(tl >> 31)) {
            int32_t ox = xe::SBits(regs[xe::PA_SC_WINDOW_OFFSET], 0, 15), oy = xe::SBits(regs[xe::PA_SC_WINDOW_OFFSET], 16, 15);
            x0 += ox; x1 += ox; y0 += oy; y1 += oy;
        }
        uint32_t stl = regs[xe::PA_SC_SCREEN_SCISSOR_TL], sbr = regs[xe::PA_SC_SCREEN_SCISSOR_BR];
        if (sbr) {
            x0 = std::max(x0, xe::SBits(stl, 0, 15));
            y0 = std::max(y0, xe::SBits(stl, 16, 15));
            x1 = std::min(x1, xe::SBits(sbr, 0, 15));
            y1 = std::min(y1, xe::SBits(sbr, 16, 15));
        }
        x0 = std::clamp(x0, 0, int32_t(w));
        y0 = std::clamp(y0, 0, int32_t(h));
        x1 = std::clamp(x1, x0, int32_t(w));
        y1 = std::clamp(y1, y0, int32_t(h));
        return VkRect2D{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
    }

    // ---- resolves ----------------------------------------------------------------------------
    void Resolve(const RegisterFile& regs) {
        prof::Scope ps_(prof::kResolve);
        stats.resolves++;
        Begin();
        const uint32_t copyCtl = regs[xe::RB_COPY_CONTROL];
        const uint32_t srcSel = copyCtl & 7;
        const bool clearColor = (copyCtl >> 8) & 1, clearDepth = (copyCtl >> 9) & 1;
        const uint32_t copyCmd = (copyCtl >> 20) & 3;

        // Rectangle from the three vertices of vertex fetch constant 0 (6 floats).
        xe::VertexFetch vf = xe::ParseVertexFetch(regs[pm4::kFetchConstBase], regs[pm4::kFetchConstBase + 1]);
        if (vf.type != 3) return;
        float v[6];
        for (int i = 0; i < 6; i++) {
            uint32_t raw = mem.Read32(vf.address + i * 4);  // host order of a big-endian dword
            // Read32 already returns the big-endian value; other endian modes are not used here.
            v[i] = xe::AsFloat(raw);
        }
        const float half = (regs[xe::PA_SU_VTX_CNTL] & 1) ? 0.0f : 0.5f;
        auto fx = [&](float f) { return int32_t(std::lround((f + half) * 256.0f)); };
        int32_t x0 = std::min({fx(v[0]), fx(v[2]), fx(v[4])}), y0 = std::min({fx(v[1]), fx(v[3]), fx(v[5])});
        int32_t x1 = std::max({fx(v[0]), fx(v[2]), fx(v[4])}), y1 = std::max({fx(v[1]), fx(v[3]), fx(v[5])});
        x0 = (x0 + 127) >> 8; y0 = (y0 + 127) >> 8; x1 = (x1 + 127) >> 8; y1 = (y1 + 127) >> 8;
        if ((regs[xe::PA_SU_SC_MODE_CNTL] >> 16) & 1) {
            int32_t ox = xe::SBits(regs[xe::PA_SC_WINDOW_OFFSET], 0, 15), oy = xe::SBits(regs[xe::PA_SC_WINDOW_OFFSET], 16, 15);
            x0 += ox; x1 += ox; y0 += oy; y1 += oy;
        }
        const uint32_t surf = regs[xe::RB_SURFACE_INFO];
        const uint32_t pitch = surf & 0x3FFF, msaa = (surf >> 16) & 3;
        x0 = std::max(x0, 0) & ~7;
        y0 = std::max(y0, 0) & ~7;
        x1 = (std::max(x1, 0) + 7) & ~7;
        y1 = (std::max(y1, 0) + 7) & ~7;
        if (pitch) x1 = std::min<int32_t>(x1, int32_t(pitch & ~7u));
        if (x1 <= x0 || y1 <= y0) return;

        static const uint32_t infoRegs[4] = {xe::RB_COLOR_INFO, xe::RB_COLOR1_INFO, xe::RB_COLOR2_INFO, xe::RB_COLOR3_INFO};
        const bool isDepth = srcSel >= 4;
        uint32_t srcInfo = isDepth ? regs[xe::RB_DEPTH_INFO] : regs[infoRegs[srcSel & 3]];
        uint32_t srcFormat = isDepth ? (srcInfo >> 16) & 1 : (srcInfo >> 16) & 0xF;
        RenderTarget* src = GetRt(srcInfo & 0xFFF, srcFormat, pitch, msaa, isDepth);
        if (!src) return;
        uint32_t w = uint32_t(x1 - x0), h = uint32_t(y1 - y0);
        w = std::min(w, src->image.w - std::min<uint32_t>(src->image.w, uint32_t(x0)));
        h = std::min(h, src->image.h - std::min<uint32_t>(src->image.h, uint32_t(y0)));
        if (!w || !h) return;
        EndRendering();

        // Copy EDRAM -> guest memory.
        if (copyCmd == 0 || copyCmd == 1) {
            const uint32_t destInfo = regs[xe::RB_COPY_DEST_INFO];
            const uint32_t destPitchReg = regs[xe::RB_COPY_DEST_PITCH];
            ResolveDest d;
            d.base = regs[xe::RB_COPY_DEST_BASE];
            d.pitch = destPitchReg & 0x3FFF;
            d.height = (destPitchReg >> 16) & 0x3FFF;
            d.format = isDepth ? xe::kTex24_8 : (destInfo >> 7) & 0x3F;
            d.endian = destInfo & 7;
            d.swapRB = (destInfo >> 24) & 1;
            d.expBias = xe::SBits(destInfo, 16, 6);
            if (!d.height) d.height = uint32_t(y1);
            if (!d.pitch) d.pitch = uint32_t(x1);
            if (d.format == xe::kTex8_A || d.format == xe::kTex8_B) d.format = xe::kTex8;
            if (!ResolveFormatSupported(d.format)) {
                static int warned = 0;
                if (warned++ < 10) std::fprintf(stderr, "[render] resolve to unsupported format %u\n", d.format);
            } else {
                // Snapshot-able 8888 surface from the top-left corner: keep it on the GPU, write guest memory later.
                if (snapEnabled && !isDepth && d.expBias == 0 && SnapResolvable(src->image.format, d.format) &&
                    (SnapClass(d.format) <= 2 || !d.swapRB) &&
                    d.endian <= 3 && x0 == 0 && y0 == 0 && !DumpFrame()) {
                    const uint32_t destBytes = ResolveDestByteSize(d);
                    const uint32_t key = d.base & 0x1FFFFFFFu;
                    if (mem.HostPtrWrite(d.base, destBytes) && SnapStore(src, d, destBytes, key, w, h)) {
                        if (!lazyWriteback) FlushPending();  // eager: write guest memory right away
                        goto clears;
                    }
                }
                SnapLog("resolve", isDepth ? "depth" : (x0 || y0) ? "offset" : d.endian > 3 ? "endian" : d.expBias ? "expbias" : "format", d.base & 0x1FFFFFFFu,
                        d.format, src->image.format, w, h, d.endian, uint32_t(x0 | y0));
                if (!pendingKeys.empty() && PendingOverlaps(d.base, ResolveDestByteSize(d))) FlushPending(prof::kFlushOverlap);
                uint32_t bpp = isDepth ? 4 : VkFormatBytes(src->image.format);
                VkDeviceSize bytes = VkDeviceSize(w) * h * bpp;
                if (bytes > readback.size) {
                    SubmitAndWait();
                    Begin();
                    DestroyBuffer(readback);
                    CreateBuffer(readback, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
                }
                VkBufferImageCopy bic{};
                bic.imageSubresource = {VkImageAspectFlags(isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, 1};
                bic.imageOffset = {x0, y0, 0};
                bic.imageExtent = {w, h, 1};
                vkCmdCopyImageToBuffer(cmd, src->image.img, VK_IMAGE_LAYOUT_GENERAL, readback.buf, 1, &bic);
                Barrier();
                {
                    prof::Scope pw_(prof::kResolveWait);
                    SubmitAndWait();
                }
                Begin();
                prof::Scope pc_(prof::kResolveCpu);
                const uint8_t* rb = static_cast<const uint8_t*>(readback.map);
                uint32_t destBytes = ResolveDestByteSize(d);
                if (!DumpFrame() && ResolveRaw(d, src->image.format, isDepth, uint32_t(x0), uint32_t(y0), w, h, rb, destBytes)) {
                    InvalidateTextures(d.base, destBytes);
                    goto clears;
                }
                {
                // Destination offset: the resolve rectangle's position in EDRAM.
                std::vector<float>& px = resolvePx;  // reused: a fresh 15 MB vector per resolve was costly
                px.resize(size_t(w) * h * 4);
                const bool fix1616 = !isDepth && Rt1616Scaled() && (srcFormat == xe::kRt16_16 || srcFormat == xe::kRt16_16_16_16) &&
                                     d.format >= 24 && d.format <= 26;
                auto toFloat = [&](size_t i) {
                    if (isDepth) {
                        float z;
                        if (depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT) std::memcpy(&z, rb + i * 4, 4);
                        else { uint32_t u; std::memcpy(&u, rb + i * 4, 4); z = float(u & 0xFFFFFF) / 16777215.0f; }
                        px[i * 4] = z;
                        px[i * 4 + 1] = 0.0f;
                        px[i * 4 + 2] = 0.0f;
                        px[i * 4 + 3] = 1.0f;
                    } else {
                        ReadbackToFloat(src->image.format, rb + i * bpp, &px[i * 4]);
                        if (fix1616)
                            for (int c = 0; c < 4; c++) px[i * 4 + c] *= (1.0f / 32.0f);
                    }
                };
                uint8_t* dst = mem.HostPtrWrite(d.base, destBytes);
                if (dst) {
                    // Bands of rows in parallel: each converts its rows and writes their texels.
                    rowPool.Run(h, [&](uint32_t ya, uint32_t yb) {
                        for (size_t i = size_t(ya) * w; i < size_t(yb) * w; i++) toFloat(i);
                        EncodeResolve(d, uint32_t(x0), uint32_t(y0) + ya, w, yb - ya, px.data() + size_t(ya) * w * 4, dst);
                    });
                } else {
                    for (size_t i = 0; i < size_t(w) * h; i++) toFloat(i);
                    // No direct pointer: encode into a copy of the surface and write it back by dwords.
                    std::vector<uint8_t> tmp(destBytes);
                    for (uint32_t i = 0; i + 4 <= destBytes; i += 4) {
                        uint32_t be = mem.Read32(d.base + i);
                        tmp[i] = uint8_t(be >> 24); tmp[i + 1] = uint8_t(be >> 16); tmp[i + 2] = uint8_t(be >> 8); tmp[i + 3] = uint8_t(be);
                    }
                    EncodeResolve(d, uint32_t(x0), uint32_t(y0), w, h, px.data(), tmp.data());
                    for (uint32_t i = 0; i + 4 <= destBytes; i += 4)
                        mem.Write32(d.base + i, (uint32_t(tmp[i]) << 24) | (uint32_t(tmp[i + 1]) << 16) | (uint32_t(tmp[i + 2]) << 8) | tmp[i + 3]);
                }
                InvalidateTextures(d.base, destBytes);
                WatchBytes("after resolve", d.base, destBytes, dst ? "(direct write)" : "(dword write path)");
                static int logged = 0;
                if (logged++ < std::max(16, opt.traceDraws * 4) || DumpFrame())
                    std::fprintf(stderr,
                                 "[render] frame %llu resolve %s rt %u (%d,%d %ux%u) -> %08x fmt %u pitch %u height %u endian %u%s bias %d "
                                 "| src %03x/%u surf %08x wo %08x sc %08x vtx %.1f,%.1f %.1f,%.1f %.1f,%.1f\n",
                                 (unsigned long long)frame, isDepth ? "depth" : "color", srcSel, x0, y0, w, h, d.base, d.format, d.pitch, d.height,
                                 d.endian, d.swapRB ? " swapRB" : "", d.expBias, srcInfo & 0xFFF, srcFormat, surf,
                                 regs[xe::PA_SC_WINDOW_OFFSET], regs[xe::PA_SU_SC_MODE_CNTL], v[0], v[1], v[2], v[3], v[4], v[5]);
                DumpResolve(px, w, h, d, srcInfo & 0xFFF, srcFormat, uint32_t(x0), uint32_t(y0));
                }
            }
        }

    clears:
        // Clears of the resolved rectangle.
        if (clearColor && !isDepth) ClearRect(src, regs, x0, y0, w, h, false);
        if (clearDepth) {
            uint32_t dinfo = regs[xe::RB_DEPTH_INFO];
            RenderTarget* z = GetRt(dinfo & 0xFFF, (dinfo >> 16) & 1, pitch, msaa, true);
            if (z) ClearRect(z, regs, x0, y0, w, h, true);
        }
    }

    // Fast resolve: the render target already holds the destination's texel layout (8888,
    // 2_10_10_10, 16-bit and 32-bit float formats), so texels are copied as bytes, tiled and
    // endian swapped without going through floats. Returns false when it does not apply.
    bool ResolveRaw(const ResolveDest& d, VkFormat srcFmt, bool isDepth, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h,
                    const uint8_t* rb, uint32_t destBytes) {
        if (isDepth || d.expBias != 0) return false;
        const bool src8888 = srcFmt == VK_FORMAT_R8G8B8A8_UNORM, src1010 = srcFmt == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        const bool is8888 = (src8888 || src1010) && (d.format == 6 || d.format == 14 || d.format == 50);  // is8888/is1010: the guest format
        const bool is1010 = (src8888 || src1010) && (d.format == 7 || d.format == 54);
        const bool convert = (is8888 && src1010) || (is1010 && src8888);  // 10-bit <-> 8-bit conversion on the way out
        const bool isFloat = (srcFmt == VK_FORMAT_R16G16_SFLOAT && d.format == 31) ||
                             (srcFmt == VK_FORMAT_R16G16B16A16_SFLOAT && d.format == 32) ||
                             (srcFmt == VK_FORMAT_R32_SFLOAT && d.format == 36) ||
                             (srcFmt == VK_FORMAT_R32G32_SFLOAT && d.format == 37);
        if (!is8888 && !is1010 && !(isFloat && !d.swapRB)) return false;
        const uint32_t bpp = VkFormatBytes(srcFmt);
        uint32_t x8 = 0;  // an endian swap is a XOR on the byte address within the swapped unit
        switch (d.endian) {
            case 0: break;
            case 1: x8 = 1; break;  // 8in16
            case 2: x8 = 3; break;  // 8in32
            case 3: x8 = 2; break;  // 16in32
            case 4: x8 = 7; break;  // 8in64
            default: return false;
        }
        if (x8 >= bpp) return false;
        uint8_t* dst = mem.HostPtrWrite(d.base, destBytes);
        if (!dst) return false;
        const uint32_t pitch = (std::max(d.pitch, 32u) + 31) & ~31u, height = (std::max(d.height, 1u) + 31) & ~31u;
        rowPool.Run(h, [&](uint32_t ya, uint32_t yb) {
        for (uint32_t y = ya; y < yb; y++) {
            const uint32_t ty = dy + y;
            if (ty >= height) break;
            const uint8_t* row = rb + size_t(y) * w * bpp;
            for (uint32_t x = 0; x < w; x++) {
                const uint32_t tx = dx + x;
                if (tx >= pitch) break;
                const uint32_t off = TiledBlockOffset(tx, ty, pitch, bpp);
                if (off + bpp > destBytes) continue;
                if (bpp == 4) {
                    uint32_t v;
                    std::memcpy(&v, row + x * 4, 4);
                    if (convert) {
                        if (src1010) {  // R10 G10 B10 A2 -> R8 G8 B8 A8
                            v = ((((v & 0x3FF) * 255 + 511) / 1023)) | (((((v >> 10) & 0x3FF) * 255 + 511) / 1023) << 8) |
                                (((((v >> 20) & 0x3FF) * 255 + 511) / 1023) << 16) | ((v >> 30) * 85u) << 24;
                        } else {        // R8 G8 B8 A8 -> R10 G10 B10 A2
                            v = ((((v & 0xFF) * 1023 + 127) / 255)) | (((((v >> 8) & 0xFF) * 1023 + 127) / 255) << 10) |
                                (((((v >> 16) & 0xFF) * 1023 + 127) / 255) << 20) | ((((v >> 24) * 3 + 127) / 255) << 30);
                        }
                    }
                    if (d.swapRB) {
                        if (is8888) v = (v & 0xFF00FF00u) | ((v & 0xFF) << 16) | ((v >> 16) & 0xFF);
                        else v = (v & 0xC00FFC00u) | ((v & 0x3FF) << 20) | ((v >> 20) & 0x3FF);
                    }
                    switch (x8) {
                        case 1: v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu); break;
                        case 3: v = __builtin_bswap32(v); break;
                        case 2: v = (v << 16) | (v >> 16); break;
                        default: break;
                    }
                    std::memcpy(dst + off, &v, 4);
                } else {
                    const uint8_t* p = row + size_t(x) * bpp;
                    for (uint32_t k = 0; k < bpp; k++) dst[(off + k) ^ x8] = p[k];
                }
            }
        }
        });
        return true;
    }

    // FA_RENDER_DUMP=<dir>: PNGs of the first resolves (clamped to 0..1), for debugging.
    void DumpResolve(const std::vector<float>& px, uint32_t w, uint32_t h, const ResolveDest& d, uint32_t tile,
                     uint32_t fmt, uint32_t dx, uint32_t dy) {
        if (!DumpFrame()) return;
        static int count = 0;
        if (count >= 400) return;
        std::vector<uint8_t> rgba(size_t(w) * h * 4);
        for (size_t i = 0; i < rgba.size(); i++) {
            float v = (i % 4 == 3) ? 1.0f : px[i];  // force opaque so colour shows
            rgba[i] = uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
        }
        char path[512];
        std::snprintf(path, sizeof path, "%s/f%04llu_resolve%03d_%08x_fmt%u_from%03x_rt%u_%ux%u.png", dumpDir,
                      (unsigned long long)frame, count, d.base, d.format, tile, fmt, w, h);
        WritePng(path, w, h, rgba.data());
        // What actually landed in guest memory, decoded the way a texture fetch would read it.
        TextureFetch t;
        t.type = 2;
        t.format = d.format;
        t.endian = d.endian & 3;
        t.base = d.base;
        t.pitch = (std::max(d.pitch, 32u) + 31) & ~31u;
        t.width = std::min(t.pitch, dx + w);
        t.height = std::max(d.height, dy + h);
        t.tiled = true;
        std::vector<uint8_t> dec;
        const uint8_t* src = mem.HostPtr(d.base, TextureByteSize(t));
        if (src && GetFormatInfo(t.format).supported && DecodeTexture(t, src, dec)) {
            for (size_t i = 3; i < dec.size(); i += 4) dec[i] = 255;
            std::snprintf(path, sizeof path, "%s/f%04llu_resolve%03d_%08x_inmemory_%ux%u.png", dumpDir,
                          (unsigned long long)frame, count, d.base, t.width, t.height);
            WritePng(path, t.width, t.height, dec.data());
        }
        count++;
    }

    void ClearRect(RenderTarget* rt, const RegisterFile& regs, int32_t x, int32_t y, uint32_t w, uint32_t h, bool depth) {
        stats.clears++;
        EndRendering();
        RenderTarget* color[4] = {};
        if (!depth) color[0] = rt;
        w = std::min(w, rt->image.w - std::min<uint32_t>(rt->image.w, uint32_t(x)));
        h = std::min(h, rt->image.h - std::min<uint32_t>(rt->image.h, uint32_t(y)));
        if (!w || !h) return;
        BeginRendering(color, depth ? rt : nullptr, VkRect2D{{0, 0}, {rt->image.w, rt->image.h}});
        VkClearAttachment ca{};
        if (depth) {
            uint32_t v = regs[xe::RB_DEPTH_CLEAR];
            ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            ca.clearValue.depthStencil = {float(v >> 8) / 16777215.0f, v & 0xFF};
        } else {
            float c[4];
            ClearColorFloat(rt->xeFormat, regs[xe::RB_COLOR_CLEAR], regs[xe::RB_COLOR_CLEAR_LO], c);
            ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            ca.colorAttachment = 0;
            std::memcpy(ca.clearValue.color.float32, c, 16);
        }
        VkClearRect cr{{{x, y}, {w, h}}, 0, 1};
        vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
        EndRendering();
        for (auto& c : curColor) c = nullptr;
        curDepth = nullptr;
    }

    void Swap() {
        FlushPending(prof::kFlushSwap);
        {
            prof::Scope ps_(prof::kOtherWait);
            SubmitAndWait();
        }
        Begin();
        generation++;
        {   // Frames that take over 250 ms (e.g. the first-use texture burst) are always reported.
            static uint64_t lastNs = 0, lastUploads = 0;
            const uint64_t now = prof::NowNs();
            if (lastNs && now - lastNs > 250000000ull)
                std::fprintf(stderr, "[slowframe] frame %llu took %.0f ms, %llu texture uploads\n", (unsigned long long)frame,
                             double(now - lastNs) / 1e6, (unsigned long long)(stats.textureUploads - lastUploads));
            lastNs = now;
            lastUploads = stats.textureUploads;
        }
        if (probeOn && frame >= probeFrame && frame <= probeFrameEnd) {
            std::fprintf(stderr, "[probe-frame] frame %llu final:", (unsigned long long)frame);
            for (size_t k = 0; k < probePts.size(); k++)
                std::fprintf(stderr, " (%d,%d)=%d,%d,%d@draw%llu", probePts[k].first, probePts[k].second,
                             int(std::lround(std::clamp(probeLast[k][0], 0.0f, 1.0f) * 255.0f)),
                             int(std::lround(std::clamp(probeLast[k][1], 0.0f, 1.0f) * 255.0f)),
                             int(std::lround(std::clamp(probeLast[k][2], 0.0f, 1.0f) * 255.0f)), (unsigned long long)probeLastDraw[k]);
            std::fprintf(stderr, "\n");
        }
        frame++;
        drawInFrame = 0;
        dumpedTexBases.clear();
        if (dumpDir && dumpTrigger && (frame & 7) == 0) {
            if (FILE* tf = std::fopen(dumpTrigger, "r")) {
                std::fclose(tf);
                std::remove(dumpTrigger);
                dumpFrameSel = int64_t(frame);
                dumpDrawLo = 0;
                dumpDrawHi = ~0ull;
                std::fprintf(stderr, "[render] dump trigger: dumping frame %llu\n", (unsigned long long)frame);
                if (probeOnTrigger && probeOn) {
                    probeFrame = probeFrameEnd = frame;
                    probeTraced.clear();
                    for (auto& v : probeLast) v = {-1.0f, -1.0f, -1.0f, -1.0f};
                }
            }
        }
        if (dumpDir && dumpAfterSec >= 0.0 &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count() >= dumpAfterSec) {
            dumpAfterSec = -1.0;
            dumpFrameSel = int64_t(frame);
            std::fprintf(stderr, "[render] timed dump: dumping frame %llu\n", (unsigned long long)frame);
        }
    }
};

// =============================================================================================
VulkanRenderer::VulkanRenderer(GuestMemory& mem, Backend* next) : p_(new Impl(mem, next)) {}
VulkanRenderer::~VulkanRenderer() = default;

bool VulkanRenderer::Init(const Options& opt, std::string& err) { return p_->Init(opt, err); }
std::string VulkanRenderer::DeviceName() const { return p_->props.deviceName; }
const VulkanRenderer::Stats& VulkanRenderer::GetStats() const { return p_->stats; }

void VulkanRenderer::Draw(const DrawCall& draw, const RegisterFile& regs) {
    if (p_->ready && !p_->broken) p_->Draw(draw, regs);
    if (p_->next) p_->next->Draw(draw, regs);
}

void VulkanRenderer::Resolve(const RegisterFile& regs) {
    if (p_->ready && !p_->broken) p_->Resolve(regs);
    if (p_->next) p_->next->Resolve(regs);
}

void VulkanRenderer::Swap(uint32_t addr, uint32_t width, uint32_t height, const RegisterFile& regs) {
    if (p_->ready && !p_->broken) p_->Swap();
    if (p_->next) p_->next->Swap(addr, width, height, regs);
}

void VulkanRenderer::Sync() {
    if (p_->ready && !p_->broken) p_->FlushPending();
    if (p_->next) p_->next->Sync();
}

void VulkanRenderer::Flush() {
    if (p_->ready && !p_->broken) {
        p_->FlushPending();
        p_->SubmitAndWait();
        p_->Begin();
    }
}

}  // namespace fa::gpu
