#pragma once

// Concepts that describe what a png2amiga-assets-generated namespace must
// expose so the engine can consume it with zero runtime conversion.
//
// The generated .hpp files look like:
//   namespace assets::my_tileset {
//     inline constexpr unsigned tile_w = 16;
//     ...
//     extern const std::array<u16, ...> tiles_data;
//     extern const std::array<MapCell, ...> tilemap;
//     extern const std::array<u16, ...> palette;
//   }
//
// Engine code takes these via a template parameter `class Asset` and lets
// the compiler static-dispatch blit loops against the compile-time sizes.

#include "types.hpp"

#include <concepts>
#include <cstddef>
#include <type_traits>

namespace pa {

// Asset adapters — one thin `struct` per generated asset namespace. The
// namespace exposes `extern const std::array` data and scalar constants;
// since a namespace can't be a type-parameter, engine users wrap it in a
// small struct that forwards:
//   - constants as `static constexpr unsigned`
//   - data accessors as `static ... ptr()` / `static auto palette_span()`
//   - helper functions as `static` methods
//
// The concepts below match that wrapper shape. For multiple assets in one
// project, boilerplate a macro (see examples/).

template <class A>
concept TilesetAsset = requires(unsigned x, unsigned y) {
    { A::tile_w }        -> std::convertible_to<unsigned>;
    { A::tile_h }        -> std::convertible_to<unsigned>;
    { A::depth }         -> std::convertible_to<unsigned>;
    { A::bytes_per_row } -> std::convertible_to<unsigned>;
    { A::tile_bytes }    -> std::convertible_to<unsigned>;
    { A::num_tiles }     -> std::convertible_to<unsigned>;
    { A::map_w }         -> std::convertible_to<unsigned>;
    { A::map_h }         -> std::convertible_to<unsigned>;
    { A::palette_size }  -> std::convertible_to<unsigned>;
    { A::tile_ptr(u16{0}) } -> std::convertible_to<const u16*>;
    { A::cell_at(x, y).index() } -> std::convertible_to<u16>;
    { A::palette_ptr() };
};

template <class A>
concept StripAsset = requires(unsigned n, unsigned k, unsigned x) {
    { A::frame_w }       -> std::convertible_to<unsigned>;
    { A::frame_h }       -> std::convertible_to<unsigned>;
    { A::storage_w }     -> std::convertible_to<unsigned>;
    { A::depth }         -> std::convertible_to<unsigned>;
    { A::bytes_per_row } -> std::convertible_to<unsigned>;
    { A::frame_bytes }   -> std::convertible_to<unsigned>;
    { A::num_frames }    -> std::convertible_to<unsigned>;
    { A::preshift }      -> std::convertible_to<unsigned>;
    { A::palette_size }  -> std::convertible_to<unsigned>;
    { A::has_mask }      -> std::convertible_to<bool>;
    { A::frame_ptr(n, k) }    -> std::convertible_to<const u16*>;
    { A::shift_for_x(x) }     -> std::convertible_to<unsigned>;
    { A::palette_ptr() };
};

template <class A>
concept AtlasAsset = requires(unsigned p) {
    { A::page_w }        -> std::convertible_to<unsigned>;
    { A::page_h }        -> std::convertible_to<unsigned>;
    { A::depth }         -> std::convertible_to<unsigned>;
    { A::page_bpr }      -> std::convertible_to<unsigned>;
    { A::page_bytes }    -> std::convertible_to<unsigned>;
    { A::num_pages }     -> std::convertible_to<unsigned>;
    { A::num_entries }   -> std::convertible_to<unsigned>;
    { A::palette_size }  -> std::convertible_to<unsigned>;
    { A::has_mask }      -> std::convertible_to<bool>;
    { A::page_ptr(p) }   -> std::convertible_to<const u16*>;
    { A::palette_ptr() };
};

// ---------------------------------------------------------------------------
// Palette-element detection: the adapter exposes a `palette_ptr()` function
// whose return type distinguishes OCS (`const u16*`) from AGA (`const u32*`).
// ---------------------------------------------------------------------------

template <class Asset>
using palette_element_t =
    std::remove_cvref_t<std::remove_pointer_t<decltype(Asset::palette_ptr())>>;

template <class Asset>
inline constexpr bool is_ocs_palette =
    std::is_same_v<palette_element_t<Asset>, u16>;

template <class Asset>
inline constexpr bool is_aga_palette =
    std::is_same_v<palette_element_t<Asset>, u32>;

} // namespace pa
