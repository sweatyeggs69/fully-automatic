// Tests for the HLE services: VFS over the synthetic test ISO and a host
// folder, input bindings, XMA context packing, and (when FA_XMA_SAMPLE points
// at a RIFF .xma file) the XMA hardware path against the offline decoder.
//
// usage: hle_tests <test.iso> <workdir>
#include <sys/mman.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <SDL.h>

#include "fa/hle/audio.hpp"
#include "fa/hle/input.hpp"
#include "fa/hle/vfs.hpp"

using namespace fa::hle;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                         \
        }                                                                     \
    } while (0)

static std::string read_all(Vfs& vfs, const std::string& p, uint32_t* st_out = nullptr) {
    std::shared_ptr<VfsFile> f;
    uint32_t info = 0;
    uint32_t st = vfs.open(p, OpenRequest{}, f, info);
    if (st_out) *st_out = st;
    if (st != status::kSuccess) return "<" + std::to_string(st) + ">";
    std::string s(f->info().size, '\0');
    s.resize(f->read(0, s.data(), s.size()));
    return s;
}

static void test_wildcards() {
    CHECK(wildcard_match("*", "anything"));
    CHECK(wildcard_match("*.xwb", "Music.XWB"));
    CHECK(!wildcard_match("*.xwb", "music.xsb"));
    CHECK(wildcard_match("a?c", "ABC"));
    CHECK(wildcard_match("*a*b", "xxaYYb"));
    CHECK(!wildcard_match("*a*b", "xxaYYbc"));
    CHECK(wildcard_match("<.xwb", "x.xwb"));
    CHECK(normalize_guest_path("\\??\\game:/Media/../default.xex") == "game:\\default.xex");
    CHECK(normalize_guest_path("game:\\\\a\\.\\b") == "game:\\a\\b");
}

