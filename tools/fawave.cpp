// fawave: list and decode XACT wave banks (.xwb), straight from the game ISO.
// Used to check the XMA decoder against the game's real audio.
//
//   fawave <game.iso|folder> banks                    list every .xwb on the disc
//   fawave <game.iso|folder|bank.xwb> list [bank]     list the entries of one bank
//   fawave <game.iso|folder|bank.xwb> decode [bank] <index|all> <outdir>
//   fawave <file.xma> wav <out.wav> [ch] [rate]      decode a RIFF .xma sample
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "fa/hle/audio.hpp"
#include "fa/hle/vfs.hpp"
#include "fa/hle/xwb.hpp"

using namespace fa::hle;
namespace fs = std::filesystem;

static int usage() {
    std::fprintf(stderr,
                 "usage: fawave <game.iso|folder> banks\n"
                 "       fawave <game.iso|folder> list <bank.xwb path on disc>\n"
                 "       fawave <game.iso|folder> decode <bank.xwb path on disc> <index|all> <outdir>\n"
                 "       fawave <bank.xwb> list | decode <index|all> <outdir>\n"
                 "       fawave <file.xma> wav <out.wav> [channels] [rate]\n");
    return 2;
}

static bool read_file(const std::string& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

static bool read_guest(Vfs& vfs, const std::string& path, std::vector<uint8_t>& out) {
    std::shared_ptr<VfsFile> f;
    uint32_t info;
    if (vfs.open(path, OpenRequest{}, f, info) != status::kSuccess) return false;
    out.resize(f->info().size);
    return f->read(0, out.data(), out.size()) == out.size();
}

static void walk(Vfs& vfs, const std::string& dir, std::vector<std::pair<std::string, uint64_t>>& found) {
    std::shared_ptr<VfsFile> f;
    uint32_t info;
    OpenRequest req;
    req.options = create_options::kDirectoryFile;
    if (vfs.open(dir, req, f, info) != status::kSuccess) return;
    FileInfo fi;
    std::string pat = "*";
    bool first = true;
    while (f->next_entry(first ? &pat : nullptr, false, fi) == status::kSuccess) {
        first = false;
        std::string child = dir + "\\" + fi.name;
        if (fi.is_dir()) walk(vfs, child, found);
        else if (wildcard_match("*.xwb", fi.name)) found.emplace_back(child, fi.size);
    }
}

static bool decode_entry(const std::vector<uint8_t>& bank_data, const XwbBank& bank, size_t i,
                         std::vector<int16_t>& pcm, std::string& err) {
    const XwbEntry& e = bank.entries[i];
    if (uint64_t(e.offset) + e.size > bank_data.size()) {
        err = "entry outside the file";
        return false;
    }
    const uint8_t* p = bank_data.data() + e.offset;
    switch (e.codec) {
        case XwbEntry::Codec::Xma:
            return XmaDecoder::decode_stream(p, e.size, e.channels, e.sample_rate, pcm, err);
        case XwbEntry::Codec::Pcm:
            if (e.bits_per_sample == 16) {
                for (size_t k = 0; k + 1 < e.size; k += 2)
                    pcm.push_back(int16_t(bank.big_endian ? (p[k] << 8 | p[k + 1]) : (p[k + 1] << 8 | p[k])));
            } else {
                for (size_t k = 0; k < e.size; ++k) pcm.push_back(int16_t((int(p[k]) - 128) << 8));
            }
            return true;
        default:
            err = std::string("codec ") + codec_name(e.codec) + " not supported";
            return false;
    }
}

// Minimal RIFF .xma reader (little-endian RIFF with a data chunk; the format
// chunk may be missing as in some rips, then stereo 48 kHz is assumed).
static int xma_to_wav(const std::string& in, const std::string& out, int force_ch, int force_rate) {
    std::vector<uint8_t> d;
    if (!read_file(in, d) || d.size() < 12 || std::memcmp(d.data(), "RIFF", 4)) {
        std::fprintf(stderr, "not a RIFF file\n");
        return 1;
    }
    int ch = 2, rate = 48000;
    size_t data_off = 0, data_size = 0;
    for (size_t o = 12; o + 8 <= d.size();) {
        uint32_t sz = d[o + 4] | d[o + 5] << 8 | d[o + 6] << 16 | uint32_t(d[o + 7]) << 24;
        if (!std::memcmp(&d[o], "data", 4)) { data_off = o + 8; data_size = std::min<size_t>(sz, d.size() - o - 8); }
        if (!std::memcmp(&d[o], "fmt ", 4) && sz >= 34 && d[o + 8] == 0x66 && d[o + 9] == 0x01) {
            ch = d[o + 10];  // WAVEFORMATEX.nChannels of XMA2WAVEFORMATEX
            rate = int(d[o + 12] | d[o + 13] << 8 | d[o + 14] << 16 | uint32_t(d[o + 15]) << 24);
        }
        o += 8 + sz + (sz & 1);
    }
    if (!data_off) { std::fprintf(stderr, "no data chunk\n"); return 1; }
    if (force_ch) ch = force_ch;
    if (force_rate) rate = force_rate;
    std::vector<int16_t> pcm;
    std::string err;
    if (!XmaDecoder::decode_stream(d.data() + data_off, data_size, ch, rate, pcm, err)) {
        std::fprintf(stderr, "decode failed: %s\n", err.c_str());
        return 1;
    }
    write_wav(out, pcm, ch, rate);
    std::printf("%zu samples, %d ch, %d Hz -> %s\n", pcm.size() / size_t(ch), ch, rate, out.c_str());
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    std::string src = argv[1], cmd = argv[2];
    if (cmd == "wav" && argc >= 4)
        return xma_to_wav(src, argv[3], argc > 4 ? std::atoi(argv[4]) : 0, argc > 5 ? std::atoi(argv[5]) : 0);

    bool direct = fs::is_regular_file(src) && (src.size() > 4 && (src.substr(src.size() - 4) == ".xwb" ||
                                                                   src.substr(src.size() - 4) == ".XWB"));
    Vfs vfs;
    if (!direct) {
        std::string err;
        if (!vfs.mount_defaults(src, (fs::temp_directory_path() / "fawave-data").string(), err)) {
            std::fprintf(stderr, "cannot open %s: %s\n", src.c_str(), err.c_str());
            return 1;
        }
    }

    if (cmd == "banks" && !direct) {
        std::vector<std::pair<std::string, uint64_t>> found;
        walk(vfs, "game:", found);
        for (auto& [p, sz] : found) {
            std::vector<uint8_t> d;
            XwbBank b;
            std::string err;
            std::string desc;
            if (read_guest(vfs, p, d) && parse_xwb(d, b, err)) {
                int xma = 0;
                for (auto& e : b.entries) xma += e.codec == XwbEntry::Codec::Xma;
                char buf[160];
                std::snprintf(buf, sizeof buf, "v%u %s, %zu entries (%d XMA), name '%s'", b.version,
                              b.big_endian ? "BE" : "LE", b.entries.size(), xma, b.name.c_str());
                desc = buf;
            } else {
                desc = "unreadable: " + err;
            }
            std::printf("%10llu  %s  %s\n", (unsigned long long)sz, p.c_str() + 5, desc.c_str());
        }
        return 0;
    }

    int argi = 3;
    std::vector<uint8_t> data;
    if (direct) {
        if (!read_file(src, data)) { std::fprintf(stderr, "cannot read %s\n", src.c_str()); return 1; }
    } else {
        if (argc <= argi) return usage();
        std::string p = std::string("game:\\") + argv[argi++];
        if (!read_guest(vfs, p, data)) { std::fprintf(stderr, "not found on disc: %s\n", p.c_str()); return 1; }
    }
    XwbBank bank;
    std::string err;
    if (!parse_xwb(data, bank, err)) { std::fprintf(stderr, "bad wave bank: %s\n", err.c_str()); return 1; }

    if (cmd == "list") {
        std::printf("bank '%s', version %u, %s, flags %08X, %zu entries\n", bank.name.c_str(), bank.version,
                    bank.big_endian ? "big-endian" : "little-endian", bank.flags, bank.entries.size());
        for (size_t i = 0; i < bank.entries.size(); ++i) {
            auto& e = bank.entries[i];
            std::printf("%4zu  %-5s %d ch %6d Hz  %9u bytes  samples %-8u loop %08X/%08X  %s\n", i, codec_name(e.codec),
                        e.channels, e.sample_rate, e.size, e.num_samples, e.loop_start, e.loop_end, e.name.c_str());
        }
        return 0;
    }
    if (cmd == "decode" && argc == argi + 2) {
        std::string which = argv[argi], outdir = argv[argi + 1];
        fs::create_directories(outdir);
        size_t ok = 0, fail = 0;
        for (size_t i = 0; i < bank.entries.size(); ++i) {
            if (which != "all" && std::to_string(i) != which) continue;
            std::vector<int16_t> pcm;
            std::string e;
            auto& en = bank.entries[i];
            if (!decode_entry(data, bank, i, pcm, e)) {
                std::printf("%4zu  failed: %s\n", i, e.c_str());
                ++fail;
                continue;
            }
            std::string name = en.name.empty() ? std::to_string(i) : std::to_string(i) + "_" + en.name;
            std::replace_if(name.begin(), name.end(), [](char c) { return c == '/' || c == '\\' || c == ' '; }, '_');
            std::string out = outdir + "/" + name + ".wav";
            write_wav(out, pcm, std::max(1, en.channels), en.sample_rate);
            std::printf("%4zu  %s  %.2f s\n", i, out.c_str(),
                        double(pcm.size()) / std::max(1, en.channels) / std::max(1, en.sample_rate));
            ++ok;
        }
        std::printf("%zu decoded, %zu failed\n", ok, fail);
        return fail ? 1 : 0;
    }
    return usage();
}
