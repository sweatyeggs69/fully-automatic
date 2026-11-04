#include "fa/gpu/texture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace fa::gpu {

TextureFetch ParseTextureFetch(const uint32_t d[6]) {
    TextureFetch t;
    t.type = d[0] & 3;
    t.pitch = ((d[0] >> 22) & 0x1FF) << 5;
    t.tiled = (d[0] >> 31) & 1;
    t.format = d[1] & 0x3F;
    t.endian = (d[1] >> 6) & 3;
    t.base = d[1] & 0xFFFFF000u;
    t.width = (d[2] & 0x1FFF) + 1;
    t.height = ((d[2] >> 13) & 0x1FFF) + 1;
    t.swizzle = (d[3] >> 1) & 0xFFF;
    if (t.swizzle == 0) t.swizzle = 0x688;
    t.dimension = (d[5] >> 9) & 3;
    for (int c = 0; c < 4; c++) t.sign[c] = uint8_t((d[0] >> (2 + 2 * c)) & 3);
    for (int c = 0; c < 3; c++) t.clamp[c] = uint8_t((d[0] >> (10 + 3 * c)) & 7);
    t.magFilter = uint8_t((d[3] >> 19) & 3);
    t.minFilter = uint8_t((d[3] >> 21) & 3);
    t.mipFilter = uint8_t((d[3] >> 23) & 3);
    t.borderColor = uint8_t(d[5] & 3);
    t.expAdjust = int8_t(int32_t(d[3] << 13) >> 26);
    t.mipBase = d[5] & 0xFFFFF000u;
    t.mipMin = uint8_t((d[4] >> 2) & 0xF);
    t.mipMax = uint8_t((d[4] >> 6) & 0xF);
    t.packedMips = (d[5] >> 11) & 1;
    if (t.dimension == 2) {  // 3D: 11 + 11 + 10 bits
        t.width = (d[2] & 0x7FF) + 1;
        t.height = ((d[2] >> 11) & 0x7FF) + 1;
        t.depth = ((d[2] >> 22) & 0x3FF) + 1;
    } else if (t.dimension == 0) {
        t.width = (d[2] & 0xFFFFFF) + 1;
        t.height = 1;
    } else {
        t.depth = ((d[2] >> 26) & 0x3F) + 1;  // stack depth (2D arrays)
    }
    return t;
}

FormatInfo GetFormatInfo(uint32_t format) {
    switch (format) {
        case kFmt8: return {1, 1, 1, true};
        case kFmt5_6_5:
        case kFmt8_8: return {1, 1, 2, true};
        case kFmt8_8_8_8:
        case kFmt8_8_8_8_A:
        case kFmt8_8_8_8_AS_16:
        case kFmt2_10_10_10:
        case kFmt2_10_10_10_AS_16: return {1, 1, 4, true};
        case kFmtDXT1:
        case kFmtDXT1_AS_16: return {4, 4, 8, true};
        case kFmtDXT2_3:
        case kFmtDXT2_3_AS_16:
        case kFmtDXT4_5:
        case kFmtDXT4_5_AS_16: return {4, 4, 16, true};
        default: return {1, 1, 4, false};
    }
}

static uint32_t Log2Bpb(uint32_t bpb) {
    // 1 -> 0, 2 -> 1, 4 -> 2, 8 -> 3, 16 -> 4
    return (bpb >> 2) + ((bpb >> 1) >> (bpb >> 2));
}

