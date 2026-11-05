// Translates hand-assembled Xenos shaders (every ALU opcode, fetches, loops, jumps, predication)
// and checks the GLSL compiles to SPIR-V with shaderc.
#include <cstdio>
#include <string>

#include <shaderc/shaderc.h>

#include "fa/gpu/shader.hpp"
#include "xenos_asm.hpp"

using namespace fa::gpu;
using namespace xasm;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static shaderc_compiler_t g_compiler;

static bool Compile(const TranslatedShader& t, const char* what) {
    if (!t.ok) {
        std::printf("FAIL %s: translate: %s\n", what, t.error.c_str());
        return false;
    }
    shaderc_compile_options_t opt = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(opt, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    shaderc_compilation_result_t r = shaderc_compile_into_spv(
        g_compiler, t.glsl.data(), t.glsl.size(),
        t.stage == ShaderStage::Vertex ? shaderc_vertex_shader : shaderc_fragment_shader, what, "main", opt);
    bool ok = shaderc_result_get_compilation_status(r) == shaderc_compilation_status_success;
    if (!ok) {
        std::printf("FAIL %s: %s\n----\n%s\n----\n", what, shaderc_result_get_error_message(r), t.glsl.c_str());
    }
    shaderc_result_release(r);
    shaderc_compile_options_release(opt);
    return ok;
}

int main() {
    g_compiler = shaderc_compiler_initialize();

    {
        auto vs = ColorVS();
        TranslatedShader t = TranslateShader(ShaderStage::Vertex, vs.data(), vs.size());
        CHECK(Compile(t, "color vs"));
        CHECK(t.vfetchUsed[0] == 1);
        CHECK(t.instructionCount == 4);
    }
    {
        auto ps = ColorPS();
        TranslatedShader t = TranslateShader(ShaderStage::Pixel, ps.data(), ps.size());
        CHECK(Compile(t, "color ps"));
        CHECK(t.colorWritten == 1);
        CHECK(t.glsl.find(ConstBlocksOn() ? "xe_cv(256)" : "xe_c.c[256]") != std::string::npos);  // pixel constants start at 256
    }
    {
        auto ps = TexturePS();
        TranslatedShader t = TranslateShader(ShaderStage::Pixel, ps.data(), ps.size());
        CHECK(Compile(t, "texture ps"));
        CHECK(t.texUsed == 1 && t.texDim[0] == kTexDim2D);
    }

    // Every vector opcode (with a co-issued scalar op), in both stages.
    for (int stage = 0; stage < 2; stage++) {
        for (uint32_t vop = 0; vop < 30; vop++) {
            std::vector<uint32_t> ins;
            Alu a;
            a.vop = vop;
            a.sop = vop;  // walk the scalar opcodes as well
            a.vdst = 3; a.vmask = 0xF; a.sdst = 4; a.smask = 0x3;
            a.s1 = R(1, AluSwz(0, 0, 1, 2)); a.s2 = C(7); a.s3 = R(2, AluSwz(3, 2, 1, 0)); a.s3.neg = true;
            Emit(ins, a);
            Alu e; e.exp = true; e.vdst = stage ? 0 : 62; e.vmask = 0xF; e.s1 = R(3); e.s2 = R(3);
            Emit(ins, e);
            auto code = Build({Exec(1, 2, 0, true)}, ins);
            char what[64];
            std::snprintf(what, sizeof what, "%s vop %u", stage ? "ps" : "vs", vop);
            CHECK(Compile(TranslateShader(ShaderStage(stage), code.data(), code.size()), what));
        }
        for (uint32_t sop = 0; sop < 51; sop++) {
            std::vector<uint32_t> ins;
            Alu a;
            a.vop = 0;
            a.sop = sop;
            a.vdst = 3; a.vmask = 0x1; a.sdst = 5; a.smask = 0xF;
            a.s1 = R(1); a.s2 = R(1); a.s3 = C(9, AluSwz(2, 2, 2, 1));
            Emit(ins, a);
            // Export the scalar result through an export with scalar mask (oPos / oC0).
            Alu e; e.exp = true; e.vdst = stage ? 0 : 62; e.vmask = 0x3; e.smask = 0xC; e.sop = 0;
            e.s1 = R(5); e.s2 = R(5); e.s3 = R(5);
            Emit(ins, e);
            auto code = Build({Exec(1, 2, 0, true)}, ins);
            char what[64];
            std::snprintf(what, sizeof what, "%s sop %u", stage ? "ps" : "vs", sop);
            CHECK(Compile(TranslateShader(ShaderStage(stage), code.data(), code.size()), what));
        }
    }

    // Control flow: cexec on a bool, a loop with relative addressing, a jump, predicated ALU.
    {
        std::vector<uint32_t> ins;
        Alu a; a.vop = 0; a.vdst = 1; a.vmask = 0xF; a.s1 = R(1); a.s2 = C(1);
        Emit(ins, a);  // instr 0 (loop body): r1 += c1
        Alu p; p.sop = 29; p.s3 = R(1); p.sdst = 6; p.smask = 1;  // setp_gt
        Emit(ins, p);  // instr 1
        Alu q; q.vop = 1; q.vdst = 2; q.vmask = 0xF; q.s1 = R(1); q.s2 = R(1);
        Emit(ins, q);
        ins[ins.size() - 2] |= (1u << 28) | (1u << 27);  // predicated on p0
        Alu rel; rel.vop = 0; rel.vdst = 3; rel.vmask = 0xF; rel.s1 = R(1); rel.s2 = C(4);
        Emit(ins, rel);
        ins[ins.size() - 2] |= 1u << 31;  // const_0_rel_abs: c[4 + aL]
        Alu e; e.exp = true; e.vdst = 62; e.vmask = 0xF; e.s1 = R(2); e.s2 = R(2);
        Emit(ins, e);
        uint32_t base = 3;  // 6 CF instructions = 3 pairs
        std::vector<Cf> cf = {
            CondExec(base + 1, 1, 0, false, 5, true),  // 0: cexec b5: setp
            LoopStart(4, 2),                            // 1: loop i2, skip to 4
            Exec(base + 0, 1, 0, false),                // 2: body
            LoopEnd(2, 2),                              // 3: endloop -> 2
            Jmp(5),                                     // 4: jmp 5
            Exec(base + 2, 3, 0, true),                 // 5: predicated mul, rel add, export; end
        };
        auto code = Build(cf, ins);
        TranslatedShader t = TranslateShader(ShaderStage::Vertex, code.data(), code.size());
        CHECK(Compile(t, "control flow vs"));
        CHECK(t.glsl.find("switch (pc)") != std::string::npos);
    }

    // Every vertex fetch format.
    {
        const uint32_t fmts[] = {6, 7, 16, 17, 25, 26, 31, 32, 33, 34, 35, 36, 37, 38, 57};
        for (uint32_t f : fmts) {
            for (int variant = 0; variant < 3; variant++) {
                std::vector<uint32_t> ins;
                EmitVfetch(ins, 1, 0, 0, 5, f, 4, 1, FetchSwz(0, 1, 2, 3), false, variant == 1, variant == 2);
                Alu e; e.exp = true; e.vdst = 62; e.vmask = 0xF; e.s1 = R(1); e.s2 = R(1);
                Emit(ins, e);
                auto code = Build({Exec(1, 2, 0b01, true)}, ins);
                char what[64];
                std::snprintf(what, sizeof what, "vfetch fmt %u variant %d", f, variant);
                TranslatedShader t = TranslateShader(ShaderStage::Vertex, code.data(), code.size());
                CHECK(Compile(t, what));
                CHECK(t.vfetchUsed[0] == (1u << 5));
            }
        }
    }

    shaderc_compiler_release(g_compiler);
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("gpu_shader_test: all passed\n");
    return 0;
}
