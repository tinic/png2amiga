// Encoder-only exploration. All outputs use the original nibnib decoder.
#ifndef NIBNIB_SOURCE
#define NIBNIB_SOURCE "../../src/nibnib.cpp"
#endif
#include NIBNIB_SOURCE
#include "png_io.hpp"
#include "scale.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <numeric>

namespace png2amiga::nibnib {
namespace {
using Delta = std::array<int, 6>;
using Palette = std::array<std::uint32_t, 256>;
Palette diverse(std::span<const Delta> seeds, unsigned kind) {
    Palette pal{};
    if (seeds.empty()) return pal;
    std::vector<double> nearest(seeds.size(), 1e100);
    std::uint32_t random = 0x8D12E5A3u;
    auto next = [&] { random ^= random << 13; random ^= random >> 17; random ^= random << 5; return random; };
    Delta center{};
    for (std::size_t c = 0; c < 256; ++c) {
        if (c) {
            double total = std::accumulate(nearest.begin(), nearest.end(), 0.0);
            if (total > 0) {
                double threshold = static_cast<double>(next()) / 4294967296.0 * total;
                for (std::size_t i = 0; i < seeds.size(); ++i) {
                    threshold -= nearest[i];
                    if (threshold <= 0) { center = seeds[i]; break; }
                }
            }
        }
        pal[c] = pack({center[0], center[1], center[2]}, {center[3], center[4], center[5]});
        for (std::size_t i = 0; i < seeds.size(); ++i) {
            double d = 0;
            for (std::size_t j = 0; j < 6; ++j) {
                const double scale = j % 3 == 1 ? 0.5 : 1.0;
                const double error = static_cast<double>(seeds[i][j] - center[j]) * scale;
                d += error * error;
            }
            // Less aggressive error weighting keeps a few rare large moves
            // from consuming the dictionary. Both are deterministic kmeans++.
            nearest[i] = std::min(nearest[i], kind == 0 ? d : std::sqrt(d));
        }
    }
    return pal;
}

Palette fit_seed(Palette pal, const Image& image, const std::vector<std::uint16_t>& target,
                 const RowGroups& groups, std::span<const OKLab> objective, int rounds) {
    const auto w = image.width(), h = image.height();
    const auto& lab = lab565();
    Palette best = pal; double best_cost = 1e100;
    for (int it = 0; it <= rounds; ++it) {
        std::vector<Delta> wants;
        const auto enc = encode_rows(target, objective, groups, w, h, pal, 8, &wants, {});
        if (enc.cost < best_cost) { best = pal; best_cost = enc.cost; }
        if (it == rounds) break;
        std::array<std::array<double, 6>, 256> sum{};
        std::array<double, 256> count{};
        std::vector<std::pair<float, std::size_t>> worst;
        for (std::size_t y = 0; y < h; ++y) {
            std::uint16_t previous = enc.rows[y].first[1];
            for (std::size_t k = 1; k < w / 2; ++k) {
                const auto i = y * (w / 2 - 1) + k - 1;
                const auto c = enc.rows[y].codes[k - 1];
                for (std::size_t j = 0; j < 6; ++j) sum[c][j] += wants[i][j];
                ++count[c];
                const auto pair = std::uint32_t{previous} * 0x10001u + pal[c];
                const auto x = y * w + 2 * k;
                worst.emplace_back(dist(lab[pair & 65535], objective.empty() ? lab[target[x]] : objective[x]) +
                    dist(lab[pair >> 16], objective.empty() ? lab[target[x + 1]] : objective[x + 1]), i);
                previous = static_cast<std::uint16_t>(pair >> 16);
            }
        }
        std::array<std::size_t, 256> order{}; std::iota(order.begin(), order.end(), 0);
        std::ranges::sort(order, {}, [&](auto c) { return count[c]; });
        std::array<bool, 256> reseed{};
        for (std::size_t i = 0; i < 24; ++i) reseed[order[i]] = true;
        const auto n = std::min<std::size_t>(worst.size(), 24 * 64);
        std::ranges::partial_sort(worst, worst.begin() + static_cast<std::ptrdiff_t>(n), std::greater<>{});
        std::size_t wi = 0; const auto previous = pal;
        for (std::size_t c = 0; c < 256; ++c) {
            Delta d{};
            if (count[c] > 0 && !reseed[c]) {
                for (std::size_t j = 0; j < 6; ++j) d[j] = static_cast<int>(std::lround(sum[c][j] / count[c]));
            } else if (n) { d = wants[worst[wi % n].second]; wi += 61; }
            pal[c] = pack({d[0], d[1], d[2]}, {d[3], d[4], d[5]});
        }
        if (pal == previous) break;
    }
    return best;
}

Encoded build_file(const Palette& pal, const Encoding& enc, std::size_t w, std::size_t h) {
    Encoded out; out.palette = pal; out.file.resize(kHeader + line_stride(w) * h);
    auto put16 = [&](std::size_t i, std::uint16_t value) {
        out.file[i] = static_cast<std::uint8_t>(value); out.file[i + 1] = static_cast<std::uint8_t>(value >> 8);
    };
    put16(0, static_cast<std::uint16_t>(w)); put16(2, static_cast<std::uint16_t>(h));
    for (std::size_t c = 0; c < 256; ++c)
        for (unsigned b = 0; b < 4; ++b) out.file[4 + c * 4 + b] = static_cast<std::uint8_t>(pal[c] >> (8 * b));
    for (std::size_t y = 0; y < h; ++y) {
        const auto offset = kHeader + y * line_stride(w);
        put16(offset, enc.rows[y].first[0]); put16(offset + 2, enc.rows[y].first[1]);
        std::ranges::copy(enc.rows[y].codes, out.file.begin() + static_cast<std::ptrdiff_t>(offset + 4));
    }
    auto image = decode(out.file); if (!image) throw std::runtime_error("decode failed");
    out.rendered = std::move(*image); return out;
}
} // namespace
} // namespace png2amiga::nibnib