static void test_vfs(const std::string& iso, const std::string& work) {
    Vfs vfs;
    std::string err;
    std::string data = work + "/vfs-data";
    fs::remove_all(data);
    CHECK(vfs.mount_defaults(iso, data, err));

    std::string xex = read_all(vfs, "game:\\default.xex");
    CHECK(xex.size() > 0x200 && xex.compare(0, 4, "XEX2") == 0);
    CHECK(read_all(vfs, "GAME:\\media\\HELLO.TXT") == "hello");
    CHECK(read_all(vfs, "d:\\Media\\hello.txt") == "hello");
    CHECK(read_all(vfs, "\\Device\\Cdrom0\\Media\\..\\Media\\hello.txt") == "hello");
    CHECK(read_all(vfs, "\\??\\game:\\Media\\hello.txt") == "hello");
    uint32_t st = 0;
    read_all(vfs, "game:\\nope.bin", &st);
    CHECK(st == status::kObjectNameNotFound);
    read_all(vfs, "game:\\nodir\\nope.bin", &st);
    CHECK(st == status::kObjectPathNotFound);
    read_all(vfs, "x:\\foo", &st);
    CHECK(st == status::kObjectPathNotFound);

    // Partial reads at an offset.
    {
        std::shared_ptr<VfsFile> f;
        uint32_t info;
        CHECK(vfs.open("game:\\Media\\hello.txt", OpenRequest{}, f, info) == status::kSuccess);
        CHECK(info == create_info::kOpened);
        char b[8] = {};
        CHECK(f->read(1, b, 8) == 4 && std::string(b, 4) == "ello");
        CHECK(f->read(5, b, 8) == 0);
        size_t w = 0;
        CHECK(f->write(0, "x", 1, w) == status::kMediaWriteProtected);
    }
    // The disc is read-only.
    {
        std::shared_ptr<VfsFile> f;
        uint32_t info;
        OpenRequest req;
        req.access = 0x40000000;
        req.disposition = Disposition::OpenIf;
        CHECK(vfs.open("game:\\new.bin", req, f, info) == status::kMediaWriteProtected);
    }
    // Directory listing with patterns.
    {
        std::shared_ptr<VfsFile> dir;
        uint32_t info;
        OpenRequest req;
        req.options = create_options::kDirectoryFile;
        CHECK(vfs.open("game:\\", req, dir, info) == status::kSuccess);
        std::vector<std::string> names;
        FileInfo fi;
        std::string all = "*";
        for (bool first = true; dir->next_entry(first ? &all : nullptr, false, fi) == status::kSuccess; first = false)
            names.push_back(fi.name + (fi.is_dir() ? "/" : ""));
        CHECK(names.size() == 2 && names[0] == "default.xex" && names[1] == "Media/");

        std::shared_ptr<VfsFile> media;
        CHECK(vfs.open("game:\\media", req, media, info) == status::kSuccess);
        std::string pat = "*.txt";
        CHECK(media->next_entry(&pat, false, fi) == status::kSuccess && fi.name == "hello.txt" && fi.size == 5);
        CHECK(media->next_entry(nullptr, false, fi) == status::kNoMoreFiles);
        std::shared_ptr<VfsFile> media2;
        CHECK(vfs.open("game:\\media", req, media2, info) == status::kSuccess);
        std::string none = "*.xwb";
        CHECK(media2->next_entry(&none, false, fi) == status::kObjectNameNotFound);

        OpenRequest freq;
        freq.options = create_options::kDirectoryFile;
        std::shared_ptr<VfsFile> f;
        CHECK(vfs.open("game:\\default.xex", freq, f, info) == status::kNotADirectory);
    }
    // Writable hdd: folder.
    {
        std::shared_ptr<VfsFile> f;
        uint32_t info;
        OpenRequest create;
        create.access = 0x40000000 | 0x80000000;
        create.disposition = Disposition::Create;
        CHECK(vfs.open("hdd:\\saves\\slot0.bin", create, f, info) == status::kObjectPathNotFound);
        OpenRequest mkdir;
        mkdir.disposition = Disposition::Create;
        mkdir.options = create_options::kDirectoryFile;
        CHECK(vfs.open("hdd:\\Saves", mkdir, f, info) == status::kSuccess && info == create_info::kCreated);
        CHECK(vfs.open("hdd:\\saves\\slot0.bin", create, f, info) == status::kSuccess && info == create_info::kCreated);
        size_t w = 0;
        CHECK(f->write(0, "progress", 8, w) == status::kSuccess && w == 8);
        f.reset();
        CHECK(read_all(vfs, "HDD:\\SAVES\\SLOT0.BIN") == "progress");
        CHECK(read_all(vfs, "\\Device\\Harddisk0\\Partition1\\saves\\slot0.bin") == "progress");
        CHECK(vfs.open("hdd:\\saves\\slot0.bin", create, f, info) == status::kObjectNameCollision);
        OpenRequest over;
        over.access = 0x40000000;
        over.disposition = Disposition::OverwriteIf;
        CHECK(vfs.open("hdd:\\saves\\slot0.bin", over, f, info) == status::kSuccess && info == create_info::kOverwritten);
        CHECK(f->info().size == 0);
        f.reset();
        FileInfo fi;
        CHECK(vfs.stat("hdd:\\saves\\slot0.bin", fi) == status::kSuccess && !fi.is_dir());
        CHECK(vfs.rename("hdd:\\saves\\slot0.bin", "slot1.bin") == status::kSuccess);
        CHECK(vfs.stat("hdd:\\saves\\slot1.bin", fi) == status::kSuccess);
        CHECK(vfs.remove("hdd:\\saves") == status::kDirectoryNotEmpty);
        CHECK(vfs.remove("hdd:\\saves\\slot1.bin") == status::kSuccess);
        CHECK(vfs.remove("hdd:\\saves") == status::kSuccess);
        CHECK(fs::exists(data + "/hdd") && fs::exists(data + "/cache"));
        VolumeInfo vi;
        CHECK(vfs.volume("game:\\", vi) == status::kSuccess && vi.free_bytes == 0);
    }
    // Symbolic links (ObCreateSymbolicLink).
    vfs.add_link("media:", "game:\\Media");
    CHECK(read_all(vfs, "media:\\hello.txt") == "hello");
}

