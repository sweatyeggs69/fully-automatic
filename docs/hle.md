# HLE services: files, input, audio

Host-side services the recompiled game talks to through its kernel imports.
Library `fa_hle` (`src/hle/`, headers in `include/fa/hle/`), defined in
`cmake/fa_hle.cmake` and used by both the top-level project and `runtime/`.

Dependencies: SDL2 and FFmpeg's libavcodec/libavutil.

    sudo pacman -S --needed sdl2 ffmpeg          # CachyOS / Arch (sdl2 may be provided by sdl2-compat)
    sudo apt install libsdl2-dev libavcodec-dev libavutil-dev   # Debian / Ubuntu

## Startup

`runtime/src/main.cpp` calls `fa::hle::init_services(cfg, fa::rt::make_runtime_hooks(), err)`
right after guest memory exists. After that, `fa::hle::services()` gives `.vfs`, `.input`,
`.audio` and `.xma`.

    fullauto Full.Auto.iso                      # game files and default.xex from the ISO
    fullauto private/default.xex <iso|folder>   # default.xex from disk, game files from an ISO or extracted folder
    FA_NO_AUDIO=1 fullauto ...                  # no sound device: the audio clock runs silently

## Files (`fa::hle::Vfs`)

| Guest path | Backed by |
|---|---|
| `game:\`, `d:\`, `\Device\Cdrom0\` | the ISO (read-only XDVDFS reader) or an extracted folder |
| `hdd:\`, `e:\`, `\Device\Harddisk0\Partition1\` | `~/.local/share/fully-automatic/hdd/` |
| `cache:\`, `z:\`, `\Device\Harddisk0\Cache0\` | `~/.local/share/fully-automatic/cache/` |

Paths are case-insensitive, `\??\` prefixes, `/`, `.` and `..` are handled, and
`Vfs::add_link` covers `ObCreateSymbolicLink`. Results are NTSTATUS codes. The kernel layer
(`NtCreateFile`, `NtReadFile`, `NtQueryDirectoryFile`, ...) owns handles and guest structs and
calls `Vfs::open`, `VfsFile::read/write/next_entry`, `Vfs::stat/volume/remove/rename`.
ISO reads use `pread`, so several game threads can stream at once.

## Input (`fa::hle::Input`, imports in `runtime/src/input/xam_input.cpp`)

XInput-style state from SDL2 game controllers (up to 4 players, hotplug, rumble) plus a
keyboard for player 1. `XamInputGetState`, `XamInputGetCapabilities(Ex)`, `XamInputSetState`,
`XamInputGetKeystroke(Ex)` are implemented.

Keyboard bindings live in `~/.config/fully-automatic/input.ini` (written with defaults on first
run). Default: W accelerate (RT), S brake (LT), A/D steer, Q/E bumpers, Space A, Left Shift B,
F X, R Y, Enter Start, Escape/Backspace Back, arrows D-pad, I/J/K/L right stick, C/V stick clicks.

Until a window exists the input layer pumps SDL itself. Once the window thread runs its event
loop it should call `services().input.set_self_pump(false)` once and `services().input.pump()`
every frame (or forward events with `handle_event`).

## Audio (`fa::hle::AudioSystem`, `fa::hle::XmaDecoder`, imports in `runtime/src/audio/`)

The game's own XAudio/XACT code (recompiled) mixes in software and hands 256-sample, 6-channel
frames to the render driver (`XAudioRegisterRenderDriverClient`, `XAudioSubmitRenderDriverFrame`).
We downmix 5.1 to stereo and play it through SDL2 at 48 kHz. The guest callback runs on a host
thread attached as a guest thread, paced by the sound device (about 43 ms of buffering).

Compressed audio is XMA, decoded by emulated XMA hardware: 320 contexts in guest physical memory,
kicked through the `XMAEnableContext` export or the APU register page at 0x7FEA0000 (polled).
Frames are decoded with FFmpeg's stock WMA Pro decoder (each XMA frame is re-wrapped in a WMA Pro
packet), so no patched FFmpeg is needed. Context layout and semantics follow Xenia's research.

Checked so far: on public Xbox 360 XMA1 voice samples the decoder matches FFmpeg's own XMA decoder
at about 75 dB SNR (rounding only), and the hardware path (rolling 3-packet input buffers, frames
split across buffers, ring buffer wrap) gives bit-identical output to the offline decoder.

## Wave bank tool (`fawave`)

Full Auto uses early XACT 2 wave banks (tool version 21 or lower) with XMA1 audio.

    ./build/fawave Full.Auto.iso banks                                  # every .xwb on the disc
    ./build/fawave Full.Auto.iso list Media/Audio/foo.xwb               # entries of one bank
    ./build/fawave Full.Auto.iso decode Media/Audio/foo.xwb all out/    # to .wav files
    ./build/fawave sample.xma wav out.wav [channels] [rate]             # a RIFF .xma file

`FA_XMA_TRACE=1` prints every frame the decoder walks. `FA_XMA_SAMPLE=/path/file.xma ./build/hle_tests ...`
also runs the hardware-path test on a real stereo XMA file.

## Not done yet

- XMA loop sub-frame precision (loops land on frame boundaries), multi-stream (more than 2 channel) XMA contexts.
- Content/save APIs (`XamContentCreate` etc.) still need mapping onto `hdd:`.
- Keystroke queues (`XamInputGetKeystroke`) always report no input.
