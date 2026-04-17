#include "strip_pack.hpp"

#include "color_space.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <unordered_map>
#include <vector>

namespace png2amiga::strip_pack {

namespace {

std::uint8_t linear_to_srgb_byte(float v) noexcept {
    float c = color_space::linear_to_srgb(v);
    int i = static_cast<int>(c * 255.0f + 0.5f);
    return static_cast<std::uint8_t>(std::clamp(i, 0, 255));
}

// Build a palette from the non-transparent pixels of the image, in first-seen
// order. If reserve_color0 is true, color 0 is reserved for transparency and
// not included here (caller prepends a Color3f{0,0,0} entry).
std::vector<Color3f> extract_palette(const Image& image,
                                     float alpha_threshold,
                                     bool exclude_transparent) {
    std::unordered_map<std::uint32_t, std::size_t> seen;
    std::vector<Color3f> pal;
    auto w = image.width();
    auto h = image.height();
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            if (exclude_transparent && image.has_alpha() &&
                image.alpha_at(x, y) < alpha_threshold) continue;
            auto& p = image[x, y];
            auto r = linear_to_srgb_byte(p.r);
            auto g = linear_to_srgb_byte(p.g);
            auto b = linear_to_srgb_byte(p.b);
            std::uint32_t key = (static_cast<std::uint32_t>(r) << 16) |
                                (static_cast<std::uint32_t>(g) << 8) |
                                static_cast<std::uint32_t>(b);
            if (seen.insert({key, pal.size()}).second) {
                pal.push_back(p);
            }
        }
    }
    return pal;
}

std::unordered_map<std::uint32_t, std::uint8_t>
build_palette_lookup(std::span<const Color3f> pal) {
    std::unordered_map<std::uint32_t, std::uint8_t> lut;
    for (std::size_t i = 0; i < pal.size(); ++i) {
        auto& p = pal[i];
        std::uint32_t key = (static_cast<std::uint32_t>(linear_to_srgb_byte(p.r)) << 16) |
                            (static_cast<std::uint32_t>(linear_to_srgb_byte(p.g)) << 8) |
                            static_cast<std::uint32_t>(linear_to_srgb_byte(p.b));
        lut[key] = static_cast<std::uint8_t>(i);
    }
    return lut;
}

} // namespace

