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

Result<void> validate_pins(const std::vector<PinSpec>& pins,
                           const std::vector<LockSpec>& locks,
                           std::size_t max_colors,
                           std::size_t image_w,
                           std::size_t image_h,
                           bool reserve_zero_black) {
    std::unordered_set<int> locked_indices;
    for (auto& l : locks) locked_indices.insert(l.index);
    if (reserve_zero_black && !has_lock_at_zero(locks))
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
                        bool reserve_zero_black) {
    auto used = locks.size();
    if (reserve_zero_black && !has_lock_at_zero(locks)) {
        ++used;
    }
    if (used >= max_colors) return 1;
    return max_colors - used;
}

AssembledPalette assemble_locked_palette(
    const Palette& quantized,
    const std::vector<LockSpec>& locks,
    std::size_t max_colors,
    bool reserve_zero_black,
    amiga::Chipset chipset,
    amiga::Mode mode) {

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
    if (reserve_zero_black && !out.locked[0]) {
        out.palette.colors[0] = Color3f{0.0f, 0.0f, 0.0f};
        out.locked[0] = true;
    }

    // Fill remaining slots from quantized palette in order
    std::size_t qi = 0;
    for (std::size_t i = 0; i < max_colors; ++i) {
        if (out.locked[i]) continue;
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

} // namespace png2amiga::palette_locks
