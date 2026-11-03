// Native timing harness: same settings for before/after encoder comparisons.
#ifndef NIBNIB_SOURCE
#define NIBNIB_SOURCE "../../src/nibnib.cpp"
#endif
#include NIBNIB_SOURCE
#include "png_io.hpp"
#include "scale.hpp"
#include <chrono>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    using namespace png2amiga;
    if (argc != 5 && argc != 6) {
        std::cerr << "usage: normal-encode input output-prefix width height [--best]\n";
        return 2;
    }
    if (argc == 6 && std::string_view(argv[5]) != "--best") return 2;
    const auto w = static_cast<std::size_t>(std::stoul(argv[3]));
    const auto h = static_cast<std::size_t>(std::stoul(argv[4]));
    if (w < 2 || w % 2 || w > 65534 || h == 0 || h > 65535) return 2;
    auto loaded = png_io::load(argv[1]);
    if (!loaded) return 1;
    auto source = scale::resample(*loaded, w, h);
    if (!source) return 1;
    const std::string prefix = argv[2];
    if (auto r = png_io::save(prefix + "-source.png", *source); !r) return 1;
    nibnib::Options options;
    options.dither.method = dither::Method::floyd_steinberg;
    options.dither.strength = 0.76f;
    options.best = argc == 6;
    const auto start = std::chrono::steady_clock::now();
    auto result = nibnib::encode(*source, options);
    const auto seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (!result) return 1;
    std::ofstream file(prefix + ".nibnib", std::ios::binary);
    file.write(reinterpret_cast<const char*>(result->file.data()),
               static_cast<std::streamsize>(result->file.size()));
    if (!file) return 1;
    if (auto r = png_io::save(prefix + ".png", result->rendered); !r) return 1;
    std::cout << "{\"s2\":"
              << ssimulacra2::compute(source->pixels(), result->rendered.pixels(), w, h)
              << ",\"bytes\":" << result->file.size() << ",\"seconds\":" << seconds << "}\n";
    return 0;
}
