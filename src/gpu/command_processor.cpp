#include "fa/gpu/command_processor.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "fa/gpu/pm4.hpp"
#include "fa/gpu/shader.hpp"
#include "fa/gpu/xenos.hpp"

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

CommandProcessor::CommandProcessor(GuestMemory& mem, Backend& backend) : mem_(mem), backend_(backend) {
    const char* e = std::getenv("FA_TILE_ONCE");
    tileOnce_ = !(e && e[0] == '0');  // FA_TILE_ONCE=0 replays every tile
}

void CommandProcessor::SetRingBuffer(uint32_t baseGuestAddr, uint32_t sizeBytes) {
    ringBase_ = baseGuestAddr;
    ringSizeDwords_ = sizeBytes / 4;
    rptr_ = wptr_ = 0;
}

uint32_t CommandProcessor::ReadDword(uint32_t base, uint32_t sizeDwords, uint32_t idx) const {
    return mem_.Read32(base + (idx % sizeDwords) * 4);
}

void CommandProcessor::TraceFlushDraws() {
    if (!tt_.draws && stats_.predicatedSkips == tt_.skips0) return;
    std::fprintf(stderr, "[tiles] d%u draws %u, predicated skips %llu\n", tt_.depth, tt_.draws,
                 (unsigned long long)(stats_.predicatedSkips - tt_.skips0));
    tt_.draws = 0;
    tt_.skips0 = stats_.predicatedSkips;
}

void CommandProcessor::TraceReg(uint32_t reg, uint32_t value) {
    const char* name = nullptr;
    switch (reg) {
        case xe::PA_SC_WINDOW_OFFSET: name = "window offset"; break;
        case xe::PA_SC_WINDOW_SCISSOR_TL: name = "window scissor tl"; break;
        case xe::PA_SC_WINDOW_SCISSOR_BR: name = "window scissor br"; break;
        case xe::PA_SC_SCREEN_SCISSOR_TL: name = "screen scissor tl"; break;
        case xe::PA_SC_SCREEN_SCISSOR_BR: name = "screen scissor br"; break;
        case xe::RB_SURFACE_INFO: name = "surface info"; break;
        case xe::RB_COLOR_INFO: name = "color info"; break;
        case xe::RB_DEPTH_INFO: name = "depth info"; break;
        case xe::RB_COPY_DEST_BASE: name = "copy dest base"; break;
        case xe::RB_COPY_DEST_PITCH: name = "copy dest pitch"; break;
        case xe::RB_MODECONTROL: name = "mode control"; break;
        default: return;
    }
    if (regs_[reg] == value) return;
    TraceFlushDraws();
    std::fprintf(stderr, "[tiles] d%u %s %08x\n", tt_.depth, name, value);
}

void CommandProcessor::TraceSwap() {
    static const bool parsed = [this] {
        if (const char* e = std::getenv("FA_TILE_TRACE")) {
            tt_.first = std::strtoull(e, nullptr, 10);
            if (const char* c = std::strchr(e, ',')) tt_.frames = std::max<uint64_t>(1, std::strtoull(c + 1, nullptr, 10));
        }
        return true;
    }();
    (void)parsed;
    if (tt_.on) {
        TraceFlushDraws();
        std::fprintf(stderr, "[tiles] swap %llu\n", (unsigned long long)stats_.swaps);
    }
    // Or on demand: `touch ~/.cache/fullauto/tile_trace` traces the next two frames (checked every 30 swaps).
    if (!tt_.on && stats_.swaps % 30 == 0) {
        static const std::string trigger = [] {
            const char* h = std::getenv("HOME");
            return h ? std::string(h) + "/.cache/fullauto/tile_trace" : std::string();
        }();
        if (!trigger.empty() && std::remove(trigger.c_str()) == 0) {
            tt_.first = stats_.swaps + 1;
            tt_.frames = 2;
        }
    }
    tt_.on = stats_.swaps + 1 >= tt_.first && stats_.swaps + 1 < tt_.first + tt_.frames;
    if (tt_.on) {
        tt_.draws = 0;
        tt_.skips0 = stats_.predicatedSkips;
        std::fprintf(stderr, "[tiles] frame after swap %llu: bin mask %016llx select %016llx\n", (unsigned long long)stats_.swaps,
                     (unsigned long long)binMask_, (unsigned long long)binSelect_);
    }
}

