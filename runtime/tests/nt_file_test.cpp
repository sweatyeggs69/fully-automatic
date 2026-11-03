// Tests the NT file imports (NtCreateFile, NtReadFile, ...) over a host-folder game and a
// writable data folder, driven by a hand-written guest function.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "fa/hle/services.hpp"
#include "fa/rt/hle_hooks.hpp"
#include "fa/rt/kernel.hpp"
#include "fa/rt/loader.hpp"
#include "fa/rt/memory.hpp"

using namespace fa::rt;

PPC_EXTERN_FUNC(NtCreateFile);
PPC_EXTERN_FUNC(NtOpenFile);
PPC_EXTERN_FUNC(NtReadFile);
PPC_EXTERN_FUNC(NtWriteFile);
PPC_EXTERN_FUNC(NtClose);
PPC_EXTERN_FUNC(NtQueryInformationFile);
PPC_EXTERN_FUNC(NtSetInformationFile);
PPC_EXTERN_FUNC(NtQueryDirectoryFile);
PPC_EXTERN_FUNC(NtQueryFullAttributesFile);

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

template <class... A>
static uint32_t K(PPCContext& ctx, PPCFunc* f, A... a) {
    uint32_t v[] = {uint32_t(a)..., 0};
    PPCRegister* r[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
    for (size_t i = 0; i < sizeof...(a); ++i) r[i]->u64 = v[i];
    f(ctx, g_base);
    return ctx.r3.u32;
}

// Builds ANSI_STRING + OBJECT_ATTRIBUTES for a path at `at`; returns the OBJECT_ATTRIBUTES address.
static uint32_t make_oa(uint32_t at, const char* path) {
    uint32_t chars = at + 0x20, ansi = at + 0x10;
    std::memcpy(host(chars), path, std::strlen(path) + 1);
    st16(ansi, uint16_t(std::strlen(path)));
    st16(ansi + 2, uint16_t(std::strlen(path) + 1));
    st32(ansi + 4, chars);
    st32(at, 0);        // root directory
    st32(at + 4, ansi); // object name
    st32(at + 8, 0x40); // attributes
    return at;
}

// 9 argument NtCreateFile: the last argument goes in the caller's frame at sp+0x54.
static uint32_t create(PPCContext& ctx, uint32_t handle_ptr, uint32_t access, uint32_t oa, uint32_t io,
                       uint32_t disposition, uint32_t options) {
    st32(ctx.r1.u32 + 0x54, options);
    return K(ctx, NtCreateFile, handle_ptr, access, oa, io, 0, 0x80, 3, disposition);
}

static void guest_main(PPCContext& ctx, uint8_t*) {
    uint32_t s = g_mem.malloc(0x1000);
    uint32_t h = s, io = s + 0x08, buf = s + 0x100, oa_at = s + 0x400;

    // Read a game file.
    CHECK(create(ctx, h, 0x80000000, make_oa(oa_at, "game:\\Media\\hello.txt"), io, 1, 0x40) == 0);
    uint32_t f = ld32(h);
    CHECK(K(ctx, NtReadFile, f, 0, 0, 0, io, buf, 16, 0) == 0);
    CHECK(ld32(io + 4) == 5 && std::memcmp(host(buf), "hello", 5) == 0);
    CHECK(K(ctx, NtReadFile, f, 0, 0, 0, io, buf, 16, 0) == STATUS_END_OF_FILE);

    // Explicit offset.
    st64(s + 0x20, 1);
    CHECK(K(ctx, NtReadFile, f, 0, 0, 0, io, buf, 16, s + 0x20) == 0);
    CHECK(ld32(io + 4) == 4 && std::memcmp(host(buf), "ello", 4) == 0);

    // File size via FileStandardInformation, position via FilePositionInformation.
    CHECK(K(ctx, NtQueryInformationFile, f, io, s + 0x30, 24, 5) == 0);
    CHECK(ld64(s + 0x38) == 5);
    st64(s + 0x20, 2);
    CHECK(K(ctx, NtSetInformationFile, f, io, s + 0x20, 8, 14) == 0);
    CHECK(K(ctx, NtReadFile, f, 0, 0, 0, io, buf, 16, 0) == 0);
    CHECK(ld32(io + 4) == 3 && std::memcmp(host(buf), "llo", 3) == 0);
    CHECK(K(ctx, NtClose, f) == 0);
    CHECK(K(ctx, NtClose, f) == STATUS_INVALID_HANDLE);

    // Missing file.
    CHECK(create(ctx, h, 0x80000000, make_oa(oa_at, "game:\\Media\\nope.bin"), io, 1, 0x40) == STATUS_OBJECT_NAME_NOT_FOUND);

    // Attributes without opening.
    CHECK(K(ctx, NtQueryFullAttributesFile, make_oa(oa_at, "d:\\Media\\hello.txt"), s + 0x30) == 0);
    CHECK(ld64(s + 0x30 + 40) == 5);

    // Directory listing (ANSI names).
    CHECK(create(ctx, h, 0x80000000, make_oa(oa_at, "game:\\Media"), io, 1, 0x21) == 0);
    uint32_t d = ld32(h);
    st32(ctx.r1.u32 + 0x54, 0);     // no file name pattern
    st32(ctx.r1.u32 + 0x5C, 1);     // restart scan
    CHECK(K(ctx, NtQueryDirectoryFile, d, 0, 0, 0, io, s + 0x200, 0x100, 1) == 0);
    CHECK(ld32(s + 0x200 + 0x3C) == 9 && std::memcmp(host(s + 0x240), "hello.txt", 9) == 0);
    st32(ctx.r1.u32 + 0x5C, 0);
    CHECK(K(ctx, NtQueryDirectoryFile, d, 0, 0, 0, io, s + 0x200, 0x100, 1) == 0x80000006);
    K(ctx, NtClose, d);

    // Write and read back on the writable cache device.
    CHECK(create(ctx, h, 0xC0000000, make_oa(oa_at, "cache:\\save.bin"), io, 2, 0x40) == 0);
    uint32_t w = ld32(h);
    std::memcpy(host(buf), "ABCDEFGH", 8);
    CHECK(K(ctx, NtWriteFile, w, 0, 0, 0, io, buf, 8, 0) == 0);
    CHECK(ld32(io + 4) == 8);
    st64(s + 0x20, 0);
    std::memset(host(buf), 0, 8);
    CHECK(K(ctx, NtReadFile, w, 0, 0, 0, io, buf, 8, s + 0x20) == 0);
    CHECK(std::memcmp(host(buf), "ABCDEFGH", 8) == 0);
    K(ctx, NtClose, w);

    // Writing to the disc fails.
    CHECK(create(ctx, h, 0x40000000, make_oa(oa_at, "game:\\Media\\hello.txt"), io, 3, 0x40) != 0);

    ctx.r3.u64 = g_fail;
}

PPCFuncMapping PPCFuncMappings[] = {
    {0x82000200, guest_main},
    {0, nullptr},
};

int main() {
    CHECK(g_mem.init());
    g_mem.commit_fixed(0x7FEA0000, 0x10000);
    std::vector<uint8_t> image(0x10000, 0);
    CHECK(map_image(image.data(), 0x82000000, 0x10000));

    auto dir = std::filesystem::temp_directory_path() / ("fa-ntfile-test-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir / "game" / "Media");
    std::ofstream(dir / "game" / "Media" / "hello.txt") << "hello";
    setenv("XDG_CONFIG_HOME", (dir / "config").c_str(), 1);
    fa::hle::ServicesConfig cfg;
    cfg.game_path = (dir / "game").string();
    cfg.data_dir = (dir / "data").string();
    cfg.audio_output = false;
    std::string err;
    CHECK(fa::hle::init_services(cfg, make_runtime_hooks(), err));

    uint32_t rc = g_kernel.run_main_thread(0x82000200, 0x40000);
    CHECK(rc == 0);
    fa::hle::shutdown_services();
    std::filesystem::remove_all(dir);
    std::printf(g_fail ? "FAILED (%d)\n" : "OK\n", g_fail);
    std::fflush(stdout);
    std::_Exit(g_fail ? 1 : 0);
}
