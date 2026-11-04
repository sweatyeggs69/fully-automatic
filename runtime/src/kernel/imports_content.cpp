// Content (save game) device and enumeration stubs, plus the wide printf helpers.
//
// There is one virtual storage device (id 1) backed by hdd:. No content packages exist yet,
// so enumerations are empty; the game then offers to create a new save, which will matter
// once XamContentCreate maps onto hdd:.
#include "fa/rt/import.hpp"
#include "fa/rt/kernel.hpp"
#include "fa/rt/memory.hpp"

#include "fa/hle/services.hpp"
#include "fa/hle/vfs.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>
#include <cstdint>
#include <cstring>
#include <string>

using namespace fa::rt;

void complete_overlapped(uint32_t ov, uint32_t result);  // imports_xmsg.cpp

namespace {
constexpr uint32_t kErrorIoPending = 997;
constexpr uint32_t kErrorNoMoreFiles = 18;
constexpr uint32_t kErrorInvalidParameter = 87;
constexpr uint32_t kErrorFileNotFound = 2;
constexpr uint32_t kErrorAlreadyExists = 183;
constexpr uint32_t kErrorDeviceNotConnected = 0x48F;
constexpr uint32_t kXContentDataSize = 0x134;  // sizeof(XCONTENT_DATA)
constexpr uint32_t kDeviceDataSize = 0x50;     // sizeof(XDEVICE_DATA)
constexpr uint32_t kDeviceId = 1;              // the one virtual hard drive

// XCONTENT_DATA: u32 device_id, u32 content_type, u16 display_name[128], char file_name[42].
constexpr uint32_t kCdName = 8, kCdFile = 0x108;

std::mutex g_cmu;

std::string data_root() {
    const auto& c = fa::hle::services().config;
    return c.data_dir.empty() ? fa::hle::default_data_dir() : c.data_dir;
}

// <data>/hdd/Content/<user>/<title>/<type>/<file>, readable through hdd:\ too.
std::filesystem::path content_dir() {
    return std::filesystem::path(data_root()) / "hdd" / "Content" / "0000000000000000" / "534507D5";
}

std::string hex8(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "%08X", v); return b; }

std::string guest_cstr(uint32_t p, size_t max) {
    std::string r;
    for (size_t i = 0; p && i < max && ld8(p + i); ++i) r += char(ld8(p + i));
    return r;
}

bool safe_name(const std::string& n) {
    if (n.empty() || n == "." || n == "..") return false;
    for (char c : n) if (c == '/' || c == '\\' || c == ':' || c == 0) return false;
    return true;
}

// Enumerator state, keyed by guest handle.
struct Enumerator {
    bool devices = false;
    std::vector<std::vector<uint8_t>> items;  // pre-built guest-order records
    size_t next = 0;
    uint32_t per_call = 1;
    uint32_t record_size = 0;
};
std::map<uint32_t, Enumerator> g_enums;

uint32_t new_enum(Enumerator e) {
    uint32_t h = g_kernel.add_handle(std::make_shared<KEvent>(true, false));
    std::lock_guard<std::mutex> lk(g_cmu);
    g_enums[h] = std::move(e);
    return h;
}

void put32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[off + i] = uint8_t(x >> (24 - 8 * i));
}
void put64(std::vector<uint8_t>& v, size_t off, uint64_t x) {
    for (int i = 0; i < 8; ++i) v[off + i] = uint8_t(x >> (56 - 8 * i));
}
void put_wide(std::vector<uint8_t>& v, size_t off, const std::string& s, size_t max_chars) {
    for (size_t i = 0; i < s.size() && i + 1 < max_chars; ++i) { v[off + i * 2] = 0; v[off + i * 2 + 1] = uint8_t(s[i]); }
}

const char* kDeviceName = "Hard Drive";

// XDEVICE_DATA {u32 device_id, u32 device_type (1 = HDD), u64 total, u64 free, u16 name[28]}
std::vector<uint8_t> device_record() {
    std::vector<uint8_t> r(kDeviceDataSize, 0);
    std::error_code ec;
    auto sp = std::filesystem::space(data_root(), ec);
    uint64_t total = ec ? (uint64_t(20) << 30) : sp.capacity, freeb = ec ? (uint64_t(10) << 30) : sp.available;
    put32(r, 0, kDeviceId);
    put32(r, 4, 1);
    put64(r, 8, total);
    put64(r, 16, freeb);
    put_wide(r, 24, kDeviceName, 28);
    return r;
}

