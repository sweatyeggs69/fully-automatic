#pragma once
// Tiny Xenos microcode assembler for tests: builds control flow, ALU and fetch instructions
// with the bit layouts the translator decodes (Xenia's ucode.h).
#include <cstdint>
#include <vector>

namespace xasm {

struct Cf { uint32_t d0 = 0, d1 = 0; };

inline Cf Exec(uint32_t addr, uint32_t count, uint32_t seq, bool end) {
    return {addr | (count << 12) | (seq << 16), (end ? 2u : 1u) << 12};
}
inline Cf CondExec(uint32_t addr, uint32_t count, uint32_t seq, bool end, uint32_t boolIdx, bool cond) {
    return {addr | (count << 12) | (seq << 16), ((end ? 4u : 3u) << 12) | (boolIdx << 2) | (uint32_t(cond) << 10)};
}
inline Cf LoopStart(uint32_t skip, uint32_t loopId) { return {skip | (loopId << 16), 7u << 12}; }
inline Cf LoopEnd(uint32_t body, uint32_t loopId) { return {body | (loopId << 16), 8u << 12}; }
inline Cf Jmp(uint32_t target) { return {target | (1u << 13), 11u << 12}; }
inline Cf Nop() { return {0, 0}; }

// Swizzle helpers: relative ALU swizzle from absolute component indices.
inline uint32_t AluSwz(int x, int y, int z, int w) {
    int c[4] = {x, y, z, w};
    uint32_t s = 0;
    for (int i = 0; i < 4; i++) s |= uint32_t((c[i] - i) & 3) << (2 * i);
    return s;
}
constexpr uint32_t kSwzId = 0;

struct Src {
    uint32_t reg = 0;
    bool temp = true;
    uint32_t swz = kSwzId;
    bool neg = false;
};
inline Src R(uint32_t r, uint32_t swz = kSwzId) { return {r, true, swz, false}; }
inline Src C(uint32_t c, uint32_t swz = kSwzId) { return {c, false, swz, false}; }

struct Alu {
    uint32_t vop = 2, sop = 50;  // max (mov when both sources are the same), retain_prev
    uint32_t vdst = 0, vmask = 0, sdst = 0, smask = 0;
    bool exp = false;
    Src s1, s2, s3;
};

inline void Emit(std::vector<uint32_t>& out, const Alu& a) {
    uint32_t w0 = a.vdst | (a.sdst << 8) | (uint32_t(a.exp) << 15) | (a.vmask << 16) | (a.smask << 20) | (a.sop << 26);
    uint32_t w1 = a.s3.swz | (a.s2.swz << 8) | (a.s1.swz << 16) | (uint32_t(a.s3.neg) << 24) |
                  (uint32_t(a.s2.neg) << 25) | (uint32_t(a.s1.neg) << 26);
    uint32_t w2 = a.s3.reg | (a.s2.reg << 8) | (a.s1.reg << 16) | (a.vop << 24) | (uint32_t(a.s3.temp) << 29) |
                  (uint32_t(a.s2.temp) << 30) | (uint32_t(a.s1.temp) << 31);
    out.push_back(w0); out.push_back(w1); out.push_back(w2);
}

// Destination swizzle for fetches: 3 bits per component (0..3 xyzw, 4 = 0, 5 = 1, 7 = keep).
inline uint32_t FetchSwz(int x, int y, int z, int w) { return uint32_t(x) | (uint32_t(y) << 3) | (uint32_t(z) << 6) | (uint32_t(w) << 9); }

inline void EmitVfetch(std::vector<uint32_t>& out, uint32_t dst, uint32_t src, uint32_t srcComp, uint32_t slot,
                       uint32_t format, uint32_t stride, int offset, uint32_t dstSwz, bool mini = false,
                       bool isSigned = false, bool isInt = false) {
    uint32_t w0 = 0 | (src << 5) | (dst << 12) | (1u << 19) | ((slot / 3) << 20) | ((slot % 3) << 25) | (srcComp << 30);
    uint32_t w1 = dstSwz | (uint32_t(isSigned) << 12) | (uint32_t(isInt) << 13) | (format << 16) | (uint32_t(mini) << 30);
    uint32_t w2 = stride | ((uint32_t(offset) & 0x7FFFFF) << 8);
    out.push_back(w0); out.push_back(w1); out.push_back(w2);
}

inline void EmitTfetch2D(std::vector<uint32_t>& out, uint32_t dst, uint32_t src, uint32_t slot, uint32_t dstSwz) {
    uint32_t w0 = 1 | (src << 5) | (dst << 12) | (slot << 20) | ((0u | (1u << 2) | (2u << 4)) << 26);
    uint32_t w1 = dstSwz | (3u << 12) | (3u << 14) | (3u << 16) | (7u << 18);
    uint32_t w2 = (1u << 14);
    out.push_back(w0); out.push_back(w1); out.push_back(w2);
}

// Assembles a shader: control flow (padded to pairs), then the instruction words.
inline std::vector<uint32_t> Build(std::vector<Cf> cf, const std::vector<uint32_t>& instr) {
    if (cf.size() & 1) cf.push_back(Nop());
    std::vector<uint32_t> out;
    for (size_t i = 0; i < cf.size(); i += 2) {
        const Cf& a = cf[i];
        const Cf& b = cf[i + 1];
        out.push_back(a.d0);
        out.push_back((a.d1 & 0xFFFF) | (b.d0 << 16));
        out.push_back((b.d0 >> 16) | (b.d1 << 16));
    }
    out.insert(out.end(), instr.begin(), instr.end());
    return out;
}
inline uint32_t CfPairs(size_t cfCount) { return uint32_t((cfCount + 1) / 2); }

// A vertex shader: position float3 + colour float4 from fetch slot 0 (stride 7 dwords), exports
// oPos = (pos, 1) and o0 = colour.
inline std::vector<uint32_t> ColorVS() {
    std::vector<uint32_t> ins;
    EmitVfetch(ins, 1, 0, 0, 0, /*k_32_32_32_FLOAT*/ 57, 7, 0, FetchSwz(0, 1, 2, 5));
    EmitVfetch(ins, 2, 0, 0, 0, /*k_32_32_32_32_FLOAT*/ 38, 7, 3, FetchSwz(0, 1, 2, 3), true);
    Alu pos; pos.exp = true; pos.vdst = 62; pos.vmask = 0xF; pos.s1 = R(1); pos.s2 = R(1);
    Emit(ins, pos);
    Alu col; col.exp = true; col.vdst = 0; col.vmask = 0xF; col.s1 = R(2); col.s2 = R(2);
    Emit(ins, col);
    // seq: fetch, fetch, alu, alu
    return Build({Exec(1, 4, 0b0101, true)}, ins);
}

// A pixel shader: oC0 = interpolator 0 * c0.
inline std::vector<uint32_t> ColorPS() {
    std::vector<uint32_t> ins;
    Alu m; m.vop = 1; m.exp = true; m.vdst = 0; m.vmask = 0xF; m.s1 = R(0); m.s2 = C(0);
    Emit(ins, m);
    return Build({Exec(1, 1, 0, true)}, ins);
}

// A pixel shader: oC0 = tfetch2D(tf<slot>, r0.xy).
inline std::vector<uint32_t> TexturePS(uint32_t slot = 0) {
    std::vector<uint32_t> ins;
    EmitTfetch2D(ins, 1, 0, slot, FetchSwz(0, 1, 2, 3));
    Alu m; m.exp = true; m.vdst = 0; m.vmask = 0xF; m.s1 = R(1); m.s2 = R(1);
    Emit(ins, m);
    return Build({Exec(1, 2, 0b01, true)}, ins);
}

}  // namespace xasm
