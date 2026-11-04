// xam input imports: XamInputGetState and friends, backed by fa::hle::Input
// (SDL2 game controllers plus a keyboard for player 1).
#include "fa/hle/services.hpp"
#include "fa/rt/import.hpp"

using namespace fa::rt;
using fa::hle::services;
namespace xi = fa::hle::xinput;

namespace {
constexpr uint32_t kErrorBadArguments = 0xA0;

// XINPUT_GAMEPAD: buttons u16, LT u8, RT u8, LX, LY, RX, RY s16 (big-endian).
void store_gamepad(uint32_t a, const fa::hle::XInputGamepad& g) {
    st16(a + 0, g.buttons);
    st8(a + 2, g.left_trigger);
    st8(a + 3, g.right_trigger);
    st16(a + 4, uint16_t(g.thumb_lx));
    st16(a + 6, uint16_t(g.thumb_ly));
    st16(a + 8, uint16_t(g.thumb_rx));
    st16(a + 10, uint16_t(g.thumb_ry));
}

bool gamepad_requested(uint32_t flags) { return !(flags & 0xFF) || (flags & xi::kFlagGamepad); }

uint32_t get_caps(uint32_t user, uint32_t flags, uint32_t out) {
    if (!out) return kErrorBadArguments;
    if (!gamepad_requested(flags)) return xi::kErrorDeviceNotConnected;
    fa::hle::XInputCapabilities c;
    uint32_t rc = services().input.get_capabilities(user, c);
    if (rc) return rc;
    st8(out + 0, c.type);
    st8(out + 1, c.sub_type);
    st16(out + 2, c.flags);
    store_gamepad(out + 4, c.gamepad);
    st16(out + 16, c.vibration.left_motor);
    st16(out + 18, c.vibration.right_motor);
    return 0;
}
}  // namespace

// XamInputGetCapabilities(user, flags, XINPUT_CAPABILITIES*)
FA_IMPORT(XamInputGetCapabilities) { ret32(ctx, get_caps(arg32(ctx, 0), arg32(ctx, 1), arg32(ctx, 2))); }

// XamInputGetCapabilitiesEx(unknown, user, flags, XINPUT_CAPABILITIES*)
FA_IMPORT(XamInputGetCapabilitiesEx) { ret32(ctx, get_caps(arg32(ctx, 1), arg32(ctx, 2), arg32(ctx, 3))); }

// XamInputGetState(user, flags, XINPUT_STATE*)
FA_IMPORT(XamInputGetState) {
    uint32_t user = arg32(ctx, 0), flags = arg32(ctx, 1), out = arg32(ctx, 2);
    if (!out) return ret32(ctx, kErrorBadArguments);
    if (!gamepad_requested(flags)) return ret32(ctx, xi::kErrorDeviceNotConnected);
    fa::hle::XInputState s;
    uint32_t rc = services().input.get_state(user, s);
    if (rc == 0) {
        st32(out, s.packet_number);
        store_gamepad(out + 4, s.gamepad);
    }
    ret32(ctx, rc);
}

// XamInputSetState(user, unknown, XINPUT_VIBRATION*)
FA_IMPORT(XamInputSetState) {
    uint32_t user = arg32(ctx, 0), vib = arg32(ctx, 2);
    if (!vib) return ret32(ctx, kErrorBadArguments);
    fa::hle::XInputVibration v{ld16(vib), ld16(vib + 2)};
    ret32(ctx, services().input.set_vibration(user, v));
}

// XamInputGetKeystroke(user, flags, XINPUT_KEYSTROKE*)
uint32_t get_keystroke(uint32_t user, uint32_t out) {
    if (!out) return kErrorBadArguments;
    fa::hle::XInputKeystroke k;
    uint32_t rc = services().input.get_keystroke(user, k);
    if (rc == 0) {
        st16(out + 0, k.virtual_key);
        st16(out + 2, k.unicode);
        st16(out + 4, k.flags);
        st8(out + 6, k.user_index);
        st8(out + 7, k.hid_code);
    }
    return rc;
}

FA_IMPORT(XamInputGetKeystroke) { ret32(ctx, get_keystroke(arg32(ctx, 0), arg32(ctx, 2))); }

// XamInputGetKeystrokeEx(DWORD* user_index in/out, flags, XINPUT_KEYSTROKE*)
FA_IMPORT(XamInputGetKeystrokeEx) {
    uint32_t user_ptr = arg32(ctx, 0), out = arg32(ctx, 2);
    uint32_t user = user_ptr ? ld32(user_ptr) : xi::kUserAny;
    uint32_t rc = get_keystroke(user, out);
    if (rc == 0 && user_ptr) st32(user_ptr, ld8(out + 6));
    ret32(ctx, rc);
}
