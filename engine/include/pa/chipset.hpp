#pragma once

// Chipset selection is a compile-time property of the build target.
// The same asset sources compile into either OCS (ECS) or AGA binaries by
// flipping the PA_CHIPSET define at configure time. Keep all chipset-
// dependent logic gated through the `chipset_traits` static table below, so
// the engine never conditionally compiles on the chipset at the call site.
//
// PA_CHIPSET_OCS is the default if nothing is defined. Define PA_CHIPSET_AGA
// to target AGA.

namespace pa {

enum class Chipset {
    OCS,
    AGA,
};

#if defined(PA_CHIPSET_AGA)
inline constexpr Chipset active_chipset = Chipset::AGA;
#else
inline constexpr Chipset active_chipset = Chipset::OCS;
#endif

// Static traits — use via `chipset_traits<active_chipset>::max_colors`.
template <Chipset C>
struct chipset_traits;

template <>
struct chipset_traits<Chipset::OCS> {
    static constexpr unsigned max_colors      = 32;    // 5 bitplanes
    static constexpr unsigned max_depth       = 5;     // single-playfield
    static constexpr unsigned max_sprites_hw  = 8;
    static constexpr unsigned palette_bits    = 12;    // 0x0RGB
    static constexpr bool     has_aga_fmode   = false;
};

template <>
struct chipset_traits<Chipset::AGA> {
    static constexpr unsigned max_colors      = 256;   // 8 bitplanes
    static constexpr unsigned max_depth       = 8;
    static constexpr unsigned max_sprites_hw  = 8;
    static constexpr unsigned palette_bits    = 24;    // 0x00RRGGBB
    static constexpr bool     has_aga_fmode   = true;
};

using active_traits = chipset_traits<active_chipset>;

} // namespace pa
