#include "nibnib.hpp"
#include "color_space.hpp"
#include "pipeline.hpp"
#include "ssimulacra2.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace png2amiga::nibnib {

namespace {

using color_space::OKLab;

constexpr std::size_t kHeader = 4 + 256 * 4;

// RGB565 field → 8-bit sRGB, by bit replication as the decoder does.
constexpr int expand5(int v) noexcept { return (v << 3) | (v >> 2); }
constexpr int expand6(int v) noexcept { return (v << 2) | (v >> 4); }

Color3f rgb565_to_linear(std::uint16_t p) noexcept {
    return color_space::srgb_to_linear(Color3f{static_cast<float>(expand5(p >> 11)) / 255.0f,
                                               static_cast<float>(expand6((p >> 5) & 63)) / 255.0f,
                                               static_cast<float>(expand5(p & 31)) / 255.0f});
}

// OKLab of every RGB565 value.
const std::vector<OKLab>& lab565() {
    static const std::vector<OKLab> lut = [] {
        std::vector<OKLab> t(65536);
        pipeline::parallel_for(256, [&](std::size_t hi) {
            for (std::size_t lo = 0; lo < 256; ++lo) {
                auto p = static_cast<std::uint16_t>(hi << 8 | lo);
                t[p] = color_space::linear_to_oklab(rgb565_to_linear(p));
            }
        });
        return t;
    }();
    return lut;
}

// Nearest field value for an sRGB channel in [0, 1].
int snap_channel(float s, int bits) noexcept {
    const int top = (1 << bits) - 1;
    const float v = std::clamp(s, 0.0f, 1.0f) * 255.0f;
    int k = std::clamp(static_cast<int>(std::lround(v * static_cast<float>(top) / 255.0f)), 0, top);
    auto level = [&](int q) { return static_cast<float>(bits == 5 ? expand5(q) : expand6(q)); };
    int best = k;
    for (int q = std::max(0, k - 1); q <= std::min(top, k + 1); ++q)
        if (std::abs(level(q) - v) < std::abs(level(best) - v)) best = q;
    return best;
}

std::uint16_t linear_to_rgb565(Color3f c) noexcept {
    auto s = color_space::linear_to_srgb(Color3f{
        std::clamp(c.r, 0.0f, 1.0f), std::clamp(c.g, 0.0f, 1.0f), std::clamp(c.b, 0.0f, 1.0f)});
    return static_cast<std::uint16_t>(snap_channel(s.r, 5) << 11 | snap_channel(s.g, 6) << 5 |
                                      snap_channel(s.b, 5));
}

// Palette word for per-channel deltas (r, g, b in field units) of the left
// and right pixel against the previous pixel. Exact when no field leaves
// its range; the search decodes every candidate, so overflow only costs.
std::uint32_t pack(std::array<int, 3> d0, std::array<int, 3> d1) noexcept {
    auto half = [](std::array<int, 3> d) {
        return static_cast<std::uint32_t>(d[2]) + (static_cast<std::uint32_t>(d[1]) << 5) +
               (static_cast<std::uint32_t>(d[0]) << 11);
    };
    return half(d0) + (half(d1) << 16);
}

std::array<int, 3> fields(std::uint16_t p) noexcept { return {p >> 11, (p >> 5) & 63, p & 31}; }

inline float dist(const OKLab& a, const OKLab& b) noexcept {
    return color_space::fma_dist_sq(a.L - b.L, a.a - b.a, a.b - b.b);
}

struct Row {
    std::array<std::uint16_t, 2> first{};
    std::vector<std::uint8_t> codes;  // one per pair after the first
    float cost = 0;
};

// Beam search over one row's codes. State is the last decoded pixel; the
// cost is the summed OKLab² distance of both pixels of each pair to the
// target. Candidates reaching the same state keep only the cheapest.
class RowSearch {
public:
    explicit RowSearch(std::size_t beam) : beam_(beam), slot_(65536, 0), stamp_(65536, 0) {}