// Saved packages live in <content>/<type>/<name>/, display name in <content>/<type>/<name>.meta
// (the 256 raw bytes of the guest's wide display name).
std::vector<std::vector<uint8_t>> scan_content(uint32_t type) {
    std::vector<std::vector<uint8_t>> out;
    std::error_code ec;
    for (auto& t : std::filesystem::directory_iterator(content_dir(), ec)) {
        if (!t.is_directory()) continue;
        uint32_t ty = uint32_t(std::strtoul(t.path().filename().string().c_str(), nullptr, 16));
        if (type && ty != type) continue;
        std::error_code ec2;
        for (auto& d : std::filesystem::directory_iterator(t.path(), ec2)) {
            if (!d.is_directory()) continue;
            std::string name = d.path().filename().string();
            if (name.size() >= 42) continue;
            std::vector<uint8_t> r(kXContentDataSize, 0);
            put32(r, 0, kDeviceId);
            put32(r, 4, ty);
            std::ifstream m(d.path().string() + ".meta", std::ios::binary);
            if (m) m.read(reinterpret_cast<char*>(r.data() + kCdName), 256);
            else put_wide(r, kCdName, name, 128);
            std::memcpy(r.data() + kCdFile, name.data(), name.size());
            out.push_back(std::move(r));
        }
    }
    return out;
}

// Content flags (low byte) for XamContentCreate.
enum { kCreateNew = 1, kCreateAlways = 2, kOpenExisting = 3, kOpenAlways = 4, kTruncateExisting = 5 };

void finish(uint32_t ov, uint32_t result, PPCContext& ctx) {
    if (ov) { complete_overlapped(ov, result); ret32(ctx, kErrorIoPending); }
    else ret32(ctx, result);
}

uint32_t content_create(uint32_t root, uint32_t data, uint32_t flags, uint32_t disp_out, uint32_t license_out) {
    std::string root_name = guest_cstr(root, 64);
    std::string file = guest_cstr(data + kCdFile, 41);
    uint32_t type = ld32(data + 4);
    if (root_name.empty() || !safe_name(file)) return kErrorInvalidParameter;
    namespace fs = std::filesystem;
    fs::path dir = content_dir() / hex8(type) / file;
    std::error_code ec;
    bool exists = fs::is_directory(dir, ec);
    bool created = false;
    switch (flags & 0xFF) {
        case kCreateNew: if (exists) return kErrorAlreadyExists; created = true; break;
        case kCreateAlways: case kTruncateExisting:
            if (exists && (flags & 0xFF) == kCreateAlways) fs::remove_all(dir, ec);
            if (!exists && (flags & 0xFF) == kTruncateExisting) return kErrorFileNotFound;
            created = !exists; break;
        case kOpenExisting: if (!exists) return kErrorFileNotFound; break;
        case kOpenAlways: created = !exists; break;
        default: if (!exists) return kErrorFileNotFound; break;
    }
    if (!exists || created) {
        fs::create_directories(dir, ec);
        if (ec) return 5;  // ERROR_ACCESS_DENIED
        std::ofstream m(dir.string() + ".meta", std::ios::binary | std::ios::trunc);
        m.write(reinterpret_cast<const char*>(host(data + kCdName)), 256);
    }
    fa::hle::services().vfs.mount(root_name + ":", fa::hle::make_host_device(dir.string(), false));
    if (disp_out) st32(disp_out, created ? 1 : 2);
    if (license_out) st32(license_out, 0xFFFFFFFF);
    std::fprintf(stderr, "Content: %s %s as %s: (type %08X)\n", created ? "created" : "opened",
                 file.c_str(), root_name.c_str(), type);
    return 0;
}
}  // namespace

// (content_type, content_flags, max_items, buffer_size*, enum_handle*)
FA_IMPORT(XamContentCreateDeviceEnumerator) {
    uint32_t items = arg32(ctx, 2), pcb = arg32(ctx, 3), ph = arg32(ctx, 4);
    if (!ph || !items) { ret32(ctx, kErrorInvalidParameter); return; }
    Enumerator e;
    e.devices = true;
    e.per_call = items;
    e.record_size = kDeviceDataSize;
    e.items.push_back(device_record());
    if (pcb) st32(pcb, items * kDeviceDataSize);
    st32(ph, new_enum(std::move(e)));
    ret32(ctx, 0);
}

