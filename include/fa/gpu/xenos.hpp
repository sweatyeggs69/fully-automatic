#pragma once
// Xenos register indices and field helpers used by the renderer. Register numbers and bit
// layouts follow the public Xenia documentation of the Xenos GPU (register_table.inc,
// registers.h, xenos.h).
#include <cstdint>
#include <cstring>

namespace fa::gpu::xe {

// ---- Registers ----------------------------------------------------------------------------
enum Reg : uint32_t {
    RB_SURFACE_INFO = 0x2000,
    RB_COLOR_INFO = 0x2001,
    RB_DEPTH_INFO = 0x2002,
    RB_COLOR1_INFO = 0x2003,
    RB_COLOR2_INFO = 0x2004,
    RB_COLOR3_INFO = 0x2005,
    PA_SC_SCREEN_SCISSOR_TL = 0x200E,
    PA_SC_SCREEN_SCISSOR_BR = 0x200F,
    PA_SC_WINDOW_OFFSET = 0x2080,
    PA_SC_WINDOW_SCISSOR_TL = 0x2081,
    PA_SC_WINDOW_SCISSOR_BR = 0x2082,
    VGT_MAX_VTX_INDX = 0x2100,
    VGT_MIN_VTX_INDX = 0x2101,
    VGT_INDX_OFFSET = 0x2102,
    VGT_MULTI_PRIM_IB_RESET_INDX = 0x2103,
    RB_COLOR_MASK = 0x2104,
    RB_BLEND_RED = 0x2105,
    RB_BLEND_GREEN = 0x2106,
    RB_BLEND_BLUE = 0x2107,
    RB_BLEND_ALPHA = 0x2108,
    RB_STENCILREFMASK_BF = 0x210C,
    RB_STENCILREFMASK = 0x210D,
    RB_ALPHA_REF = 0x210E,
    PA_CL_VPORT_XSCALE = 0x210F,
    PA_CL_VPORT_XOFFSET = 0x2110,
    PA_CL_VPORT_YSCALE = 0x2111,
    PA_CL_VPORT_YOFFSET = 0x2112,
    PA_CL_VPORT_ZSCALE = 0x2113,
    PA_CL_VPORT_ZOFFSET = 0x2114,
    SQ_PROGRAM_CNTL = 0x2180,
    SQ_CONTEXT_MISC = 0x2181,
    SQ_INTERPOLATOR_CNTL = 0x2182,
    VGT_DMA_BASE = 0x21FA,
    VGT_DMA_SIZE = 0x21FB,
    VGT_DRAW_INITIATOR = 0x21FC,
    RB_DEPTHCONTROL = 0x2200,
    RB_BLENDCONTROL0 = 0x2201,
    RB_COLORCONTROL = 0x2202,
    PA_CL_CLIP_CNTL = 0x2204,
    PA_SU_SC_MODE_CNTL = 0x2205,
    PA_CL_VTE_CNTL = 0x2206,
    RB_MODECONTROL = 0x2208,
    RB_BLENDCONTROL1 = 0x2209,
    RB_BLENDCONTROL2 = 0x220A,
    RB_BLENDCONTROL3 = 0x220B,
    PA_SU_POINT_SIZE = 0x2280,
    PA_SU_POINT_MINMAX = 0x2281,
    PA_SC_AA_CONFIG = 0x2301,
    PA_SU_VTX_CNTL = 0x2302,
    RB_COPY_CONTROL = 0x2318,
    RB_COPY_DEST_BASE = 0x2319,
    RB_COPY_DEST_PITCH = 0x231A,
    RB_COPY_DEST_INFO = 0x231B,
    RB_DEPTH_CLEAR = 0x231D,
    RB_COLOR_CLEAR = 0x231E,
    RB_COLOR_CLEAR_LO = 0x231F,
    PA_SU_POLY_OFFSET_FRONT_SCALE = 0x2380,
    PA_SU_POLY_OFFSET_FRONT_OFFSET = 0x2381,
    PA_SU_POLY_OFFSET_BACK_SCALE = 0x2382,
    PA_SU_POLY_OFFSET_BACK_OFFSET = 0x2383,

