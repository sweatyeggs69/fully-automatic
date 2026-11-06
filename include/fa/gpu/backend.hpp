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
    // Tile replay (FA_TILE_ONCE=1): non-zero while the command processor runs the first tile of a
    // tiled frame and skips the others. It is the tile height in rows: a scissor or resolve that
    // ends there stands for the whole frame, so the backend extends it to the full target.
    uint32_t tileRows = 0;
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

// Direct draw record (direct graphics hooks, notes/direct-draw-record-spec.md): one draw as the game
// issued it through D3D, read from the game's device shadow instead of decoded from PM4.
// Register state travels as a payload of register runs holding only what changed since the
// previous record (constants, fetch constants, bool/loop constants and render state are all
// registers: kAluConstBase.., kFetchConstBase.., kBoolConstBase.., 0x2000..). The first record of a
// frame carries everything. The payload lives in the producer's frame arena until the backend has
// finished the GPU work that used it.
struct DirectRegRun {
    uint32_t first;  // register index
    uint32_t count;  // dwords of values (host order) that follow this header
};
struct DirectDraw {
    uint32_t seq = 0;          // draw order within the frame
    uint32_t packetAddr = 0;   // matching DRAW_INDX packet while the command processor still runs (0 = none)
    uint32_t primType = 0;     // Xenos VGT primitive type
    uint32_t indexCount = 0;
    uint32_t indexAddr = 0;    // guest address of the first index; 0 = auto indices
    uint8_t index32 = 0, indexEndian = 0;
    // Identities: the game's shader and vertex declaration objects, and where the microcode is.
    uint32_t vsObj = 0, vsUcode = 0, vsBytes = 0;
    uint32_t psObj = 0, psUcode = 0, psBytes = 0;
    uint32_t declObj = 0;
    // The game's own dirty masks for this draw (informational: the payload is authoritative).
    uint64_t vsConstDirty = 0, psConstDirty = 0;
    uint32_t fetchDirty = 0, stateDirty = 0;
    uint32_t payloadBytes = 0;  // bytes of DirectRegRun headers and values
    uint32_t tileRows = 0;      // RegisterFile::tileRows of the command processor's tile replay skip (0 = none)
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
    // An interrupt follows: every write deferred with WriteWhenDone must be in guest memory before the
    // game's interrupt handler runs (it reads the fences and scratch registers the stream wrote).
    virtual void Drain() {}
    // Direct draw record: applies the payload to the backend's own register state and draws with it.
    // Returns false when the backend cannot draw records (the caller falls back to the command stream).
    virtual bool DrawDirect(const DirectDraw& draw, const uint8_t* payload) { (void)draw; (void)payload; return false; }
};

}  // namespace fa::gpu