    Row run(std::span<const std::uint16_t> target,
            const std::array<std::uint32_t, 256>& pal,
            std::vector<std::array<int, 6>>* wants = nullptr) {
        const auto& lab = lab565();
        const std::size_t n = target.size() / 2;
        Row row;
        row.first = {target[0], target[1]};
        row.codes.assign(n - 1, 0);
        if (n < 2) return row;
        history_.resize(n);
        history_[0].assign(1, {target[1], 0, 0, 0.0f});
        for (std::size_t k = 1; k < n; ++k) {
            const OKLab t0 = lab[target[2 * k]], t1 = lab[target[2 * k + 1]];
            if (++gen_ == 0) {
                std::ranges::fill(stamp_, 0u);
                gen_ = 1;
            }
            cand_.clear();
            const auto& prev = history_[k - 1];
            for (std::uint32_t s = 0; s < prev.size(); ++s) {
                const std::uint32_t base = std::uint32_t{prev[s].p} * 0x10001u;
                const float c0 = prev[s].cost;
                for (std::uint32_t c = 0; c < 256; ++c) {
                    const std::uint32_t pair = base + pal[c];
                    const auto o1 = static_cast<std::uint16_t>(pair >> 16);
                    const float cost =
                        c0 + dist(lab[pair & 0xffff], t0) + dist(lab[o1], t1);
                    if (stamp_[o1] == gen_) {
                        auto& e = cand_[slot_[o1]];
                        if (cost < e.cost) e = {o1, static_cast<std::uint8_t>(c), s, cost};
                    } else {
                        stamp_[o1] = gen_;
                        slot_[o1] = static_cast<std::uint32_t>(cand_.size());
                        cand_.push_back({o1, static_cast<std::uint8_t>(c), s, cost});
                    }
                }
            }
            if (cand_.size() > beam_) {
                std::ranges::nth_element(cand_, cand_.begin() + static_cast<std::ptrdiff_t>(beam_),
                                         {}, &State::cost);
                cand_.resize(beam_);
            }
            history_[k] = cand_;
        }
        const auto& last = history_[n - 1];
        auto best = static_cast<std::size_t>(
            std::ranges::min_element(last, {}, &State::cost) - last.begin());
        row.cost = last[best].cost;
        for (std::size_t k = n - 1; k >= 1; --k) {
            const auto& st = history_[k][best];
            row.codes[k - 1] = st.code;
            if (wants) {
                const auto pf = fields(history_[k - 1][st.parent].p);
                const auto f0 = fields(target[2 * k]), f1 = fields(target[2 * k + 1]);
                (*wants)[k - 1] = {f0[0] - pf[0], f0[1] - pf[1], f0[2] - pf[2],
                                   f1[0] - pf[0], f1[1] - pf[1], f1[2] - pf[2]};
            }
            best = st.parent;
        }
        return row;
    }

private:
    struct State {
        std::uint16_t p;
        std::uint8_t code;
        std::uint32_t parent;
        float cost;
    };
    std::size_t beam_;
    std::vector<std::uint32_t> slot_;
    std::vector<std::uint32_t> stamp_;
    std::uint32_t gen_ = 0;
    std::vector<State> cand_;
    std::vector<std::vector<State>> history_;
};

struct Encoding {
    std::vector<Row> rows;
    double cost = 0;
};

Encoding encode_rows(const std::vector<std::uint16_t>& target,
                     std::size_t w,
                     std::size_t h,
                     const std::array<std::uint32_t, 256>& pal,
                     std::size_t beam,
                     std::vector<std::array<int, 6>>* wants,
                     const std::function<void(float)>& row_done) {
    Encoding enc;
    std::atomic<std::size_t> done{0};
    enc.rows.resize(h);
    if (wants) wants->assign(h * (w / 2 - 1), {});
    pipeline::parallel_for(h, [&](std::size_t y) {
        thread_local std::unique_ptr<RowSearch> search;
        thread_local std::size_t search_beam = 0;
        if (!search || search_beam != beam) {
            search = std::make_unique<RowSearch>(beam);
            search_beam = beam;
        }
        std::vector<std::array<int, 6>> row_wants;
        if (wants) row_wants.resize(w / 2 - 1);
        enc.rows[y] = search->run(std::span(target).subspan(y * w, w), pal,
                                  wants ? &row_wants : nullptr);
        if (wants) std::ranges::copy(row_wants, wants->begin() + static_cast<std::ptrdiff_t>(y * (w / 2 - 1)));
        if (row_done)
            row_done(static_cast<float>(done.fetch_add(1, std::memory_order_relaxed) + 1) /
                     static_cast<float>(h));
    });
    for (const auto& r : enc.rows)
        enc.cost += static_cast<double>(r.cost);
    return enc;
}

}  // namespace

