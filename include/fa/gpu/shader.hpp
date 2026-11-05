#pragma once
// Xenos shader microcode -> GLSL (Vulkan, #version 450) translator. The renderer compiles the
// GLSL to SPIR-V at runtime (shaderc) and caches it by microcode hash.
//
// Microcode layout (per the public Xenia ucode.h): control flow instructions come first, two
// 48-bit instructions per three dwords; ALU and fetch instructions are 96 bits (three dwords)
// each, addressed in three-dword units from the start of the shader.
//
// The generated shaders share one interface (see GlslInterface() in shader_translator.cpp):
//   set 0 binding 0  uniform XeConsts  { vec4 c[512]; }  vertex c0..255, pixel c256..511
//   set 0 binding 1  uniform XeBoolLoop { uvec4 b[2]; uvec4 l[8]; }  256 bools, 32 loop consts
//   set 0 binding 2  uniform XeSystem   (viewport transform, alpha test, vertex fetch table)
//   set 0 binding 3  buffer XeVertexWords { uint w[]; }  raw guest vertex data (vertex pulling)
//   set 1 binding N / 32+N / 64+N  texture fetch slot N as sampler2D / samplerCube / sampler3D
//   VS -> PS: 16 vec4 interpolators at locations 0..15. PS writes oC0..3 at locations 0..3.
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace fa::gpu {

enum class ShaderStage : uint32_t { Vertex = 0, Pixel = 1 };

// Texture fetch dimensions (xenos::FetchOpDimension).
enum TexDim : uint8_t { kTexDim1D = 0, kTexDim2D = 1, kTexDim3D = 2, kTexDimCube = 3 };

struct TranslatedShader {
    ShaderStage stage = ShaderStage::Vertex;
    uint64_t hash = 0;
    bool ok = false;
    std::string error;
    std::string glsl;
    std::string disasm;  // human readable listing (FA_DUMP_SHADERS)

    // What the shader touches, so the renderer only uploads / binds what is needed.
    uint32_t vfetchUsed[3] = {0, 0, 0};  // bit i of word i/32: vertex fetch constant i (0..95)
    // Dwords one vertex index can reach in vertex fetch constant i: index * vfetchStride[i] +
    // vfetchEnd[i]. vfetchStride[i] == 0 means the extent is unknown (computed index).
    uint32_t vfetchStride[96] = {};
    uint32_t vfetchWhy[96] = {};  // why the extent is unknown: 1 src reg!=r0, 2 src relative, 4 src comp!=x, 8 stride 0, 16 offset<0, 32 r0.x written first
    uint32_t vfetchEnd[96] = {};
    uint32_t constMax = 256;             // float constants read (count from the stage's base: vs 0, ps 256)
    uint32_t texUsed = 0;                // bit n: texture fetch constant n (0..31)
    uint8_t texDim[32] = {};             // dimension per used texture slot
    uint32_t colorWritten = 0;           // PS: bit n = oCn exported
    uint32_t instructionCount = 0;       // ALU + fetch instructions translated
    bool writesDepth = false;
    bool kills = false;
    bool writesPointSize = false;
};

uint64_t HashUcode(const uint32_t* dwords, size_t count);

// Translates `count` dwords of microcode (host order).
TranslatedShader TranslateShader(ShaderStage stage, const uint32_t* dwords, size_t count);

// Byte layout of the XeSystem uniform block (std140), filled by the renderer per draw.
struct XeSystemConstants {
    float ndcScale[4];     // screen (EDRAM pixels) -> Vulkan NDC: ndc = screen * scale + offset
    float ndcOffset[4];
    float vportScale[4];   // PA_CL_VPORT_*SCALE (1.0 where disabled)
    float vportOffset[4];  // PA_CL_VPORT_*OFFSET (0.0 where disabled)
    uint32_t flags[4];     // x: bit0 xy already divided by w, bit1 z divided, bit2 w is 1/w;
                           // y: alpha test function (7 = always); z: param gen register or ~0;
                           // w: number of interpolators the PS reads
    float misc[4];         // x: alpha ref, y: point size (pixels), z: pixel centre offset, w: 0
    uint32_t vf[96][4];    // vertex fetch slot: x = word offset in XeVertexWords, y = endian, z = size words
    float texXf[32][4];    // texture slot conversion: x = mode (0 none, 1 scale + clamp, 2 float bits as unorm32),
                           // y = scale, z = clamp low, w = mask of result components to convert
    uint32_t cblk[32];     // float constant block b (c[16b..16b+15]) starts at vec4 index cblk[b] of the ring
};
static_assert(sizeof(XeSystemConstants) == 96 + 96 * 16 + 32 * 16 + 128, "std140 layout");

// Float constants are read through per-block ring offsets (XeSystem.cblk) unless FA_CONST_BLOCKS=0,
// so a draw uploads only the 256-byte blocks written since the last upload.
inline bool ConstBlocksOn() {
    static const bool on = [] { const char* e = std::getenv("FA_CONST_BLOCKS"); return !(e && e[0] == '0'); }();
    return on;
}

}  // namespace fa::gpu