// (user_index, device_id, content_type, content_flags, items_per_enumerate,
//  buffer_size*, enum_handle*)
FA_IMPORT(XamContentCreateEnumerator) {
    uint32_t dev = arg32(ctx, 1), type = arg32(ctx, 2), items = arg32(ctx, 4);
    uint32_t pcb = arg32(ctx, 5), ph = arg32(ctx, 6);
    if (!ph || !items) { ret32(ctx, kErrorInvalidParameter); return; }
    Enumerator e;
    e.per_call = items;
    e.record_size = kXContentDataSize;
    if (dev == 0 || dev == kDeviceId) e.items = scan_content(type);
    if (pcb) st32(pcb, items * kXContentDataSize);
    st32(ph, new_enum(std::move(e)));
    ret32(ctx, 0);
}

// (handle, flags, buffer, buffer_size, item_count*, overlapped*)
FA_IMPORT(XamEnumerate) {
    uint32_t h = arg32(ctx, 0), buf = arg32(ctx, 2), count_out = arg32(ctx, 4), ov = arg32(ctx, 5);
    uint32_t n = 0, result = kErrorNoMoreFiles;
    {
        std::lock_guard<std::mutex> lk(g_cmu);
        auto it = g_enums.find(h);
        if (it != g_enums.end() && buf) {
            Enumerator& e = it->second;
            while (n < e.per_call && e.next < e.items.size()) {
                std::memcpy(host(buf + n * e.record_size), e.items[e.next].data(), e.record_size);
                ++e.next; ++n;
            }
            if (n) result = 0;
        }
    }
    if (ov) {
        complete_overlapped(ov, result);
        st32(ov + 4, n);
        ret32(ctx, kErrorIoPending);
    } else {
        if (count_out) st32(count_out, n);
        ret32(ctx, result);
    }
}

// (device_id, XDEVICE_DATA*)
FA_IMPORT(XamContentGetDeviceData) {
    uint32_t id = arg32(ctx, 0), out = arg32(ctx, 1);
    if (id != kDeviceId) { ret32(ctx, kErrorDeviceNotConnected); return; }
    if (!out) { ret32(ctx, kErrorInvalidParameter); return; }
    auto r = device_record();
    std::memcpy(host(out), r.data(), r.size());
    ret32(ctx, 0);
}

// (device_id, wchar* name, name_chars)
FA_IMPORT(XamContentGetDeviceName) {
    uint32_t id = arg32(ctx, 0), out = arg32(ctx, 1), chars = arg32(ctx, 2);
    if (id != kDeviceId) { ret32(ctx, kErrorDeviceNotConnected); return; }
    if (!out || !chars) { ret32(ctx, kErrorInvalidParameter); return; }
    size_t n = std::strlen(kDeviceName);
    if (n + 1 > chars) n = chars - 1;
    for (size_t i = 0; i < n; ++i) st16(out + uint32_t(i) * 2, uint8_t(kDeviceName[i]));
    st16(out + uint32_t(n) * 2, 0);
    ret32(ctx, 0);
}

// (device_id, overlapped*)
FA_IMPORT(XamContentGetDeviceState) {
    uint32_t id = arg32(ctx, 0), ov = arg32(ctx, 1);
    finish(ov, id == kDeviceId ? 0 : kErrorDeviceNotConnected, ctx);
}

// (user_index, root_name, XCONTENT_DATA*, flags, disposition*, license_mask*, overlapped*)
FA_IMPORT(XamContentCreate) {
    uint32_t ov = arg32(ctx, 6);
    finish(ov, content_create(arg32(ctx, 1), arg32(ctx, 2), arg32(ctx, 3), arg32(ctx, 4), arg32(ctx, 5)), ctx);
}

// (user_index, root_name, XCONTENT_DATA*, flags, disposition*, license_mask*,
//  cache_size, content_size (u64), overlapped*)
FA_IMPORT(XamContentCreateEx) {
    uint32_t ov = arg32(ctx, 8);
    finish(ov, content_create(arg32(ctx, 1), arg32(ctx, 2), arg32(ctx, 3), arg32(ctx, 4), arg32(ctx, 5)), ctx);
}

