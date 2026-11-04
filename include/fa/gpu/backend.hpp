#pragma once
// Rendering backend interface. The command processor decodes PM4 and tracks Xenos
// register state, then calls into a backend to actually draw. Implementations:
//   NullBackend    - logs/counts only (tests, headless runs)
//   FrameBackend   - decodes the swapped front buffer to RGBA8 for the window (src/gpu)
#include <array>
#include <cstdint>
#include <vector>

namespace fa::gpu {

constexpr uint32_t kRegisterCount = 0x5003;

struct RegisterFile {
    std::array<uint32_t, kRegisterCount> r{};
    uint32_t& operator[](uint32_t i) { return r[i]; }
    uint32_t operator[](uint32_t i) const { return r[i]; }
};

struct DrawCall {
    uint32_t primType = 0;      // Xenos VGT primitive type
    uint32_t indexCount = 0;
    bool indexed = false;       // false = auto-generated indices
    bool index32 = false;
    uint32_t indexGuestAddr = 0;
    uint32_t indexEndian = 0;   // Xenos endian swap mode for index data
    std::vector<uint32_t> inlineIndices;  // indices carried inside the packet (host order)
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual void Draw(const DrawCall& draw, const RegisterFile& regs) = 0;
    // EVENT_WRITE_EXT resolve: copy EDRAM contents to a texture in guest memory.
    virtual void Resolve(const RegisterFile& regs) = 0;
    // Present: the game finished a frame (VdSwap / PM4 XE_SWAP). VdSwap loads the front
    // buffer's texture fetch constant into fetch slot 0 (regs 0x4800..0x4805) first.
    virtual void Swap(uint32_t frontBufferGuestAddr, uint32_t width, uint32_t height, const RegisterFile& regs) = 0;
};

}  // namespace fa::gpu
