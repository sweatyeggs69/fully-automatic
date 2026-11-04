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
    {
        size_t w = 0, start = 0;
        const bool split = dc.indexed && resetEnabled;
        for (uint32_t v : raw) {
            if (split && (v & resetMask) == (resetIndex & resetMask)) {
                runs.push_back({start, w});
                start = w;
                continue;
            }
            raw[w++] = (v + offset) & 0xFFFFFF;
        }
        runs.push_back({start, w});
    }
    struct RunRef {
        const uint32_t* p; size_t n;
        size_t size() const { return n; }
        uint32_t operator[](size_t i) const { return p[i]; }
        uint32_t back() const { return p[n - 1]; }
        const uint32_t* begin() const { return p; }
        const uint32_t* end() const { return p + n; }
    };
    static thread_local std::vector<RunRef> rr;
    rr.clear();
    for (auto& r : runs) rr.push_back({raw.data() + r.b, r.e - r.b});

    auto& o = out.indices;
    o.reserve(raw.size() * 2);
    switch (dc.primType) {
        case xe::kPrimPointList:
            out.prim = HostPrim::Points;
            for (auto& r : rr) o.insert(o.end(), r.begin(), r.end());
            break;
        case xe::kPrimLineList:
            out.prim = HostPrim::Lines;
            for (auto& r : rr)
                for (size_t i = 0; i + 1 < r.size(); i += 2) { o.push_back(r[i]); o.push_back(r[i + 1]); }
            break;
        case xe::kPrimLineStrip:
        case xe::kPrimLineLoop:
            out.prim = HostPrim::Lines;
            for (auto& r : rr) {
                for (size_t i = 0; i + 1 < r.size(); i++) { o.push_back(r[i]); o.push_back(r[i + 1]); }
                if (dc.primType == xe::kPrimLineLoop && r.size() > 2) { o.push_back(r.back()); o.push_back(r[0]); }
            }
            break;
        case xe::kPrimTriangleList:
        case xe::kPrimTriangleWithWFlags:
            out.prim = HostPrim::Triangles;
            for (auto& r : rr)
                for (size_t i = 0; i + 2 < r.size(); i += 3) o.insert(o.end(), {r[i], r[i + 1], r[i + 2]});
            break;
        case xe::kPrimTriangleStrip:
            out.prim = HostPrim::Triangles;
            for (auto& r : rr)
                for (size_t i = 0; i + 2 < r.size(); i++) {
                    if (i & 1) o.insert(o.end(), {r[i + 1], r[i], r[i + 2]});
                    else o.insert(o.end(), {r[i], r[i + 1], r[i + 2]});
                }
            break;
        case xe::kPrimTriangleFan:
        case xe::kPrimPolygon:
            out.prim = HostPrim::Triangles;
            for (auto& r : rr)
                for (size_t i = 1; i + 1 < r.size(); i++) o.insert(o.end(), {r[0], r[i], r[i + 1]});
            break;
        case xe::kPrimQuadList:
            out.prim = HostPrim::Triangles;
            for (auto& r : rr)
                for (size_t i = 0; i + 3 < r.size(); i += 4)
                    o.insert(o.end(), {r[i], r[i + 1], r[i + 2], r[i], r[i + 2], r[i + 3]});
            break;
        case xe::kPrimQuadStrip:
            out.prim = HostPrim::Triangles;
            for (auto& r : rr)
                for (size_t i = 0; i + 3 < r.size(); i += 2)
                    o.insert(o.end(), {r[i], r[i + 1], r[i + 3], r[i], r[i + 3], r[i + 2]});
            break;
        case xe::kPrimRectangleList:
            out.prim = HostPrim::Rects;
            for (auto& r : rr)
                for (size_t i = 0; i + 2 < r.size(); i += 3) o.insert(o.end(), {r[i], r[i + 1], r[i + 2]});
            break;
        default:
            out.supported = false;
            return false;
    }
    return !o.empty();
}

}  // namespace fa::gpu
