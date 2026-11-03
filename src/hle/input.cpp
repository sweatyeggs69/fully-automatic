#include "fa/hle/input.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <SDL.h>

namespace fa::hle {

namespace {

std::string trim(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    return s.substr(i);
}

std::string upper(std::string s) {
    for (auto& c : s) c = char(std::toupper((unsigned char)c));
    return s;
}

bool parse_target(const std::string& t, KeyBinding& b) {
    using K = KeyBinding::Kind;
    static const std::pair<const char*, uint16_t> buttons[] = {
        {"A", xinput::kA}, {"B", xinput::kB}, {"X", xinput::kX}, {"Y", xinput::kY},
        {"LB", xinput::kLeftShoulder}, {"RB", xinput::kRightShoulder},
        {"START", xinput::kStart}, {"BACK", xinput::kBack}, {"GUIDE", xinput::kGuide},
        {"LS", xinput::kLeftThumb}, {"RS", xinput::kRightThumb},
        {"UP", xinput::kDpadUp}, {"DOWN", xinput::kDpadDown},
        {"LEFT", xinput::kDpadLeft}, {"RIGHT", xinput::kDpadRight},
    };
    std::string u = upper(t);
    for (auto& [n, bit] : buttons)
        if (u == n) {
            b.kind = K::Button;
            b.button = bit;
            return true;
        }
    if (u == "LT") { b.kind = K::LeftTrigger; return true; }
    if (u == "RT") { b.kind = K::RightTrigger; return true; }
    static const char* axes[] = {"LX", "LY", "RX", "RY"};
    for (int i = 0; i < 4; ++i) {
        if (u.size() == 3 && u.compare(0, 2, axes[i]) == 0 && (u[2] == '+' || u[2] == '-')) {
            b.kind = K::Axis;
            b.axis = i;
            b.value = u[2] == '+' ? 32767 : -32768;
            return true;
        }
    }
    return false;
}

int16_t clamp16(int v) { return int16_t(std::clamp(v, -32768, 32767)); }

}  // namespace

const char* Input::default_bindings_text() {
    return
        "# Keyboard bindings for player 1, used alongside any connected controller.\n"
        "# Format: <SDL key name> = <target>\n"
        "# Targets: A B X Y LB RB LT RT START BACK GUIDE LS RS UP DOWN LEFT RIGHT\n"
        "#          LX- LX+ LY- LY+ RX- RX+ RY- RY+   (stick directions, Y+ is up)\n"
        "W = RT\n"
        "S = LT\n"
        "A = LX-\n"
        "D = LX+\n"
        "Q = LB\n"
        "E = RB\n"
        "Space = A\n"
        "Left Shift = B\n"
        "F = X\n"
        "R = Y\n"
        "C = LS\n"
        "V = RS\n"
        "Return = START\n"
        "Escape = BACK\n"
        "Backspace = BACK\n"
        "Up = UP\n"
        "Down = DOWN\n"
        "Left = LEFT\n"
        "Right = RIGHT\n"
        "I = RY+\n"
        "K = RY-\n"
        "J = RX-\n"
        "L = RX+\n";
}

std::vector<std::pair<int, KeyBinding>> Input::parse_bindings(const std::string& text,
                                                              std::vector<std::string>& errors) {
    std::vector<std::pair<int, KeyBinding>> out;
    std::istringstream in(text);
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        line = trim(line);
        if (line.empty()) continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) {
            errors.push_back("line " + std::to_string(lineno) + ": missing '='");
            continue;
        }
        std::string key = trim(line.substr(0, eq)), target = trim(line.substr(eq + 1));
        SDL_Scancode sc = SDL_GetScancodeFromName(key.c_str());
        KeyBinding b;
        if (sc == SDL_SCANCODE_UNKNOWN) {
            errors.push_back("line " + std::to_string(lineno) + ": unknown key '" + key + "'");
            continue;
        }
        if (!parse_target(target, b)) {
            errors.push_back("line " + std::to_string(lineno) + ": unknown target '" + target + "'");
            continue;
        }
        out.emplace_back(int(sc), b);
    }
    return out;
}

