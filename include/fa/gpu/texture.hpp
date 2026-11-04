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
    uint32_t depth = 1;       // 3D depth or stack depth
    uint8_t sign[4] = {};     // per channel: 0 unsigned, 1 signed, 2 unsigned biased, 3 gamma
    uint8_t clamp[3] = {};    // xenos::ClampMode for X, Y, Z
    uint8_t magFilter = 0, minFilter = 0, mipFilter = 0;  // 0 point, 1 linear
    uint8_t borderColor = 0;
    int8_t expAdjust = 0;
    uint32_t mipBase = 0;     // physical address of mip level 1 (dword 5)
    uint8_t mipMin = 0, mipMax = 0;  // fetch constant mip level range
    bool packedMips = false;  // small mips share one tail tile
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

// Applies a Xenos endian swap (0 none, 1 8in16, 2 8in32, 3 16in32) while copying. src may equal dst.
void EndianSwapCopy(uint32_t endian, const uint8_t* src, uint8_t* dst, uint32_t size);

// ---- Host upload (renderer) ---------------------------------------------------------------
// Base level decoded for upload to a Vulkan image: channels in XYZW order (no fetch swizzle,
// the renderer applies it), either RGBA8 unorm or RGBA32 float. Cube maps carry 6 layers.
struct HostTexture {
    uint32_t width = 0, height = 0, layers = 1, levels = 1;
    bool isFloat = false;           // RGBA32F (16 bytes per texel) instead of RGBA8
    std::vector<uint8_t> data;      // layer-major, row-major
};
// Format block layout for any format the host decoder handles (supported = false otherwise).
FormatInfo GetHostFormatInfo(uint32_t format);
// Bytes of guest memory the base level occupies (all faces for cube maps).
uint32_t HostTextureByteSize(const TextureFetch& t);
// Mip levels the host can decode for this fetch: 1 + the unpacked mips stored at mipBase (levels
// whose smaller side is 16 texels or less sit in a packed tail tile and are not decoded; sampling
// clamps to the last decoded level). Only tiled 2D textures with a mip filter get more than one.
uint32_t HostMipLevels(const TextureFetch& t);
// Bytes of guest memory at mipBase covering levels 1 .. HostMipLevels-1 (0 when there are none).
uint32_t HostMipByteSize(const TextureFetch& t);
// `mipSrc` points at the guest bytes at mipBase (HostMipByteSize bytes); null decodes the base level only.
// With more than one level (out.levels), out.data holds the levels back to back, base first.
bool DecodeTextureHost(const TextureFetch& t, const uint8_t* src, HostTexture& out, const uint8_t* mipSrc = nullptr);

// ---- Resolve (EDRAM -> guest memory) ------------------------------------------------------
struct ResolveDest {
    uint32_t base = 0;        // guest address
    uint32_t pitch = 0;       // texels (RB_COPY_DEST_PITCH), aligned to 32 here
    uint32_t height = 0;      // texels, aligned to 32 here
    uint32_t format = 6;      // xenos texture format of the destination
    uint32_t endian = 0;      // Endian128 (0..5)
    bool swapRB = false;      // RB_COPY_DEST_INFO.copy_dest_swap
    int expBias = 0;          // multiply by 2^expBias before conversion
};
// Bytes of the destination surface (tiled 2D, what must be writable at base).
uint32_t ResolveDestByteSize(const ResolveDest& d);
bool ResolveFormatSupported(uint32_t format);
// Converts `w` x `h` RGBA float pixels (row-major) and stores them at texel (dx, dy) of the
// tiled destination. `dst` points at the destination surface (ResolveDestByteSize bytes).
void EncodeResolve(const ResolveDest& d, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h, const float* rgba,
                   uint8_t* dst);

}  // namespace fa::gpu
