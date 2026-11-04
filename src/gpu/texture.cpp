#include "fa/gpu/texture.hpp"

#include <algorithm>
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

Layout GetLayout(const TextureFetch& t) {
    Layout l;
    l.fi = GetFormatInfo(t.format);
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

// The GPU applies the endian swap to every 32-bit (or 16-bit) unit it reads.
void SwapCopy(uint32_t endian, const uint8_t* src, uint8_t* dst, uint32_t size) {
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
        default: std::memcpy(dst, src, size); break;
    }
}

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

}  // namespace fa::gpu