void Input::apply_keys(const std::vector<std::pair<int, KeyBinding>>& binds, const uint8_t* keys, int nkeys,
                       XInputGamepad& pad) {
    int axis[4] = {pad.thumb_lx, pad.thumb_ly, pad.thumb_rx, pad.thumb_ry};
    bool axis_touched[4] = {};
    int key_axis[4] = {};
    for (auto& [sc, b] : binds) {
        if (sc < 0 || sc >= nkeys || !keys[sc]) continue;
        switch (b.kind) {
            case KeyBinding::Kind::Button: pad.buttons |= b.button; break;
            case KeyBinding::Kind::LeftTrigger: pad.left_trigger = 255; break;
            case KeyBinding::Kind::RightTrigger: pad.right_trigger = 255; break;
            case KeyBinding::Kind::Axis:
                key_axis[b.axis] += b.value;
                axis_touched[b.axis] = true;
                break;
        }
    }
    // A pressed key overrides the stick on that axis; opposite keys cancel.
    for (int i = 0; i < 4; ++i)
        if (axis_touched[i]) axis[i] = key_axis[i];
    pad.thumb_lx = clamp16(axis[0]);
    pad.thumb_ly = clamp16(axis[1]);
    pad.thumb_rx = clamp16(axis[2]);
    pad.thumb_ry = clamp16(axis[3]);
}

Input::Input() = default;
Input::~Input() { shutdown(); }

bool Input::init(const std::string& config_dir) {
    std::vector<std::string> errors;
    std::string text = default_bindings_text();
    if (!config_dir.empty()) {
        std::string path = config_dir + "/input.ini";
        std::ifstream f(path);
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            text = ss.str();
        } else {
            std::error_code ec;
            std::filesystem::create_directories(config_dir, ec);
            std::ofstream o(path);
            if (o) o << default_bindings_text();
        }
    }
    binds_ = parse_bindings(text, errors);
    for (auto& e : errors) std::fprintf(stderr, "[input] input.ini %s\n", e.c_str());

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "[input] SDL game controller init failed: %s\n", SDL_GetError());
        sdl_ok_ = false;
        return false;
    }
    sdl_ok_ = true;
    std::lock_guard lk(mu_);
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i)) open_pad(i);
    return true;
}

void Input::shutdown() {
    if (!sdl_ok_) return;
    {
        std::lock_guard lk(mu_);
        for (auto& s : slots_) {
            if (s.pad) SDL_GameControllerClose(s.pad);
            s = Slot{};
        }
    }
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    sdl_ok_ = false;
}

void Input::open_pad(int device_index) {
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device_index);
    for (auto& s : slots_)
        if (s.instance_id == id) return;
    for (int u = 0; u < xinput::kMaxUsers; ++u) {
        auto& s = slots_[u];
        if (s.pad) continue;
        s.pad = SDL_GameControllerOpen(device_index);
        if (!s.pad) return;
        s.instance_id = id;
        SDL_GameControllerSetPlayerIndex(s.pad, u);
        std::fprintf(stderr, "[input] player %d: %s\n", u + 1, SDL_GameControllerName(s.pad));
        return;
    }
}

void Input::close_pad(int instance_id) {
    for (int u = 0; u < xinput::kMaxUsers; ++u) {
        auto& s = slots_[u];
        if (s.instance_id != instance_id) continue;
        SDL_GameControllerClose(s.pad);
        std::fprintf(stderr, "[input] player %d disconnected\n", u + 1);
        uint32_t pn = s.state.packet_number;
        s = Slot{};
        s.state.packet_number = pn + 1;
    }
}

void Input::handle_event(const SDL_Event& e) {
    std::lock_guard lk(mu_);
    if (e.type == SDL_CONTROLLERDEVICEADDED) open_pad(e.cdevice.which);
    else if (e.type == SDL_CONTROLLERDEVICEREMOVED) close_pad(e.cdevice.which);
}

