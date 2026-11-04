// Xenos microcode -> GLSL translator. See include/fa/gpu/shader.hpp for the interface the
// generated code uses. Instruction semantics follow the descriptions in Xenia's ucode.h
// (Direct3D 9 multiply rules: 0 * anything = 0, max/min via >= / <).
#include "fa/gpu/shader.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace fa::gpu {

uint64_t HashUcode(const uint32_t* d, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull ^ (uint64_t(n) * 0x9E3779B97F4A7C15ull);
    for (size_t i = 0; i < n; i++) {
        h ^= d[i];
        h *= 0x100000001B3ull;
        h ^= h >> 29;
    }
    return h;
}

namespace {

inline uint32_t B(uint32_t v, unsigned s, unsigned n) { return (v >> s) & ((1u << n) - 1); }
inline int32_t SB(uint32_t v, unsigned s, unsigned n) { return int32_t(v << (32 - s - n)) >> (32 - n); }

struct Cf {
    uint32_t d0 = 0, d1 = 0;  // d1 holds 16 bits
    uint32_t op() const { return (d1 >> 12) & 0xF; }
};

void UnpackCf(const uint32_t* d, Cf out[2]) {
    out[0].d0 = d[0];
    out[0].d1 = d[1] & 0xFFFF;
    out[1].d0 = (d[1] >> 16) | (d[2] << 16);
    out[1].d1 = d[2] >> 16;
}

enum CfOp : uint32_t {
    kCfNop = 0, kCfExec = 1, kCfExecEnd = 2, kCfCondExec = 3, kCfCondExecEnd = 4, kCfCondExecPred = 5,
    kCfCondExecPredEnd = 6, kCfLoopStart = 7, kCfLoopEnd = 8, kCfCondCall = 9, kCfReturn = 10,
    kCfCondJmp = 11, kCfAlloc = 12, kCfCondExecPredClean = 13, kCfCondExecPredCleanEnd = 14,
    kCfMarkVsFetchDone = 15,
};

bool IsExec(uint32_t op) { return (op >= 1 && op <= 6) || op == 13 || op == 14; }

const char* kVecNames[32] = {"add", "mul", "max", "min", "seq", "sgt", "sge", "sne", "frc", "trunc", "floor",
                             "mad", "cndeq", "cndge", "cndgt", "dp4", "dp3", "dp2add", "cube", "max4",
                             "setp_eq_push", "setp_ne_push", "setp_gt_push", "setp_ge_push", "kill_eq",
                             "kill_gt", "kill_ge", "kill_ne", "dst", "maxa", "op30", "op31"};
// Operands used by each vector opcode (0..3).
const uint8_t kVecOperands[32] = {2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 3, 3, 3, 3, 2,
                                  2, 3, 2, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0};
const char* kScalarNames[64] = {
    "adds", "adds_prev", "muls", "muls_prev", "muls_prev2", "maxs", "mins", "seqs", "sgts", "sges", "snes",
    "frcs", "truncs", "floors", "exp", "logc", "log", "rcpc", "rcpf", "rcp", "rsqc", "rsqf", "rsq", "maxas",
    "maxasf", "subs", "subs_prev", "setp_eq", "setp_ne", "setp_gt", "setp_ge", "setp_inv", "setp_pop",
    "setp_clr", "setp_rstr", "kills_eq", "kills_gt", "kills_ge", "kills_ne", "kills_one", "sqrt", "op41",
    "mulsc", "mulsc", "addsc", "addsc", "subsc", "subsc", "sin", "cos", "retain_prev"};
// 0 = no operand, 1 = src3.a, 2 = src3.ab, 3 = constant src3.a and temp .b (mulsc etc).
uint8_t ScalarOperandKind(uint32_t op) {
    switch (op) {
        case 0: case 2: case 4: case 5: case 6: case 23: case 24: case 25: return 2;
        case 33: case 41: case 50: return 0;
        case 42: case 43: case 44: case 45: case 46: case 47: return 3;
        default: return op <= 49 ? 1 : 0;
    }
}

const char kComp[] = "xyzw";

std::string Num(int v) { return std::to_string(v); }

std::string Mask(uint32_t m) {
    std::string s;
    for (int i = 0; i < 4; i++)
        if (m & (1u << i)) s += kComp[i];
    return s;
}

class Translator {
public:
    Translator(ShaderStage stage, const uint32_t* d, size_t n, TranslatedShader& out)
        : stage_(stage), u_(d), n_(n), out_(out) {}

    void Run();

private:
    bool Ps() const { return stage_ == ShaderStage::Pixel; }
    uint32_t U(size_t i) const { return i < n_ ? u_[i] : 0; }
    void Line(const std::string& s) {
        body_.append(size_t(indent_) * 2, ' ');
        body_ += s;
        body_ += '\n';
    }
    void Dis(const std::string& s) { out_.disasm += s + "\n"; }

    std::string BoolConst(uint32_t idx) const {
        char b[96];
        std::snprintf(b, sizeof b, "((xe_bl.b[%u][%u] & %uu) != 0u)", idx >> 7, (idx >> 5) & 3, 1u << (idx & 31));
        return b;
    }

    // ---- operands -------------------------------------------------------------------------
    std::string TempRef(uint32_t idx, bool rel) const {
        return rel ? "r[clamp(" + Num(int(idx)) + " + aL, 0, 63)]" : "r[" + Num(int(idx)) + "]";
    }
    std::string ConstRef(uint32_t idx, bool addressed, bool a0rel) const {
        int base = Ps() ? 256 : 0;
        if (!addressed) return "xe_c.c[" + Num(base + int(idx)) + "]";
        return "xe_c.c[clamp(" + Num(base + int(idx)) + " + " + (a0rel ? "a0" : "aL") + ", " + Num(base) + ", " +
               Num(base + 255) + ")]";
    }

