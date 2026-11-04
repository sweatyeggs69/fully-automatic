#pragma once
// Xenos texture helpers: fetch constant parsing, 2D tiling (untile), endian swap and a CPU
// decode of the formats the front end and movies use into plain RGBA8. Used to show the game's
// front buffer (and, as a debug view, the textures it draws with) before real rendering exists.
#include <cstdint>
#include <vector>

namespace fa::gpu {

// Texture formats (xenos::TextureFormat), only the ones decoded here.
enum TexFormat : uint32_t {
    kFmt8 = 2,
    kFmt5_6_5 = 4,
    kFmt8_8_8_8 = 6,
    kFmt2_10_10_10 = 7,
    kFmt8_8 = 10,
    kFmt8_8_8_8_A = 14,
    kFmtDXT1 = 18,
    kFmtDXT2_3 = 19,
    kFmtDXT4_5 = 20,
    kFmt8_8_8_8_AS_16 = 50,
    kFmtDXT1_AS_16 = 51,
    kFmtDXT2_3_AS_16 = 52,
    kFmtDXT4_5_AS_16 = 53,
    kFmt2_10_10_10_AS_16 = 54,
};

// Endian swap modes (xenos::Endian).
enum TexEndian : uint32_t { kEndianNone = 0, kEndian8in16 = 1, kEndian8in32 = 2, kEndian16in32 = 3 };

struct TextureFetch {
    uint32_t type = 0;        // 2 = texture, 3 = vertex
    uint32_t format = 0;
    uint32_t endian = 0;
    uint32_t base = 0;        // physical address of the base level
    uint32_t pitch = 0;       // row pitch in texels (0 = derive from width)
    uint32_t width = 0, height = 0;
    uint32_t dimension = 1;   // 0 1D, 1 2D, 2 3D, 3 cube
    uint32_t swizzle = 0x688; // 3 bits per output channel: 0..3 = XYZW, 4 = 0, 5 = 1
    bool tiled = false;
    bool IsTexture() const { return type == 2; }
};

// Six dwords of a fetch constant (registers 0x4800 + 6 * slot), host order.
TextureFetch ParseTextureFetch(const uint32_t d[6]);

struct FormatInfo {
    uint32_t blockW = 1, blockH = 1, bytesPerBlock = 4;
    bool supported = false;
};
FormatInfo GetFormatInfo(uint32_t format);

// Byte offset of block (x, y) inside a tiled 2D surface whose row pitch is `pitchBlocks`
// (a multiple of 32). bytesPerBlock is 1, 2, 4, 8 or 16.
uint32_t TiledBlockOffset(uint32_t x, uint32_t y, uint32_t pitchBlocks, uint32_t bytesPerBlock);

// Bytes of guest memory the base level occupies (what must be readable at `base`).
uint32_t TextureByteSize(const TextureFetch& t);

// Decode the base level to RGBA8 (R, G, B, A bytes per pixel, row-major, width * 4 per row).
// `src` points at the raw guest bytes of the base level (TextureByteSize bytes).
// Returns false for unsupported formats. For k_8 / k_8_8 the swizzle is ignored and the
// result is grey, which is what a movie's luma plane looks like.
bool DecodeTexture(const TextureFetch& t, const uint8_t* src, std::vector<uint8_t>& rgba);

}  // namespace fa::gpu