void CommandProcessor::SetReg(uint32_t reg, uint32_t value) {
    if (reg >= kRegisterCount) return;
    if (tt_.on) TraceReg(reg, value);
    // The second tile's scissor starts where the first tile ends.
    if (reg == xe::PA_SC_WINDOW_SCISSOR_TL && TileOf() == 1 && ((value >> 16) & 0x3FFF)) {
        if (tileOnce_ && !tileRows_)
            std::fprintf(stderr, "[tile] once: %u-row tiles on surface %08x, later tiles skipped\n", (value >> 16) & 0x3FFF,
                         regs_[xe::RB_SURFACE_INFO]);
        tileRows_ = (value >> 16) & 0x3FFF;
        tileSurface_ = regs_[xe::RB_SURFACE_INFO];
    }
    regs_[reg] = value;
    if (reg - pm4::kAluConstBase < 2048u) regs_.aluDirty |= 1u << ((reg - pm4::kAluConstBase) >> 6);
    // Scratch registers are mirrored to memory (SCRATCH_ADDR + 4 * i) when their SCRATCH_UMSK bit
    // is set. D3D's fences are written this way.
    if (reg >= pm4::kScratchReg0 && reg < pm4::kScratchReg0 + 8) {
        uint32_t i = reg - pm4::kScratchReg0;
        if (regs_[pm4::kScratchUmsk] & (1u << i)) GuestWrite(regs_[pm4::kScratchAddr] + i * 4, value);
    }
}

void CommandProcessor::LoadShader(uint32_t type, std::vector<uint32_t>&& ucode, uint32_t srcAddr) {
    if (type > 1) { ++stats_.unknown; return; }
    ShaderSlot& s = shaders_[type];
    s.ucode = std::move(ucode);
    s.hash = HashUcode(s.ucode.data(), s.ucode.size()) ^ (uint64_t(type) << 63);
    s.srcAddr = srcAddr;
    ++stats_.shaderLoads;
}

CommandProcessor::Status CommandProcessor::Pump() {
    if (ringSizeDwords_ == 0) return Status::Idle;
    for (;;) {
        // Finish a pending indirect buffer first.
        while (ib_.active) {
            if (ib_.pos >= ib_.size) { ib_.active = false; break; }
            uint32_t avail = ib_.size - ib_.pos;
            if (ExecutePacket(ib_.pos, ib_.base, ib_.size, avail, 1) == Result::Blocked) { backend_.Sync(); backend_.Idle(); return Status::Blocked; }
        }
        if (rptr_ == wptr_) break;
        uint32_t avail = (wptr_ - rptr_) % ringSizeDwords_;
        if (avail == 0) break;
        uint32_t pos = rptr_;
        Result r = ExecutePacket(pos, ringBase_, ringSizeDwords_, avail, 0);
        if (r == Result::Blocked) { backend_.Sync(); backend_.Idle(); return Status::Blocked; }
        if (r == Result::Incomplete) break;
        rptr_ = pos % ringSizeDwords_;
        if (rptrWriteback_) mem_.Write32(rptrWriteback_, rptr_);
    }
    // Everything submitted so far is done: the guest may now read the results (it polls the read pointer).
    backend_.Sync();
    backend_.Idle();
    return Status::Idle;
}

