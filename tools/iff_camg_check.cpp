// iff_camg_check: regression check for the CAMG viewport bits that
// api::convert_iff (the web export path) writes. Issue #13: a hires
// conversion exported from the web app had no HIRES bit, so AMOS and
// other loaders opened it on a lores screen.
//
//   iff_camg_check <input.png>
//
// Converts the input through api::convert_iff for each mode below and
// checks HIRES (0x8000), LACE (0x0004) and HAM (0x0800) in CAMG.
// Exit 0 when every mode matches, 1 otherwise.

#include "api.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

struct Case {
    const char* mode;
    bool hires;
    bool lace;
    bool ham;
};

constexpr Case kCases[] = {
    {"lores", false, false, false},
    {"lores-lace", false, true, false},
    {"hires", true, false, false},
    {"hires-lace", true, true, false},
    {"ham6", false, false, true},
    {"ham6-hires", true, false, true},
    {"ham6-hires-lace", true, true, true},
    {"ehb-lace", false, true, false},
};

bool find_camg(const std::vector<std::uint8_t>& d, std::uint32_t& camg) {
    for (std::size_t i = 12; i + 12 <= d.size(); ++i) {
        if (d[i] == 'C' && d[i + 1] == 'A' && d[i + 2] == 'M' && d[i + 3] == 'G') {
            camg = (std::uint32_t{d[i + 8]} << 24) | (std::uint32_t{d[i + 9]} << 16) |
                   (std::uint32_t{d[i + 10]} << 8) | std::uint32_t{d[i + 11]};
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: iff_camg_check <input.png>\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<std::uint8_t> png((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
    if (png.empty()) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }

    int failures = 0;
    for (const auto& c : kCases) {
        png2amiga::api::Options opts;
        opts.mode = c.mode;
        opts.width = 64;  // small and quick; CAMG does not depend on size
        auto r = png2amiga::api::convert_iff(png.data(), png.size(), opts);
        std::uint32_t camg = 0;
        if (!r.error.empty() || !find_camg(r.data, camg)) {
            std::printf("camg mode=%s result=error detail=%s\n", c.mode,
                        r.error.empty() ? "no_camg_chunk" : r.error.c_str());
            ++failures;
            continue;
        }
        const bool hires = camg & 0x8000u;
        const bool lace = camg & 0x0004u;
        const bool ham = camg & 0x0800u;
        const bool ok = hires == c.hires && lace == c.lace && ham == c.ham;
        std::printf("camg mode=%s value=0x%08x hires=%d lace=%d ham=%d result=%s\n", c.mode,
                    static_cast<unsigned>(camg), hires, lace, ham, ok ? "ok" : "FAIL");
        if (!ok) ++failures;
    }
    std::printf("iff_camg_check failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
