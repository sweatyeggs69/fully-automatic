#pragma once
// XACT wave bank (.xwb) reader, enough to list and extract entries.
// Layout per version follows vgmstream's notes (src/meta/xwb.c). Full Auto
// ships early XACT 2 banks (tool version <= 21) with XMA1 audio.
#include <cstdint>
#include <string>
#include <vector>

namespace fa::hle {

struct XwbEntry {
    enum class Codec { Pcm, Xma, Adpcm, Wma, Unknown } codec = Codec::Unknown;
    int channels = 0;
    int sample_rate = 0;
    int bits_per_sample = 16;
    int block_align = 0;
    uint32_t flags = 0;
    uint32_t num_samples = 0;     // 0 if not stored
    uint32_t offset = 0;          // absolute offset in the bank file
    uint32_t size = 0;
    uint32_t loop_start = 0;      // raw loop fields (bytes/bits or samples, version dependent)
    uint32_t loop_end = 0;
    std::string name;             // empty if the bank has no names
};

struct XwbBank {
    bool big_endian = false;
    uint32_t version = 0;
    uint32_t flags = 0;
    std::string name;
    std::vector<XwbEntry> entries;
};

// Parses a whole bank held in memory. Returns false and sets err on failure.
bool parse_xwb(const std::vector<uint8_t>& data, XwbBank& out, std::string& err);

const char* codec_name(XwbEntry::Codec c);

// Writes a 16-bit PCM WAV file.
bool write_wav(const std::string& path, const std::vector<int16_t>& pcm, int channels, int sample_rate);

}  // namespace fa::hle