// (root_name, overlapped*)
FA_IMPORT(XamContentClose) {
    std::string root = guest_cstr(arg32(ctx, 0), 64);
    if (!root.empty()) fa::hle::services().vfs.unmount(root + ":");
    finish(arg32(ctx, 1), 0, ctx);
}

// (root_name, overlapped*): files are written through directly, nothing to flush.
FA_IMPORT(XamContentFlush) { finish(arg32(ctx, 1), 0, ctx); }

// (user_index, XCONTENT_DATA*, overlapped*)
FA_IMPORT(XamContentDelete) {
    uint32_t data = arg32(ctx, 1);
    std::string file = guest_cstr(data + kCdFile, 41);
    uint32_t r = 0;
    if (!safe_name(file)) r = kErrorInvalidParameter;
    else {
        std::filesystem::path dir = content_dir() / hex8(ld32(data + 4)) / file;
        std::error_code ec;
        if (!std::filesystem::remove_all(dir, ec)) r = kErrorFileNotFound;
        std::filesystem::remove(dir.string() + ".meta", ec);
    }
    finish(arg32(ctx, 2), r, ctx);
}

// (user_index, XCONTENT_DATA*, is_creator*, creator_xuid*, overlapped*): the local player owns all saves.
FA_IMPORT(XamContentGetCreator) {
    uint32_t is_creator = arg32(ctx, 2), xuid = arg32(ctx, 3);
    if (is_creator) st32(is_creator, 1);
    if (xuid) st64(xuid, 0xE000000000000001ull);
    finish(arg32(ctx, 4), 0, ctx);
}

// (user_index, content_type, content_flags, min_free_bytes (u64), device_id*, overlapped*)
// Auto-selects the single virtual device instead of showing UI.
FA_IMPORT(XamShowDeviceSelectorUI) {
    uint32_t out = arg32(ctx, 4), ov = arg32(ctx, 5);
    if (!out) { ret32(ctx, kErrorInvalidParameter); return; }
    st32(out, kDeviceId);
    finish(ov, 0, ctx);
}