static void test_input() {
    std::vector<std::string> errors;
    auto binds = Input::parse_bindings(Input::default_bindings_text(), errors);
    CHECK(errors.empty());
    CHECK(binds.size() > 15);
    auto bad = Input::parse_bindings("NoSuchKey = A\nW = Q\nW\n", errors);
    CHECK(bad.empty() && errors.size() == 3);

    std::vector<uint8_t> keys(SDL_NUM_SCANCODES, 0);
    XInputGamepad pad;
    keys[SDL_SCANCODE_W] = 1;
    keys[SDL_SCANCODE_SPACE] = 1;
    keys[SDL_SCANCODE_A] = 1;
    Input::apply_keys(binds, keys.data(), int(keys.size()), pad);
    CHECK(pad.right_trigger == 255 && pad.left_trigger == 0);
    CHECK(pad.buttons == xinput::kA);
    CHECK(pad.thumb_lx == -32768);
    keys[SDL_SCANCODE_D] = 1;  // opposite directions cancel
    pad = {};
    Input::apply_keys(binds, keys.data(), int(keys.size()), pad);
    CHECK(pad.thumb_lx == -1);  // -32768 + 32767
    static_assert(sizeof(XInputGamepad) == 12 && sizeof(XInputState) == 16 && sizeof(XInputCapabilities) == 20);
}

static void test_context_packing() {
    uint8_t g[64];
    XmaContextData d;
    d.input_buffer_0_packet_count = 0xABC;
    d.loop_count = 255;
    d.input_buffer_1_valid = 1;
    d.output_buffer_block_count = 31;
    d.output_buffer_write_offset = 17;
    d.sample_rate = 3;
    d.is_stereo = 1;
    d.output_buffer_valid = 1;
    d.input_buffer_read_offset = 0x3FFFFFF;
    d.loop_end = 12345;
    d.current_buffer = 1;
    d.input_buffer_0_ptr = 0x12345678;
    d.output_buffer_read_offset = 9;
    d.stop_when_done = 1;
    d.store(g);
    // DWORD 0 holds packet count in its low 12 bits and is stored big-endian.
    uint32_t dw0 = uint32_t(g[0]) << 24 | g[1] << 16 | g[2] << 8 | g[3];
    CHECK((dw0 & 0xFFF) == 0xABC && ((dw0 >> 21) & 1) == 1 && (dw0 >> 27) == 17);
    XmaContextData e;
    e.load(g);
    CHECK(e.input_buffer_0_packet_count == 0xABC && e.loop_count == 255 && e.input_buffer_1_valid == 1);
    CHECK(e.output_buffer_block_count == 31 && e.output_buffer_write_offset == 17 && e.sample_rate == 3);
    CHECK(e.is_stereo == 1 && e.output_buffer_valid == 1 && e.input_buffer_read_offset == 0x3FFFFFF);
    CHECK(e.loop_end == 12345 && e.current_buffer == 1 && e.input_buffer_0_ptr == 0x12345678);
    CHECK(e.output_buffer_read_offset == 9 && e.stop_when_done == 1 && e.input_buffer_0_valid == 0);
}

static void test_downmix() {
    std::vector<float> in(6 * 256, 0.0f), out(512);
    for (int i = 0; i < 256; ++i) {
        in[i] = 0.25f;            // FL
        in[256 + i] = -0.25f;     // FR
        in[512 + i] = 0.5f;       // C
    }
    AudioSystem::downmix_frame(in.data(), out.data());
    CHECK(std::fabs(out[0] - (0.25f + 0.70710678f * 0.5f)) < 1e-5f);
    CHECK(std::fabs(out[1] - (-0.25f + 0.70710678f * 0.5f)) < 1e-5f);

    // Past full scale: soft limited, never above 1, still ordered, and counted.
    for (int i = 0; i < 256; ++i) {
        in[i] = 0.9f + 0.002f * float(i);   // FL rising from 0.9 to about 1.41
        in[256 + i] = -1.5f;                // FR well past -1
        in[512 + i] = 0.0f;
    }
    int expect = 256;  // every FR sample
    for (int i = 0; i < 256; ++i) expect += in[i] > 1.0f;
    int limited = AudioSystem::downmix_frame(in.data(), out.data());
    CHECK(out[0] > 0.8f && out[0] < 0.9f);
    bool ordered = true, bounded = true;
    for (int i = 0; i < 256; ++i) {
        if (std::fabs(out[2 * i]) > 1.0f || std::fabs(out[2 * i + 1]) > 1.0f) bounded = false;
        if (i && out[2 * i] < out[2 * i - 2]) ordered = false;
    }
    CHECK(bounded);
    CHECK(ordered);
    CHECK(out[1] < -0.99f);
    CHECK(limited == expect && expect > 400);
}

