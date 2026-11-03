#pragma once
// One place that owns the HLE services for the running game.
//
// Startup order (runtime): create guest memory, then
//   fa::hle::init_services(cfg, hooks);
// then run the game. The kernel layer uses services().vfs for Nt* file calls,
// the import functions in runtime/src/audio and runtime/src/input use
// .audio / .xma / .input, and the window thread calls services().input.pump()
// once per frame.
#include <string>

#include "fa/hle/audio.hpp"
#include "fa/hle/input.hpp"
#include "fa/hle/vfs.hpp"

namespace fa::hle {

struct ServicesConfig {
    std::string game_path;      // game ISO, or a folder with the extracted disc
    std::string data_dir;       // empty: default_data_dir()
    std::string config_dir;     // empty: default_config_dir()
    bool audio_output = true;   // false: silent clock (headless runs, CI)
};

struct Services {
    Vfs vfs;
    Input input;
    AudioSystem audio;
    XmaDecoder xma;
    ServicesConfig config;
};

Services& services();

// Mounts the game, opens audio and input, starts the XMA worker.
// Returns false (with err) only if the game cannot be opened; audio and input
// failures are logged and the game runs without them.
bool init_services(const ServicesConfig& cfg, const GuestHooks& hooks, std::string& err);
void shutdown_services();

}  // namespace fa::hle
