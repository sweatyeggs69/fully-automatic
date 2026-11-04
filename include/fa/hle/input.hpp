#pragma once
// Controller input for the recompiled game, XInput style, backed by SDL2.
//
// Host-endian structs that mirror the guest XINPUT_* layouts field for field.
// The Xam import functions (runtime/src/input/xam_input.cpp) byte-swap them
// into guest memory.
//
// Threading: SDL events must be pumped on the thread that owns the window.
// Call Input::pump() from that thread once per frame (or forward events with
// handle_event()). get_state() and friends may be called from any thread and
// return the last pumped snapshot. If nobody pumps (no window yet), call
// set_self_pump(true) and get_state() pumps itself, rate limited.
#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

union SDL_Event;
struct _SDL_GameController;

namespace fa::hle {

namespace xinput {
constexpr uint16_t kDpadUp = 0x0001;
constexpr uint16_t kDpadDown = 0x0002;
constexpr uint16_t kDpadLeft = 0x0004;
constexpr uint16_t kDpadRight = 0x0008;
constexpr uint16_t kStart = 0x0010;
constexpr uint16_t kBack = 0x0020;
constexpr uint16_t kLeftThumb = 0x0040;
constexpr uint16_t kRightThumb = 0x0080;
constexpr uint16_t kLeftShoulder = 0x0100;
constexpr uint16_t kRightShoulder = 0x0200;
constexpr uint16_t kGuide = 0x0400;
constexpr uint16_t kA = 0x1000;
constexpr uint16_t kB = 0x2000;
constexpr uint16_t kX = 0x4000;
constexpr uint16_t kY = 0x8000;

constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorDeviceNotConnected = 0x48F;
constexpr uint32_t kErrorEmpty = 0x10D2;

constexpr uint32_t kFlagGamepad = 0x01;

// XINPUT_KEYSTROKE flags and VK_PAD_* virtual keys.
constexpr uint16_t kKeyDown = 0x0001, kKeyUp = 0x0002, kKeyRepeat = 0x0004;
constexpr uint16_t kVkPadA = 0x5800, kVkPadB = 0x5801, kVkPadX = 0x5802, kVkPadY = 0x5803;
constexpr uint16_t kVkPadRShoulder = 0x5804, kVkPadLShoulder = 0x5805;
constexpr uint16_t kVkPadLTrigger = 0x5806, kVkPadRTrigger = 0x5807;
constexpr uint16_t kVkPadDpadUp = 0x5810, kVkPadDpadDown = 0x5811, kVkPadDpadLeft = 0x5812, kVkPadDpadRight = 0x5813;
constexpr uint16_t kVkPadStart = 0x5814, kVkPadBack = 0x5815, kVkPadLThumbPress = 0x5816, kVkPadRThumbPress = 0x5817;
constexpr uint16_t kVkPadLThumbUp = 0x5820, kVkPadLThumbDown = 0x5821, kVkPadLThumbRight = 0x5822, kVkPadLThumbLeft = 0x5823;
constexpr uint16_t kVkPadRThumbUp = 0x5830, kVkPadRThumbDown = 0x5831, kVkPadRThumbRight = 0x5832, kVkPadRThumbLeft = 0x5833;
constexpr uint32_t kUserAny = 0xFF;
constexpr int kMaxUsers = 4;
}  // namespace xinput

struct XInputGamepad {          // guest XINPUT_GAMEPAD, 12 bytes
    uint16_t buttons = 0;
    uint8_t left_trigger = 0;
    uint8_t right_trigger = 0;
    int16_t thumb_lx = 0;
    int16_t thumb_ly = 0;
    int16_t thumb_rx = 0;
    int16_t thumb_ry = 0;
    bool operator==(const XInputGamepad&) const = default;
};

struct XInputState {            // guest XINPUT_STATE, 16 bytes
    uint32_t packet_number = 0;
    XInputGamepad gamepad;
};

struct XInputVibration {        // guest XINPUT_VIBRATION, 4 bytes
    uint16_t left_motor = 0;    // low frequency
    uint16_t right_motor = 0;   // high frequency
};

struct XInputCapabilities {     // guest XINPUT_CAPABILITIES, 20 bytes
    uint8_t type = 1;           // XINPUT_DEVTYPE_GAMEPAD
    uint8_t sub_type = 1;       // XINPUT_DEVSUBTYPE_GAMEPAD
    uint16_t flags = 0;
    XInputGamepad gamepad;      // which controls exist (all set)
    XInputVibration vibration;  // which motors exist
};

struct XInputKeystroke {        // guest XINPUT_KEYSTROKE, 8 bytes
    uint16_t virtual_key = 0;
    uint16_t unicode = 0;
    uint16_t flags = 0;
    uint8_t user_index = 0;
    uint8_t hid_code = 0;
};

// What a keyboard key drives on the virtual pad of user 0.
struct KeyBinding {
    enum class Kind { Button, LeftTrigger, RightTrigger, Axis } kind = Kind::Button;
    uint16_t button = 0;     // for Button
    int axis = 0;            // 0 LX, 1 LY, 2 RX, 3 RY
    int16_t value = 0;       // axis deflection for Axis
};

class Input {
public:
    Input();
    ~Input();
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    // Initialises the SDL game controller subsystem. Loads key bindings from
    // <config_dir>/input.ini if present (and writes a default one if not).
    bool init(const std::string& config_dir);
    void shutdown();

    void pump();                         // poll SDL and refresh snapshots
    void handle_event(const SDL_Event& e);
    void set_self_pump(bool on) { self_pump_ = on; }

    uint32_t get_state(uint32_t user, XInputState& out);
    uint32_t get_capabilities(uint32_t user, XInputCapabilities& out);
    uint32_t set_vibration(uint32_t user, const XInputVibration& v);
    bool connected(uint32_t user);
    // Pops the next virtual key event (down / repeat / up) for a user (0xFF = any).
    // Returns kErrorEmpty when there is none.
    uint32_t get_keystroke(uint32_t user, XInputKeystroke& out);

    // Exposed for tests: parses an input.ini text into bindings
    // (SDL scancode -> binding). Unknown lines are reported in errors.
    static std::vector<std::pair<int, KeyBinding>> parse_bindings(const std::string& text,
                                                                  std::vector<std::string>& errors);
    static const char* default_bindings_text();
    // Combines a keyboard snapshot into a pad state (exposed for tests).
    static void apply_keys(const std::vector<std::pair<int, KeyBinding>>& binds, const uint8_t* keys,
                           int nkeys, XInputGamepad& pad);

private:
    struct Slot {
        _SDL_GameController* pad = nullptr;
        int instance_id = -1;
        XInputState state;
        XInputVibration rumble;
        uint32_t rumble_refresh_ms = 0;
        // Keystroke generation: virtual keys currently held, next repeat time, pending events.
        uint32_t held_keys = 0;
        uint32_t held_since_ms[24] = {};
        std::deque<XInputKeystroke> keystrokes;
    };
    void generate_keystrokes(int user, Slot& s, uint32_t now);
    void maybe_self_pump();
    void open_pad(int device_index);
    void close_pad(int instance_id);
    void refresh();

    std::mutex mu_;
    std::array<Slot, xinput::kMaxUsers> slots_;
    std::vector<std::pair<int, KeyBinding>> binds_;
    bool sdl_ok_ = false;
    bool self_pump_ = false;
    uint32_t last_self_pump_ms_ = 0;
    bool keyboard_ = true;
};

}  // namespace fa::hle
