#include "fa/hle/services.hpp"

#include <cstdio>

namespace fa::hle {

Services& services() {
    static Services s;
    return s;
}

bool init_services(const ServicesConfig& in, const GuestHooks& hooks, std::string& err) {
    Services& s = services();
    s.config = in;
    if (s.config.data_dir.empty()) s.config.data_dir = default_data_dir();
    if (s.config.config_dir.empty()) s.config.config_dir = default_config_dir();

    if (!s.vfs.mount_defaults(s.config.game_path, s.config.data_dir, err)) return false;
    if (!s.input.init(s.config.config_dir))
        std::fprintf(stderr, "[hle] controllers unavailable, keyboard only\n");
    // Until a window thread takes over pumping (set_self_pump(false) + pump()
    // each frame), XamInputGetState refreshes the state itself.
    s.input.set_self_pump(true);
    s.audio.init(hooks, !s.config.audio_output);
    if (!s.xma.init(hooks)) std::fprintf(stderr, "[hle] XMA decoder could not allocate its context array\n");
    return true;
}

void shutdown_services() {
    Services& s = services();
    s.audio.shutdown();
    s.xma.shutdown();
    s.input.shutdown();
}

}  // namespace fa::hle