int main(int argc, char** argv) {
    using namespace png2amiga;
    using namespace png2amiga::nibnib;
    if (argc != 6 && argc != 7) { std::cerr << "usage: sweep input baseline.nibnib prefix width height [diverse-source-beam]\n"; return 2; }
    const auto beam = argc == 7 ? static_cast<std::size_t>(std::stoul(argv[6])) : 256;
    const auto w = static_cast<std::size_t>(std::stoul(argv[4])), h = static_cast<std::size_t>(std::stoul(argv[5]));
    auto loaded = png_io::load(argv[1]); if (!loaded) return 1;
    auto image = scale::resample(*loaded, w, h); if (!image) return 1;
    std::ifstream in(argv[2], std::ios::binary); if (!in) return 1;
    std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), {});
    auto baseline = decode(file); if (!baseline || baseline->width() != w || baseline->height() != h) return 1;
    nibnib::Palette warm{};
    for (std::size_t c = 0; c < 256; ++c)
        for (unsigned b = 0; b < 4; ++b) warm[c] |= std::uint32_t{file[4 + c * 4 + b]} << (8 * b);
    std::vector<std::uint16_t> target(w * h);
    dither::Settings settings; settings.method = dither::Method::floyd_steinberg; settings.strength = 0.76f;
    dither::diffuse_raw_buffer(*image, settings, [&](const OKLab& c, std::size_t x, std::size_t y) -> dither::PickResult {
        const auto p = linear_to_rgb565(color_space::oklab_to_linear(c)); target[y * w + x] = p; return {lab565()[p], 0.5f};
    });
    std::vector<Delta> seeds;
    for (std::size_t y = 0; y < h; ++y)
        for (std::size_t x = 2; x < w; x += 2) {
            const auto p = fields(target[y * w + x - 1]), a = fields(target[y * w + x]), b = fields(target[y * w + x + 1]);
            seeds.push_back({a[0] - p[0], a[1] - p[1], a[2] - p[2], b[0] - p[0], b[1] - p[1], b[2] - p[2]});
        }
    std::vector<OKLab> source; for (auto c : image->pixels()) source.push_back(color_space::linear_to_oklab(c));
    const auto target_rows = group_rows(target, {}, w, h), source_rows = group_rows(target, source, w, h);
    ssimulacra2::PrecomputedSource reference; reference.prepare(image->pixels(), w, h);
    std::cout << "{\"variant\":\"baseline\",\"s2\":" << ssimulacra2::compute(reference, baseline->pixels()) << "}" << std::endl;
    for (unsigned init = 0; init < (argc == 7 ? 1u : 3u); ++init)
        for (unsigned objective = argc == 7 ? 1u : 0u; objective < 2; ++objective) {
            const auto start = std::chrono::steady_clock::now();
            const auto span = objective ? std::span<const OKLab>(source) : std::span<const OKLab>{};
            const auto initial = init == 2 ? warm : diverse(seeds, init);
            const auto pal = fit_seed(initial, *image, target, objective ? source_rows : target_rows, span, 12);
            const auto enc = encode_rows(target, span, objective ? source_rows : target_rows, w, h, pal, beam, nullptr, {});
            auto out = build_file(pal, enc, w, h);
            const auto name = std::string(init == 2 ? "warm" : init == 0 ? "diverse" : "gentle") + (objective ? "-source" : "-target");
            const auto path = std::string(argv[3]) + "-" + name;
            std::ofstream output(path + ".nibnib", std::ios::binary); output.write(reinterpret_cast<const char*>(out.file.data()), static_cast<std::streamsize>(out.file.size()));
            if (auto r = png_io::save(path + ".png", out.rendered); !r) return 1;
            std::cout << "{\"variant\":\"" << name << "\",\"s2\":" << ssimulacra2::compute(reference, out.rendered.pixels()) << ",\"seconds\":" << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() << "}" << std::endl;
        }
}
