// Guest timebase: rescales the host TSC that the recompiled mftb reads to the 50 MHz the game
// expects. See include/fa/rt/timebase.hpp for why this patches code at startup.
//
// Each patched site
//     rdtsc                       0F 31
//     [mov r,r ...]               optional register moves the compiler put in between
//     shl rdx, 32                 48 C1 E2 20
// becomes `jmp trampoline` (plus NOP padding), and the trampoline does
//     rdtsc; shl rdx,32; or rax,rdx          rax = host TSC
//     sub rax,[tsc0]; mul [mult]; shrd rax,rdx,32   rax = (tsc - tsc0) * guest_hz / tsc_hz
//     mov rdx,rax; shr rdx,32; mov eax,eax   edx:eax as rdtsc would leave them
//     <the copied register moves and the shl>
//     jmp back
// It only touches rax, rdx and the flags (the original sequence already clobbers those) and
// never the stack, so the red zone of a leaf function stays intact.
#include "fa/rt/timebase.hpp"

#include <link.h>
#include <sys/mman.h>
#include <unistd.h>
#include <x86intrin.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <vector>

namespace fa::rt {

namespace {

struct alignas(64) Scale {
    uint64_t tsc0;   // host TSC at calibration: guest time 0
    uint64_t mult;   // guest_hz / tsc_hz in 32.32 fixed point
};

// Where the trampolines read the scale. Points into the trampoline block once one exists (so a
// rel32 always reaches it), and at this fallback otherwise.
Scale g_scale_storage{0, 0};
Scale* g_scale = &g_scale_storage;
uint64_t g_guest_hz = kGuestTimebaseHz;

// Host TSC reads in the runtime use rdtscp, so the patcher never matches its own code.
uint64_t host_tsc() {
    unsigned aux;
    return __rdtscp(&aux);
}

uint64_t mono_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

// shl rdx, 32
bool is_shl_rdx_32(const uint8_t* p) { return p[0] == 0x48 && p[1] == 0xC1 && p[2] == 0xE2 && p[3] == 0x20; }

// Register to register mov (89 /r or 8B /r with mod = 11), optionally with a REX.W/R/B prefix.
// Returns its length, or 0.
size_t reg_mov_len(const uint8_t* p) {
    size_t n = 0;
    if ((p[0] & 0xF0) == 0x40) n = 1;
    uint8_t op = p[n], modrm = p[n + 1];
    if ((op == 0x89 || op == 0x8B) && (modrm & 0xC0) == 0xC0) return n + 2;
    return 0;
}

// Length of the instructions after an rdtsc at `p` that the trampoline takes over, ending with
// the shl rdx,32; 0 when the site does not match.
size_t site_len(const uint8_t* p, const uint8_t* end) {
    size_t len = 2;
    for (int i = 0; i < 3; ++i) {
        if (p + len + 4 > end) return 0;
        if (is_shl_rdx_32(p + len)) return len + 4;
        size_t m = reg_mov_len(p + len);
        if (!m) return 0;
        len += m;
    }
    return 0;
}

constexpr size_t kTrampSize = 64;

void put32(uint8_t* p, int32_t v) { std::memcpy(p, &v, 4); }

bool rel32(const uint8_t* from_next, const uint8_t* to, int32_t& out) {
    int64_t d = int64_t(to - from_next);
    if (d < INT32_MIN || d > INT32_MAX) return false;
    out = int32_t(d);
    return true;
}

// Writes the trampoline for the site at `site` (len bytes, copied from `orig`) into `t`.
bool emit_trampoline(uint8_t* t, const uint8_t* site, const uint8_t* orig, size_t len) {
    // The rdtsc opcode byte comes from a volatile so this function's own code never contains
    // the byte pattern the scanner looks for.
    static volatile uint8_t k0F = 0x0F;
    uint8_t* q = t;
    auto emit = [&](std::initializer_list<uint8_t> b) { for (uint8_t x : b) *q++ = x; };
    auto emit_rip = [&](std::initializer_list<uint8_t> b, const void* target) {
        emit(b);
        int32_t d;
        if (!rel32(q + 4, static_cast<const uint8_t*>(target), d)) return false;
        put32(q, d);
        q += 4;
        return true;
    };
    *q++ = k0F;
    emit({0x31});                          // rdtsc
    emit({0x48, 0xC1, 0xE2, 0x20});        // shl rdx, 32
    emit({0x48, 0x09, 0xD0});              // or rax, rdx
    if (!emit_rip({0x48, 0x2B, 0x05}, &g_scale->tsc0)) return false;  // sub rax, [tsc0]
    if (!emit_rip({0x48, 0xF7, 0x25}, &g_scale->mult)) return false;  // mul qword [mult]
    emit({0x48, 0x0F, 0xAC, 0xD0, 0x20});  // shrd rax, rdx, 32
    emit({0x48, 0x89, 0xC2});              // mov rdx, rax
    emit({0x48, 0xC1, 0xEA, 0x20});        // shr rdx, 32
    emit({0x89, 0xC0});                    // mov eax, eax
    std::memcpy(q, orig + 2, len - 2);     // the moves and the shl, as the compiler wrote them
    q += len - 2;
    if (!emit_rip({0xE9}, site + len)) return false;  // jmp back
    return size_t(q - t) <= kTrampSize;
}

// Maps `bytes` of executable memory within rel32 reach of [lo, hi).
uint8_t* alloc_near(const uint8_t* lo, const uint8_t* hi, size_t bytes) {
    const uintptr_t step = 1 << 20;
    const uintptr_t span = (bytes + step - 1) & ~(step - 1);
    const uintptr_t reach = 0x7FF00000;  // a little under 2 GiB
    uintptr_t up = (uintptr_t(hi) + step - 1) & ~(step - 1);
    uintptr_t down = uintptr_t(lo) & ~(step - 1);
    for (uintptr_t off = 0; off + span < reach; off += step) {
        uintptr_t cands[2] = {up + off, down > off + span + step ? down - off - span : 0};
        for (uintptr_t cand : cands) {
            if (!cand) continue;
            void* p = mmap(reinterpret_cast<void*>(cand), bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p == MAP_FAILED) continue;
            if (uintptr_t(p) != cand) { munmap(p, bytes); continue; }  // old kernel ignored the flag
            return static_cast<uint8_t*>(p);
        }
    }
    return nullptr;
}

}  // namespace

uint64_t calibrate_guest_timebase(uint64_t guest_hz) {
    g_guest_hz = guest_hz;
    uint64_t n0 = mono_ns(), t0 = host_tsc();
    timespec nap{0, 50 * 1000 * 1000};
    nanosleep(&nap, nullptr);
    uint64_t n1 = mono_ns(), t1 = host_tsc();
    double tsc_hz = double(t1 - t0) * 1e9 / double(n1 - n0 ? n1 - n0 : 1);
    if (tsc_hz < 1e6) tsc_hz = 1e9;
    g_scale->mult = uint64_t(double(guest_hz) * 4294967296.0 / tsc_hz + 0.5);
    g_scale->tsc0 = t1;
    return uint64_t(tsc_hz);
}

uint64_t guest_timebase_now() {
    unsigned __int128 v = (unsigned __int128)(host_tsc() - g_scale->tsc0) * g_scale->mult;
    return uint64_t(v >> 32);
}

size_t patch_rdtsc_sites(uint8_t* begin, uint8_t* end, size_t* skipped) {
    size_t bad = 0;
    std::vector<std::pair<uint8_t*, size_t>> sites;
    for (uint8_t* p = begin; p + 2 <= end; ++p) {
        if (p[0] != 0x0F || p[1] != 0x31) continue;
        size_t len = site_len(p, end);
        if (len >= 5) { sites.push_back({p, len}); p += len - 1; }
        else ++bad;
    }
    size_t done = 0;
    if (!sites.empty()) {
        size_t bytes = (sizeof(Scale) + sites.size() * kTrampSize + 4095) & ~size_t(4095);
        uint8_t* block = alloc_near(begin, end, bytes);
        if (!block) {
            std::fprintf(stderr, "timebase: no memory near the code for %zu trampolines\n", sites.size());
            if (skipped) *skipped = bad + sites.size();
            return 0;
        }
        // Move the scale next to the trampolines (the copy keeps the calibration).
        Scale* s = reinterpret_cast<Scale*>(block);
        *s = *g_scale;
        g_scale = s;
        uint8_t* t = block + sizeof(Scale);
        for (auto [p, len] : sites) {
            uint8_t orig[16];
            std::memcpy(orig, p, len);
            int32_t d;
            if (!emit_trampoline(t, p, orig, len) || !rel32(p + 5, t, d)) { ++bad; continue; }
            p[0] = 0xE9;
            put32(p + 1, d);
            std::memset(p + 5, 0x90, len - 5);
            t += kTrampSize;
            ++done;
        }
        mprotect(block, bytes, PROT_READ | PROT_EXEC);  // the scale stays readable
        __builtin___clear_cache(reinterpret_cast<char*>(begin), reinterpret_cast<char*>(end));
    }
    if (skipped) *skipped = bad;
    return done;
}

void install_guest_timebase() {
    const char* mode = std::getenv("FA_TIMEBASE");
    if (mode && (!std::strcmp(mode, "off") || !std::strcmp(mode, "0"))) {
        std::fprintf(stderr, "timebase: FA_TIMEBASE=off, mftb reads the raw host TSC\n");
        return;
    }
    uint64_t guest_hz = kGuestTimebaseHz;
    if (const char* hz = std::getenv("FA_TIMEBASE_HZ")) guest_hz = std::strtoull(hz, nullptr, 0);
    if (!guest_hz) guest_hz = kGuestTimebaseHz;
    uint64_t tsc_hz = calibrate_guest_timebase(guest_hz);

    // Patch the executable segments of the main program (the recompiled code is linked in).
    struct Seg { uint8_t* begin; uint8_t* end; };
    std::vector<Seg> segs;
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* user) -> int {
        auto* out = static_cast<std::vector<Seg>*>(user);
        for (int i = 0; i < info->dlpi_phnum; ++i) {
            const auto& ph = info->dlpi_phdr[i];
            if (ph.p_type != PT_LOAD || !(ph.p_flags & PF_X)) continue;
            auto* b = reinterpret_cast<uint8_t*>(info->dlpi_addr + ph.p_vaddr);
            out->push_back({b, b + ph.p_memsz});
        }
        return 1;  // the first entry is the main program
    }, &segs);

    size_t done = 0, skipped = 0;
    long page = sysconf(_SC_PAGESIZE);
    for (auto s : segs) {
        auto* pb = reinterpret_cast<uint8_t*>(uintptr_t(s.begin) & ~uintptr_t(page - 1));
        size_t len = size_t(s.end - pb);
        if (mprotect(pb, len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            std::perror("timebase: mprotect code");
            continue;
        }
        size_t sk = 0;
        done += patch_rdtsc_sites(s.begin, s.end, &sk);
        skipped += sk;
        mprotect(pb, len, PROT_READ | PROT_EXEC);
    }
    std::fprintf(stderr, "timebase: host TSC %.1f MHz scaled to %.3f MHz, %zu mftb sites patched (%zu other 0F31 byte pairs left alone)\n",
                 double(tsc_hz) / 1e6, double(guest_hz) / 1e6, done, skipped);
}

}  // namespace fa::rt
