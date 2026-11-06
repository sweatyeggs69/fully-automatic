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
    // Float constant blocks (64 dwords = 16 vec4 each, 32 blocks) written since the renderer last
    // uploaded them. Set by the command processor, cleared by the renderer as it uploads.
    mutable uint32_t aluDirty = ~0u;
    uint32_t& operator[](uint32_t i) { return r[i]; }
    uint32_t operator[](uint32_t i) const { return r[i]; }
};

struct DrawCall {
    uint32_t primType = 0;      // Xenos VGT primitive type
    uint32_t indexCount = 0;
    bool indexed = false;       // false = auto-generated indices
    bool index32 = false;
    uint32_t indexGuestAddr = 0;
    uint32_t packetAddr = 0;    // physical address of the DRAW_INDX packet header (diagnostics, direct hooks)
    uint32_t indexEndian = 0;   // Xenos endian swap mode for index data
    std::vector<uint32_t> inlineIndices;  // indices carried inside the packet (host order)
    // Shader microcode loaded with IM_LOAD / IM_LOAD_IMMEDIATE (host order dwords), owned by the
    // command processor and valid during the Draw call.
    const uint32_t* vsUcode = nullptr;
    uint32_t vsDwords = 0;
    uint64_t vsHash = 0;
    const uint32_t* psUcode = nullptr;
    uint32_t psDwords = 0;
    uint64_t psHash = 0;
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual void Draw(const DrawCall& draw, const RegisterFile& regs) = 0;
    // Resolve: a draw issued with RB_MODECONTROL.edram_mode = copy. Copies (and optionally
    // clears) an EDRAM rectangle into guest memory as described by RB_COPY_* registers; the
    // rectangle comes from the three vertices at vertex fetch constant 0.
    virtual void Resolve(const RegisterFile& regs) = 0;
    // Present: the game finished a frame (VdSwap / PM4 XE_SWAP). VdSwap loads the front
    // buffer's texture fetch constant into fetch slot 0 (regs 0x4800..0x4805) first.
    virtual void Swap(uint32_t frontBufferGuestAddr, uint32_t width, uint32_t height, const RegisterFile& regs) = 0;
    // The command stream is about to make GPU progress visible to the guest (fence, memory write,
    // interrupt): backends that defer writes into guest memory must finish them now.
    virtual void Sync() {}
    // A write of the command stream the guest may wait on (fence, memory write). A backend that runs
    // the GPU behind the command stream applies it once the GPU finished the work before it and
    // returns true; false means the command processor writes it now.
    virtual bool WriteWhenDone(uint32_t addr, uint32_t value) { return false; }
    // The command processor ran out of commands or is blocked waiting: a good moment to hand
    // recorded work to the GPU and apply finished writes.
    virtual void Idle() {}
};

}  // namespace fa::gpu