    struct Alu {
        uint32_t w0, w1, w2;
        uint32_t vdst() const { return B(w0, 0, 6); }
        bool vdstRel() const { return B(w0, 6, 1); }
        bool absConst() const { return B(w0, 7, 1); }
        uint32_t sdst() const { return B(w0, 8, 6); }
        bool sdstRel() const { return B(w0, 14, 1); }
        bool exp() const { return B(w0, 15, 1); }
        uint32_t vmask() const { return B(w0, 16, 4); }
        uint32_t smask() const { return B(w0, 20, 4); }
        bool vclamp() const { return B(w0, 24, 1); }
        bool sclamp() const { return B(w0, 25, 1); }
        uint32_t sop() const { return B(w0, 26, 6); }
        uint32_t swz(int i) const { return i == 1 ? B(w1, 16, 8) : i == 2 ? B(w1, 8, 8) : B(w1, 0, 8); }
        bool neg(int i) const { return i == 1 ? B(w1, 26, 1) : i == 2 ? B(w1, 25, 1) : B(w1, 24, 1); }
        bool predCond() const { return B(w1, 27, 1); }
        bool pred() const { return B(w1, 28, 1); }
        bool constA0() const { return B(w1, 29, 1); }
        bool const1Rel() const { return B(w1, 30, 1); }
        bool const0Rel() const { return B(w1, 31, 1); }
        uint32_t reg(int i) const { return i == 1 ? B(w2, 16, 8) : i == 2 ? B(w2, 8, 8) : B(w2, 0, 8); }
        uint32_t vop() const { return B(w2, 24, 5); }
        bool isTemp(int i) const { return i == 1 ? B(w2, 31, 1) : i == 2 ? B(w2, 30, 1) : B(w2, 29, 1); }
        bool constAddressed(int i) const {
            if (i == 1) return const0Rel();
            if (i == 2) return isTemp(1) ? const0Rel() : const1Rel();
            return (isTemp(1) && isTemp(2)) ? const0Rel() : const1Rel();
        }
    };

    // Base register expression (no swizzle) plus the abs flag.
    std::string SrcBase(const Alu& a, int i, bool& absOut) const {
        uint32_t r = a.reg(i);
        if (a.isTemp(i)) {
            absOut = (r & 0x80) != 0;
            return TempRef(r & 0x3F, (r & 0x40) != 0);
        }
        absOut = a.absConst();
        return ConstRef(r, a.constAddressed(i), a.constA0());
    }
    static std::string Wrap(std::string e, bool abs, bool neg) {
        if (abs) e = "abs(" + e + ")";
        if (neg) e = "(-" + e + ")";
        return e;
    }
    std::string SrcVec(const Alu& a, int i) const {
        bool abs;
        std::string e = SrcBase(a, i, abs);
        uint32_t s = a.swz(i);
        std::string sw;
        for (int c = 0; c < 4; c++) sw += kComp[((s >> (2 * c)) + c) & 3];
        if (sw != "xyzw") e += "." + sw;
        return Wrap(e, abs, a.neg(i));
    }
    // Scalar operand component: slot 3 = `a` (W), 0 = `b` (X).
    std::string SrcScalar(const Alu& a, int i, int slot) const {
        bool abs;
        std::string e = SrcBase(a, i, abs);
        uint32_t s = a.swz(i);
        e += ".";
        e += kComp[((s >> (2 * slot)) + slot) & 3];
        return Wrap(e, abs, a.neg(i));
    }

    // ---- instructions -------------------------------------------------------------------
    void EmitAlu(const uint32_t* w);
    void EmitVfetch(const uint32_t* w);
    void EmitTfetch(const uint32_t* w);
    void EmitExecBody(uint32_t addr, uint32_t count, uint32_t seq);
    void WriteFetchResult(uint32_t dst, bool dstRel, uint32_t dstSwz, const std::string& value);

