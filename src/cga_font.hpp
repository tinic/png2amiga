#pragma once

// CGA 8x8 font data. Used by cga_text80x100 graphics mode for per-cell
// glyph matching.
//
// Source: viler-int10h/vga-text-mode-fonts (CGA.F08), a public-domain
// reconstruction of the IBM PC 5150/5160 Color Graphics Adapter character
// ROM. 256 glyphs × 8 scanlines × 1 byte each = 2048 bytes total. Each
// byte is an 8-pixel scanline, MSB-first: bit 7 = leftmost pixel.

#include <array>
#include <cstdint>

namespace png2amiga::palette {

#include "cga_font_data.inc"

// Get a specific scanline (0..7) of CGA glyph `ch` as an 8-bit pattern.
constexpr std::uint8_t cga_glyph_scanline(std::uint8_t ch, std::size_t line) noexcept {
    return kCgaFont8x8[static_cast<std::size_t>(ch) * 8 + (line & 7)];
}

// Font descriptor passed to glyph-matching encoders.
struct FontRef {
    const std::uint8_t* data;  // pointer to contiguous glyph bytes
    std::size_t glyph_height;  // scanlines per glyph
};

inline constexpr FontRef kFontCga8x8{kCgaFont8x8.data(), 8};

constexpr std::uint8_t font_scanline(const FontRef& f, std::uint8_t ch, std::size_t line) noexcept {
    return f.data[static_cast<std::size_t>(ch) * f.glyph_height +
                  (line < f.glyph_height ? line : line % f.glyph_height)];
}

// ----------------------------------------------------------------------
// Glyph-class dedup primitives (used by the cga-text encoder).
//
// Background: the encoder evaluates (char, fg, bg) per cell. Two
// distinct chars whose visible cell_h scanlines are bit-identical
// render the same pixels for every (fg, bg) — keeping both in the
// search is wasted work. Beyond that, (G, fg=A, bg=B) renders exactly
// the same pixels as (~G, fg=B, bg=A): every "on" pixel becomes "off"
// and the attribute swap relabels the two colours. So when both G and
// ~G appear in the font's cell-pattern set, only one is useful.
//
// These helpers are constexpr so the dedup can run at compile time
// (see `cga_canonical_bitmap` below) and auto-track any font swap —
// nothing is hardcoded against the current CGA character ROM.
// ----------------------------------------------------------------------

// Pack the cell_h scanlines of `ch` starting at `offset` into a u64
// (one byte per scanline, low byte = top scanline).
constexpr std::uint64_t glyph_pattern_key(const FontRef& f,
                                          std::uint8_t ch,
                                          std::size_t cell_h,
                                          std::size_t offset) noexcept {
    std::uint64_t key = 0;
    for (std::size_t line = 0; line < cell_h; ++line) {
        key |= static_cast<std::uint64_t>(font_scanline(f, ch, offset + line)) << (line * 8);
    }
    return key;
}

// Build a cell_h*8-bit fg mask (bit i = pixel i is foreground).
constexpr std::uint64_t glyph_fg_mask(const FontRef& f,
                                      std::uint8_t ch,
                                      std::size_t cell_h,
                                      std::size_t offset) noexcept {
    std::uint64_t mask = 0;
    for (std::size_t line = 0; line < cell_h; ++line) {
        auto sl = font_scanline(f, ch, offset + line);
        for (std::size_t px = 0; px < 8; ++px) {
            if (sl & (0x80u >> px)) {
                mask |= std::uint64_t{1} << (line * 8 + px);
            }
        }
    }
    return mask;
}

// Fold (key, ~key) into a single representative — the smaller of the
// two — so inversion-pair classes share one bucket.
constexpr std::uint64_t glyph_canonical_key(std::uint64_t key, std::size_t cell_h) noexcept {
    std::uint64_t bits_mask = (cell_h >= 8) ? ~std::uint64_t{0}
                                            : (std::uint64_t{1} << (cell_h * 8)) - 1;
    std::uint64_t inv = (~key) & bits_mask;
    return key < inv ? key : inv;
}

// 256-bit bitmap: bit `ch` is set ⇔ `ch` is the lowest-numbered char
// whose (cell_h scanlines starting at `offset`) pattern reduces to
// that class's canonical key.
struct GlyphCanonicalBitmap {
    std::array<std::uint64_t, 4> bits{};
    constexpr bool test(std::uint8_t ch) const noexcept {
        return ((bits[ch >> 6] >> (ch & 63)) & 1u) != 0u;
    }
};

constexpr GlyphCanonicalBitmap make_glyph_canonical_bitmap(const FontRef& f,
                                                           std::size_t cell_h,
                                                           std::size_t offset) noexcept {
    GlyphCanonicalBitmap out{};
    // Linear-search "seen" set sized to the worst case (256 unique
    // classes). consteval contexts can't use std::unordered_set, and
    // for n ≤ 256 the O(n²) walk is trivial at compile time.
    std::array<std::uint64_t, 256> seen{};
    std::size_t n_seen = 0;
    for (int i = 0; i < 256; ++i) {
        auto ch = static_cast<std::uint8_t>(i);
        auto key = glyph_pattern_key(f, ch, cell_h, offset);
        auto canon = glyph_canonical_key(key, cell_h);
        bool dup = false;
        for (std::size_t k = 0; k < n_seen; ++k) {
            if (seen[k] == canon) {
                dup = true;
                break;
            }
        }
        if (dup) continue;
        seen[n_seen++] = canon;
        out.bits[static_cast<std::size_t>(i) >> 6] |= std::uint64_t{1} << (i & 63);
    }
    return out;
}

// CGA cells today are 1 / 2 / 4 / 8 scanlines tall. Index the table
// by a compact 0..3 slot. font_height = 8, so max offset = 8 - cell_h.
constexpr std::size_t cga_cell_h_index(std::size_t cell_h) noexcept {
    return cell_h == 1   ? 0
           : cell_h == 2 ? 1
           : cell_h == 4 ? 2
           : cell_h == 8 ? 3
                         : 0;  // unreachable for current modes
}

// Precomputed canonical bitmaps for kFontCga8x8 across every
// (cell_h, offset) the encoder ever uses. Generated at compile time
// from the font bytes — change the font, and the table tracks
// automatically with zero hardcoded data.
//
// Emscripten's clang refuses the `[](){...}() consteval` IILE form,
// so use a named consteval helper — the result is still required to
// be a constant expression by `inline constexpr auto`, no runtime
// init.
namespace detail_cga {
consteval std::array<std::array<GlyphCanonicalBitmap, 8>, 4>
build_cga_canonical_bitmaps() noexcept {
    std::array<std::array<GlyphCanonicalBitmap, 8>, 4> t{};
    constexpr std::size_t cell_h_vals[] = {1, 2, 4, 8};
    for (std::size_t i = 0; i < 4; ++i) {
        std::size_t cell_h = cell_h_vals[i];
        std::size_t max_off = (kFontCga8x8.glyph_height + 1) - cell_h;
        for (std::size_t off = 0; off < max_off; ++off) {
            t[i][off] = make_glyph_canonical_bitmap(kFontCga8x8, cell_h, off);
        }
    }
    return t;
}
}  // namespace detail_cga

inline constexpr auto kCgaCanonicalBitmaps = detail_cga::build_cga_canonical_bitmaps();

}  // namespace png2amiga::palette
