// Content (save game) device and enumeration stubs, plus the wide printf helpers.
//
// There is one virtual storage device (id 1) backed by hdd:. No content packages exist yet,
// so enumerations are empty; the game then offers to create a new save, which will matter
// once XamContentCreate maps onto hdd:.
#include "fa/rt/import.hpp"
#include "fa/rt/kernel.hpp"
#include "fa/rt/memory.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

using namespace fa::rt;

void complete_overlapped(uint32_t ov, uint32_t result);  // imports_xmsg.cpp

namespace {
constexpr uint32_t kErrorIoPending = 997;
constexpr uint32_t kXContentDataSize = 0x134;  // sizeof(XCONTENT_DATA)
constexpr uint32_t kDeviceId = 1;
}  // namespace

// (user_index, device_id, content_type, content_flags, items_per_enumerate,
//  buffer_size*, enum_handle*)
// Real Xbox gives r3=user r4=device r5=type; the buffer size is items * sizeof(XCONTENT_DATA).
FA_IMPORT(XamContentCreateEnumerator) {
    uint32_t items = arg32(ctx, 4), pcb = arg32(ctx, 5), ph = arg32(ctx, 6);
    if (!ph) { ret32(ctx, 87); return; }  // ERROR_INVALID_PARAMETER
    if (pcb) st32(pcb, items * kXContentDataSize);
    st32(ph, g_kernel.add_handle(std::make_shared<KEvent>(true, false)));
    ret32(ctx, 0);
}

// (user_index, content_type, content_flags, min_free_bytes (u64), device_id*, overlapped*)
// Auto-selects the single virtual device instead of showing UI.
FA_IMPORT(XamShowDeviceSelectorUI) {
    uint32_t out = arg32(ctx, 4), ov = arg32(ctx, 5);
    if (!out) { ret32(ctx, 87); return; }
    st32(out, kDeviceId);
    if (ov) {
        complete_overlapped(ov, 0);
        ret32(ctx, kErrorIoPending);
    } else {
        ret32(ctx, 0);
    }
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
