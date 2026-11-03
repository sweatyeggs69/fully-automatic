#pragma once
// Xenos PM4 packet definitions (ring buffer command format).
#include <cstdint>

namespace fa::gpu::pm4 {

enum PacketType : uint32_t { kType0 = 0, kType1 = 1, kType2 = 2, kType3 = 3 };

inline PacketType TypeOf(uint32_t h) { return PacketType(h >> 30); }
// Type 0: write `count` consecutive (or the same, if one_reg) registers starting at base.
inline uint32_t T0Count(uint32_t h) { return ((h >> 16) & 0x3FFF) + 1; }
inline uint32_t T0Base(uint32_t h) { return h & 0x7FFF; }
inline bool T0OneReg(uint32_t h) { return (h >> 15) & 1; }
// Type 1: two register writes.
inline uint32_t T1Reg0(uint32_t h) { return h & 0x7FF; }
inline uint32_t T1Reg1(uint32_t h) { return (h >> 11) & 0x7FF; }
// Type 3: opcode with `count` payload dwords.
inline uint32_t T3Count(uint32_t h) { return ((h >> 16) & 0x3FFF) + 1; }
inline uint32_t T3Opcode(uint32_t h) { return (h >> 8) & 0x7F; }

// Opcodes (values as used by the Xenos command processor).
enum Opcode : uint32_t {
    kNop = 0x10,
    kMeInit = 0x48,
    kInterrupt = 0x54,
    kRegRmw = 0x21,
    kDrawIndx = 0x22,
    kVizQuery = 0x23,
    kImLoad = 0x27,
    kImLoadImmediate = 0x2B,
    kLoadAluConstant = 0x2F,
    kSetConstant = 0x2D,
    kDrawIndx2 = 0x36,
    kIndirectBufferPfd = 0x37,
    kInvalidateState = 0x3B,
    kWaitRegMem = 0x3C,
    kMemWrite = 0x3D,
    kIndirectBuffer = 0x3F,
    kCondWrite = 0x45,
    kEventWrite = 0x46,
    kEventWriteShd = 0x58,
    kEventWriteExt = 0x5A,
    kSetConstant2 = 0x55,
    kSetShaderConstants = 0x56,
    kSetBinMaskLo = 0x60,
    kSetBinMaskHi = 0x61,
    kSetBinSelectLo = 0x62,
    kSetBinSelectHi = 0x63,
    kXeSwap = 0x64,
};

// Constant-register windows used by SET_CONSTANT.
constexpr uint32_t kAluConstBase = 0x4000;
constexpr uint32_t kFetchConstBase = 0x4800;
constexpr uint32_t kBoolConstBase = 0x4900;
constexpr uint32_t kLoopConstBase = 0x4908;
constexpr uint32_t kRegisterBase = 0x2000;

}  // namespace fa::gpu::pm4