// FA_AUDIO_DUMP path: submitted frames land in a valid float WAV and in the stats.
static void test_audio_dump(const std::string& work) {
    std::string path = work + "/dump_test.wav";
    {
        AudioSystem audio;
        audio.init(GuestHooks{}, /*disable_output=*/true);
        CHECK(audio.start_dump(path));
        // One frame: FL = 0.5 on every sample, everything else silent (big-endian floats).
        std::vector<uint8_t> be(6 * 256 * 4, 0);
        for (int i = 0; i < 256; ++i) { be[4 * i] = 0x3F; }  // 0x3F000000 = 0.5f
        std::vector<uint8_t> zero(be.size(), 0);
        for (int k = 0; k < 200; ++k) audio.submit_frame_host(0, k == 199 ? zero.data() : be.data());
        auto st = audio.take_stats();
        CHECK(st.frames == 200);
        CHECK(st.silent_frames == 1);
        CHECK(std::fabs(st.peak - 0.5f) < 1e-6f);
        audio.shutdown();
    }
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
    CHECK(d.size() == 44 + 200 * 256 * 2 * 4);
    if (d.size() < 52) return;
    CHECK(!std::memcmp(d.data(), "RIFF", 4) && !std::memcmp(d.data() + 8, "WAVE", 4));
    uint32_t data_len = d[40] | d[41] << 8 | d[42] << 16 | uint32_t(d[43]) << 24;
    CHECK(data_len == 200 * 256 * 2 * 4);
    float l, r;
    std::memcpy(&l, &d[44], 4);
    std::memcpy(&r, &d[48], 4);
    CHECK(l == 0.5f && r == 0.0f);
}

