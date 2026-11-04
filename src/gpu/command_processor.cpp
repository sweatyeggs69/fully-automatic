#include "fa/gpu/command_processor.hpp"

#include <vector>

#include "fa/gpu/pm4.hpp"

namespace fa::gpu {

namespace {

bool Compare(uint32_t fn, uint32_t value, uint32_t ref) {
    switch (fn & 7) {
        case 0: return false;
        case 1: return value < ref;
        case 2: return value <= ref;
        case 3: return value == ref;
        case 4: return value != ref;
        case 5: return value >= ref;
        case 6: return value > ref;
        default: return true;
    }
}

}  // namespace

CommandProcessor::CommandProcessor(GuestMemory& mem, Backend& backend) : mem_(mem), backend_(backend) {}

void CommandProcessor::SetRingBuffer(uint32_t baseGuestAddr, uint32_t sizeBytes) {
    ringBase_ = baseGuestAddr;
    ringSizeDwords_ = sizeBytes / 4;
    rptr_ = wptr_ = 0;
}

uint32_t CommandProcessor::ReadDword(uint32_t base, uint32_t sizeDwords, uint32_t idx) const {
    return mem_.Read32(base + (idx % sizeDwords) * 4);
}

void CommandProcessor::SetReg(uint32_t reg, uint32_t value) {
    if (reg >= kRegisterCount) return;
    regs_[reg] = value;
    // Scratch registers are mirrored to memory (SCRATCH_ADDR + 4 * i) when their SCRATCH_UMSK bit
    // is set. D3D's fences are written this way.
    if (reg >= pm4::kScratchReg0 && reg < pm4::kScratchReg0 + 8) {
        uint32_t i = reg - pm4::kScratchReg0;
        if (regs_[pm4::kScratchUmsk] & (1u << i)) mem_.Write32(regs_[pm4::kScratchAddr] + i * 4, value);
    }
}

CommandProcessor::Status CommandProcessor::Pump() {
    if (ringSizeDwords_ == 0) return Status::Idle;
    for (;;) {
        // Finish a pending indirect buffer first.
        while (ib_.active) {
            if (ib_.pos >= ib_.size) { ib_.active = false; break; }
            uint32_t avail = ib_.size - ib_.pos;
            if (ExecutePacket(ib_.pos, ib_.base, ib_.size, avail, 1) == Result::Blocked) return Status::Blocked;
        }
        if (rptr_ == wptr_) break;
        uint32_t avail = (wptr_ - rptr_) % ringSizeDwords_;
        if (avail == 0) break;
        uint32_t pos = rptr_;
        Result r = ExecutePacket(pos, ringBase_, ringSizeDwords_, avail, 0);
        if (r == Result::Blocked) return Status::Blocked;
        if (r == Result::Incomplete) break;
        rptr_ = pos % ringSizeDwords_;
        if (rptrWriteback_) mem_.Write32(rptrWriteback_, rptr_);
    }
    return Status::Idle;
}

CommandProcessor::Result CommandProcessor::ExecutePacket(uint32_t& pos, uint32_t base, uint32_t sizeDwords,
                                                         uint32_t availDwords, int depth) {
    uint32_t header = ReadDword(base, sizeDwords, pos);
    uint32_t total = 1;
    std::vector<uint32_t> d;
    switch (pm4::TypeOf(header)) {
        case pm4::kType0: total = 1 + pm4::T0Count(header); break;
        case pm4::kType1: total = 3; break;
        case pm4::kType2: total = 1; break;
        case pm4::kType3: total = 1 + pm4::T3Count(header); break;
    }
    if (total > availDwords) return Result::Incomplete;
    for (uint32_t i = 1; i < total; ++i) d.push_back(ReadDword(base, sizeDwords, pos + i));

    switch (pm4::TypeOf(header)) {
        case pm4::kType0: {
            uint32_t reg = pm4::T0Base(header);
            for (uint32_t i = 0; i < d.size(); ++i) SetReg(pm4::T0OneReg(header) ? reg : reg + i, d[i]);
            break;
        }
        case pm4::kType1:
            SetReg(pm4::T1Reg0(header), d[0]);
            SetReg(pm4::T1Reg1(header), d[1]);
            break;
        case pm4::kType2: break;  // padding NOP
        case pm4::kType3: {
            Result r = ExecuteType3(pm4::T3Opcode(header), d, depth);
            if (r == Result::Blocked) return r;  // pos untouched, retried on next Pump
            break;
        }
    }
    ++stats_.packets;
    pos += total;
    return Result::Done;
}

CommandProcessor::Result CommandProcessor::ExecuteType3(uint32_t op, const std::vector<uint32_t>& d, int depth) {
    const uint32_t n = uint32_t(d.size());
    switch (op) {
        case pm4::kNop:
        case pm4::kMeInit:
        case pm4::kInvalidateState:
        case pm4::kEventWrite:
        case pm4::kVizQuery:
        case pm4::kImLoad:
        case pm4::kImLoadImmediate:  // shader ucode is fetched from guest memory at draw time
        case pm4::kSetBinMaskLo: case pm4::kSetBinMaskHi:
        case pm4::kSetBinSelectLo: case pm4::kSetBinSelectHi:
            break;

        case pm4::kInterrupt:
            if (interrupt_) interrupt_(n ? d[0] : 0);
            break;

        case pm4::kIndirectBuffer:
        case pm4::kIndirectBufferPfd:
            if (n >= 2 && depth == 0 && !ib_.active) {
                ib_ = {true, d[0] & 0x1FFFFFFF, d[1] & 0xFFFFF, 0};
            } else {
                ++stats_.unknown;  // nested indirect buffers are not used by the game
            }
            break;

        case pm4::kWaitRegMem: {
            if (n < 4) break;
            uint32_t value = (d[0] & 0x10) ? mem_.Read32(d[1] & ~3u) : (d[1] < kRegisterCount ? regs_[d[1]] : 0);
            if (!Compare(d[0], value & d[3], d[2])) return Result::Blocked;
            break;
        }

        case pm4::kRegRmw: {
            if (n < 3) break;
            uint32_t andMask = (d[0] & 0x80000000u) ? (d[1] < kRegisterCount ? regs_[d[1] & 0x1FFF] : 0) : d[1];
            uint32_t orMask = (d[0] & 0x40000000u) ? (d[2] < kRegisterCount ? regs_[d[2] & 0x1FFF] : 0) : d[2];
            uint32_t dst = d[0] & 0x1FFF;
            if (dst < kRegisterCount) regs_[dst] = (regs_[dst] & andMask) | orMask;
            break;
        }

        case pm4::kCondWrite: {
            if (n < 6) break;
            uint32_t value = (d[0] & 0x10) ? mem_.Read32(d[1] & ~3u) : (d[1] < kRegisterCount ? regs_[d[1]] : 0);
            if (Compare(d[0], value & d[3], d[2])) {
                if (d[0] & 0x100) mem_.Write32(d[4] & ~3u, d[5]);
                else SetReg(d[4], d[5]);
            }
            break;
        }

        case pm4::kMemWrite:
            for (uint32_t i = 1; i < n; ++i) mem_.Write32((d[0] & ~3u) + (i - 1) * 4, d[i]);
            break;

        case pm4::kEventWriteShd:  // fence / timestamp write
            if (n >= 3) mem_.Write32(d[1] & ~3u, d[2]);
            break;

        case pm4::kEventWriteExt:  // EDRAM resolve
            backend_.Resolve(regs_);
            break;

        case pm4::kSetConstant: {
            if (n < 2) break;
            uint32_t index = d[0] & 0x7FF, type = (d[0] >> 16) & 0xFF, base;
            switch (type) {
                case 0: base = pm4::kAluConstBase; break;
                case 1: base = pm4::kFetchConstBase; break;
                case 2: base = pm4::kBoolConstBase; break;
                case 3: base = pm4::kLoopConstBase; break;
                case 4: base = pm4::kRegisterBase; break;
                default: ++stats_.unknown; return Result::Done;
            }
            for (uint32_t i = 1; i < n; ++i) SetReg(base + index + i - 1, d[i]);
            break;
        }

        case pm4::kSetConstant2:
        case pm4::kSetShaderConstants:
            for (uint32_t i = 1; i < n; ++i) SetReg((d[0] & 0xFFFF) + i - 1, d[i]);
            break;

        case pm4::kLoadAluConstant: {
            if (n < 3) break;
            uint32_t addr = d[0] & 0x3FFFFFFF, index = d[1] & 0x7FF, type = (d[1] >> 16) & 0xFF;
            uint32_t count = d[2] & 0xFFF;
            uint32_t base = type == 0 ? pm4::kAluConstBase : type == 1 ? pm4::kFetchConstBase
                          : type == 2 ? pm4::kBoolConstBase : type == 3 ? pm4::kLoopConstBase : pm4::kRegisterBase;
            for (uint32_t i = 0; i < count; ++i) SetReg(base + index + i, mem_.Read32(addr + i * 4));
            break;
        }

        case pm4::kDrawIndx:
        case pm4::kDrawIndx2: {
            bool v1 = op == pm4::kDrawIndx;
            uint32_t first = v1 ? 1 : 0;  // DRAW_INDX has a viz-query dword in front
            if (n <= first) break;
            uint32_t init = d[first];
            DrawCall dc;
            dc.primType = init & 0x3F;
            uint32_t source = (init >> 6) & 3;  // 0 = DMA, 1 = immediate, 2 = auto index
            dc.index32 = (init >> 11) & 1;
            dc.indexCount = init >> 16;
            dc.indexed = source != 2;
            if (v1 && source == 0 && n >= first + 3) {
                dc.indexGuestAddr = d[first + 1] & 0x1FFFFFFF;
                dc.indexEndian = d[first + 2] >> 30;
            } else if (source != 2) {
                // Indices travel in the packet: 32-bit, or two packed 16-bit per dword (high half first).
                for (uint32_t i = first + 1; i < n; ++i) {
                    if (dc.index32) dc.inlineIndices.push_back(d[i]);
                    else { dc.inlineIndices.push_back(d[i] >> 16); dc.inlineIndices.push_back(d[i] & 0xFFFF); }
                }
                if (dc.inlineIndices.size() > dc.indexCount) dc.inlineIndices.resize(dc.indexCount);
            }
            backend_.Draw(dc, regs_);
            ++stats_.draws;
            break;
        }

        case pm4::kXeSwap:  // d[0] = 'SWAP', d[1] = front buffer, d[2] = width, d[3] = height
            if (n >= 4) {
                backend_.Swap(d[1], d[2], d[3]);
                ++stats_.swaps;
            }
            break;

        default:
            ++stats_.unknown;
            break;
    }
    return Result::Done;
}

}  // namespace fa::gpu