Result<PackResult> pack_strip(const Image& image, const Options& opts) {
    auto w = image.width();
    auto h = image.height();

    if (opts.frame_w == 0 || opts.frame_h == 0) {
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
            "frame size must be > 0"}};
    }
    // Non-16-aligned logical widths are allowed — we pad storage up to the
    // next word boundary. The logical width is preserved in PackResult.
    if (w % opts.frame_w != 0) {
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
            std::format("strip width {} not a multiple of frame width {}",
                        w, opts.frame_w)}};
    }
    if (h != opts.frame_h) {
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
            std::format("strip height {} ≠ frame height {}", h, opts.frame_h)}};
    }
    if (opts.depth < 1 || opts.depth > 8) {
        return std::unexpected{Error{ErrorCode::invalid_depth,
            std::format("depth must be 1..8, got {}", opts.depth)}};
    }
    if (opts.preshift == 0 || (opts.preshift & (opts.preshift - 1)) != 0 ||
        opts.preshift > 16) {
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
            std::format("preshift {} must be a power of 2 in [1, 16]",
                        opts.preshift)}};
    }

    auto num_frames = w / opts.frame_w;
    auto preshift = opts.preshift;
    auto shift_step = 16u / static_cast<unsigned>(preshift);

    // Storage width: pad to 16 always; with preshift > 1 add 16 more pixels
    // so the largest shift (12 for preshift=4, 14 for preshift=8, etc.) plus
    // the blitter's 16-pixel trailing slack fits.
    auto storage_w_min = opts.frame_w + (preshift > 1 ? 16u : 0u);
    auto storage_w = ((storage_w_min + 15) / 16) * 16;
    auto has_alpha = image.has_alpha();
    bool emit_mask = opts.emit_mask && has_alpha;

    // Palette: optionally reserve color 0 for transparency, then collect
    // remaining palette colors from the opaque pixels.
    auto visible_pal = extract_palette(image, opts.alpha_threshold,
                                       opts.reserve_color0 && has_alpha);
    std::vector<Color3f> pal;
    if (opts.reserve_color0) pal.push_back({0.0f, 0.0f, 0.0f});
    for (auto& c : visible_pal) pal.push_back(c);

    auto max_colors = std::size_t{1} << opts.depth;
    if (pal.size() > max_colors) {
        return std::unexpected{Error{ErrorCode::unsupported_mode,
            std::format("strip has {} unique visible colors, depth {} allows only {}",
                        pal.size(), opts.depth, max_colors)}};
    }

    auto pal_lookup = build_palette_lookup(pal);

    // Layout: (num_frames * preshift) copies stacked vertically at
    // storage_w × frame_h each. Copy k for frame f contains the sprite
    // offset horizontally by k*shift_step pixels; margin pixels are 0.
    auto total_copies = num_frames * preshift;
    std::vector<std::uint8_t> indices(total_copies * storage_w * opts.frame_h, 0);
    std::vector<std::uint8_t> mask_indices;
    if (emit_mask) mask_indices.resize(indices.size());

    for (std::size_t f = 0; f < num_frames; ++f) {
        for (std::size_t k = 0; k < preshift; ++k) {
            auto copy_idx = f * preshift + k;
            auto x_off = k * shift_step;
            for (std::size_t y = 0; y < opts.frame_h; ++y) {
                for (std::size_t x = 0; x < opts.frame_w; ++x) {
                    auto sx = f * opts.frame_w + x;
                    auto sy = y;
                    auto& p = image[sx, sy];
                    auto alpha = image.alpha_at(sx, sy);
                    bool transparent = has_alpha && alpha < opts.alpha_threshold;

                    std::uint8_t idx = 0;
                    if (!transparent) {
                        std::uint32_t key =
                            (static_cast<std::uint32_t>(linear_to_srgb_byte(p.r)) << 16) |
                            (static_cast<std::uint32_t>(linear_to_srgb_byte(p.g)) << 8) |
                            static_cast<std::uint32_t>(linear_to_srgb_byte(p.b));
                        auto it = pal_lookup.find(key);
                        idx = (it != pal_lookup.end()) ? it->second : std::uint8_t{0};
                    }
                    auto dst_x = x + x_off;
                    indices[(copy_idx * opts.frame_h + y) * storage_w + dst_x] = idx;
                    if (emit_mask) {
                        mask_indices[(copy_idx * opts.frame_h + y) * storage_w + dst_x] =
                            transparent ? 0 : 1;
                    }
                }
            }
        }
    }

    auto planes = bitplane::encode(indices, storage_w,
                                   total_copies * opts.frame_h,
                                   opts.depth, opts.layout);
    if (!planes) return std::unexpected{planes.error()};

    PackResult r;
    r.planes = std::move(*planes);
    r.palette = std::move(pal);
    r.num_frames = num_frames;
    r.frame_w = opts.frame_w;
    r.frame_h = opts.frame_h;
    r.storage_w = storage_w;
    r.bytes_per_row = r.planes.bytes_per_row;
    r.frame_bytes = opts.frame_h * opts.depth * r.bytes_per_row;
    r.preshift = preshift;
    r.has_mask = emit_mask;

    if (emit_mask) {
        auto mask_planes = bitplane::encode(mask_indices, storage_w,
                                            total_copies * opts.frame_h, 1,
                                            opts.layout);
        if (!mask_planes) return std::unexpected{mask_planes.error()};
        r.mask_data = std::move(mask_planes->data);
        r.mask_bytes = opts.frame_h * r.bytes_per_row;
    }

    return r;
}

} // namespace png2amiga::strip_pack