    ShaderStage stage_;
    const uint32_t* u_;
    size_t n_;
    TranslatedShader& out_;
    std::string body_;
    int indent_ = 1;
    uint32_t prevVfetch_[3] = {0, 0, 0};
    bool haveVfetch_ = false;
    // Vertex fetch extent analysis (see TranslatedShader::vfetchExtent).
    bool r0Written_ = false;
    uint32_t vfStride_[96] = {}, vfEnd_[96] = {};
    uint32_t vfUnbounded_[3] = {0, 0, 0};
    uint32_t vfWhy_[96] = {};
};

void Translator::WriteFetchResult(uint32_t dst, bool dstRel, uint32_t dstSwz, const std::string& value) {
    // value is a vec4 expression; destination swizzle picks XYZW / 0 / 1 / keep per component.
    std::string mask, src;
    for (int c = 0; c < 4; c++) {
        uint32_t s = (dstSwz >> (3 * c)) & 7;
        if (s == 7) continue;
        mask += kComp[c];
        if (!src.empty()) src += ", ";
        if (s < 4) src += std::string("xe_f.") + kComp[s];
        else src += s == 5 ? "1.0" : "0.0";
    }
    if (mask.empty()) return;
    if (dstRel || (dst == 0 && mask.find('x') != std::string::npos)) r0Written_ = true;  // only r0.x (the vertex index) matters
    Line("{");
    indent_++;
    Line("vec4 xe_f = " + value + ";");
    std::string lhs = TempRef(dst, dstRel);
    if (mask.size() == 1) Line(lhs + "." + mask + " = " + src + ";");
    else Line(lhs + "." + mask + " = vec" + Num(int(mask.size())) + "(" + src + ");");
    indent_--;
    Line("}");
}

void Translator::EmitVfetch(const uint32_t* w) {
    uint32_t w0 = w[0], w1 = w[1], w2 = w[2];
    bool mini = B(w1, 30, 1);
    const uint32_t* full = mini ? prevVfetch_ : w;
    if (!mini) {
        prevVfetch_[0] = w0; prevVfetch_[1] = w1; prevVfetch_[2] = w2;
        haveVfetch_ = true;
    }
    uint32_t src = B(full[0], 5, 6);
    bool srcRel = B(full[0], 11, 1);
    uint32_t srcComp = B(full[0], 30, 2);
    uint32_t slot = B(full[0], 20, 5) * 3 + B(full[0], 25, 2);
    uint32_t stride = B(full[2], 0, 8);
    bool rounded = B(full[1], 15, 1);

    uint32_t dst = B(w0, 12, 6);
    bool dstRel = B(w0, 18, 1);
    uint32_t dstSwz = B(w1, 0, 12);
    bool isSigned = B(w1, 12, 1);
    bool isInt = B(w1, 13, 1);
    uint32_t fmt = B(w1, 16, 6);
    int expAdj = SB(w1, 24, 6);
    int offset = SB(w2, 8, 23);
    bool pred = B(w1, 31, 1), predCond = B(w2, 31, 1);

    if (slot < 96) {
        out_.vfetchUsed[slot / 32] |= 1u << (slot % 32);
        // Extent is only known when the index is the vertex index in r0.x (r0 not written before this fetch) and the
        // stride is explicit and the offset non-negative.
        const uint32_t why = (src != 0 ? 1u : 0u) | (srcRel ? 2u : 0u) | (srcComp != 0 ? 4u : 0u) | (stride == 0 ? 8u : 0u) |
                             (offset < 0 ? 16u : 0u) | (r0Written_ ? 32u : 0u);
        if (why) vfUnbounded_[slot / 32] |= 1u << (slot % 32);
        vfWhy_[slot] |= why;
        vfStride_[slot] = std::max(vfStride_[slot], stride);
        vfEnd_[slot] = std::max<uint32_t>(vfEnd_[slot], uint32_t(std::max(offset, 0)) + 4);
    }
    char dis[160];
    std::snprintf(dis, sizeof dis, "  vfetch%s r%u, r%u.%c, vf%u fmt=%u stride=%u off=%d %s%s", mini ? "_mini" : "",
                  dst, src, kComp[srcComp], slot, fmt, stride, offset, isSigned ? "signed " : "", isInt ? "int" : "norm");
    Dis(dis);

    if (pred) { Line(std::string("if (p0 == ") + (predCond ? "true" : "false") + ") {"); indent_++; }
    Line("{");
    indent_++;
    std::string idx = TempRef(src, srcRel) + "." + kComp[srcComp];
    Line("int xe_vi = int(floor(" + idx + (rounded ? " + 0.5" : "") + "));");
    Line("uint xe_va = uint(xe_vi * " + Num(int(stride)) + " + " + Num(offset) + ");");
    auto word = [&](int k) { return "xe_vword(" + Num(int(slot)) + "u, xe_va + " + Num(k) + "u)"; };

    // Per-format decode into xe_vf (missing components: 0, 0, 1).
    std::string v;
    auto unorm = [](int bits) { return 1.0 / double((1ull << bits) - 1); };
    auto snorm = [](int bits) { return 1.0 / double((1ull << (bits - 1)) - 1); };
    auto packed = [&](const int* widths, const int* offs, int ncomp, int k = 0) {
        std::string wn = "xe_w" + Num(k);
        Line("uint " + wn + " = " + word(k) + ";");
        std::string comps[4] = {"0.0", "0.0", "0.0", "1.0"};
        for (int c = 0; c < ncomp; c++) {
            char b[200];
            if (isSigned) {
                if (isInt)
                    std::snprintf(b, sizeof b, "float(bitfieldExtract(int(%s), %d, %d))", wn.c_str(), offs[c], widths[c]);
                else
                    std::snprintf(b, sizeof b, "max(float(bitfieldExtract(int(%s), %d, %d)) * %.9g, -1.0)", wn.c_str(), offs[c],
                                  widths[c], snorm(widths[c]));
            } else {
                if (isInt)
                    std::snprintf(b, sizeof b, "float(bitfieldExtract(%s, %d, %d))", wn.c_str(), offs[c], widths[c]);
                else
                    std::snprintf(b, sizeof b, "float(bitfieldExtract(%s, %d, %d)) * %.9g", wn.c_str(), offs[c], widths[c],
                                  unorm(widths[c]));
            }
            comps[c] = b;
        }
        v = "vec4(" + comps[0] + ", " + comps[1] + ", " + comps[2] + ", " + comps[3] + ")";
    };
    bool isFloat = false;
    switch (fmt) {
        case 6: { int wd[4] = {8, 8, 8, 8}, of[4] = {0, 8, 16, 24}; packed(wd, of, 4); break; }
        case 7: { int wd[4] = {10, 10, 10, 2}, of[4] = {0, 10, 20, 30}; packed(wd, of, 4); break; }
        case 16: { int wd[3] = {11, 11, 10}, of[3] = {0, 11, 22}; packed(wd, of, 3); break; }
        case 17: { int wd[3] = {10, 11, 11}, of[3] = {0, 10, 21}; packed(wd, of, 3); break; }
        case 25: { int wd[2] = {16, 16}, of[2] = {0, 16}; packed(wd, of, 2); break; }
        case 26: {
            int wd[2] = {16, 16}, of[2] = {0, 16};
            packed(wd, of, 2);
            std::string lo = v;
            Line("vec4 xe_lo = " + lo + ";");
            packed(wd, of, 2, 1);
            Line("vec4 xe_hi = " + v + ";");
            v = "vec4(xe_lo.xy, xe_hi.xy)";
            break;
        }
        case 31: isFloat = true; v = "vec4(unpackHalf2x16(" + word(0) + "), 0.0, 1.0)"; break;
        case 32: isFloat = true; v = "vec4(unpackHalf2x16(" + word(0) + "), unpackHalf2x16(" + word(1) + "))"; break;
        case 36: isFloat = true; v = "vec4(uintBitsToFloat(" + word(0) + "), 0.0, 0.0, 1.0)"; break;
        case 37:
            isFloat = true;
            v = "vec4(uintBitsToFloat(" + word(0) + "), uintBitsToFloat(" + word(1) + "), 0.0, 1.0)";
            break;
        case 57:
            isFloat = true;
            v = "vec4(uintBitsToFloat(" + word(0) + "), uintBitsToFloat(" + word(1) + "), uintBitsToFloat(" + word(2) +
                "), 1.0)";
            break;
        case 38:
            isFloat = true;
            v = "uintBitsToFloat(uvec4(" + word(0) + ", " + word(1) + ", " + word(2) + ", " + word(3) + "))";
            break;
        case 33: case 34: case 35: {
            int nc = fmt == 33 ? 1 : fmt == 34 ? 2 : 4;
            std::string comps[4] = {"0.0", "0.0", "0.0", "1.0"};
            for (int c = 0; c < nc; c++) {
                std::string wv = word(c);
                if (isSigned) comps[c] = isInt ? "float(int(" + wv + "))" : "max(float(int(" + wv + ")) * 4.656612875e-10, -1.0)";
                else comps[c] = isInt ? "float(" + wv + ")" : "float(" + wv + ") * 2.328306437e-10";
            }
            v = "vec4(" + comps[0] + ", " + comps[1] + ", " + comps[2] + ", " + comps[3] + ")";
            break;
        }
        default:
            out_.error += "unsupported vertex format " + Num(int(fmt)) + "; ";
            v = "vec4(0.0, 0.0, 0.0, 1.0)";
            break;
    }
    if (expAdj && !isFloat) {
        char b[64];
        std::snprintf(b, sizeof b, " * vec4(%.9g)", double(expAdj > 0 ? (1u << expAdj) : 1.0 / double(1u << -expAdj)));
        v = "(" + v + ")" + b;
    }
    WriteFetchResult(dst, dstRel, dstSwz, v);
    indent_--;
    Line("}");
    if (pred) { indent_--; Line("}"); }
}

void Translator::EmitTfetch(const uint32_t* w) {
    uint32_t w0 = w[0], w1 = w[1], w2 = w[2];
    uint32_t op = B(w0, 0, 5);
    uint32_t src = B(w0, 5, 6);
    bool srcRel = B(w0, 11, 1);
    uint32_t dst = B(w0, 12, 6);
    bool dstRel = B(w0, 18, 1);
    uint32_t slot = B(w0, 20, 5);
    bool denorm = B(w0, 25, 1);
    uint32_t srcSwz = B(w0, 26, 6);
    uint32_t dstSwz = B(w1, 0, 12);
    bool pred = B(w1, 31, 1), predCond = B(w2, 31, 1);
    uint32_t dim = B(w2, 14, 2);
    int offX = SB(w2, 16, 5), offY = SB(w2, 21, 5);

    char dis[160];
    std::snprintf(dis, sizeof dis, "  tfetch op=%u dim=%u r%u, r%u, tf%u", op, dim, dst, src, slot);
    Dis(dis);
    if (op != 1) {
        // getTextureComputedLod / getWeights / gradients: no host equivalent we need yet.
        if (op >= 16 && op <= 19) {
            if (pred) { Line(std::string("if (p0 == ") + (predCond ? "true" : "false") + ") {"); indent_++; }
            WriteFetchResult(dst, dstRel, dstSwz, "vec4(0.0)");
            if (pred) { indent_--; Line("}"); }
        }
        return;
    }
    out_.texUsed |= 1u << slot;
    if (dim == kTexDim1D) dim = kTexDim2D;
    if (dim == kTexDim3D) dim = kTexDim2D;  // sampled as its first slice
    out_.texDim[slot] = uint8_t(dim);

    if (pred) { Line(std::string("if (p0 == ") + (predCond ? "true" : "false") + ") {"); indent_++; }
    Line("{");
    indent_++;
    std::string r = TempRef(src, srcRel);
    std::string c0 = r + "." + kComp[srcSwz & 3], c1 = r + "." + kComp[(srcSwz >> 2) & 3],
                c2 = r + "." + kComp[(srcSwz >> 4) & 3];
    std::string sampler = (dim == kTexDimCube ? "xe_tcube_" : "xe_t2d_") + Num(int(slot));
    if (dim == kTexDimCube) {
        // tfetchCube takes (sc / 2|ma| + 1.5, tc / 2|ma| + 1.5, face); rebuild the direction.
        Line("vec3 xe_tc = vec3(" + c0 + ", " + c1 + ", " + c2 + ");");
        Line("vec4 xe_tr = texture(" + sampler + ", xe_cube_dir(xe_tc));");
    } else {
        Line("vec2 xe_tc = vec2(" + c0 + ", " + (B(w2, 14, 2) == kTexDim1D ? std::string("0.5") : c1) + ");");
        if (denorm || offX || offY) Line("vec2 xe_ts = vec2(textureSize(" + sampler + ", 0));");
        if (denorm) Line("xe_tc /= xe_ts;");
        if (offX || offY) {
            char b[96];
            std::snprintf(b, sizeof b, "xe_tc += vec2(%g, %g) / xe_ts;", offX * 0.5, offY * 0.5);
            Line(b);
        }
        Line("vec4 xe_tr = textureLod(" + sampler + ", xe_tc, 0.0);");
    }
    WriteFetchResult(dst, dstRel, dstSwz, "xe_tr");
    indent_--;
    Line("}");
    if (pred) { indent_--; Line("}"); }
}

void Translator::EmitAlu(const uint32_t* w) {
    Alu a{w[0], w[1], w[2]};
    uint32_t vop = a.vop(), sop = a.sop();
    uint32_t vm = a.vmask(), sm = a.smask();
    bool exp = a.exp();
    {
        char dis[200];
        std::snprintf(dis, sizeof dis, "  alu%s %s %s%u.%s, %s %s%u.%s  [%08x %08x %08x]", a.pred() ? (a.predCond() ? " (p0)" : " (!p0)") : "",
                      kVecNames[vop], exp ? "o" : "r", a.vdst(), Mask(vm).c_str(), sop < 51 ? kScalarNames[sop] : "op?",
                      exp ? "o" : "r", exp ? a.vdst() : a.sdst(), Mask(sm).c_str(), w[0], w[1], w[2]);
        Dis(dis);
    }

    // Which results are actually written (exports merge both into vector_dest).
    uint32_t vecMask = exp ? (vm & ~sm) : vm;
    uint32_t scaMask = exp ? (sm & ~vm) : sm;
    uint32_t one = exp ? (vm & sm) : 0;
    uint32_t zero = (exp && a.sdstRel()) ? (0xFu & ~(vm | sm)) : 0;

    // Vector ops with side effects must run even with an empty write mask.
    bool vecSide = (vop >= 20 && vop <= 27) || vop == 29;
    bool needVec = vecMask || vecSide;
    // All scalar ops except retain_prev update ps, so they always run.
    bool needSca = true;

    if (a.pred()) { Line(std::string("if (p0 == ") + (a.predCond() ? "true" : "false") + ") {"); indent_++; }
    Line("{");
    indent_++;
    int nv = kVecOperands[vop];
    if (needVec) {
        for (int i = 1; i <= nv; i++) Line("vec4 xe_s" + Num(i) + " = " + SrcVec(a, i) + ";");
    }
    // Scalar operands.
    uint8_t sk = ScalarOperandKind(sop);
    if (needSca) {
        if (sk == 1) Line("float xe_a = " + SrcScalar(a, 3, 3) + ";");
        else if (sk == 2) {
            Line("float xe_a = " + SrcScalar(a, 3, 3) + ";");
            Line("float xe_b = " + SrcScalar(a, 3, 0) + ";");
        } else if (sk == 3) {
            uint32_t s3 = a.swz(3);
            // Constant `a` from src3, temp register `b` built from opcode bit, src3_sel and swizzle bits.
            std::string ce = ConstRef(a.reg(3), a.constAddressed(3), a.constA0()) + "." + kComp[((s3 >> 6) + 3) & 3];
            uint32_t treg = (sop & 1) | (B(a.w2, 29, 1) << 1) | (s3 & 0x3C);
            std::string te = TempRef(treg, false) + "." + kComp[s3 & 3];
            Line("float xe_a = " + Wrap(ce, a.absConst(), a.neg(3)) + ";");
            Line("float xe_b = " + Wrap(te, a.absConst(), a.neg(3)) + ";");
        }
    }

    // ---- vector op ----
    if (needVec) {
        std::string v;
        switch (vop) {
            case 0: v = "xe_s1 + xe_s2"; break;
            case 1: v = "xe_mul4(xe_s1, xe_s2)"; break;
            case 2: v = "xe_max4(xe_s1, xe_s2)"; break;
            case 3: v = "xe_min4(xe_s1, xe_s2)"; break;
            case 4: v = "vec4(equal(xe_s1, xe_s2))"; break;
            case 5: v = "vec4(greaterThan(xe_s1, xe_s2))"; break;
            case 6: v = "vec4(greaterThanEqual(xe_s1, xe_s2))"; break;
            case 7: v = "vec4(notEqual(xe_s1, xe_s2))"; break;
            case 8: v = "fract(xe_s1)"; break;
            case 9: v = "trunc(xe_s1)"; break;
            case 10: v = "floor(xe_s1)"; break;
            case 11: v = "xe_mul4(xe_s1, xe_s2) + xe_s3"; break;
            case 12: v = "mix(xe_s3, xe_s2, equal(xe_s1, vec4(0.0)))"; break;
            case 13: v = "mix(xe_s3, xe_s2, greaterThanEqual(xe_s1, vec4(0.0)))"; break;
            case 14: v = "mix(xe_s3, xe_s2, greaterThan(xe_s1, vec4(0.0)))"; break;
            case 15: v = "vec4(xe_dp4(xe_s1, xe_s2))"; break;
            case 16: v = "vec4(xe_dp4(vec4(xe_s1.xyz, 0.0), vec4(xe_s2.xyz, 0.0)))"; break;
            case 17: v = "vec4(xe_mul(xe_s1.x, xe_s2.x) + xe_mul(xe_s1.y, xe_s2.y) + xe_s3.x)"; break;
            case 18: v = "xe_cube(vec3(xe_s1.z, xe_s1.w, xe_s1.x))"; break;
            case 19: v = "vec4(xe_max4h(xe_s1))"; break;
            case 20: case 21: case 22: case 23: {
                const char* cmp = vop == 20 ? "==" : vop == 21 ? "!=" : vop == 22 ? ">" : ">=";
                Line(std::string("p0 = xe_s1.w == 0.0 && xe_s2.w ") + cmp + " 0.0;");
                v = std::string("(xe_s1.x == 0.0 && xe_s2.x ") + cmp + " 0.0) ? vec4(0.0) : vec4(xe_s1.x + 1.0)";
                break;
            }
            case 24: case 25: case 26: case 27: {
                const char* fn = vop == 24 ? "equal" : vop == 25 ? "greaterThan" : vop == 26 ? "greaterThanEqual" : "notEqual";
                Line(std::string("bool xe_k = any(") + fn + "(xe_s1, xe_s2));");
                if (Ps()) { Line("if (xe_k) xe_killed = true;"); out_.kills = true; }
                v = "xe_k ? vec4(1.0) : vec4(0.0)";
                break;
            }
            case 28: v = "vec4(1.0, xe_mul(xe_s1.y, xe_s2.y), xe_s1.z, xe_s2.w)"; break;
            case 29:
                Line("int xe_na0 = int(clamp(floor(xe_s1.w + 0.5), -256.0, 255.0));");
                v = "xe_max4(xe_s1, xe_s2)";
                break;
            default: v = "vec4(0.0)"; break;
        }
        Line("vec4 xe_v = " + v + ";");
        if (a.vclamp()) Line("xe_v = clamp(xe_v, 0.0, 1.0);");
    }

    // ---- scalar op ----
    {
        std::string s;
        const char* FLT_MAX = "3.402823466e+38";
        switch (sop) {
            case 0: s = "xe_a + xe_b"; break;
            case 1: s = "xe_a + ps"; break;
            case 2: s = "xe_mul(xe_a, xe_b)"; break;
            case 3: s = "xe_mul(xe_a, ps)"; break;
            case 4:
                s = std::string("(ps == -") + FLT_MAX + " || isinf(ps) || isnan(ps) || isinf(xe_b) || isnan(xe_b) || xe_b <= 0.0) ? -" +
                    FLT_MAX + " : xe_mul(xe_a, ps)";
                break;
            case 5: s = "xe_a >= xe_b ? xe_a : xe_b"; break;
            case 6: s = "xe_a < xe_b ? xe_a : xe_b"; break;
            case 7: s = "xe_a == 0.0 ? 1.0 : 0.0"; break;
            case 8: s = "xe_a > 0.0 ? 1.0 : 0.0"; break;
            case 9: s = "xe_a >= 0.0 ? 1.0 : 0.0"; break;
            case 10: s = "xe_a != 0.0 ? 1.0 : 0.0"; break;
            case 11: s = "fract(xe_a)"; break;
            case 12: s = "trunc(xe_a)"; break;
            case 13: s = "floor(xe_a)"; break;
            case 14: s = "exp2(xe_a)"; break;
            case 15: s = std::string("xe_clampneg(xe_a == 1.0 ? 0.0 : log2(xe_a))"); break;
            case 16: s = "xe_a == 1.0 ? 0.0 : log2(xe_a)"; break;
            case 17: s = "xe_clampinf(xe_a == 1.0 ? 1.0 : 1.0 / xe_a)"; break;
            case 18: s = "xe_zeroinf(xe_a == 1.0 ? 1.0 : 1.0 / xe_a)"; break;
            case 19: s = "xe_a == 1.0 ? 1.0 : 1.0 / xe_a"; break;
            case 20: s = "xe_clampinf(xe_a == 1.0 ? 1.0 : inversesqrt(xe_a))"; break;
            case 21: s = "xe_zeroinf(xe_a == 1.0 ? 1.0 : inversesqrt(xe_a))"; break;
            case 22: s = "xe_a == 1.0 ? 1.0 : inversesqrt(xe_a)"; break;
            case 23:
                Line("a0 = int(clamp(floor(xe_a + 0.5), -256.0, 255.0));");
                s = "xe_a >= xe_b ? xe_a : xe_b";
                break;
            case 24:
                Line("a0 = int(clamp(floor(xe_a), -256.0, 255.0));");
                s = "xe_a >= xe_b ? xe_a : xe_b";
                break;
            case 25: s = "xe_a - xe_b"; break;
            case 26: s = "xe_a - ps"; break;
            case 27: Line("p0 = xe_a == 0.0;"); s = "p0 ? 0.0 : 1.0"; break;
            case 28: Line("p0 = xe_a != 0.0;"); s = "p0 ? 0.0 : 1.0"; break;
            case 29: Line("p0 = xe_a > 0.0;"); s = "p0 ? 0.0 : 1.0"; break;
            case 30: Line("p0 = xe_a >= 0.0;"); s = "p0 ? 0.0 : 1.0"; break;
            case 31: Line("p0 = xe_a == 1.0;"); s = "p0 ? 0.0 : (xe_a == 0.0 ? 1.0 : xe_a)"; break;
            case 32: Line("p0 = xe_a - 1.0 <= 0.0;"); s = "p0 ? 0.0 : xe_a - 1.0"; break;
            case 33: Line("p0 = false;"); s = FLT_MAX; break;
            case 34: Line("p0 = xe_a == 0.0;"); s = "p0 ? 0.0 : xe_a"; break;
            case 35: case 36: case 37: case 38: case 39: {
                const char* c = sop == 35 ? "xe_a == 0.0" : sop == 36 ? "xe_a > 0.0" : sop == 37 ? "xe_a >= 0.0"
                              : sop == 38 ? "xe_a != 0.0" : "xe_a == 1.0";
                Line(std::string("bool xe_sk = ") + c + ";");
                if (Ps()) { Line("if (xe_sk) xe_killed = true;"); out_.kills = true; }
                s = "xe_sk ? 1.0 : 0.0";
                break;
            }
            case 40: s = "sqrt(xe_a)"; break;
            case 42: case 43: s = "xe_mul(xe_a, xe_b)"; break;
            case 44: case 45: s = "xe_a + xe_b"; break;
            case 46: case 47: s = "xe_a - xe_b"; break;
            case 48: s = "sin(xe_a)"; break;
            case 49: s = "cos(xe_a)"; break;
            default: s = "ps"; break;  // retain_prev and unknown opcodes
        }
        Line("float xe_r = " + s + ";");
        if (a.sclamp()) Line("xe_r = clamp(xe_r, 0.0, 1.0);");
        Line("ps = xe_r;");
    }
    if (vop == 29 && needVec) Line("a0 = xe_na0;");

    // ---- writes ----
    auto assign = [&](const std::string& lhs, uint32_t mask, const std::string& vec4expr) {
        if (!mask) return;
        std::string m = Mask(mask);
        if (m == "xyzw") Line(lhs + " = " + vec4expr + ";");
        else Line(lhs + "." + m + " = (" + vec4expr + ")." + m + ";");
    };
    if (!exp) {
        if (vecMask) { if (a.vdstRel() || (a.vdst() == 0 && (vecMask & 1))) r0Written_ = true; assign(TempRef(a.vdst(), a.vdstRel()), vecMask, "xe_v"); }
        if (scaMask) { if (a.sdstRel() || (a.sdst() == 0 && (scaMask & 1))) r0Written_ = true; assign(TempRef(a.sdst(), a.sdstRel()), scaMask, "vec4(xe_r)"); }
    } else {
        uint32_t d = a.vdst();
        std::string lhs;
        if (!Ps()) {
            if (d < 16) lhs = "xe_o[" + Num(int(d)) + "]";
            else if (d == 62) lhs = "xe_pos";
            else if (d == 63) { lhs = "xe_misc"; out_.writesPointSize = true; }
        } else {
            if (d < 4) { lhs = "xe_oc[" + Num(int(d)) + "]"; out_.colorWritten |= 1u << d; }
            else if (d == 61) { lhs = "xe_od"; out_.writesDepth = true; }
        }
        if (lhs.empty()) {
            Line("// export to register " + Num(int(d)) + " ignored (memory export)");
        } else {
            if (vecMask) assign(lhs, vecMask, "xe_v");
            if (scaMask) assign(lhs, scaMask, "vec4(xe_r)");
            if (one) assign(lhs, one, "vec4(1.0)");
            if (zero) assign(lhs, zero, "vec4(0.0)");
        }
    }
    indent_--;
    Line("}");
    if (a.pred()) { indent_--; Line("}"); }
}

void Translator::EmitExecBody(uint32_t addr, uint32_t count, uint32_t seq) {
    for (uint32_t i = 0; i < count; i++, seq >>= 2) {
        size_t at = size_t(addr + i) * 3;
        if (at + 3 > n_) {
            out_.error += "exec past end of microcode; ";
            return;
        }
        const uint32_t* w = u_ + at;
        out_.instructionCount++;
        if (seq & 1) {
            if ((w[0] & 31) == 0) EmitVfetch(w);
            else EmitTfetch(w);
        } else {
            EmitAlu(w);
        }
    }
}

const char* kCommon = R"(#version 450
layout(std140, set = 0, binding = 0) uniform XeConsts { vec4 c[512]; } xe_c;
layout(std140, set = 0, binding = 1) uniform XeBoolLoop { uvec4 b[2]; uvec4 l[8]; } xe_bl;
layout(std140, set = 0, binding = 2) uniform XeSystem {
  vec4 ndc_scale; vec4 ndc_offset; vec4 vport_scale; vec4 vport_offset;
  uvec4 flags; vec4 misc; uvec4 vf[96];
} xe_s;
layout(std430, set = 0, binding = 3) readonly buffer XeVertexWords { uint w[]; } xe_vb;

vec4 r[64];
float ps = 0.0;
bool p0 = false;
int a0 = 0;
int aL = 0;

float xe_mul(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
vec4 xe_mul4(vec4 a, vec4 b) {
  bvec4 z = bvec4(uvec4(equal(a, vec4(0.0))) | uvec4(equal(b, vec4(0.0))));
  return mix(a * b, vec4(0.0), z);
}
float xe_dp4(vec4 a, vec4 b) { vec4 m = xe_mul4(a, b); return m.x + m.y + m.z + m.w; }
vec4 xe_max4(vec4 a, vec4 b) { return mix(b, a, greaterThanEqual(a, b)); }
vec4 xe_min4(vec4 a, vec4 b) { return mix(b, a, lessThan(a, b)); }
float xe_max4h(vec4 v) {
  if (v.x > v.y && v.x > v.z && v.x > v.w) return v.x;
  if (v.y > v.z && v.y > v.w) return v.y;
  return v.z > v.w ? v.z : v.w;
}
float xe_clampinf(float v) { return isinf(v) ? (v < 0.0 ? -3.402823466e+38 : 3.402823466e+38) : v; }
float xe_zeroinf(float v) { return isinf(v) ? (v < 0.0 ? -0.0 : 0.0) : v; }
float xe_clampneg(float v) { return (isinf(v) && v < 0.0) ? -3.402823466e+38 : v; }
vec4 xe_cube(vec3 d) {
  vec3 a = abs(d);
  if (a.z >= a.x && a.z >= a.y) return vec4(-d.y, d.z < 0.0 ? -d.x : d.x, 2.0 * d.z, d.z < 0.0 ? 5.0 : 4.0);
  if (a.y >= a.x) return vec4(d.y < 0.0 ? -d.z : d.z, d.x, 2.0 * d.y, d.y < 0.0 ? 3.0 : 2.0);
  return vec4(-d.y, d.x < 0.0 ? d.z : -d.z, 2.0 * d.x, d.x < 0.0 ? 1.0 : 0.0);
}
vec3 xe_cube_dir(vec3 t) {
  float u = (t.x - 1.5) * 2.0, v = (t.y - 1.5) * 2.0;
  int f = int(t.z + 0.5);
  if (f == 0) return vec3(1.0, -v, -u);
  if (f == 1) return vec3(-1.0, -v, u);
  if (f == 2) return vec3(u, 1.0, v);
  if (f == 3) return vec3(u, -1.0, -v);
  if (f == 4) return vec3(u, -v, 1.0);
  return vec3(-u, -v, -1.0);
}
uint xe_swap(uint v, uint e) {
  if (e == 1u || e == 2u) v = ((v & 0x00FF00FFu) << 8u) | ((v >> 8u) & 0x00FF00FFu);
  if (e == 2u || e == 3u) v = (v << 16u) | (v >> 16u);
  return v;
}
uint xe_vword(uint slot, uint i) {
  uvec4 f = xe_s.vf[slot];
  if (i >= f.z) return 0u;
  return xe_swap(xe_vb.w[f.x + i], f.y);
}
)";

}  // namespace