// Simple bump allocator over a 4 GiB guest space.
struct FakeGuest {
    uint8_t* base = nullptr;
    uint32_t next = 0x40000000;
    uint32_t phys_next = 0x00100000;  // physical allocations live at 0xA0000000 + p
    FakeGuest() {
        void* p = mmap(nullptr, 0x100000000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        base = p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
    }
    ~FakeGuest() {
        if (base) munmap(base, 0x100000000ull);
    }
    uint32_t alloc(uint32_t size, uint32_t align) {
        next = (next + align - 1) & ~(align - 1);
        uint32_t r = next;
        next += size;
        return r;
    }
    uint32_t phys_alloc(uint32_t size) {
        uint32_t r = phys_next;
        phys_next += (size + 0xFFF) & ~0xFFFu;
        return r;
    }
    GuestHooks hooks() {
        GuestHooks h;
        h.membase = base;
        h.alloc = [this](uint32_t s, uint32_t a) { return alloc(s, a); };
        h.free = [](uint32_t) {};
        return h;
    }
};

static bool load_xma(const char* path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
    for (size_t o = 12; o + 8 <= d.size();) {
        uint32_t sz = d[o + 4] | d[o + 5] << 8 | d[o + 6] << 16 | uint32_t(d[o + 7]) << 24;
        if (!std::memcmp(&d[o], "data", 4)) {
            out.assign(d.begin() + long(o + 8), d.begin() + long(o + 8 + std::min<size_t>(sz, d.size() - o - 8)));
            out.resize(out.size() / 2048 * 2048);
            return true;
        }
        o += 8 + sz + (sz & 1);
    }
    return false;
}

// Plays a stream through the emulated hardware the way a game's XAudio does:
// two rolling input buffers of a few packets each, a small output ring, and a
// consumer that drains the ring and re-validates it.
static void test_xma_hardware(const char* sample) {
    std::vector<uint8_t> stream;
    if (!load_xma(sample, stream)) {
        std::fprintf(stderr, "cannot read XMA sample %s\n", sample);
        ++g_fail;
        return;
    }
    std::vector<int16_t> ref;
    std::string err;
    CHECK(XmaDecoder::decode_stream(stream.data(), stream.size(), 2, 48000, ref, err));

    FakeGuest g;
    if (!g.base) {
        std::fprintf(stderr, "skip hardware test: cannot reserve guest space\n");
        return;
    }
    XmaDecoder xma;
    CHECK(xma.init(g.hooks(), false));
    uint32_t ctx = xma.create_context();
    CHECK(ctx != 0 && xma.context_index(ctx) == 0);

    const uint32_t kPk = 3;  // packets per input buffer
    uint32_t in_phys[2] = {g.phys_alloc(kPk * 2048), g.phys_alloc(kPk * 2048)};
    // 5 KiB ring: holds two and a half stereo frames, so it never fills up
    // exactly and read == write always means "empty" here.
    uint32_t out_blocks = 20;
    uint32_t out_phys = g.phys_alloc(out_blocks * 256);
    auto ph = [&](uint32_t p) { return g.base + 0xA0000000u + p; };

    uint32_t total_packets = uint32_t(stream.size() / 2048), next_packet = 0;
    auto fill = [&](int which, XmaContextData& d) {
        uint32_t n = std::min(kPk, total_packets - next_packet);
        if (n == 0) return false;
        std::memcpy(ph(in_phys[which]), stream.data() + size_t(next_packet) * 2048, size_t(n) * 2048);
        next_packet += n;
        if (which == 0) { d.input_buffer_0_ptr = in_phys[0]; d.input_buffer_0_packet_count = n; d.input_buffer_0_valid = 1; }
        else { d.input_buffer_1_ptr = in_phys[1]; d.input_buffer_1_packet_count = n; d.input_buffer_1_valid = 1; }
        return true;
    };

    XmaContextData d;
    d.load(g.base + ctx);
    d.sample_rate = 3;
    d.is_stereo = 1;
    d.output_buffer_ptr = out_phys;
    d.output_buffer_block_count = out_blocks;
    d.output_buffer_valid = 1;
    fill(0, d);
    fill(1, d);
    d.input_buffer_read_offset = 32;
    d.store(g.base + ctx);

    std::vector<int16_t> got;
    for (int iter = 0; iter < 200000; ++iter) {
        // Kick through the register page, as inline XDK code would.
        uint32_t kick = 1u;  // little-endian register value, as the game's byte-reversed store leaves it
        std::memcpy(g.base + XmaDecoder::kRegisterBase + XmaDecoder::kRegKick, &kick, 4);
        xma.work_once();
        d.load(g.base + ctx);
        // Drain everything between read and write offsets.
        uint32_t cap = out_blocks * 256, rd = d.output_buffer_read_offset * 256, wr = d.output_buffer_write_offset * 256;
        uint32_t used = wr >= rd ? wr - rd : cap - rd + wr;
        for (uint32_t i = 0; i < used; i += 2) {
            const uint8_t* s = ph(out_phys) + (rd + i) % cap;
            got.push_back(int16_t(s[0] << 8 | s[1]));
        }
        d.output_buffer_read_offset = wr / 256;
        d.output_buffer_valid = 1;
        bool more = true;
        if (!d.input_buffer_0_valid) more &= fill(0, d) || d.input_buffer_1_valid;
        if (!d.input_buffer_1_valid) more &= fill(1, d) || d.input_buffer_0_valid;
        d.store(g.base + ctx);
        if (!d.input_buffer_0_valid && !d.input_buffer_1_valid) break;
    }
    std::fprintf(stderr, "xma hardware path: %zu samples, offline: %zu samples\n", got.size(), ref.size());
    CHECK(got.size() == ref.size());
    size_t mism = 0;
    for (size_t i = 0; i < std::min(got.size(), ref.size()); ++i) mism += got[i] != ref[i];
    CHECK(mism == 0);
    xma.release_context(ctx);
    xma.shutdown();
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: hle_tests <test.iso> <workdir>\n");
        return 2;
    }
    test_wildcards();
    test_vfs(argv[1], argv[2]);
    test_input();
    test_context_packing();
    test_downmix();
    test_audio_dump(argv[2]);
    if (const char* s = std::getenv("FA_XMA_SAMPLE")) test_xma_hardware(s);
    if (g_fail) {
        std::fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("all HLE tests passed\n");
    return 0;
}