uint32_t TiledBlockOffset(uint32_t x, uint32_t y, uint32_t pitchBlocks, uint32_t bytesPerBlock) {
    // Xenos 2D tiling: 32x32 block macro tiles, swizzled inside. Returns a byte offset.
    const uint32_t lb = Log2Bpb(bytesPerBlock);
    uint32_t macroY = ((y >> 5) * (pitchBlocks >> 5)) << (lb + 7);
    uint32_t microY = ((y & 6) << 2) << lb;
    uint32_t row = macroY + ((microY & ~0xFu) << 1) + (microY & 0xF) + ((y & 8) << (3 + lb)) + ((y & 1) << 4);
    uint32_t macroX = (x >> 5) << (lb + 7);
    uint32_t microX = (x & 7) << lb;
    uint32_t off = row + (macroX + ((microX & ~0xFu) << 1) + (microX & 0xF));
    off = ((off & ~0x1FFu) << 3) + ((off & 0x1C0) << 2) + (off & 0x3F) + ((y & 16) << 7) +
          (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
    // `off` counts in units of bytesPerBlock-sized elements shifted by lb; convert back to bytes.
    return (off >> lb) * bytesPerBlock;
}

namespace {
struct Layout {
    FormatInfo fi;
    uint32_t wBlocks, hBlocks, pitchBlocks, rowBytes, size;
};

uint32_t Align(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

Layout GetLayoutFor(const TextureFetch& t, const FormatInfo& fi) {
    Layout l;
    l.fi = fi;
    l.wBlocks = (t.width + l.fi.blockW - 1) / l.fi.blockW;
    l.hBlocks = (t.height + l.fi.blockH - 1) / l.fi.blockH;
    uint32_t pitchTexels = t.pitch ? t.pitch : Align(t.width, 32);
    pitchTexels = std::max(pitchTexels, t.width);
    l.pitchBlocks = (pitchTexels + l.fi.blockW - 1) / l.fi.blockW;
    if (t.tiled) {
        l.pitchBlocks = Align(l.pitchBlocks, 32);
        l.rowBytes = l.pitchBlocks * l.fi.bytesPerBlock;
        l.size = l.rowBytes * Align(l.hBlocks, 32);
    } else {
        l.rowBytes = Align(l.pitchBlocks * l.fi.bytesPerBlock, 256);
        l.size = l.rowBytes * l.hBlocks;
    }
    return l;
}

Layout GetLayout(const TextureFetch& t) { return GetLayoutFor(t, GetFormatInfo(t.format)); }

}  // namespace

// The GPU applies the endian swap to every 32-bit (or 16-bit) unit it reads.
void EndianSwapCopy(uint32_t endian, const uint8_t* src, uint8_t* dst, uint32_t size) {
    switch (endian) {
        case kEndian8in16:
            for (uint32_t i = 0; i + 1 < size; i += 2) { dst[i] = src[i + 1]; dst[i + 1] = src[i]; }
            break;
        case kEndian8in32:
            for (uint32_t i = 0; i + 3 < size; i += 4) {
                dst[i] = src[i + 3]; dst[i + 1] = src[i + 2]; dst[i + 2] = src[i + 1]; dst[i + 3] = src[i];
            }
            break;
        case kEndian16in32:
            for (uint32_t i = 0; i + 3 < size; i += 4) {
                dst[i] = src[i + 2]; dst[i + 1] = src[i + 3]; dst[i + 2] = src[i]; dst[i + 3] = src[i + 1];
            }
            break;
        default:
            if (dst != src) std::memmove(dst, src, size);
            break;
    }
}

namespace {
void SwapCopy(uint32_t endian, const uint8_t* src, uint8_t* dst, uint32_t size) { EndianSwapCopy(endian, src, dst, size); }

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

void Rgb565(uint16_t c, uint8_t out[3]) {
    out[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
    out[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    out[2] = uint8_t((c & 31) * 255 / 31);
}

// Colour half of a DXT block -> 16 XYZW texels. `fourColour` forces the DXT3/5 behaviour.
void DecodeDxtColour(const uint8_t* b, bool fourColour, uint8_t px[16][4]) {
    uint16_t c0 = Le16(b), c1 = Le16(b + 2);
    uint32_t idx = Le32(b + 4);
    uint8_t pal[4][4];
    Rgb565(c0, pal[0]); Rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = 255;
    if (fourColour || c0 > c1) {
        for (int k = 0; k < 3; k++) {
            pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; k++) { pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2); pal[3][k] = 0; }
        pal[2][3] = 255; pal[3][3] = 0;
    }
    for (int i = 0; i < 16; i++) std::memcpy(px[i], pal[(idx >> (2 * i)) & 3], 4);
}

void DecodeBlock(uint32_t format, const uint8_t* b, uint8_t px[16][4]) {
    switch (format) {
        case kFmt8: px[0][0] = px[0][1] = px[0][2] = b[0]; px[0][3] = 255; break;
        case kFmt8_8: px[0][0] = px[0][1] = px[0][2] = b[0]; px[0][3] = b[1]; break;
        case kFmt5_6_5: {
            uint16_t c = Le16(b);
            px[0][0] = uint8_t((c & 31) * 255 / 31);
            px[0][1] = uint8_t(((c >> 5) & 63) * 255 / 63);
            px[0][2] = uint8_t(((c >> 11) & 31) * 255 / 31);
            px[0][3] = 255;
            break;
        }
        case kFmt8_8_8_8: case kFmt8_8_8_8_A: case kFmt8_8_8_8_AS_16:
            std::memcpy(px[0], b, 4);
            break;
        case kFmt2_10_10_10: case kFmt2_10_10_10_AS_16: {
            uint32_t v = Le32(b);
            px[0][0] = uint8_t((v & 1023) >> 2);
            px[0][1] = uint8_t(((v >> 10) & 1023) >> 2);
            px[0][2] = uint8_t(((v >> 20) & 1023) >> 2);
            px[0][3] = uint8_t(((v >> 30) & 3) * 85);
            break;
        }
        case kFmtDXT1: case kFmtDXT1_AS_16: DecodeDxtColour(b, false, px); break;
        case kFmtDXT2_3: case kFmtDXT2_3_AS_16:
            DecodeDxtColour(b + 8, true, px);
            for (int i = 0; i < 16; i++) px[i][3] = uint8_t(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
            break;
        case kFmtDXT4_5: case kFmtDXT4_5_AS_16: {
            DecodeDxtColour(b + 8, true, px);
            uint8_t a[8] = {b[0], b[1]};
            if (a[0] > a[1]) for (int k = 1; k < 7; k++) a[k + 1] = uint8_t(((7 - k) * a[0] + k * a[1]) / 7);
            else {
                for (int k = 1; k < 5; k++) a[k + 1] = uint8_t(((5 - k) * a[0] + k * a[1]) / 5);
                a[6] = 0; a[7] = 255;
            }
            uint64_t bits = 0;
            for (int k = 0; k < 6; k++) bits |= uint64_t(b[2 + k]) << (8 * k);
            for (int i = 0; i < 16; i++) px[i][3] = a[(bits >> (3 * i)) & 7];
            break;
        }
        default: std::memset(px, 0, 16 * 4); break;
    }
}
}  // namespace

uint32_t TextureByteSize(const TextureFetch& t) { return GetLayout(t).size; }

bool DecodeTexture(const TextureFetch& t, const uint8_t* src, std::vector<uint8_t>& rgba) {
    Layout l = GetLayout(t);
    if (!l.fi.supported || !t.width || !t.height) return false;
    std::vector<uint8_t> swapped(l.size);
    SwapCopy(t.endian, src, swapped.data(), l.size);

    rgba.assign(size_t(t.width) * t.height * 4, 0);
    const bool grey = t.format == kFmt8 || t.format == kFmt8_8;
    uint32_t sw[4];
    for (int c = 0; c < 4; c++) sw[c] = (t.swizzle >> (3 * c)) & 7;
    const uint32_t texels = l.fi.blockW * l.fi.blockH;

    for (uint32_t by = 0; by < l.hBlocks; by++) {
        for (uint32_t bx = 0; bx < l.wBlocks; bx++) {
            uint32_t off = t.tiled ? TiledBlockOffset(bx, by, l.pitchBlocks, l.fi.bytesPerBlock)
                                   : by * l.rowBytes + bx * l.fi.bytesPerBlock;
            if (off + l.fi.bytesPerBlock > l.size) continue;
            uint8_t px[16][4];
            DecodeBlock(t.format, swapped.data() + off, px);
            for (uint32_t i = 0; i < texels; i++) {
                uint32_t x = bx * l.fi.blockW + i % l.fi.blockW, y = by * l.fi.blockH + i / l.fi.blockW;
                if (x >= t.width || y >= t.height) continue;
                uint8_t* o = &rgba[(size_t(y) * t.width + x) * 4];
                if (grey) { std::memcpy(o, px[i], 4); continue; }
                const uint8_t comp[6] = {px[i][0], px[i][1], px[i][2], px[i][3], 0, 255};
                for (int c = 0; c < 4; c++) o[c] = sw[c] < 6 ? comp[sw[c]] : 0;
            }
        }
    }
    return true;
}


// ============================================================================================
// Host upload decode and resolve encode (renderer).
// ============================================================================================
namespace {

float HalfToFloat(uint16_t h) {
    uint32_t sign = uint32_t(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, bits;
    if (e == 0) {
        if (m == 0) bits = sign;
        else {
            e = 127 - 15 + 1;
            while (!(m & 1024)) { m <<= 1; e--; }
            bits = sign | (e << 23) | ((m & 1023) << 13);
        }
    } else if (e == 31) {
        bits = sign | 0x7F800000u | (m << 13);
    } else {
        bits = sign | ((e + 127 - 15) << 23) | (m << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

uint16_t FloatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000;
    int32_t e = int32_t((x >> 23) & 0xFF) - 127 + 15;
    uint32_t m = x & 0x7FFFFF;
    if (((x >> 23) & 0xFF) == 0xFF) return uint16_t(sign | 0x7C00 | (m ? 0x200 : 0));
    if (e >= 31) return uint16_t(sign | 0x7C00);
    if (e <= 0) {
        if (e < -10) return uint16_t(sign);
        m |= 0x800000;
        uint32_t shift = uint32_t(14 - e);
        uint32_t hm = m >> shift;
        if ((m >> (shift - 1)) & 1) hm++;
        return uint16_t(sign | hm);
    }
    uint32_t hm = m >> 13;
    uint16_t r = uint16_t(sign | (uint32_t(e) << 10) | hm);
    if (m & 0x1000) r++;  // round half up
    return r;
}

float F32(const uint8_t* p) { float f; std::memcpy(&f, p, 4); return f; }

// BC4-style 8-byte block: two 8-bit endpoints + 3-bit indices.
void DecodeBc4(const uint8_t* b, float out[16]) {
    float a[8] = {b[0] / 255.0f, b[1] / 255.0f};
    if (b[0] > b[1]) for (int k = 1; k < 7; k++) a[k + 1] = ((7 - k) * a[0] + k * a[1]) / 7.0f;
    else {
        for (int k = 1; k < 5; k++) a[k + 1] = ((5 - k) * a[0] + k * a[1]) / 5.0f;
        a[6] = 0.0f; a[7] = 1.0f;
    }
    uint64_t bits = 0;
    for (int k = 0; k < 6; k++) bits |= uint64_t(b[2 + k]) << (8 * k);
    for (int i = 0; i < 16; i++) out[i] = a[(bits >> (3 * i)) & 7];
}

}  // namespace

FormatInfo GetHostFormatInfo(uint32_t f) {
    switch (f) {
        case 2: case 8: case 9: return {1, 1, 1, true};
        case 3: case 4: case 5: case 10: case 15: case 24: case 30: return {1, 1, 2, true};
        case 6: case 7: case 14: case 16: case 17: case 22: case 23: case 25: case 31: case 33: case 36:
        case 50: case 54: case 55: case 56: return {1, 1, 4, true};
        case 26: case 32: case 34: case 37: return {1, 1, 8, true};
        case 35: case 38: return {1, 1, 16, true};
        case 57: return {1, 1, 12, false};  // 96-bit texels cannot be tiled; not decoded
        case 18: case 51: case 58: case 59: case 60: return {4, 4, 8, true};
        case 19: case 20: case 49: case 52: case 53: return {4, 4, 16, true};
        default: return {1, 1, 4, false};
    }
}

static uint32_t FaceBytes(const TextureFetch& t) { return GetLayoutFor(t, GetHostFormatInfo(t.format)).size; }

uint32_t HostTextureByteSize(const TextureFetch& t) {
    uint32_t face = FaceBytes(t);
    if (t.dimension == 3) return Align(face, 4096) * 5 + face;
    return face;
}

// Unsigned 8-bit and DXT formats going to an RGBA8 host texture: the bytes are the result, so
// skip the float round trip. Returns false for formats it does not cover.
static bool DecodeBytesFast(uint32_t f, const uint8_t* b, uint8_t* dst, uint32_t bx, uint32_t by, const FormatInfo& fi,
                            uint32_t width, uint32_t height) {
    uint8_t px[16][4];
    switch (f) {
        case 2: case 8: case 9: px[0][0] = b[0]; px[0][1] = 0; px[0][2] = 0; px[0][3] = 255; break;
        case 10: px[0][0] = b[0]; px[0][1] = b[1]; px[0][2] = 0; px[0][3] = 255; break;
        case 6: case 14: case 50: std::memcpy(px[0], b, 4); break;
        case 18: case 19: case 20: case 51: case 52: case 53: DecodeBlock(f, b, px); break;
        default: return false;
    }
    const uint32_t bw = fi.blockW, bh = fi.blockH;
    for (uint32_t iy = 0; iy < bh; iy++) {
        const uint32_t y = by * bh + iy;
        if (y >= height) break;
        for (uint32_t ix = 0; ix < bw; ix++) {
            const uint32_t x = bx * bw + ix;
            if (x >= width) break;
            std::memcpy(dst + (size_t(y) * width + x) * 4, px[iy * bw + ix], 4);
        }
    }
    return true;
}


static TextureFetch MipFetch(const TextureFetch& t, uint32_t m) {
    TextureFetch tl = t;
    tl.width = std::max(1u, t.width >> m);
    tl.height = std::max(1u, t.height >> m);
    tl.pitch = 0;
    return tl;
}

uint32_t HostMipLevels(const TextureFetch& t) {
    static const bool off = [] { const char* e = std::getenv("FA_MIPS"); return e && e[0] == '0'; }();  // FA_MIPS=0: base level only
    if (off) return 1;
    if (t.dimension != 1 || !t.tiled || t.depth > 1 || t.mipFilter == 2 || t.mipMax == 0 || !t.mipBase) return 1;
    if (!GetHostFormatInfo(t.format).supported) return 1;
    uint32_t n = 1;
    for (uint32_t m = 1; m <= t.mipMax; m++) {
        const uint32_t w = std::max(1u, t.width >> m), h = std::max(1u, t.height >> m);
        if (t.packedMips && std::min(w, h) <= 16) break;
        n++;
    }
    return n;
}

uint32_t HostMipByteSize(const TextureFetch& t) {
    const uint32_t n = HostMipLevels(t);
    uint32_t total = 0;
    const FormatInfo fi = GetHostFormatInfo(t.format);
    for (uint32_t m = 1; m < n; m++) total += GetLayoutFor(MipFetch(t, m), fi).size;
    return total;
}

bool DecodeTextureHost(const TextureFetch& t, const uint8_t* src, HostTexture& out, const uint8_t* mipSrc) {
    FormatInfo fi = GetHostFormatInfo(t.format);
    if (!fi.supported || !t.width || !t.height) return false;
    Layout l = GetLayoutFor(t, fi);
    const uint32_t f = t.format;
    bool floatFormat = f == 24 || f == 25 || f == 26 || f == 30 || f == 31 || f == 32 || f == 33 || f == 34 ||
                       f == 35 || f == 36 || f == 37 || f == 38 || f == 22 || f == 23;
    bool anySign = false;
    for (int c = 0; c < 4; c++) anySign |= t.sign[c] != 0;
    out.isFloat = floatFormat || anySign || t.expAdjust != 0;
    out.width = t.width;
    out.height = t.height;
    out.layers = t.dimension == 3 ? 6 : 1;
    out.levels = mipSrc ? HostMipLevels(t) : 1;
    const size_t texelBytes = out.isFloat ? 16 : 4;
    size_t layerBytes = 0;  // all levels of one layer
    for (uint32_t m = 0; m < out.levels; m++) {
        const TextureFetch tl = MipFetch(t, m);
        layerBytes += size_t(tl.width) * tl.height * texelBytes;
    }
    out.data.assign(layerBytes * out.layers, 0);
    const float expScale = std::ldexp(1.0f, t.expAdjust);
    std::vector<uint8_t> sw;
    const uint32_t texels = fi.blockW * fi.blockH;

    auto decodeLevel = [&](const TextureFetch& t, const Layout& l, const uint8_t* face, uint8_t* dstLayer) {
        sw.resize(l.size);
        EndianSwapCopy(t.endian, face, sw.data(), l.size);
        for (uint32_t by = 0; by < l.hBlocks; by++) {
            for (uint32_t bx = 0; bx < l.wBlocks; bx++) {
                uint32_t off = t.tiled ? TiledBlockOffset(bx, by, l.pitchBlocks, fi.bytesPerBlock)
                                       : by * l.rowBytes + bx * fi.bytesPerBlock;
                if (off + fi.bytesPerBlock > l.size) continue;
                const uint8_t* b = sw.data() + off;
                if (!out.isFloat && DecodeBytesFast(f, b, dstLayer, bx, by, fi, t.width, t.height)) continue;
                float px[16][4];
                for (auto& p : px) { p[0] = p[1] = p[2] = 0.0f; p[3] = 1.0f; }
                auto bitsN = [&](uint32_t v, int sh, int n) { return float((v >> sh) & ((1u << n) - 1)) / float((1u << n) - 1); };
                uint32_t v32 = 0;
                uint16_t v16 = 0;
                if (fi.bytesPerBlock >= 4) std::memcpy(&v32, b, 4);
                if (fi.bytesPerBlock >= 2) std::memcpy(&v16, b, 2);
                switch (f) {
                    case 2: case 8: case 9: px[0][0] = b[0] / 255.0f; break;
                    case 3: px[0][0] = bitsN(v16, 0, 5); px[0][1] = bitsN(v16, 5, 5); px[0][2] = bitsN(v16, 10, 5); px[0][3] = bitsN(v16, 15, 1); break;
                    case 4: px[0][0] = bitsN(v16, 0, 5); px[0][1] = bitsN(v16, 5, 6); px[0][2] = bitsN(v16, 11, 5); break;
                    case 5: px[0][0] = bitsN(v16, 0, 6); px[0][1] = bitsN(v16, 6, 5); px[0][2] = bitsN(v16, 11, 5); break;
                    case 10: px[0][0] = b[0] / 255.0f; px[0][1] = b[1] / 255.0f; break;
                    case 15: for (int c = 0; c < 4; c++) px[0][c] = bitsN(v16, 4 * c, 4); break;
                    case 6: case 14: case 50: for (int c = 0; c < 4; c++) px[0][c] = b[c] / 255.0f; break;
                    case 7: case 54:
                        px[0][0] = bitsN(v32, 0, 10); px[0][1] = bitsN(v32, 10, 10); px[0][2] = bitsN(v32, 20, 10); px[0][3] = bitsN(v32, 30, 2);
                        break;
                    case 16: case 55: px[0][0] = bitsN(v32, 0, 11); px[0][1] = bitsN(v32, 11, 11); px[0][2] = bitsN(v32, 22, 10); break;
                    case 17: case 56: px[0][0] = bitsN(v32, 0, 10); px[0][1] = bitsN(v32, 10, 11); px[0][2] = bitsN(v32, 21, 11); break;
                    case 22: case 23: px[0][0] = float(v32 >> 8) / 16777215.0f; px[0][1] = float(v32 & 0xFF); break;
                    case 24: case 25: case 26: {
                        int n = f == 24 ? 1 : f == 25 ? 2 : 4;
                        for (int c = 0; c < n; c++) {
                            uint16_t h;
                            std::memcpy(&h, b + 2 * c, 2);
                            px[0][c] = t.sign[c] == 1 ? std::max(float(int16_t(h)) / 32767.0f, -1.0f) : h / 65535.0f;
                        }
                        break;
                    }
                    case 30: case 31: case 32: {
                        int n = f == 30 ? 1 : f == 31 ? 2 : 4;
                        for (int c = 0; c < n; c++) {
                            uint16_t h;
                            std::memcpy(&h, b + 2 * c, 2);
                            px[0][c] = HalfToFloat(h);
                        }
                        break;
                    }
                    case 33: case 34: case 35: {
                        int n = f == 33 ? 1 : f == 34 ? 2 : 4;
                        for (int c = 0; c < n; c++) {
                            uint32_t u;
                            std::memcpy(&u, b + 4 * c, 4);
                            px[0][c] = float(double(u) / 4294967295.0);
                        }
                        break;
                    }
                    case 36: px[0][0] = F32(b); break;
                    case 37: px[0][0] = F32(b); px[0][1] = F32(b + 4); break;
                    case 38: for (int c = 0; c < 4; c++) px[0][c] = F32(b + 4 * c); break;
                    case 18: case 19: case 20: case 51: case 52: case 53: {
                        uint8_t p8[16][4];
                        DecodeBlock(f, b, p8);
                        for (int i = 0; i < 16; i++)
                            for (int c = 0; c < 4; c++) px[i][c] = p8[i][c] / 255.0f;
                        break;
                    }
                    case 49: {
                        float xs[16], ys[16];
                        DecodeBc4(b, xs);
                        DecodeBc4(b + 8, ys);
                        for (int i = 0; i < 16; i++) { px[i][0] = xs[i]; px[i][1] = ys[i]; }
                        break;
                    }
                    case 58:
                        for (int i = 0; i < 16; i++) px[i][0] = ((b[i / 2] >> ((i & 1) * 4)) & 15) / 15.0f;
                        break;
                    case 59: {
                        float xs[16];
                        DecodeBc4(b, xs);
                        for (int i = 0; i < 16; i++) px[i][0] = xs[i];
                        break;
                    }
                    case 60: {
                        float e[4][2] = {{b[0] / 255.0f, b[1] / 255.0f}, {b[2] / 255.0f, b[3] / 255.0f}};
                        for (int c = 0; c < 2; c++) {
                            e[2][c] = (2 * e[0][c] + e[1][c]) / 3.0f;
                            e[3][c] = (e[0][c] + 2 * e[1][c]) / 3.0f;
                        }
                        uint32_t idx;
                        std::memcpy(&idx, b + 4, 4);
                        for (int i = 0; i < 16; i++) {
                            px[i][0] = e[(idx >> (2 * i)) & 3][0];
                            px[i][1] = e[(idx >> (2 * i)) & 3][1];
                        }
                        break;
                    }
                    default: break;
                }
                for (uint32_t i = 0; i < texels; i++) {
                    uint32_t x = bx * fi.blockW + i % fi.blockW, y = by * fi.blockH + i / fi.blockW;
                    if (x >= t.width || y >= t.height) continue;
                    float v[4] = {px[i][0], px[i][1], px[i][2], px[i][3]};
                    if (!floatFormat) {
                        for (int c = 0; c < 4; c++) {
                            if (t.sign[c] == 1) {
                                // Signed: reinterpret the unorm8 value as int8.
                                int8_t sv = int8_t(uint8_t(std::lround(v[c] * 255.0f)));
                                v[c] = std::max(float(sv) / 127.0f, -1.0f);
                            } else if (t.sign[c] == 2) {
                                v[c] = v[c] * 2.0f - 1.0f;
                            } else if (t.sign[c] == 3) {
                                v[c] = std::pow(v[c], 2.2f);
                            }
                        }
                    }
                    uint8_t* o = dstLayer + (size_t(y) * t.width + x) * texelBytes;
                    if (out.isFloat) {
                        for (int c = 0; c < 4; c++) v[c] *= expScale;
                        std::memcpy(o, v, 16);
                    } else {
                        for (int c = 0; c < 4; c++) o[c] = uint8_t(std::clamp(v[c], 0.0f, 1.0f) * 255.0f + 0.5f);
                    }
                }
            }
        }
    };
    for (uint32_t layer = 0; layer < out.layers; layer++) {
        const uint8_t* face = src + size_t(layer) * Align(l.size, 4096);
        uint8_t* dstLayer = out.data.data() + layer * layerBytes;
        decodeLevel(t, l, face, dstLayer);
        const uint8_t* mipFace = mipSrc;
        size_t dstOff = size_t(t.width) * t.height * texelBytes;
        for (uint32_t m = 1; m < out.levels; m++) {
            const TextureFetch tl = MipFetch(t, m);
            const Layout ll = GetLayoutFor(tl, fi);
            decodeLevel(tl, ll, mipFace, dstLayer + dstOff);
            mipFace += ll.size;
            dstOff += size_t(tl.width) * tl.height * texelBytes;
        }
    }
    return true;
}

static uint32_t ResolveBpp(uint32_t f) {
    switch (f) {
        case 2: return 1;
        case 3: case 4: case 10: case 15: case 24: case 30: return 2;
        case 6: case 7: case 14: case 16: case 17: case 22: case 23: case 25: case 31: case 36: case 50: case 54:
        case 55: case 56: return 4;
        case 26: case 32: case 37: return 8;
        case 38: return 16;
        default: return 0;
    }
}

bool ResolveFormatSupported(uint32_t format) { return ResolveBpp(format) != 0; }

uint32_t ResolveDestByteSize(const ResolveDest& d) {
    uint32_t bpp = ResolveBpp(d.format);
    return Align(std::max(d.pitch, 32u), 32) * Align(std::max(d.height, 1u), 32) * bpp;
}

void EncodeResolve(const ResolveDest& d, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h, const float* rgba,
                   uint8_t* dst) {
    const uint32_t bpp = ResolveBpp(d.format);
    if (!bpp) return;
    const uint32_t pitch = Align(std::max(d.pitch, 32u), 32), height = Align(std::max(d.height, 1u), 32);
    const uint32_t size = pitch * height * bpp;
    // An endian swap is a XOR on the byte address within the swapped unit.
    uint32_t x8 = 0;
    switch (d.endian) {
        case 1: x8 = 1; break;  // 8in16
        case 2: x8 = 3; break;  // 8in32
        case 3: x8 = 2; break;  // 16in32
        case 4: x8 = 7; break;  // 8in64
        case 5: x8 = 15; break; // 8in128
        default: break;
    }
    const float scale = std::ldexp(1.0f, d.expBias);
    // + 0.5 and truncate: the same as lround for non-negative values, without the libm call.
    auto un = [](float v, uint32_t maxv) { return uint32_t(double(std::clamp(v, 0.0f, 1.0f)) * maxv + 0.5); };
    for (uint32_t y = 0; y < h; y++) {
        uint32_t ty = dy + y;
        if (ty >= height) break;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t tx = dx + x;
            if (tx >= pitch) break;
            const float* p = rgba + (size_t(y) * w + x) * 4;
            float v[4] = {p[0] * scale, p[1] * scale, p[2] * scale, p[3] * scale};
            if (d.swapRB) std::swap(v[0], v[2]);
            uint8_t b[16] = {};
            uint32_t u = 0;
            switch (d.format) {
                case 2: b[0] = uint8_t(un(v[0], 255)); break;
                case 3: u = un(v[0], 31) | (un(v[1], 31) << 5) | (un(v[2], 31) << 10) | (un(v[3], 1) << 15); std::memcpy(b, &u, 2); break;
                case 4: u = un(v[0], 31) | (un(v[1], 63) << 5) | (un(v[2], 31) << 11); std::memcpy(b, &u, 2); break;
                case 10: b[0] = uint8_t(un(v[0], 255)); b[1] = uint8_t(un(v[1], 255)); break;
                case 15: u = un(v[0], 15) | (un(v[1], 15) << 4) | (un(v[2], 15) << 8) | (un(v[3], 15) << 12); std::memcpy(b, &u, 2); break;
                case 6: case 14: case 50: for (int c = 0; c < 4; c++) b[c] = uint8_t(un(v[c], 255)); break;
                case 7: case 54: u = un(v[0], 1023) | (un(v[1], 1023) << 10) | (un(v[2], 1023) << 20) | (un(v[3], 3) << 30); std::memcpy(b, &u, 4); break;
                case 16: case 55: u = un(v[0], 2047) | (un(v[1], 2047) << 11) | (un(v[2], 1023) << 22); std::memcpy(b, &u, 4); break;
                case 17: case 56: u = un(v[0], 1023) | (un(v[1], 2047) << 10) | (un(v[2], 2047) << 21); std::memcpy(b, &u, 4); break;
                case 22: case 23: u = (un(v[0], 16777215) << 8) | (uint32_t(std::clamp(v[1], 0.0f, 255.0f)) & 0xFF); std::memcpy(b, &u, 4); break;
                case 24: case 25: case 26: {
                    int n = d.format == 24 ? 1 : d.format == 25 ? 2 : 4;
                    for (int c = 0; c < n; c++) {
                        int16_t s16 = int16_t(std::lround(std::clamp(v[c], -1.0f, 1.0f) * 32767.0f));
                        std::memcpy(b + 2 * c, &s16, 2);
                    }
                    break;
                }
                case 30: case 31: case 32: {
                    int n = d.format == 30 ? 1 : d.format == 31 ? 2 : 4;
                    for (int c = 0; c < n; c++) {
                        uint16_t hv = FloatToHalf(v[c]);
                        std::memcpy(b + 2 * c, &hv, 2);
                    }
                    break;
                }
                case 36: std::memcpy(b, &v[0], 4); break;
                case 37: std::memcpy(b, v, 8); break;
                case 38: std::memcpy(b, v, 16); break;
                default: break;
            }
            uint32_t off = TiledBlockOffset(tx, ty, pitch, bpp);
            if (off + bpp > size) continue;
            for (uint32_t k = 0; k < bpp; k++) dst[(off + k) ^ x8] = b[k];
        }
    }
}

}  // namespace fa::gpu
