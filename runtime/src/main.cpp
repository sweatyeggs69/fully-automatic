// fullauto: runs the recompiled Full Auto executable.
//
//   fullauto <default.xex | game.iso>
#include "fa/rt/loader.hpp"
#include "fa/rt/memory.hpp"
#include "fa/xdvdfs.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <fstream>
#include <iterator>

using namespace fa::rt;

static bool read_game(const std::string& path, std::vector<uint8_t>& xex) {
    if (path.size() > 4 && strcasecmp(path.c_str() + path.size() - 4, ".iso") == 0) {
        fa::XdvdfsImage iso;
        fa::IsoEntry e;
        if (!iso.open(path) || !iso.find("default.xex", e) || !iso.read(e, xex)) {
            std::fprintf(stderr, "cannot read default.xex from %s: %s\n", path.c_str(), iso.error().c_str());
            return false;
        }
        return true;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return false; }
    xex.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: fullauto <default.xex | game.iso>\n"); return 2; }
    std::vector<uint8_t> xex;
    if (!read_game(argv[1], xex)) return 1;
    if (!g_mem.init()) return 1;
    // XMA/APU register page: plain memory the audio layer polls.
    g_mem.commit_fixed(0x7FEA0000, 0x10000);

    LoadedImage img;
    if (!load_xex(xex, img)) return 1;
    g_kernel.set_tls_info(img.tls);
    std::fprintf(stderr, "entering 0x%08X (stack 0x%X)\n", img.entry, img.stack_size);
    uint32_t rc = g_kernel.run_main_thread(img.entry, img.stack_size);
    std::fprintf(stderr, "guest main returned 0x%08X\n", rc);
    std::fflush(stderr);
    std::_Exit(int(rc & 0xFF));
}