void Translator::Run() {
    out_.stage = stage_;
    out_.hash = HashUcode(u_, n_);
    if (n_ < 3) {
        out_.error = "empty shader";
        return;
    }
    // Control flow: pairs until the lowest exec address.
    uint32_t pairBound = uint32_t(n_ / 3);
    for (uint32_t i = 0; i < pairBound; i++) {
        Cf cf[2];
        UnpackCf(u_ + i * 3, cf);
        for (auto& c : cf)
            if (IsExec(c.op())) pairBound = std::min(pairBound, B(c.d0, 0, 12));
    }
    std::vector<Cf> cfs;
    for (uint32_t i = 0; i < pairBound; i++) {
        Cf cf[2];
        UnpackCf(u_ + i * 3, cf);
        cfs.push_back(cf[0]);
        cfs.push_back(cf[1]);
    }
    bool complex = false;
    for (auto& c : cfs) {
        uint32_t op = c.op();
        if (op == kCfLoopStart || op == kCfLoopEnd || op == kCfCondCall || op == kCfReturn || op == kCfCondJmp)
            complex = true;
    }

    if (complex) {
        Line("int pc = 0;");
        Line("int xe_guard = 0;");
        Line("int xe_lcnt[4]; int xe_lal[4]; int xe_ld = 0;");
        Line("int xe_cs[4]; int xe_cd = 0;");
        Line("while (true) {");
        indent_++;
        Line("if (++xe_guard > 65536) return;");
        Line("switch (pc) {");
        indent_++;
    }
    bool ended = false;
    for (uint32_t ci = 0; ci < cfs.size(); ci++) {
        const Cf& c = cfs[ci];
        uint32_t op = c.op();
        if (complex) {
            indent_--;
            Line("case " + Num(int(ci)) + ":");
            indent_++;
        }
        char dis[128];
        switch (op) {
            case kCfExec: case kCfExecEnd: case kCfCondExec: case kCfCondExecEnd: case kCfCondExecPred:
            case kCfCondExecPredEnd: case kCfCondExecPredClean: case kCfCondExecPredCleanEnd: {
                uint32_t addr = B(c.d0, 0, 12), count = B(c.d0, 12, 3), seq = B(c.d0, 16, 12);
                bool end = op == kCfExecEnd || op == kCfCondExecEnd || op == kCfCondExecPredEnd || op == kCfCondExecPredCleanEnd;
                std::string cond;
                if (op == kCfCondExec || op == kCfCondExecEnd || op == kCfCondExecPredClean || op == kCfCondExecPredCleanEnd) {
                    uint32_t b = B(c.d1, 2, 8);
                    bool want = B(c.d1, 10, 1);
                    cond = (want ? "" : "!") + BoolConst(b);
                    std::snprintf(dis, sizeof dis, "cf %u: %s b%u==%d addr=%u count=%u seq=%03x", ci, end ? "cexece" : "cexec", b, want, addr, count, seq);
                } else if (op == kCfCondExecPred || op == kCfCondExecPredEnd) {
                    bool want = B(c.d1, 10, 1);
                    cond = want ? "p0" : "!p0";
                    std::snprintf(dis, sizeof dis, "cf %u: %s (%sp0) addr=%u count=%u seq=%03x", ci, end ? "exece" : "exec", want ? "" : "!", addr, count, seq);
                } else {
                    std::snprintf(dis, sizeof dis, "cf %u: %s addr=%u count=%u seq=%03x", ci, end ? "exece" : "exec", addr, count, seq);
                }
                Dis(dis);
                if (!cond.empty()) { Line("if (" + cond + ") {"); indent_++; }
                EmitExecBody(addr, count, seq);
                if (end) Line("return;");
                if (!cond.empty()) { indent_--; Line("}"); }
                if (end && cond.empty() && !complex) ended = true;
                break;
            }
            case kCfLoopStart: {
                uint32_t skip = B(c.d0, 0, 13), id = B(c.d0, 16, 5);
                bool repeat = B(c.d0, 13, 1);
                std::snprintf(dis, sizeof dis, "cf %u: loop i%u skip=%u%s", ci, id, skip, repeat ? " repeat" : "");
                Dis(dis);
                Line("{");
                indent_++;
                Line("uint xe_lc = xe_bl.l[" + Num(int(id >> 2)) + "][" + Num(int(id & 3)) + "];");
                Line("int xe_d = min(xe_ld, 3);");
                Line("xe_lcnt[xe_d] = int(xe_lc & 255u); xe_lal[xe_d] = aL; xe_ld = xe_d + 1;");
                if (!repeat) Line("aL = int((xe_lc >> 8u) & 255u);");
                Line("if ((xe_lc & 255u) == 0u) { xe_ld = xe_d; aL = xe_lal[xe_d]; pc = " + Num(int(skip)) + "; break; }");
                indent_--;
                Line("}");
                break;
            }
            case kCfLoopEnd: {
                uint32_t body = B(c.d0, 0, 13), id = B(c.d0, 16, 5);
                bool pbreak = B(c.d0, 21, 1), want = B(c.d1, 10, 1);
                std::snprintf(dis, sizeof dis, "cf %u: endloop i%u body=%u%s", ci, id, body, pbreak ? " predbreak" : "");
                Dis(dis);
                Line("{");
                indent_++;
                Line("int xe_d = max(xe_ld - 1, 0);");
                Line("xe_lcnt[xe_d] -= 1;");
                std::string contCond = "xe_lcnt[xe_d] != 0";
                if (pbreak) contCond += std::string(" && p0 != ") + (want ? "true" : "false");
                Line("if (" + contCond + ") {");
                indent_++;
                Line("uint xe_lc = xe_bl.l[" + Num(int(id >> 2)) + "][" + Num(int(id & 3)) + "];");
                Line("aL += bitfieldExtract(int(xe_lc), 16, 8);");
                Line("pc = " + Num(int(body)) + "; break;");
                indent_--;
                Line("}");
                Line("xe_ld = xe_d; aL = xe_lal[xe_d];");
                indent_--;
                Line("}");
                break;
            }
            case kCfCondCall: case kCfCondJmp: {
                uint32_t target = B(c.d0, 0, 13);
                bool uncond = B(c.d0, 13, 1), predicated = B(c.d0, 14, 1);
                bool want = B(c.d1, 10, 1);
                uint32_t b = B(c.d1, 2, 8);
                std::string cond = uncond ? "true" : predicated ? (want ? "p0" : "!p0") : (want ? "" : "!") + BoolConst(b);
                std::snprintf(dis, sizeof dis, "cf %u: %s %u", ci, op == kCfCondCall ? "call" : "jmp", target);
                Dis(dis);
                Line("if (" + cond + ") {");
                indent_++;
                if (op == kCfCondCall) Line("xe_cs[min(xe_cd, 3)] = " + Num(int(ci + 1)) + "; xe_cd = min(xe_cd + 1, 4);");
                Line("pc = " + Num(int(target)) + "; break;");
                indent_--;
                Line("}");
                break;
            }
            case kCfReturn:
                std::snprintf(dis, sizeof dis, "cf %u: ret", ci);
                Dis(dis);
                Line("if (xe_cd > 0) { xe_cd -= 1; pc = xe_cs[xe_cd]; break; }");
                break;
            default:
                break;  // nop, alloc, mark_vs_fetch_done
        }
        if (ended) break;
    }
    if (complex) {
        indent_--;
        Line("default:");
        Line("  return;");
        indent_--;
        Line("}");  // switch
        indent_--;
        Line("}");  // while
    }

    // ---- assemble the whole shader ----
    std::string g = kCommon;
    for (uint32_t s = 0; s < 32; s++) {
        if (!(out_.texUsed & (1u << s))) continue;
        if (out_.texDim[s] == kTexDimCube)
            g += "layout(set = 1, binding = " + Num(int(32 + s)) + ") uniform samplerCube xe_tcube_" + Num(int(s)) + ";\n";
        else
            g += "layout(set = 1, binding = " + Num(int(s)) + ") uniform sampler2D xe_t2d_" + Num(int(s)) + ";\n";
    }
    if (!Ps()) {
        // One variable per location: some drivers (lavapipe) mis-link arrays of interpolators.
        for (int i = 0; i < 16; i++) g += "layout(location = " + Num(i) + ") out vec4 xe_interp" + Num(i) + ";\n";
        g += "vec4 xe_o[16];\nvec4 xe_pos = vec4(0.0, 0.0, 0.0, 1.0);\nvec4 xe_misc = vec4(-1.0, 0.0, 0.0, 0.0);\n";
    } else {
        for (int i = 0; i < 16; i++) g += "layout(location = " + Num(i) + ") in vec4 xe_interp" + Num(i) + ";\n";
        for (int i = 0; i < 4; i++) g += "layout(location = " + Num(i) + ") out vec4 xe_out" + Num(i) + ";\n";
        g += "vec4 xe_oc[4];\nvec4 xe_od = vec4(0.0);\nbool xe_killed = false;\n";
    }
    g += "\nvoid xe_body() {\n" + body_ + "}\n\nvoid main() {\n";
    g += "  for (int i = 0; i < 64; i++) r[i] = vec4(0.0);\n";
    if (!Ps()) {
        g += "  for (int i = 0; i < 16; i++) xe_o[i] = vec4(0.0);\n";
        g += "  r[0].x = float(gl_VertexIndex);\n";
        g += "  xe_body();\n";
        for (int i = 0; i < 16; i++) g += "  xe_interp" + Num(i) + " = xe_o[" + Num(i) + "];\n";
        g += R"(  vec4 p = xe_pos;
  // PA_CL_VTE_CNTL.VTX_W0_FMT set: the shader wrote W itself (the usual D3D case); clear: it
  // wrote 1/W. Either way w below is the real clip W.
  float w = (xe_s.flags.x & 4u) != 0u ? p.w : 1.0 / p.w;
  vec3 n = p.xyz;
  if ((xe_s.flags.x & 1u) == 0u) n.xy /= w;
  if ((xe_s.flags.x & 2u) == 0u) n.z /= w;
  vec3 scr = n * xe_s.vport_scale.xyz + xe_s.vport_offset.xyz;
  vec3 ndc = scr * xe_s.ndc_scale.xyz + xe_s.ndc_offset.xyz;
  gl_Position = vec4(ndc * w, w);
  gl_PointSize = xe_misc.x > 0.0 ? xe_misc.x : xe_s.misc.y;
}
)";
    } else {
        g += "  for (int i = 0; i < 4; i++) xe_oc[i] = vec4(0.0);\n";
        for (int i = 0; i < 16; i++) g += "  if (" + Num(i) + "u < xe_s.flags.w) r[" + Num(i) + "] = xe_interp" + Num(i) + ";\n";
        g += R"(  if (xe_s.flags.z < 64u) {
    vec2 fc = floor(gl_FragCoord.xy);
    r[xe_s.flags.z] = vec4(gl_FrontFacing ? fc.x : -fc.x, fc.y, 0.0, 0.0);
  }
  xe_body();
  if (xe_killed) discard;
  uint af = xe_s.flags.y;
  if (af != 7u) {
    float al = xe_oc[0].w, ar = xe_s.misc.x;
    bool pass = ((af & 1u) != 0u && al < ar) || ((af & 2u) != 0u && al == ar) || ((af & 4u) != 0u && al > ar);
    if (!pass) discard;
  }
  xe_out0 = xe_oc[0];
  xe_out1 = xe_oc[1];
  xe_out2 = xe_oc[2];
  xe_out3 = xe_oc[3];
)";
        if (out_.writesDepth) g += "  gl_FragDepth = clamp(xe_od.x, 0.0, 1.0);\n";
        g += "}\n";
    }
    out_.glsl = std::move(g);
    for (uint32_t i = 0; i < 96; i++) {
        if (!(out_.vfetchUsed[i / 32] & (1u << (i % 32)))) continue;
        bool bounded = !(vfUnbounded_[i / 32] & (1u << (i % 32)));
        out_.vfetchStride[i] = bounded ? vfStride_[i] : 0;  // 0 = extent unknown, upload the whole buffer
        out_.vfetchEnd[i] = vfEnd_[i];
        out_.vfetchWhy[i] = vfWhy_[i];
    }
    out_.ok = true;
}

TranslatedShader TranslateShader(ShaderStage stage, const uint32_t* dwords, size_t count) {
    TranslatedShader out;
    Translator t(stage, dwords, count, out);
    t.Run();
    return out;
}

}  // namespace fa::gpu
