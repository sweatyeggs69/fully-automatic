#include "fa/gpu/primitive.hpp"

#include "fa/gpu/texture.hpp"
#include "fa/gpu/xenos.hpp"

#include <cstring>

namespace fa::gpu {

namespace {

// Reads the raw guest indices (before the index offset).
bool ReadIndices(const DrawCall& dc, const GuestMemory& mem, std::vector<uint32_t>& idx) {
    idx.clear();
    if (!dc.indexed) {
        idx.resize(dc.indexCount);
        for (uint32_t i = 0; i < dc.indexCount; i++) idx[i] = i;
        return true;
    }
    if (!dc.inlineIndices.empty()) {
        idx = dc.inlineIndices;
        return true;
    }
    if (!dc.indexGuestAddr) return false;
    uint32_t bytes = dc.indexCount * (dc.index32 ? 4 : 2);
    uint32_t padded = (bytes + 3) & ~3u;
    const uint8_t* p = mem.HostPtr(dc.indexGuestAddr, padded);
    idx.resize(dc.indexCount);
    if (!p) {
        // Not in one allocation: fall back to dword reads (host order = big-endian guest view).
        for (uint32_t i = 0; i < dc.indexCount; i++) {
            if (dc.index32) {
                idx[i] = mem.Read32(dc.indexGuestAddr + i * 4);
            } else {
                uint32_t w = mem.Read32((dc.indexGuestAddr + i * 2) & ~3u);
                idx[i] = ((dc.indexGuestAddr + i * 2) & 2) ? (w & 0xFFFF) : (w >> 16);
            }
        }
        return true;
    }
    // Common cases first: 16-bit indices swapped per 16 bits and 32-bit indices swapped per 32 bits are a plain
    // byte swap per index, no intermediate copy.
    if (!dc.index32 && dc.indexEndian == kEndian8in16) {
        for (uint32_t i = 0; i < dc.indexCount; i++) {
            uint16_t v;
            std::memcpy(&v, p + size_t(i) * 2, 2);
            idx[i] = uint16_t((v << 8) | (v >> 8));
        }
        return true;
    }
    if (dc.index32 && dc.indexEndian == kEndian8in32) {
        for (uint32_t i = 0; i < dc.indexCount; i++) {
            uint32_t v;
            std::memcpy(&v, p + size_t(i) * 4, 4);
            idx[i] = __builtin_bswap32(v);
        }
        return true;
    }
    // The GPU applies the endian swap per 16/32-bit unit of the buffer, then reads little-endian.
    static thread_local std::vector<uint8_t> sw;
    sw.resize(padded);
    EndianSwapCopy(dc.indexEndian, p, sw.data(), padded);
    for (uint32_t i = 0; i < dc.indexCount; i++) {
        if (dc.index32) {
            uint32_t v;
            std::memcpy(&v, sw.data() + i * 4, 4);
            idx[i] = v;
        } else {
            uint16_t v;
            std::memcpy(&v, sw.data() + i * 2, 2);
            idx[i] = v;
        }
    }
    return true;
}

}  // namespace

bool BuildHostDraw(const DrawCall& dc, const RegisterFile& regs, const GuestMemory& mem, HostDraw& out) {
    out.indices.clear();
    out.supported = true;
    static thread_local std::vector<uint32_t> raw;  // scratch, keeps its capacity between draws
    if (!dc.indexCount || !ReadIndices(dc, mem, raw)) return false;

    const uint32_t offset = regs[xe::VGT_INDX_OFFSET] & 0xFFFFFF;
    const bool resetEnabled = (regs[xe::PA_SU_SC_MODE_CNTL] >> 21) & 1;
    const uint32_t resetIndex = regs[xe::VGT_MULTI_PRIM_IB_RESET_INDX] & 0xFFFFFF;
    const uint32_t resetMask = dc.index32 ? 0xFFFFFF : 0xFFFF;

    // Split into runs at the primitive reset index (strips / fans / loops restart there). Runs are
    // [begin, end) spans of raw, compacted in place, with the index offset applied.
    struct Run { size_t b, e; };
    static thread_local std::vector<Run> runs;
    runs.clear();
    if (!(dc.indexed && resetEnabled)) {
        const size_t n = raw.size();
        uint32_t* d = raw.data();
        for (size_t i = 0; i < n; i++) d[i] = (d[i] + offset) & 0xFFFFFF;
        runs.push_back({0, n});
    } else {
        size_t w = 0, start = 0;
        for (uint32_t v : raw) {
            if ((v & resetMask) == (resetIndex & resetMask)) {
                runs.push_back({start, w});
                start = w;
                continue;
            }
            raw[w++] = (v + offset) & 0xFFFFFF;
        }
        runs.push_back({start, w});
    }

    auto& o = out.indices;
    auto grow = [&o](size_t n) {  // appends n uninitialised-to-zero entries and returns a pointer to them
        const size_t at = o.size();
        o.resize(at + n);
        return o.data() + at;
    };
    for (const Run& run : runs) {
        const uint32_t* p = raw.data() + run.b;
        const size_t n = run.e - run.b;
        switch (dc.primType) {
            case xe::kPrimPointList:
                out.prim = HostPrim::Points;
                if (n) std::memcpy(grow(n), p, n * 4);
                break;
            case xe::kPrimLineList:
                out.prim = HostPrim::Lines;
                if (n >= 2) std::memcpy(grow(n / 2 * 2), p, n / 2 * 2 * 4);
                break;
            case xe::kPrimLineStrip:
            case xe::kPrimLineLoop:
                out.prim = HostPrim::Lines;
                if (n >= 2) {
                    uint32_t* w = grow(2 * (n - 1));
                    for (size_t i = 0; i + 1 < n; i++) { w[2 * i] = p[i]; w[2 * i + 1] = p[i + 1]; }
                }
                if (dc.primType == xe::kPrimLineLoop && n > 2) { uint32_t* w = grow(2); w[0] = p[n - 1]; w[1] = p[0]; }
                break;
            case xe::kPrimTriangleList:
            case xe::kPrimTriangleWithWFlags:
                out.prim = HostPrim::Triangles;
                if (n >= 3) std::memcpy(grow(n / 3 * 3), p, n / 3 * 3 * 4);
                break;
            case xe::kPrimTriangleStrip:
                out.prim = HostPrim::Triangles;
                if (n >= 3) {
                    uint32_t* w = grow(3 * (n - 2));
                    for (size_t i = 0; i + 2 < n; i++, w += 3) {
                        if (i & 1) { w[0] = p[i + 1]; w[1] = p[i]; w[2] = p[i + 2]; }
                        else { w[0] = p[i]; w[1] = p[i + 1]; w[2] = p[i + 2]; }
                    }
                }
                break;
            case xe::kPrimTriangleFan:
            case xe::kPrimPolygon:
                out.prim = HostPrim::Triangles;
                if (n >= 3) {
                    uint32_t* w = grow(3 * (n - 2));
                    for (size_t i = 1; i + 1 < n; i++, w += 3) { w[0] = p[0]; w[1] = p[i]; w[2] = p[i + 1]; }
                }
                break;
            case xe::kPrimQuadList:
                out.prim = HostPrim::Triangles;
                if (n >= 4) {
                    uint32_t* w = grow(6 * (n / 4));
                    for (size_t i = 0; i + 3 < n; i += 4, w += 6) {
                        w[0] = p[i]; w[1] = p[i + 1]; w[2] = p[i + 2];
                        w[3] = p[i]; w[4] = p[i + 2]; w[5] = p[i + 3];
                    }
                }
                break;
            case xe::kPrimQuadStrip:
                out.prim = HostPrim::Triangles;
                if (n >= 4) {
                    uint32_t* w = grow(6 * ((n - 4) / 2 + 1));
                    for (size_t i = 0; i + 3 < n; i += 2, w += 6) {
                        w[0] = p[i]; w[1] = p[i + 1]; w[2] = p[i + 3];
                        w[3] = p[i]; w[4] = p[i + 3]; w[5] = p[i + 2];
                    }
                }
                break;
            case xe::kPrimRectangleList:
                out.prim = HostPrim::Rects;
                if (n >= 3) std::memcpy(grow(n / 3 * 3), p, n / 3 * 3 * 4);
                break;
            default:
                out.supported = false;
                return false;
        }
    }
    return !o.empty();
}

}  // namespace fa::gpu