    // Constant windows (same as pm4.hpp).
    SHADER_CONSTANT_000_X = 0x4000,   // 512 float4: vertex 0..255, pixel 256..511
    SHADER_CONSTANT_FETCH_00_0 = 0x4800,  // 32 x 6 dwords
    SHADER_CONSTANT_BOOL_000_031 = 0x4900,  // 8 dwords = 256 bools
    SHADER_CONSTANT_LOOP_00 = 0x4908,       // 32 loop constants
};

inline uint32_t Bits(uint32_t v, unsigned shift, unsigned count) { return (v >> shift) & ((1u << count) - 1); }
inline int32_t SBits(uint32_t v, unsigned shift, unsigned count) {
    int32_t x = int32_t(v << (32 - shift - count));
    return x >> (32 - count);
}
inline float AsFloat(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }
inline uint32_t AsUint(float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; }

// ---- Enums --------------------------------------------------------------------------------
enum PrimitiveType : uint32_t {
    kPrimNone = 0x00,
    kPrimPointList = 0x01,
    kPrimLineList = 0x02,
    kPrimLineStrip = 0x03,
    kPrimTriangleList = 0x04,
    kPrimTriangleFan = 0x05,
    kPrimTriangleStrip = 0x06,
    kPrimTriangleWithWFlags = 0x07,
    kPrimRectangleList = 0x08,
    kPrimLineLoop = 0x0C,
    kPrimQuadList = 0x0D,
    kPrimQuadStrip = 0x0E,
    kPrimPolygon = 0x0F,
};

enum EdramMode : uint32_t { kEdramNoOp = 0, kEdramColorDepth = 4, kEdramDepthOnly = 5, kEdramCopy = 6 };

// RB_COLOR_INFO.color_format
enum ColorRtFormat : uint32_t {
    kRt8888 = 0,
    kRt8888Gamma = 1,
    kRt2_10_10_10 = 2,
    kRt2_10_10_10Float = 3,
    kRt16_16 = 4,
    kRt16_16_16_16 = 5,
    kRt16_16Float = 6,
    kRt16_16_16_16Float = 7,
    kRt2_10_10_10As10 = 10,
    kRt2_10_10_10FloatAs16 = 12,
    kRt32Float = 14,
    kRt32_32Float = 15,
};

// Texture / resolve destination formats (xenos::TextureFormat); see texture.hpp for the ones the
// decoder knows.
enum TextureFormatId : uint32_t {
    kTex8 = 2, kTex1_5_5_5 = 3, kTex5_6_5 = 4, kTex6_5_5 = 5, kTex8_8_8_8 = 6, kTex2_10_10_10 = 7,
    kTex8_A = 8, kTex8_B = 9, kTex8_8 = 10, kTex8_8_8_8_A = 14, kTex4_4_4_4 = 15, kTex10_11_11 = 16,
    kTex11_11_10 = 17, kTexDXT1 = 18, kTexDXT2_3 = 19, kTexDXT4_5 = 20, kTex24_8 = 22, kTex24_8Float = 23,
    kTex16 = 24, kTex16_16 = 25, kTex16_16_16_16 = 26, kTex16Float = 30, kTex16_16Float = 31,
    kTex16_16_16_16Float = 32, kTex32 = 33, kTex32_32 = 34, kTex32_32_32_32 = 35, kTex32Float = 36,
    kTex32_32Float = 37, kTex32_32_32_32Float = 38, kTexDXN = 49, kTex8_8_8_8As16 = 50, kTexDXT1As16 = 51,
    kTexDXT2_3As16 = 52, kTexDXT4_5As16 = 53, kTex2_10_10_10As16 = 54, kTex10_11_11As16 = 55,
    kTex11_11_10As16 = 56, kTex32_32_32Float = 57, kTexDXT3A = 58, kTexDXT5A = 59, kTexCTX1 = 60,
};

// Vertex fetch formats (xenos::VertexFormat).
enum VertexFormat : uint32_t {
    kVtx8_8_8_8 = 6, kVtx2_10_10_10 = 7, kVtx10_11_11 = 16, kVtx11_11_10 = 17, kVtx16_16 = 25,
    kVtx16_16_16_16 = 26, kVtx16_16Float = 31, kVtx16_16_16_16Float = 32, kVtx32 = 33, kVtx32_32 = 34,
    kVtx32_32_32_32 = 35, kVtx32Float = 36, kVtx32_32Float = 37, kVtx32_32_32_32Float = 38,
    kVtx32_32_32Float = 57,
};

// Vertex fetch constant (two dwords at 0x4800 + 2 * index).
struct VertexFetch {
    uint32_t type = 0;      // 3 = vertex
    uint32_t address = 0;   // bytes
    uint32_t endian = 0;
    uint32_t sizeWords = 0;
};
inline VertexFetch ParseVertexFetch(uint32_t d0, uint32_t d1) {
    VertexFetch v;
    v.type = d0 & 3;
    v.address = d0 & ~3u;
    v.endian = d1 & 3;
    v.sizeWords = (d1 >> 2) & 0xFFFFFF;
    return v;
}

}  // namespace fa::gpu::xe
