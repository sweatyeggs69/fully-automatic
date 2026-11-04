// XMsg: messages from the title to the system "apps" (music player, XGI stats/sessions,
// Live base). Modelled on Xenia's app handlers: messages complete synchronously, and when the
// caller passes an XOVERLAPPED it is completed at once and the call returns ERROR_IO_PENDING.
#include "fa/rt/import.hpp"
#include "fa/rt/kernel.hpp"
#include "fa/rt/memory.hpp"

#include <cstdio>
#include <mutex>
#include <set>
#include <utility>

using namespace fa::rt;

namespace {

constexpr uint32_t X_E_SUCCESS = 0;
constexpr uint32_t X_E_FAIL = 0x80004005;
constexpr uint32_t X_E_INVALIDARG = 0x80070057;
constexpr uint32_t X_ERROR_IO_PENDING = 997;

constexpr uint32_t kAppXmp = 0xFA;       // music player
constexpr uint32_t kAppXgi = 0xFB;       // contexts, properties, sessions, achievements
constexpr uint32_t kAppXLiveBase = 0xFC;

// Logs each (app, message) pair once.
void log_message(uint32_t app, uint32_t msg, uint32_t buf, uint32_t len, uint32_t result) {
    static std::mutex mu;
    static std::set<std::pair<uint32_t, uint32_t>> seen;
    std::lock_guard<std::mutex> lk(mu);
    if (!seen.insert({app, msg}).second) return;
    std::fprintf(stderr, "XMsg: app %02X message %08X (buffer %08X, %u bytes) -> %08X\n", app, msg, buf, len, result);
}

// Title-side music player state. Nothing plays; the title keeps control of the music.
struct Xmp {
    uint32_t volume = 0x3F800000;  // 1.0f
    uint32_t playback_mode = 0, repeat_mode = 0, flags = 0;
} g_xmp;

uint32_t xmp_message(uint32_t msg, uint32_t buf) {
    auto arg = [&](int n) { return ld32(buf + n * 4); };
    auto out = [](uint32_t p, uint32_t v) { if (p) st32(p, v); };
    switch (msg) {
        case 0x00070002:  // XMPPlayTitlePlaylist
        case 0x00070003:  // XMPContinue
        case 0x00070004:  // XMPStop
        case 0x00070005:  // XMPPause
        case 0x00070006:  // XMPNext
        case 0x00070007:  // XMPPrevious
        case 0x00070013:  // XMPDeleteTitlePlaylist
        case 0x0007001A:  // XMPSetPlaybackController
            return X_E_SUCCESS;
        case 0x00070008:  // XMPSetPlaybackBehavior(client, mode, repeat, flags)
            g_xmp.playback_mode = arg(1); g_xmp.repeat_mode = arg(2); g_xmp.flags = arg(3);
            return X_E_SUCCESS;
        case 0x00070009:  // XMPGetStatus(client, state*): 0 = stopped
            out(arg(1), 0);
            return X_E_SUCCESS;
        case 0x0007000B:  // XMPGetVolume(client, float*)
            out(arg(1), g_xmp.volume);
            return X_E_SUCCESS;
        case 0x0007000C:  // XMPSetVolume(client, float)
            g_xmp.volume = arg(1);
            return X_E_SUCCESS;
        case 0x0007000D:  // XMPCreateTitlePlaylist(..., playlist_handle* at +32): handle = storage
            out(arg(8), arg(1));
            return X_E_SUCCESS;
        case 0x0007001B:  // XMPGetPlaybackController(client, controller*, locked*): the title
            out(arg(1), 0);
            out(arg(2), 0);
            return X_E_SUCCESS;
        case 0x00070029:  // XMPGetPlaybackBehavior(client, mode*, repeat*, flags*)
            out(arg(1), g_xmp.playback_mode);
            out(arg(2), g_xmp.repeat_mode);
            out(arg(3), g_xmp.flags);
            return X_E_SUCCESS;
        case 0x0007002E:  // size query for XMPCreateTitlePlaylist storage (client, count, size*)
            out(arg(2), 4 + arg(1) * 128);
            return X_E_SUCCESS;
        default:
            return X_E_FAIL;
    }
}

uint32_t xgi_message(uint32_t msg) {
    switch (msg) {
        case 0x000B0006:  // XUserSetContext
        case 0x000B0007:  // XUserSetProperty
        case 0x000B0008:  // XUserWriteAchievements
        case 0x000B0011:  // XSessionDelete
        case 0x000B0012:  // XSessionJoin
        case 0x000B0014:  // XSessionStart
        case 0x000B0015:  // XSessionEnd
        case 0x000B0071:
            return X_E_SUCCESS;
        default:
            return X_E_FAIL;  // includes XSessionCreate: no sessions without Live
    }
}

uint32_t dispatch(uint32_t app, uint32_t msg, uint32_t buf, uint32_t len) {
    uint32_t r;
    switch (app) {
        case kAppXmp: r = buf ? xmp_message(msg, buf) : X_E_FAIL; break;
        case kAppXgi: r = xgi_message(msg); break;
        case kAppXLiveBase: r = X_E_FAIL; break;
        default: r = X_E_INVALIDARG; break;  // no such app
    }
    log_message(app, msg, buf, len, r);
    return r;
}

// XOVERLAPPED {u32 result, length, context, event, completion_routine, completion_context,
// extended_error}. Completion routines are not queued (the game has not been seen to use them).
void complete_overlapped(uint32_t ov, uint32_t result) {
    st32(ov + 0x00, result);
    st32(ov + 0x04, 0);
    st32(ov + 0x18, result);
    if (uint32_t h = ld32(ov + 0x0C)) {
        auto o = g_kernel.get_handle(h);
        if (o && o->type == KObject::Type::Event) {
            std::lock_guard<std::mutex> lk(g_kmu);
            static_cast<KEvent&>(*o).set = true;
            g_kcv.notify_all();
        }
    }
    if (ld32(ov + 0x10)) std::fprintf(stderr, "XMsg: overlapped %08X has a completion routine (not called)\n", ov);
}

uint32_t start_io_request(uint32_t app, uint32_t msg, uint32_t ov, uint32_t buf, uint32_t len) {
    uint32_t r = dispatch(app, msg, buf, len);
    if (ov) {
        complete_overlapped(ov, r);
        r = X_ERROR_IO_PENDING;
    }
    return r;
}

}  // namespace

// (app, message, overlapped, buffer, buffer_length)
FA_IMPORT(XMsgStartIORequest) {
    ret32(ctx, start_io_request(arg32(ctx, 0), arg32(ctx, 1), arg32(ctx, 2), arg32(ctx, 3), arg32(ctx, 4)));
}

// (app, message, overlapped, buffer, buffer_length, unknown*)
FA_IMPORT(XMsgStartIORequestEx) {
    ret32(ctx, start_io_request(arg32(ctx, 0), arg32(ctx, 1), arg32(ctx, 2), arg32(ctx, 3), arg32(ctx, 4)));
}

// Like Xenia, report the oldest system version: titles use it to enable newer dashboard
// features, and pretending to be old keeps them off.
FA_IMPORT(XamGetSystemVersion) { ret32(ctx, 0); }
