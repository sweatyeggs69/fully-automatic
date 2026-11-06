#pragma once
#include <cstdint>
#include <functional>
#include <vector>
#include "fa/gpu/backend.hpp"
#include "fa/gpu/guest_memory.hpp"

namespace fa::gpu {

// Diagnostics: called with the guest address and dword count of every indirect buffer the ring points at.
inline void (*g_indirect_buffer_observer)(uint32_t addr, uint32_t dwords) = nullptr;
// Diagnostics: called for every draw (not resolves) just before the backend sees it.
inline void (*g_draw_observer)(const DrawCall& draw, const RegisterFile& regs) = nullptr;

// Consumes the PM4 ring buffer the game fills (set up through VdInitializeRingBuffer).
// Single threaded: the runtime core calls Pump() from its GPU thread whenever the
// guest advances the write pointer, or on a timer while a wait is pending.
class CommandProcessor {
public:
    struct Stats {
        uint64_t packets = 0, draws = 0, swaps = 0, unknown = 0, resolves = 0, shaderLoads = 0, predicatedSkips = 0;
        uint64_t tileSkips = 0;  // draws and resolves of replayed tiles not sent to the backend (FA_TILE_ONCE)
        uint64_t unknownOps[128] = {};  // unhandled type-3 packets by opcode
    };
    enum class Status { Idle, Blocked };

    CommandProcessor(GuestMemory& mem, Backend& backend);

    // Ring buffer location and size, both in bytes (size power of two).
    void SetRingBuffer(uint32_t baseGuestAddr, uint32_t sizeBytes);
    // Where the GPU mirrors its read pointer (VdEnableRingBufferRPtrWriteBack). 0 = none.
    void SetReadPtrWriteback(uint32_t guestAddr) { rptrWriteback_ = guestAddr; }
    // Called when the guest writes CP_RB_WPTR (the dword write index).
    void SetWritePtr(uint32_t wptrDwords) { wptr_ = wptrDwords; }
    // VdSetGraphicsInterruptCallback hook: fired for PM4 INTERRUPT / swap events.
    void SetInterruptHandler(std::function<void(uint32_t cpuMask)> fn) { interrupt_ = std::move(fn); }

    // Run until the ring is drained or a WAIT_REG_MEM cannot yet be satisfied.
    Status Pump();

    uint32_t ReadPtr() const { return rptr_; }
    const RegisterFile& Regs() const { return regs_; }
    RegisterFile& MutableRegs() { return regs_; }  // MMIO writes from the CPU side
    const Stats& GetStats() const { return stats_; }

private:
    enum class Result { Done, Blocked, Incomplete };
    // Decode and run one packet at `pos` (advanced on Done). Blocked = retry later, Incomplete = not fully written yet.
    Result ExecutePacket(uint32_t& pos, uint32_t base, uint32_t sizeDwords, uint32_t availDwords, int depth);
    Result ExecuteType3(uint32_t op, const std::vector<uint32_t>& payload, int depth);
    uint32_t ReadDword(uint32_t base, uint32_t sizeDwords, uint32_t idx) const;
    void SetReg(uint32_t reg, uint32_t value);
    // A write the guest may wait on: the backend applies it when the GPU caught up, or it lands now.
    void GuestWrite(uint32_t addr, uint32_t value) {
        if (!backend_.WriteWhenDone(addr, value)) mem_.Write32(addr, value);
    }

    GuestMemory& mem_;
    Backend& backend_;
    RegisterFile regs_;
    uint32_t ringBase_ = 0, ringSizeDwords_ = 0, rptr_ = 0, wptr_ = 0, rptrWriteback_ = 0;
    std::function<void(uint32_t)> interrupt_;
    struct IndirectBuffer { bool active = false; uint32_t base = 0, size = 0, pos = 0; } ib_;
    // Shader microcode by type (0 = vertex, 1 = pixel), as the last IM_LOAD left it.
    struct ShaderSlot { std::vector<uint32_t> ucode; uint64_t hash = 0; uint32_t srcAddr = 0; } shaders_[2];
    // Predicated tiling: a type-3 packet with header bit 0 set runs only when
    // (bin_select & bin_mask) != 0. D3D replays a frame once per screen tile this way.
    uint64_t binMask_ = 0xFFFFFFFFull, binSelect_ = 0xFFFFFFFFull;
    void LoadShader(uint32_t type, std::vector<uint32_t>&& ucode, uint32_t srcAddr);
    // Tile replay. D3D's predicated tiling replays the frame once per screen tile (bin select bits
    // 2t and 2t + 1 of the high word select tile t, for its two passes). With FA_TILE_ONCE=1 the
    // first tile is drawn at full frame height and the draws and resolves of the other tiles are
    // dropped; every other packet (registers, fences, waits) still runs. The tile height is learnt
    // from the second tile's scissor, so the first tiled frame is drawn the normal way.
    int TileOf() const {
        const uint32_t hi = uint32_t(binSelect_ >> 32) & 0x3FFFFFFFu;
        return (binSelect_ == ~0ull || !hi) ? -1 : __builtin_ctz(hi) / 2;
    }
    bool tileOnce_ = false, sawTile0_ = false;
    uint32_t tileRows_ = 0, tileSurface_ = 0;
    uint64_t tileSkips0_ = 0;
    // FA_TILE_TRACE=<first swap>[,<frames>], or touching ~/.cache/fullauto/tile_trace: logs the frame's structure (indirect buffers, bin
    // mask/select, tile window and target registers, draw runs, resolves) for tile replay work.
    struct TileTrace {
        uint64_t first = ~0ull, frames = 1;
        bool on = false;
        uint32_t draws = 0, depth = 0;
        uint64_t skips0 = 0;
    } tt_;
    void TraceFlushDraws();
    void TraceReg(uint32_t reg, uint32_t value);
    void TraceSwap();
    Stats stats_;
    uint32_t curPacket_ = 0;  // physical address of the packet being executed
    std::vector<uint32_t> payload_;  // scratch for the packet being executed
};

}  // namespace fa::gpu
