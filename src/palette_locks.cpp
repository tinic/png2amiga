#include "palette_locks.hpp"

#include "color_space.hpp"
#include "palette.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <unordered_set>

namespace png2amiga::palette_locks {

namespace {

Color3f clamp_srgb(int r, int g, int b) {
    auto clamp = [](int v) {
        return static_cast<std::uint8_t>(std::clamp(v, 0, 255));
    };
    return color_space::srgb_u8_to_linear(clamp(r), clamp(g), clamp(b));
}

} // namespace

Color3f to_color(const LockSpec& lock,
                 amiga::Chipset chipset,
                 amiga::Mode mode) {
    auto c = clamp_srgb(lock.r, lock.g, lock.b);
    if (amiga::is_stf(mode)) {
        return palette::quantize_to_stf(c);
    }
    if (chipset != amiga::Chipset::aga) {
        return palette::quantize_to_ocs(c);
    }
    return c;
}

bool has_lock_at_zero(const std::vector<LockSpec>& locks) {
    return std::any_of(locks.begin(), locks.end(),
                       [](const LockSpec& l) { return l.index == 0; });
}

Result<void> validate_locks(const std::vector<LockSpec>& locks,
                            std::size_t max_colors) {
    std::unordered_set<int> seen;
    for (auto& lock : locks) {
        if (lock.index < 0 || static_cast<std::size_t>(lock.index) >= max_colors) {
            return std::unexpected{Error{
                ErrorCode::invalid_depth,
                std::format("--lock-index {} out of range (palette has {} slots)",
                            lock.index, max_colors),
            }};
        }
        if (!seen.insert(lock.index).second) {
            return std::unexpected{Error{
                ErrorCode::invalid_depth,
                std::format("--lock-index {} specified more than once", lock.index),
            }};
        }
    }
    return {};
}

Result<std::size_t> validate_reserves(
    const std::vector<ReserveSpec>& reserves,
    const std::vector<LockSpec>& locks,
    std::size_t max_colors,
    bool lock_zero_black) {
    if (reserves.empty()) return std::size_t{0};
    std::unordered_set<int> locked;
    for (auto& l : locks) locked.insert(l.index);
    if (lock_zero_black && !has_lock_at_zero(locks)) locked.insert(0);
    std::unordered_set<int> seen;
    std::size_t in_palette = 0;
    for (auto& r : reserves) {
        if (r.index < 0) {
            return std::unexpected{Error{ErrorCode::invalid_depth,
                std::format("--reserve-range index {} negative", r.index)}};
        }
        // Open-end ranges (e.g. "5-") were expanded up to 255 at parse
        // time; silently drop entries past the actual palette.
        if (static_cast<std::size_t>(r.index) >= max_colors) continue;
        if (!seen.insert(r.index).second) {
            return std::unexpected{Error{ErrorCode::invalid_depth,
                std::format("--reserve-range: index {} listed twice", r.index)}};
        }
        if (locked.contains(r.index)) {
            return std::unexpected{Error{ErrorCode::invalid_depth,
                std::format("--reserve-range: index {} is already locked "
                            "(via --lock-index or implicit black-zero)",
                            r.index)}};
        }
        ++in_palette;
    }
    if (in_palette >= max_colors) {
        return std::unexpected{Error{ErrorCode::invalid_depth,
            std::format("--reserve-range covers all {} palette slots — "
                        "leave at least one for image content",
                        max_colors)}};
    }
    return in_palette;
}

Result<void> validate_pins(const std::vector<PinSpec>& pins,
                           const std::vector<LockSpec>& locks,
                           std::size_t max_colors,
                           std::size_t image_w,
                           std::size_t image_h,
                           bool lock_zero_black) {
    std::unordered_set<int> locked_indices;
    for (auto& l : locks) locked_indices.insert(l.index);
    if (lock_zero_black && !has_lock_at_zero(locks))
        locked_indices.insert(0);

    std::unordered_set<int> pin_targets;
    for (auto& pin : pins) {
        if (pin.index < 0 || static_cast<std::size_t>(pin.index) >= max_colors) {
            return std::unexpected{Error{
                ErrorCode::invalid_depth,
                std::format("--pin-index-at {} out of range (palette has {} slots)",
                            pin.index, max_colors),
            }};
        }
        if (locked_indices.contains(pin.index)) {
            return std::unexpected{Error{
                ErrorCode::invalid_depth,
                std::format("--pin-index-at {} targets a locked slot",
                            pin.index),
            }};
        }
        if (!pin_targets.insert(pin.index).second) {
            return std::unexpected{Error{
                ErrorCode::invalid_depth,
                std::format("--pin-index-at {} specified more than once",
                            pin.index),
            }};
        }
        if (pin.x < 0 || pin.y < 0 ||
            static_cast<std::size_t>(pin.x) >= image_w ||
            static_cast<std::size_t>(pin.y) >= image_h) {
            return std::unexpected{Error{
                ErrorCode::invalid_dimensions,
                std::format("--pin-index-at {} ({},{}) is outside image {}x{}",
                            pin.index, pin.x, pin.y, image_w, image_h),
            }};
        }
    }
    return {};
}

std::size_t quant_count(std::size_t max_colors,
                        const std::vector<LockSpec>& locks,
                        bool lock_zero_black) {
    auto used = locks.size();
    if (lock_zero_black && !has_lock_at_zero(locks)) {
        ++used;
    }
    if (used >= max_colors) return 1;
    return max_colors - used;
}

Result<Palette> two_pass_quantize(
    std::function<Result<Palette>(std::size_t)> quantize_fn,
    std::size_t kfirst,
    std::size_t kfallback,
    bool lock_color0) {
    auto first = quantize_fn(kfirst);
    if (!first) return std::unexpected{first.error()};
    if (!lock_color0 || kfirst == kfallback
        || !contains_locked_black(*first)) {
        return first;
    }
    auto second = quantize_fn(kfallback);
    if (!second) return std::unexpected{second.error()};
    return second;
}

bool contains_locked_black(const Palette& palette) {
    constexpr float eps = 1e-6f;
    for (auto& c : palette.colors) {
        if (std::abs(c.r) < eps && std::abs(c.g) < eps
            && std::abs(c.b) < eps) return true;
    }
    return false;
}

void finalize_palette(std::vector<Color3f>& colors,
                      std::size_t num_colors,
                      bool lock_color0) {
    if (lock_color0) {
        constexpr float eps = 1e-6f;
        auto is_black = [](const Color3f& c) {
            return std::abs(c.r) < eps
                && std::abs(c.g) < eps
                && std::abs(c.b) < eps;
        };
        for (auto it = colors.begin(); it != colors.end(); ++it) {
            if (is_black(*it)) { colors.erase(it); break; }
        }
        colors.insert(colors.begin(), Color3f{0.0f, 0.0f, 0.0f});
    }
    if (colors.size() > num_colors) colors.resize(num_colors);
    while (colors.size() < num_colors)
        colors.push_back(Color3f{0.0f, 0.0f, 0.0f});
}

AssembledPalette assemble_locked_palette(
    const Palette& quantized,
    const std::vector<LockSpec>& locks,
    std::size_t max_colors,
    bool lock_zero_black,
    amiga::Chipset chipset,
    amiga::Mode mode,
    const std::vector<bool>& reserved_skip) {

    AssembledPalette out;
    out.palette.name = quantized.name;
    out.palette.colors.assign(max_colors, Color3f{0.0f, 0.0f, 0.0f});
    out.locked.assign(max_colors, false);

    // Place locked slots
    for (auto& lock : locks) {
        auto idx = static_cast<std::size_t>(lock.index);
        if (idx >= max_colors) continue;  // validate_locks should have caught this
        out.palette.colors[idx] = to_color(lock, chipset, mode);
        out.locked[idx] = true;
    }

    // Reserve index 0 = black if requested AND not user-locked
    if (lock_zero_black && !out.locked[0]) {
        out.palette.colors[0] = Color3f{0.0f, 0.0f, 0.0f};
        out.locked[0] = true;
    }

    // Fill remaining slots from quantized palette in order, skipping
    // any quantizer entry that exactly matches a *locked* slot (e.g.
    // when lock_zero=true and the quantizer naturally picked black,
    // both ending up at index 0 and 1 → wasted entry). Caller asks
    // the quantizer for at least `max_colors` candidates so the
    // dedupe pass has a spare to drop.
    //
    // Note: we don't dedupe against already-placed *quantizer* slots
    // because median-cut + post-snap can collapse multiple centroids
    // onto the same gamut grid point (most common in VGA-13h's 18-bit
    // and STF's 9-bit). Filtering those out leaves trailing slots at
    // default (0,0,0), which renders as duplicate blacks — visually
    // worse than the original snap-collision duplicate. The proper
    // fix there is a gamut-aware quantizer; for now we accept the
    // residual duplication and only catch the lock-vs-quantizer
    // case which is what users notice on lores / hires / HAM / EHB.
    // Bit-exact equality. OCS / STF / VGA quantizers all produce
    // grid-snapped output where the locked color (also grid-snapped
    // via to_color) lands on the same exact float value when they
    // collide → tight eps catches the user-reported makena lores-d=5
    // dup-black case. AGA / median-cut produces continuous-space
    // centroids where near-black float values aren't bit-equal to
    // (0,0,0), so we don't drop them — they fill all 256 slots
    // (visually some may byte-display as #000000 due to median-cut +
    // 8-bit DAC rounding, but no slot is left empty by the dedupe).
    constexpr float eps = 1e-6f;
    auto eq = [](const Color3f& a, const Color3f& b) {
        return std::abs(a.r - b.r) < eps
            && std::abs(a.g - b.g) < eps
            && std::abs(a.b - b.b) < eps;
    };
    // Dedupe against ALL locked slots (lock_zero, user --lock-index,
    // --reserve-range). Critical when the user pins many slots to
    // colours from the existing palette: the quantizer's naturals
    // coincide bit-exactly with those locks, so without dedupe we'd
    // place each colour twice (once at the locked slot, once at a
    // quantizer-fill slot) and the palette would report fewer unique
    // colours than max_colors (e.g. 31 of 32 with one such collision).
    //
    // The caller compensates by asking the quantizer for the FULL
    // max_colors - lock_zero count — NOT max_colors - lock_zero -
    // locks - reserves — so the dedupe always has enough surplus
    // candidates to leave the unlocked tail filled, even when many
    // quantizer entries get dropped.
    auto is_locked_dup = [&](const Color3f& c) {
        for (std::size_t i = 0; i < max_colors; ++i)
            if (out.locked[i] && eq(c, out.palette.colors[i])) return true;
        return false;
    };
    std::size_t qi = 0;
    for (std::size_t i = 0; i < max_colors; ++i) {
        if (out.locked[i]) continue;
        // Skip indices the caller will overlay with reserves.
        if (i < reserved_skip.size() && reserved_skip[i]) continue;
        while (qi < quantized.colors.size()
               && is_locked_dup(quantized.colors[qi])) {
            ++qi;
        }
        if (qi < quantized.colors.size()) {
            out.palette.colors[i] = quantized.colors[qi++];
        }
    }

    return out;
}

Result<void> apply_pins(Palette& palette,
                        std::vector<std::uint8_t>& indices,
                        std::vector<bool>& locked,
                        const std::vector<PinSpec>& pins,
                        std::size_t image_w,
                        std::size_t image_h) {
    for (auto& pin : pins) {
        if (pin.index < 0 || static_cast<std::size_t>(pin.index) >= palette.colors.size()) {
            return std::unexpected{Error{
                ErrorCode::invalid_depth,
                std::format("--pin-index-at {} out of range", pin.index),
            }};
        }
        if (pin.x < 0 || pin.y < 0 ||
            static_cast<std::size_t>(pin.x) >= image_w ||
            static_cast<std::size_t>(pin.y) >= image_h) {
            return std::unexpected{Error{
                ErrorCode::invalid_dimensions,
                std::format("--pin-index-at {} ({},{}) is outside image {}x{}",
                            pin.index, pin.x, pin.y, image_w, image_h),
            }};
        }
        auto pixel_offset = static_cast<std::size_t>(pin.y) * image_w +
                            static_cast<std::size_t>(pin.x);
        if (pixel_offset >= indices.size()) {
            return std::unexpected{Error{
                ErrorCode::invalid_dimensions,
                std::format("--pin-index-at {}: pixel offset out of bounds",
                            pin.index),
            }};
        }
        auto src = static_cast<std::size_t>(indices[pixel_offset]);
        auto target = static_cast<std::size_t>(pin.index);
        if (src == target) {
            // Already there — just lock it.
            if (target < locked.size()) locked[target] = true;
            continue;
        }
        if (target < locked.size() && locked[target]) {
            return std::unexpected{Error{
                ErrorCode::invalid_depth,
                std::format("--pin-index-at {} targets a locked slot",
                            pin.index),
            }};
        }
        if (target >= palette.colors.size() || src >= palette.colors.size()) {
            return std::unexpected{Error{
                ErrorCode::invalid_depth,
                std::format("--pin-index-at {}: source/target out of palette",
                            pin.index),
            }};
        }
        // Swap palette entries
        std::swap(palette.colors[src], palette.colors[target]);
        // Swap all pixel indices using these two slots
        auto src_u8 = static_cast<std::uint8_t>(src);
        auto tgt_u8 = static_cast<std::uint8_t>(target);
        for (auto& idx : indices) {
            if (idx == src_u8) idx = tgt_u8;
            else if (idx == tgt_u8) idx = src_u8;
        }
        // Lock the target so subsequent pins can't stomp it.
        // Source slot is no longer "the locked one" (its content moved to target).
        if (target < locked.size()) locked[target] = true;
    }
    return {};
}

void sort_by_brightness(std::vector<Color3f>& palette,
                        const std::vector<bool>& locked,
                        std::vector<std::uint8_t>& indices,
                        std::size_t sort_n,
                        bool hb_mirror) {
    if (sort_n > palette.size()) sort_n = palette.size();
    if (sort_n < 2) return;

    auto is_locked = [&](std::size_t i) -> bool {
        return i < locked.size() && locked[i];
    };

    // Collect unlocked indices and sort them by their colour's OKLab L.
    std::vector<std::size_t> unlocked_src;
    unlocked_src.reserve(sort_n);
    std::vector<std::size_t> unlocked_dst;
    unlocked_dst.reserve(sort_n);
    for (std::size_t i = 0; i < sort_n; ++i) {
        if (is_locked(i)) continue;
        unlocked_src.push_back(i);
        unlocked_dst.push_back(i);
    }
    std::sort(unlocked_src.begin(), unlocked_src.end(),
        [&](std::size_t a, std::size_t b) {
            auto la = color_space::linear_to_oklab(palette[a]).L;
            auto lb = color_space::linear_to_oklab(palette[b]).L;
            return la < lb;
        });

    // perm[old_base_idx] = new_base_idx for indices in [0, sort_n).
    // Locked slots map to themselves; unlocked source k maps to
    // unlocked destination k (the k-th remaining slot in palette order).
    std::vector<std::uint8_t> perm(sort_n);
    for (std::size_t i = 0; i < sort_n; ++i) perm[i] = static_cast<std::uint8_t>(i);
    for (std::size_t k = 0; k < unlocked_src.size(); ++k) {
        perm[unlocked_src[k]] = static_cast<std::uint8_t>(unlocked_dst[k]);
    }

    // Apply the permutation by writing into a fresh array (the cycle-
    // aware in-place version is fiddly and the palettes are tiny).
    std::vector<Color3f> reordered = palette;
    for (std::size_t i = 0; i < sort_n; ++i) {
        reordered[perm[i]] = palette[i];
    }

    // For EHB: rebuild the half-brite section from the reordered base.
    // The HB indices [32, 64) track base via index = base_perm[base] + 32.
    if (hb_mirror && palette.size() >= 64 && sort_n == 32) {
        for (std::size_t i = 0; i < 32; ++i) {
            reordered[32 + i] = palette::half_brite(reordered[i]);
        }
    }
    palette = std::move(reordered);

    // Remap dither indices.
    for (auto& idx : indices) {
        if (idx < sort_n) {
            idx = perm[idx];
        } else if (hb_mirror && idx >= 32 && idx < 32 + sort_n) {
            idx = static_cast<std::uint8_t>(perm[idx - 32] + 32);
        }
    }
}

} // namespace png2amiga::palette_locks