std::size_t line_stride(std::size_t width) noexcept {
    return (width / 2 + 3 + 3) / 4 * 4;
}

Result<Encoded> encode(const Image& image, const Options& opts) {
    const std::size_t w = image.width(), h = image.height();
    if (w < 2 || w % 2 != 0 || h == 0 || w > 65535 || h > 65535)
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
                                     "nibnib needs an even width of at least 2 (and at most 65534)"}};
    // Rows finish out of order across threads: report only forward steps.
    std::mutex report_mu;
    float reported = -1.0f;
    auto report = [&](float f, std::string_view s) {
        if (!opts.on_progress) return;
        std::lock_guard lock(report_mu);
        if (f <= reported && s != "done") return;
        reported = f;
        opts.on_progress(f, s);
    };

    // Target: the image dithered to RGB565 with the chosen method.
    std::vector<std::uint16_t> target(w * h);
    dither::diffuse_raw_buffer(
        image, opts.dither, [&](const OKLab& t, std::size_t x, std::size_t y) -> dither::PickResult {
            const auto p = linear_to_rgb565(color_space::oklab_to_linear(t));
            target[y * w + x] = p;
            return {lab565()[p], 0.5f};
        });

    // Pair deltas the target wants, against its own previous pixel: the
    // palette seeds.
    std::vector<std::array<int, 6>> seeds(h * (w / 2 - 1));
    for (std::size_t y = 0; y < h; ++y)
        for (std::size_t k = 1; k < w / 2; ++k) {
            const auto pf = fields(target[y * w + 2 * k - 1]);
            const auto f0 = fields(target[y * w + 2 * k]), f1 = fields(target[y * w + 2 * k + 1]);
            seeds[y * (w / 2 - 1) + k - 1] = {f0[0] - pf[0], f0[1] - pf[1], f0[2] - pf[2],
                                              f1[0] - pf[0], f1[1] - pf[1], f1[2] - pf[2]};
        }

    // All 256 codes are fitted (upstream's fixed small-delta cube scored
    // 2.6 S2 lower: the fit finds the safe moves an image needs). Each
    // round encodes with a narrow beam, moves every code to the mean
    // delta its pairs wanted, and moves the least-used codes to the pairs
    // coded worst. The final encode uses a wide beam. --best repeats it
    // from other seeds and keeps the highest SSIMULACRA2.
    constexpr int kFitIters = 12;
    constexpr std::size_t kFitBeam = 8;
    constexpr std::size_t kReseed = 24;
    constexpr std::size_t kBeam = 256;
    const std::size_t restarts = opts.best ? 4 : 1;
    // Progress: each pass costs about its beam width per row.
    const float pass_share =
        1.0f / static_cast<float>(restarts) /
        static_cast<float>(static_cast<std::size_t>(kFitIters) * kFitBeam + kBeam);
    const auto& lab = lab565();

    auto fit = [&](std::size_t restart) {
        std::array<std::uint32_t, 256> pal{};
        const std::size_t stride = std::max<std::size_t>(1, seeds.size() / 256);
        const std::size_t offset = 2 * restart * stride / 7;
        for (std::size_t c = 0; c < 256; ++c) {
            const auto& d = seeds[std::min(seeds.size() - 1, c * stride + offset)];
            pal[c] = pack({d[0], d[1], d[2]}, {d[3], d[4], d[5]});
        }
        for (int it = 0; it < kFitIters; ++it) {
            const float lo =
                (static_cast<float>(restart) * static_cast<float>(kFitIters * static_cast<int>(kFitBeam) +
                                                                  static_cast<int>(kBeam)) +
                 static_cast<float>(it * static_cast<int>(kFitBeam))) *
                pass_share;
            std::vector<std::array<int, 6>> wants;
            auto enc = encode_rows(target, w, h, pal, kFitBeam, &wants, [&](float f) {
                report(lo + f * static_cast<float>(kFitBeam) * pass_share, "fitting palette");
            });
            std::array<std::array<double, 6>, 256> sum{};
            std::array<double, 256> count{};
            std::vector<std::pair<float, std::size_t>> worst;
            worst.reserve(wants.size());
            for (std::size_t y = 0; y < h; ++y) {
                std::uint16_t p = enc.rows[y].first[1];
                for (std::size_t k = 1; k < w / 2; ++k) {
                    const std::size_t i = y * (w / 2 - 1) + k - 1;
                    const auto c = enc.rows[y].codes[k - 1];
                    for (std::size_t d = 0; d < 6; ++d)
                        sum[c][d] += wants[i][d];
                    count[c] += 1;
                    const std::uint32_t pair = std::uint32_t{p} * 0x10001u + pal[c];
                    worst.emplace_back(dist(lab[pair & 0xffff], lab[target[y * w + 2 * k]]) +
                                           dist(lab[pair >> 16], lab[target[y * w + 2 * k + 1]]),
                                       i);
                    p = static_cast<std::uint16_t>(pair >> 16);
                }
            }
            std::array<std::size_t, 256> by_use{};
            for (std::size_t c = 0; c < 256; ++c)
                by_use[c] = c;
            std::ranges::sort(by_use, {}, [&](std::size_t c) { return count[c]; });
            std::array<bool, 256> reseed{};
            for (std::size_t i = 0; i < kReseed; ++i)
                reseed[by_use[i]] = true;
            const std::size_t nworst = std::min(worst.size(), kReseed * 64);
            std::ranges::partial_sort(
                worst, worst.begin() + static_cast<std::ptrdiff_t>(nworst), std::greater<>{});
            std::size_t wi = 0;
            for (std::size_t c = 0; c < 256; ++c) {
                std::array<int, 6> d{};
                if (count[c] > 0 && !reseed[c]) {
                    for (std::size_t j = 0; j < 6; ++j)
                        d[j] = static_cast<int>(std::lround(sum[c][j] / count[c]));
                } else if (nworst > 0) {
                    d = wants[worst[wi % nworst].second];
                    wi += 61;
                }
                pal[c] = pack({d[0], d[1], d[2]}, {d[3], d[4], d[5]});
            }
        }
        return pal;
    };

    auto build = [&](const std::array<std::uint32_t, 256>& pal,
                     const Encoding& enc) -> Result<Encoded> {
        Encoded out;
        out.palette = pal;
        const std::size_t stride = line_stride(w);
        // Bytes go through a raw pointer into a sized buffer: GCC 15 at
        // -O3 flags both byte-wise push_back (free-nonheap-object) and
        // vector operator[] (null-dereference) here, falsely; the null
        // check lets it prove the pointer valid.
        std::vector<std::uint8_t> file(kHeader + stride * h, 0);
        std::uint8_t* const base = file.data();
        if (base == nullptr)
            return std::unexpected{Error{ErrorCode::write_failed, "nibnib: no output buffer"}};
        auto put16 = [](std::uint8_t* d, std::uint16_t v) {
            d[0] = static_cast<std::uint8_t>(v & 0xff);
            d[1] = static_cast<std::uint8_t>(v >> 8);
        };
        put16(base, static_cast<std::uint16_t>(w));
        put16(base + 2, static_cast<std::uint16_t>(h));
        for (std::size_t i = 0; i < 256; ++i)
            for (std::size_t b = 0; b < 4; ++b)
                base[4 + 4 * i + b] = static_cast<std::uint8_t>(pal[i] >> (8 * b));
        for (std::size_t y = 0; y < h; ++y) {
            const auto& r = enc.rows[y];
            std::uint8_t* const row = base + kHeader + y * stride;
            put16(row, r.first[0]);
            put16(row + 2, r.first[1]);
            std::ranges::copy(r.codes, row + 4);
        }
        out.file = std::move(file);
        auto dec = decode(out.file);
        if (!dec) return std::unexpected{dec.error()};
        out.rendered = std::move(*dec);
        return out;
    };

    report(0.0f, "fitting palette");
    std::optional<Encoded> best;
    float best_score = 0;
    for (std::size_t restart = 0; restart < restarts; ++restart) {
        const auto pal = fit(restart);
        const float lo = (static_cast<float>(restart) *
                              static_cast<float>(kFitIters * static_cast<int>(kFitBeam) +
                                                 static_cast<int>(kBeam)) +
                          static_cast<float>(kFitIters * static_cast<int>(kFitBeam))) *
                         pass_share;
        auto enc = encode_rows(target, w, h, pal, kBeam, nullptr, [&](float f) {
            report(lo + f * static_cast<float>(kBeam) * pass_share, "encoding rows");
        });
        auto out = build(pal, enc);
        if (!out) return std::unexpected{out.error()};
        if (restarts == 1) {
            best = std::move(*out);
            break;
        }
        const float score = ssimulacra2::compute(image.pixels(), out->rendered.pixels(), w, h);
        if (!best || score > best_score) {
            best = std::move(*out);
            best_score = score;
        }
    }
    for (std::size_t i = 0; i < w * h; ++i)
        best->quant_error += dist(color_space::linear_to_oklab(image.pixels()[i]),
                                  color_space::linear_to_oklab(best->rendered.pixels()[i]));
    report(1.0f, "done");
    return std::move(*best);
}

