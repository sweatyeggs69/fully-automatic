#pragma once
#include <cstdint>
#include <functional>
#include <vector>
#include "fa/gpu/backend.hpp"
#include "fa/gpu/guest_memory.hpp"

namespace fa::gpu {

// Consumes the PM4 ring buffer the game fills (set up through VdInitializeRingBuffer).
// Single threaded: the runtime core calls Pump() from its GPU thread whenever the
// guest advances the write pointer, or on a timer while a wait is pending.
class CommandProcessor {
public:
    struct Stats {
        uint64_t packets = 0, draws = 0, swaps = 0, unknown = 0, resolves = 0, shaderLoads = 0, predicatedSkips = 0;
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
    Stats stats_;
};

}  // namespace fa::gpu