CommandProcessor::Result CommandProcessor::ExecutePacket(uint32_t& pos, uint32_t base, uint32_t sizeDwords,
                                                         uint32_t availDwords, int depth) {
    uint32_t header = ReadDword(base, sizeDwords, pos);
    if (tt_.on && tt_.depth != uint32_t(depth)) { TraceFlushDraws(); tt_.depth = uint32_t(depth); }
    curPacket_ = (base + pos * 4) & 0x1FFFFFFF;
    uint32_t total = 1;
    // Reused across packets: a fresh vector per packet was a heap allocation for every packet.
    std::vector<uint32_t>& d = payload_;
    d.clear();
    switch (pm4::TypeOf(header)) {
        case pm4::kType0: total = 1 + pm4::T0Count(header); break;
        case pm4::kType1: total = 3; break;
        case pm4::kType2: total = 1; break;
        case pm4::kType3: total = 1 + pm4::T3Count(header); break;
    }
    if (total > availDwords) return Result::Incomplete;
    d.resize(total - 1);
    for (uint32_t i = 1; i < total; ++i) d[i - 1] = ReadDword(base, sizeDwords, pos + i);

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
            if ((header & 1) && !(binSelect_ & binMask_)) {  // predicated for another tile
                ++stats_.predicatedSkips;
                break;
            }
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
            break;

        // Tiled-rendering predication (see binMask_).
        case pm4::kSetBinMask:
            if (n >= 2) binMask_ = (uint64_t(d[0]) << 32) | d[1];
            if (tt_.on) { TraceFlushDraws(); std::fprintf(stderr, "[tiles] d%d bin mask %016llx\n", depth, (unsigned long long)binMask_); }
            break;
        case pm4::kSetBinSelect:
            if (n >= 2) binSelect_ = (uint64_t(d[0]) << 32) | d[1];
            if (tt_.on) { TraceFlushDraws(); std::fprintf(stderr, "[tiles] d%d bin select %016llx\n", depth, (unsigned long long)binSelect_); }
            break;
        case pm4::kSetBinMaskLo:
            if (n >= 1) binMask_ = (binMask_ & 0xFFFFFFFF00000000ull) | d[0];
            if (tt_.on) { TraceFlushDraws(); std::fprintf(stderr, "[tiles] d%d bin mask lo -> %016llx\n", depth, (unsigned long long)binMask_); }
            break;
        case pm4::kSetBinMaskHi:
            if (n >= 1) binMask_ = (binMask_ & 0xFFFFFFFFull) | (uint64_t(d[0]) << 32);
            if (tt_.on) { TraceFlushDraws(); std::fprintf(stderr, "[tiles] d%d bin mask hi -> %016llx\n", depth, (unsigned long long)binMask_); }
            break;
        case pm4::kSetBinSelectLo:
            if (n >= 1) binSelect_ = (binSelect_ & 0xFFFFFFFF00000000ull) | d[0];
            if (tt_.on) { TraceFlushDraws(); std::fprintf(stderr, "[tiles] d%d bin select lo -> %016llx\n", depth, (unsigned long long)binSelect_); }
            break;
        case pm4::kSetBinSelectHi:
            if (n >= 1) binSelect_ = (binSelect_ & 0xFFFFFFFFull) | (uint64_t(d[0]) << 32);
            if (tt_.on) { TraceFlushDraws(); std::fprintf(stderr, "[tiles] d%d bin select hi -> %016llx\n", depth, (unsigned long long)binSelect_); }
            break;

        case pm4::kImLoad: {
            // d0 = address | shader type (0 vertex, 1 pixel), d1 = start << 16 | size in dwords.
            if (n < 2) break;
            uint32_t type = d[0] & 3, addr = d[0] & ~3u, size = d[1] & 0xFFFF;
            std::vector<uint32_t> u(size);
            for (uint32_t i = 0; i < size; i++) u[i] = mem_.Read32(addr + i * 4);
            LoadShader(type, std::move(u), addr);
            break;
        }
        case pm4::kImLoadImmediate: {
            // d0 = shader type, d1 = start << 16 | size in dwords, then the microcode.
            if (n < 2) break;
            uint32_t type = d[0] & 3, size = std::min<uint32_t>(d[1] & 0xFFFF, n - 2);
            LoadShader(type, std::vector<uint32_t>(d.begin() + 2, d.begin() + 2 + size), 0);
            break;
        }

        case pm4::kInterrupt:
            backend_.Sync();
            backend_.Drain();  // the handler reads fences and scratch registers written before this packet
            if (interrupt_) interrupt_(n ? d[0] : 0);
            break;

        case pm4::kIndirectBuffer:
        case pm4::kIndirectBufferPfd:
            if (n >= 2 && depth == 0 && !ib_.active) {
                ib_ = {true, d[0] & 0x1FFFFFFF, d[1] & 0xFFFFF, 0};
                if (g_indirect_buffer_observer) g_indirect_buffer_observer(ib_.base, ib_.size);
                if (tt_.on) {
                    TraceFlushDraws();
                    std::fprintf(stderr, "[tiles] ib %08x, %u dwords\n", ib_.base, ib_.size);
                }
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
            if (dst - pm4::kAluConstBase < 2048u) regs_.aluDirty |= 1u << ((dst - pm4::kAluConstBase) >> 6);
            break;
        }

        case pm4::kCondWrite: {
            if (n < 6) break;
            uint32_t value = (d[0] & 0x10) ? mem_.Read32(d[1] & ~3u) : (d[1] < kRegisterCount ? regs_[d[1]] : 0);
            if (Compare(d[0], value & d[3], d[2])) {
                backend_.Sync();
                if (d[0] & 0x100) GuestWrite(d[4] & ~3u, d[5]);
                else SetReg(d[4], d[5]);
            }
            break;
        }

        case pm4::kMemWrite:
            backend_.Sync();
            for (uint32_t i = 1; i < n; ++i) GuestWrite((d[0] & ~3u) + (i - 1) * 4, d[i]);
            break;

        case pm4::kEventWriteShd:  // fence / timestamp write
            backend_.Sync();
            if (n >= 3) GuestWrite(d[1] & ~3u, d[2]);
            break;

        case pm4::kEventWriteExt:  // screen extents report; resolves are copy-mode draws
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
            dc.packetAddr = curPacket_;
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
            regs_[xe::VGT_DRAW_INITIATOR] = init;
            if (tileOnce_ && tileRows_) {
                const int tile = TileOf();
                if (tile == 0) sawTile0_ = true;
                if (tile >= 1 && sawTile0_) { ++stats_.tileSkips; break; }
                // Only the tiled target (the surface the tiles were drawn to) is extended: passes
                // before and after the tiles run with the first tile's select too.
                regs_.tileRows = tile == 0 && regs_[xe::RB_SURFACE_INFO] == tileSurface_ ? tileRows_ : 0;
            }
            if ((regs_[xe::RB_MODECONTROL] & 7) == xe::kEdramCopy) {
                if (tt_.on) {
                    TraceFlushDraws();
                    std::fprintf(stderr, "[tiles] d%d resolve: copy control %08x dest %08x pitch %08x info %08x, window offset %08x, surface %08x color %08x depth %08x\n",
                                 depth, regs_[xe::RB_COPY_CONTROL], regs_[xe::RB_COPY_DEST_BASE], regs_[xe::RB_COPY_DEST_PITCH],
                                 regs_[xe::RB_COPY_DEST_INFO], regs_[xe::PA_SC_WINDOW_OFFSET], regs_[xe::RB_SURFACE_INFO],
                                 regs_[xe::RB_COLOR_INFO], regs_[xe::RB_DEPTH_INFO]);
                }
                if (!g_direct_resolve || !g_direct_resolve(dc, regs_, backend_)) backend_.Resolve(regs_);
                ++stats_.resolves;
                break;
            }
            for (int t = 0; t < 2; t++) {
                const ShaderSlot& s = shaders_[t];
                if (s.ucode.empty()) continue;
                (t ? dc.psUcode : dc.vsUcode) = s.ucode.data();
                (t ? dc.psDwords : dc.vsDwords) = uint32_t(s.ucode.size());
                (t ? dc.psHash : dc.vsHash) = s.hash;
            }
            if (g_draw_observer) g_draw_observer(dc, regs_);
            if (g_draw_feeder) g_draw_feeder(dc, regs_);
            if (!g_direct_draw || !g_direct_draw(dc, regs_, backend_)) backend_.Draw(dc, regs_);
            ++stats_.draws;
            if (tt_.on) tt_.draws++;
            break;
        }

        case pm4::kXeSwap:  // d[0] = 'SWAP', d[1] = front buffer, d[2] = width, d[3] = height
            if (n >= 4) {
                backend_.Swap(d[1], d[2], d[3], regs_);
                TraceSwap();
                sawTile0_ = false;
                ++stats_.swaps;
                if (tileOnce_ && stats_.swaps % 300 == 0 && stats_.tileSkips != tileSkips0_) {
                    std::fprintf(stderr, "[tile] %llu swaps: %llu tile draws/resolves skipped (%.0f per frame since last)\n",
                                 (unsigned long long)stats_.swaps, (unsigned long long)stats_.tileSkips,
                                 double(stats_.tileSkips - tileSkips0_) / 300.0);
                    tileSkips0_ = stats_.tileSkips;
                }
            }
            break;

        default:
            ++stats_.unknown;
            ++stats_.unknownOps[op & 0x7F];
            break;
    }
    return Result::Done;
}

}  // namespace fa::gpu