Result<Image> decode(std::span<const std::uint8_t> file) {
    auto rd16 = [&](std::size_t i) {
        return static_cast<std::uint16_t>(file[i] | file[i + 1] << 8);
    };
    auto rd32 = [&](std::size_t i) {
        return std::uint32_t{file[i]} | std::uint32_t{file[i + 1]} << 8 |
               std::uint32_t{file[i + 2]} << 16 | std::uint32_t{file[i + 3]} << 24;
    };
    if (file.size() < kHeader)
        return std::unexpected{Error{ErrorCode::invalid_png, "nibnib: truncated header"}};
    const std::size_t w = rd16(0), h = rd16(2);
    if (w == 0 || w % 2 != 0)
        return std::unexpected{Error{ErrorCode::invalid_dimensions, "nibnib: width must be even"}};
    const std::size_t stride = line_stride(w);
    if (file.size() != kHeader + stride * h)
        return std::unexpected{Error{ErrorCode::invalid_png, "nibnib: size does not match header"}};
    std::array<std::uint32_t, 256> pal{};
    for (std::size_t i = 0; i < 256; ++i)
        pal[i] = rd32(4 + 4 * i);
    Image img(w, h);
    for (std::size_t y = 0; y < h; ++y) {
        std::size_t i = kHeader + y * stride;
        std::uint32_t pix = rd32(i);
        i += 4;
        img[0, y] = rgb565_to_linear(static_cast<std::uint16_t>(pix));
        img[1, y] = rgb565_to_linear(static_cast<std::uint16_t>(pix >> 16));
        for (std::size_t x = 2; x < w; x += 2) {
            pix = (pix >> 16) * 0x10001u + pal[file[i++]];
            img[x, y] = rgb565_to_linear(static_cast<std::uint16_t>(pix));
            img[x + 1, y] = rgb565_to_linear(static_cast<std::uint16_t>(pix >> 16));
        }
    }
    return img;
}

}  // namespace png2amiga::nibnib
