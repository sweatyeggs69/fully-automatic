#include <cstdio>
#include <cstdlib>
#include <vector>

#include "fa/gpu/command_processor.hpp"
#include "fa/gpu/null_backend.hpp"
#include "fa/gpu/pm4.hpp"

using namespace fa::gpu;

struct FakeMem : GuestMemory {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(1 << 20);  // big-endian storage
    uint32_t Read32(uint32_t a) const override {
        return (uint32_t(bytes[a]) << 24) | (bytes[a + 1] << 16) | (bytes[a + 2] << 8) | bytes[a + 3];
    }
    void Write32(uint32_t a, uint32_t v) override {
        bytes[a] = v >> 24; bytes[a + 1] = v >> 16; bytes[a + 2] = v >> 8; bytes[a + 3] = v;
    }
    const uint8_t* HostPtr(uint32_t a, uint32_t) const override { return &bytes[a]; }
};

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static uint32_t T3(uint32_t op, uint32_t payloadDwords) { return (3u << 30) | ((payloadDwords - 1) << 16) | (op << 8); }

int main() {
    FakeMem mem;
    NullBackend be;
    CommandProcessor cp(mem, be);
    const uint32_t ring = 0x1000, ringBytes = 0x1000;
    cp.SetRingBuffer(ring, ringBytes);
    cp.SetReadPtrWriteback(0x100);

    uint32_t w = 0;
    auto put = [&](uint32_t v) { mem.Write32(ring + (w++ % (ringBytes / 4)) * 4, v); };

    // SET_CONSTANT: two ALU constants at index 4.
    put(T3(pm4::kSetConstant, 3)); put(4); put(0x3F800000); put(0x40000000);
    // Type 0 register write.
    put((1u << 16) | 0x2100); put(0xAAAA); put(0xBBBB);
    // DRAW_INDX_2 auto-index triangle list: 3 indices, source 2.
    put(T3(pm4::kDrawIndx2, 1)); put((3u << 16) | (2u << 6) | 4);
    // Inline 16-bit indexed draw: indices 0,1,2,3 packed.
    put(T3(pm4::kDrawIndx2, 3)); put((4u << 16) | (1u << 6) | 4); put(0x00000001); put(0x00020003);
    // Fence write.
    put(T3(pm4::kEventWriteShd, 3)); put(0); put(0x200); put(0xDEAD);
    // Resolve + swap.
    put(T3(pm4::kEventWriteExt, 1)); put(0);
    put(T3(pm4::kXeSwap, 4)); put(0x50415753); put(0x00A00000); put(1280); put(720);

    cp.SetWritePtr(w);
    CHECK(cp.Pump() == CommandProcessor::Status::Idle);
    CHECK(cp.Regs()[pm4::kAluConstBase + 4] == 0x3F800000);
    CHECK(cp.Regs()[pm4::kAluConstBase + 5] == 0x40000000);
    CHECK(cp.Regs()[0x2100] == 0xAAAA && cp.Regs()[0x2101] == 0xBBBB);
    CHECK(be.draws.size() == 2);
    CHECK(be.draws[0].primType == 4 && be.draws[0].indexCount == 3 && !be.draws[0].indexed);
    CHECK(be.draws[1].inlineIndices.size() == 4 && be.draws[1].inlineIndices[0] == 0 && be.draws[1].inlineIndices[3] == 3);
    CHECK(mem.Read32(0x200) == 0xDEAD);
    CHECK(be.resolves == 1);
    CHECK(be.swaps.size() == 1 && be.swaps[0].width == 1280 && be.swaps[0].height == 720);
    CHECK(cp.ReadPtr() == w);
    CHECK(mem.Read32(0x100) == w);

    // WAIT_REG_MEM blocks until memory changes, then the following draw runs.
    size_t drawsBefore = be.draws.size();
    put(T3(pm4::kWaitRegMem, 5)); put(0x13); put(0x300); put(7); put(0xFFFFFFFF); put(10);  // mem == 7
    put(T3(pm4::kDrawIndx2, 1)); put((3u << 16) | (2u << 6) | 4);
    cp.SetWritePtr(w);
    CHECK(cp.Pump() == CommandProcessor::Status::Blocked);
    CHECK(be.draws.size() == drawsBefore);
    mem.Write32(0x300, 7);
    CHECK(cp.Pump() == CommandProcessor::Status::Idle);
    CHECK(be.draws.size() == drawsBefore + 1);

    // Indirect buffer.
    mem.Write32(0x2000, T3(pm4::kDrawIndx2, 1)); mem.Write32(0x2004, (6u << 16) | (2u << 6) | 4);
    put(T3(pm4::kIndirectBuffer, 2)); put(0x2000); put(2);
    cp.SetWritePtr(w);
    CHECK(cp.Pump() == CommandProcessor::Status::Idle);
    CHECK(be.draws.back().indexCount == 6);

    // Ring wrap-around: pad to the end of the ring then continue at the start.
    while (w % (ringBytes / 4) != ringBytes / 4 - 1) put(0x80000000);  // type-2 NOPs
    put(T3(pm4::kDrawIndx2, 1)); put((9u << 16) | (2u << 6) | 4);  // header on last dword? no: header then payload wraps
    cp.SetWritePtr(w % (ringBytes / 4));
    size_t n = be.draws.size();
    CHECK(cp.Pump() == CommandProcessor::Status::Idle);
    CHECK(be.draws.size() == n + 1 && be.draws.back().indexCount == 9);

    // Scratch register writeback: SCRATCH_REG0/1 mirror to SCRATCH_ADDR when their UMSK bit is set.
    put((0u << 16) | pm4::kScratchAddr); put(0x400);
    put((0u << 16) | pm4::kScratchUmsk); put(0x1);
    put((1u << 16) | pm4::kScratchReg0); put(4); put(9);
    cp.SetWritePtr(w % (ringBytes / 4));
    CHECK(cp.Pump() == CommandProcessor::Status::Idle);
    CHECK(mem.Read32(0x400) == 4);
    CHECK(mem.Read32(0x404) == 0);

    std::printf(failures ? "gpu_pm4_test: %d failure(s)\n" : "gpu_pm4_test: ok\n", failures);
    return failures ? 1 : 0;
}
