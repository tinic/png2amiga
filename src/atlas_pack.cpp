#include "atlas_pack.hpp"

#include "color_space.hpp"
#include "png_io.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <numeric>
#include <unordered_map>
#include <vector>

namespace png2amiga::atlas_pack {

namespace {

std::uint8_t linear_to_srgb_byte(float v) noexcept {
    float c = color_space::linear_to_srgb(v);
    int i = static_cast<int>(c * 255.0f + 0.5f);
    return static_cast<std::uint8_t>(std::clamp(i, 0, 255));
}

// Loaded source sprite — the full decoded image plus its original name.
struct Loaded {
    std::string name;
    Image image;
};

// Shelf-packing state for one page. Advances shelves top-to-bottom, left to
// right within each shelf. A shelf's height is fixed by its first placed rect.
struct PageShelf {
    std::size_t shelf_y{0};
    std::size_t shelf_h{0};
    std::size_t shelf_x{0};
};

std::vector<Color3f> collect_palette(std::span<const Loaded> inputs,
                                     float alpha_threshold,
                                     bool exclude_transparent) {
    std::unordered_map<std::uint32_t, std::size_t> seen;
    std::vector<Color3f> pal;
    for (auto& in : inputs) {
        auto& img = in.image;
        for (std::size_t y = 0; y < img.height(); ++y) {
            for (std::size_t x = 0; x < img.width(); ++x) {
                if (exclude_transparent && img.has_alpha() &&
                    img.alpha_at(x, y) < alpha_threshold) continue;
                auto& p = img[x, y];
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

Result<PackResult> pack_atlas(const std::vector<Input>& inputs,
                              const Options& opts) {
    if (opts.page_w % 16 != 0) {
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
            std::format("page_w {} must be a multiple of 16", opts.page_w)}};
    }
    if (opts.depth < 1 || opts.depth > 8) {
        return std::unexpected{Error{ErrorCode::invalid_depth,
            std::format("depth must be 1..8, got {}", opts.depth)}};
    }

    // Load every input into memory. We sort a copy of indices by height
    // descending to improve shelf-packing utilization.
    std::vector<Loaded> loaded;
    loaded.reserve(inputs.size());
    for (auto& in : inputs) {
        auto img = png_io::load(in.path);
        if (!img) return std::unexpected{img.error()};
        loaded.push_back({in.name, std::move(*img)});
    }

    // Shared palette across all inputs. Optionally reserve color 0 for
    // transparency; the rest comes from each input's visible pixels in
    // first-seen order.
    bool any_alpha = false;
    for (auto& l : loaded) if (l.image.has_alpha()) { any_alpha = true; break; }
    auto visible = collect_palette(loaded, opts.alpha_threshold,
                                   opts.reserve_color0 && any_alpha);
    std::vector<Color3f> pal;
    if (opts.reserve_color0) pal.push_back({0.0f, 0.0f, 0.0f});
    for (auto& c : visible) pal.push_back(c);
    auto max_colors = std::size_t{1} << opts.depth;
    if (pal.size() > max_colors) {
        return std::unexpected{Error{ErrorCode::unsupported_mode,
            std::format("atlas has {} unique colors, depth {} allows only {}",
                        pal.size(), opts.depth, max_colors)}};
    }
    auto pal_lookup = build_palette_lookup(pal);

    // Order by height descending for shelf packing (classic FFDH).
    std::vector<std::size_t> order(loaded.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        auto& ia = loaded[a].image;
        auto& ib = loaded[b].image;
        if (ia.height() != ib.height()) return ia.height() > ib.height();
        return ia.width() > ib.width();
    });

    // Per-page pixel buffers (indexed) + optional mask buffers. Allocated on
    // demand as new pages are started.
    struct Page {
        std::vector<std::uint8_t> indices;
        std::vector<std::uint8_t> mask;
        PageShelf shelf;
    };
    auto page_sz = opts.page_w * opts.page_h;

    std::vector<Page> pages;
    bool emit_mask = opts.emit_mask && any_alpha;

    auto open_page = [&]() {
        pages.emplace_back();
        pages.back().indices.assign(page_sz, 0);
        if (emit_mask) pages.back().mask.assign(page_sz, 0);
    };

    std::vector<Entry> placements(loaded.size());

    auto place = [&](std::size_t id) -> Result<void> {
        auto& src = loaded[id].image;
        auto w = src.width();
        auto h = src.height();
        auto w_pad = ((w + 15) / 16) * 16;
        if (w_pad > opts.page_w || h > opts.page_h) {
            return std::unexpected{Error{ErrorCode::invalid_dimensions,
                std::format("'{}' {}x{} (padded {}x{}) exceeds page {}x{}",
                            loaded[id].name, w, h, w_pad, h,
                            opts.page_w, opts.page_h)}};
        }

        // Try placing into the current page's shelf; advance shelves as needed;
        // open a new page if the image won't fit anywhere in the current one.
        while (true) {
            if (pages.empty()) open_page();
            auto& pg = pages.back();
            auto& s = pg.shelf;

            // Shelf init: if empty, set its height to the sprite's.
            if (s.shelf_h == 0) {
                s.shelf_h = h;
                s.shelf_x = 0;
            }

            // Does it fit on the current shelf?
            if (s.shelf_x + w_pad <= opts.page_w && h <= s.shelf_h) {
                // Copy sprite into page at (s.shelf_x, s.shelf_y). Padding
                // pixels stay 0.
                for (std::size_t y = 0; y < h; ++y) {
                    for (std::size_t x = 0; x < w; ++x) {
                        auto& p = src[x, y];
                        auto alpha = src.alpha_at(x, y);
                        bool transparent = src.has_alpha() && alpha < opts.alpha_threshold;
                        std::uint8_t idx = 0;
                        if (!transparent) {
                            std::uint32_t key =
                                (static_cast<std::uint32_t>(linear_to_srgb_byte(p.r)) << 16) |
                                (static_cast<std::uint32_t>(linear_to_srgb_byte(p.g)) << 8) |
                                static_cast<std::uint32_t>(linear_to_srgb_byte(p.b));
                            auto it = pal_lookup.find(key);
                            idx = (it != pal_lookup.end()) ? it->second : std::uint8_t{0};
                        }
                        auto dst_x = s.shelf_x + x;
                        auto dst_y = s.shelf_y + y;
                        pg.indices[dst_y * opts.page_w + dst_x] = idx;
                        if (emit_mask) {
                            pg.mask[dst_y * opts.page_w + dst_x] = transparent ? 0 : 1;
                        }
                    }
                }
                placements[id] = Entry{
                    loaded[id].name,
                    static_cast<std::uint8_t>(pages.size() - 1),
                    static_cast<std::uint16_t>(s.shelf_x),
                    static_cast<std::uint16_t>(s.shelf_y),
                    static_cast<std::uint16_t>(w),
                    static_cast<std::uint16_t>(h),
                    static_cast<std::uint16_t>(w_pad),
                };
                s.shelf_x += w_pad;
                return {};
            }

            // Doesn't fit on current shelf — advance to the next shelf.
            auto next_shelf_y = s.shelf_y + s.shelf_h;
            if (next_shelf_y + h <= opts.page_h) {
                s.shelf_y = next_shelf_y;
                s.shelf_h = 0;
                continue;
            }

            // Doesn't fit on this page at all — start a new page.
            open_page();
        }
    };

    for (auto id : order) {
        auto r = place(id);
        if (!r) return std::unexpected{r.error()};
    }

    // Bitplane-encode each page. Also optionally encode masks.
    PackResult out;
    out.pages.reserve(pages.size());
    out.masks.reserve(emit_mask ? pages.size() : 0);
    std::size_t used_pixels = 0;
    for (auto& pg : pages) {
        auto enc = bitplane::encode(pg.indices, opts.page_w, opts.page_h,
                                    opts.depth, opts.layout);
        if (!enc) return std::unexpected{enc.error()};
        out.pages.push_back(std::move(*enc));
        if (emit_mask) {
            auto menc = bitplane::encode(pg.mask, opts.page_w, opts.page_h, 1,
                                         opts.layout);
            if (!menc) return std::unexpected{menc.error()};
            out.masks.push_back(std::move(menc->data));
        }
    }
    for (auto& e : placements) used_pixels += std::size_t{e.w_padded} * e.h;

    out.entries = std::move(placements);
    out.palette = std::move(pal);
    out.page_w = opts.page_w;
    out.page_h = opts.page_h;
    out.page_bpr = out.pages.empty() ? 0 : out.pages.front().bytes_per_row;
    out.page_bytes = out.pages.empty() ? 0 : out.pages.front().total_bytes();
    out.page_mask_bytes = emit_mask ? (opts.page_h * out.page_bpr) : 0;
    out.depth = opts.depth;
    out.has_mask = emit_mask;
    auto total_page_px = pages.size() * page_sz;
    out.utilization = total_page_px
        ? static_cast<float>(used_pixels) / static_cast<float>(total_page_px)
        : 0.0f;
    return out;
}

} // namespace png2amiga::atlas_pack