void Input::refresh() {
    // Caller holds mu_.
    uint32_t now = SDL_GetTicks();
    for (int u = 0; u < xinput::kMaxUsers; ++u) {
        auto& s = slots_[u];
        XInputGamepad g;
        if (s.pad) {
            auto btn = [&](SDL_GameControllerButton b, uint16_t bit) {
                if (SDL_GameControllerGetButton(s.pad, b)) g.buttons |= bit;
            };
            btn(SDL_CONTROLLER_BUTTON_DPAD_UP, xinput::kDpadUp);
            btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN, xinput::kDpadDown);
            btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT, xinput::kDpadLeft);
            btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, xinput::kDpadRight);
            btn(SDL_CONTROLLER_BUTTON_START, xinput::kStart);
            btn(SDL_CONTROLLER_BUTTON_BACK, xinput::kBack);
            btn(SDL_CONTROLLER_BUTTON_LEFTSTICK, xinput::kLeftThumb);
            btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK, xinput::kRightThumb);
            btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, xinput::kLeftShoulder);
            btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, xinput::kRightShoulder);
            btn(SDL_CONTROLLER_BUTTON_GUIDE, xinput::kGuide);
            btn(SDL_CONTROLLER_BUTTON_A, xinput::kA);
            btn(SDL_CONTROLLER_BUTTON_B, xinput::kB);
            btn(SDL_CONTROLLER_BUTTON_X, xinput::kX);
            btn(SDL_CONTROLLER_BUTTON_Y, xinput::kY);
            auto ax = [&](SDL_GameControllerAxis a) { return int(SDL_GameControllerGetAxis(s.pad, a)); };
            auto trig = [&](SDL_GameControllerAxis a) { return uint8_t(std::clamp(ax(a), 0, 32767) * 255 / 32767); };
            g.left_trigger = trig(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
            g.right_trigger = trig(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
            g.thumb_lx = clamp16(ax(SDL_CONTROLLER_AXIS_LEFTX));
            g.thumb_ly = clamp16(-1 - ax(SDL_CONTROLLER_AXIS_LEFTY));  // SDL down is +, XInput up is +
            g.thumb_rx = clamp16(ax(SDL_CONTROLLER_AXIS_RIGHTX));
            g.thumb_ry = clamp16(-1 - ax(SDL_CONTROLLER_AXIS_RIGHTY));

            // SDL2 rumble needs a duration; refresh it while the game holds it on.
            if ((s.rumble.left_motor || s.rumble.right_motor) && now >= s.rumble_refresh_ms) {
                SDL_GameControllerRumble(s.pad, s.rumble.left_motor, s.rumble.right_motor, 1000);
                s.rumble_refresh_ms = now + 500;
            }
        }
        if (u == 0 && keyboard_) {
            int n = 0;
            const uint8_t* keys = SDL_GetKeyboardState(&n);
            if (keys) apply_keys(binds_, keys, n, g);
        }
        if (!(g == s.state.gamepad)) {
            s.state.gamepad = g;
            s.state.packet_number++;
        }
    }
}

void Input::pump() {
    if (!sdl_ok_) return;
    SDL_GameControllerUpdate();
    std::lock_guard lk(mu_);
    // Pick up hotplugged pads even when nobody forwards SDL events.
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i)) open_pad(i);
    for (auto& s : slots_)
        if (s.pad && !SDL_GameControllerGetAttached(s.pad)) close_pad(s.instance_id);
    refresh();
}

bool Input::connected(uint32_t user) {
    if (user == xinput::kUserAny) user = 0;
    if (user >= xinput::kMaxUsers) return false;
    std::lock_guard lk(mu_);
    // Player 1 always exists: the keyboard stands in for a missing controller.
    return slots_[user].pad != nullptr || (user == 0 && keyboard_);
}

uint32_t Input::get_state(uint32_t user, XInputState& out) {
    if (self_pump_ && sdl_ok_) {
        uint32_t now = SDL_GetTicks();
        if (now - last_self_pump_ms_ >= 4) {
            last_self_pump_ms_ = now;
            // Event pumping belongs to the window thread once video is up.
            if (!SDL_WasInit(SDL_INIT_VIDEO)) SDL_PumpEvents();
            pump();
        }
    }
    if (user == xinput::kUserAny) user = 0;
    if (!connected(user)) return xinput::kErrorDeviceNotConnected;
    std::lock_guard lk(mu_);
    out = slots_[user].state;
    return xinput::kErrorSuccess;
}

uint32_t Input::get_capabilities(uint32_t user, XInputCapabilities& out) {
    if (user == xinput::kUserAny) user = 0;
    if (!connected(user)) return xinput::kErrorDeviceNotConnected;
    out = XInputCapabilities{};
    out.gamepad.buttons = 0xF7FF & ~0x0400;  // all buttons except guide
    out.gamepad.left_trigger = out.gamepad.right_trigger = 0xFF;
    out.gamepad.thumb_lx = out.gamepad.thumb_ly = out.gamepad.thumb_rx = out.gamepad.thumb_ry = int16_t(0xFFC0);
    out.vibration.left_motor = out.vibration.right_motor = 0xFFFF;
    return xinput::kErrorSuccess;
}

uint32_t Input::set_vibration(uint32_t user, const XInputVibration& v) {
    if (user == xinput::kUserAny) user = 0;
    if (!connected(user)) return xinput::kErrorDeviceNotConnected;
    std::lock_guard lk(mu_);
    auto& s = slots_[user];
    s.rumble = v;
    if (s.pad) {
        SDL_GameControllerRumble(s.pad, v.left_motor, v.right_motor, (v.left_motor || v.right_motor) ? 1000 : 0);
        s.rumble_refresh_ms = SDL_GetTicks() + 500;
    }
    return xinput::kErrorSuccess;
}

}  // namespace fa::hle