// ---- wide printf --------------------------------------------------------------------
// Guest va_list is a pointer to 8-byte big-endian slots; 32-bit values sit in the low half.
namespace {

struct VaList {
    uint32_t p;
    uint64_t next() { uint64_t v = ld64(p); p += 8; return v; }
};

// Formats the guest wide string `fmt` into `out` (host UTF-16 code units). Returns false
// for a null format.
bool format_wide(uint32_t fmt, uint32_t va, std::u16string& out) {
    if (!fmt) return false;
    VaList args{va};
    auto narrow_arg = [&](uint32_t s, int prec) {
        std::u16string r;
        if (!s) { for (char c : std::string("(null)")) r += char16_t(c); return r; }
        for (int i = 0; (prec < 0 || i < prec) && ld8(s + i); ++i) r += char16_t(ld8(s + i));
        return r;
    };
    auto wide_arg = [&](uint32_t s, int prec) {
        std::u16string r;
        if (!s) { for (char c : std::string("(null)")) r += char16_t(c); return r; }
        for (int i = 0; (prec < 0 || i < prec) && ld16(s + i * 2); ++i) r += char16_t(ld16(s + i * 2));
        return r;
    };
    auto W = [&](uint32_t n) -> uint16_t { return ld16(fmt + n * 2); };
    for (uint32_t i = 0; W(i);) {
        uint16_t c = W(i++);
        if (c != '%') { out += char16_t(c); continue; }
        if (W(i) == '%') { out += u'%'; ++i; continue; }
        std::string flags;
        int width = -1, prec = -1;
        for (;; ++i) {
            uint16_t f = W(i);
            if (f == '-' || f == '+' || f == ' ' || f == '#' || f == '0') flags += char(f); else break;
        }
        if (W(i) == '*') {
            width = int32_t(args.next()); ++i;
            if (width < 0) { flags += '-'; width = -width; }
        } else if (W(i) >= '0' && W(i) <= '9') {
            width = 0;
            while (W(i) >= '0' && W(i) <= '9') width = width * 10 + (W(i++) - '0');
        }
        if (W(i) == '.') {
            ++i; prec = 0;
            if (W(i) == '*') { prec = int32_t(args.next()); ++i; }
            else while (W(i) >= '0' && W(i) <= '9') prec = prec * 10 + (W(i++) - '0');
        }
        // Length: h, l, ll, w, L, I64 (I32 and plain I are 32-bit here).
        enum { kDefault, kShort, kLong, kLongLong, kWide } len = kDefault;
        for (;;) {
            uint16_t l = W(i);
            if (l == 'h') { len = kShort; ++i; }
            else if (l == 'l') { ++i; if (W(i) == 'l') { len = kLongLong; ++i; } else len = kLong; }
            else if (l == 'w') { len = kWide; ++i; }
            else if (l == 'L' || l == 'j' || l == 'z' || l == 't') { ++i; }
            else if (l == 'I') {
                ++i;
                if (W(i) == '6' && W(i + 1) == '4') { len = kLongLong; i += 2; }
                else if (W(i) == '3' && W(i + 1) == '2') i += 2;
            } else break;
        }
        uint16_t t = W(i);
        if (!t) break;
        ++i;
        std::u16string piece;
        char buf[512];
        std::string spec = "%" + flags;
        if (width >= 0) spec += std::to_string(width);
        if (prec >= 0) spec += "." + std::to_string(prec);
        auto pad = [&](std::u16string& s) {
            if (width >= 0 && int(s.size()) < width) {
                std::u16string sp(width - s.size(), u' ');
                if (flags.find('-') != std::string::npos) s += sp; else s = sp + s;
            }
        };
        switch (t) {
            case 'd': case 'i': {
                uint64_t v = args.next();
                int64_t sv = len == kLongLong ? int64_t(v) : int64_t(int32_t(v));
                if (len == kShort) sv = int16_t(v);
                std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), (long long)sv);
                break;
            }
            case 'u': case 'x': case 'X': case 'o': {
                uint64_t v = args.next();
                if (len == kShort) v = uint16_t(v);
                else if (len != kLongLong) v = uint32_t(v);
                std::snprintf(buf, sizeof buf, (spec + "ll" + char(t)).c_str(), (unsigned long long)v);
                break;
            }
            case 'p':
                std::snprintf(buf, sizeof buf, "%08X", uint32_t(args.next()));
                break;
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                uint64_t bits = args.next();
                double d;
                std::memcpy(&d, &bits, 8);
                std::snprintf(buf, sizeof buf, (spec + char(t)).c_str(), d);
                break;
            }
            case 'c': case 'C': {
                uint32_t v = uint32_t(args.next());
                // %c is wide, %C narrow, unless overridden by h/l/w.
                (void)len;
                piece += char16_t(t == 'c' && len != kShort ? uint16_t(v) : uint8_t(v));
                pad(piece);
                out += piece;
                continue;
            }
            case 's': case 'S': {
                uint32_t s = uint32_t(args.next());
                bool is_wide = (t == 's') ? (len != kShort) : (len == kLong || len == kWide);
                piece = is_wide ? wide_arg(s, prec) : narrow_arg(s, prec);
                pad(piece);
                out += piece;
                continue;
            }
            case 'n': args.next(); continue;
            default:
                // Unknown conversion: emit it verbatim.
                out += u'%'; out += char16_t(t);
                continue;
        }
        for (const char* q = buf; *q; ++q) out += char16_t(uint8_t(*q));
    }
    return true;
}

}  // namespace

// _vscwprintf(format, va_list): length of the formatted string, without the terminator.
FA_IMPORT(_vscwprintf) {
    std::u16string s;
    if (!format_wide(arg32(ctx, 0), arg32(ctx, 1), s)) { ret32(ctx, uint32_t(-1)); return; }
    ret32(ctx, uint32_t(s.size()));
}

// vswprintf(buffer, format, va_list): no size argument on this CRT.
FA_IMPORT(vswprintf) {
    uint32_t dst = arg32(ctx, 0);
    std::u16string s;
    if (!dst || !format_wide(arg32(ctx, 1), arg32(ctx, 2), s)) { ret32(ctx, uint32_t(-1)); return; }
    for (size_t i = 0; i < s.size(); ++i) st16(dst + uint32_t(i) * 2, s[i]);
    st16(dst + uint32_t(s.size()) * 2, 0);
    ret32(ctx, uint32_t(s.size()));
}
