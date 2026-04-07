#pragma once

#include "types.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace png2amiga::bitplane {

// ---------------------------------------------------------------------------
// Bitplane layout — how bitplanes are organized in memory
// ---------------------------------------------------------------------------

enum class Layout : unsigned char {
    // Non-interleaved (standard): all rows of plane 0, then all rows of plane 1, etc.
    // Used by most paint programs and IFF ILBM with non-interleaved body.
    // Memory: [plane0_row0][plane0_row1]...[plane0_rowN][plane1_row0]...
    standard,

    // Interleaved (line-interleaved): for each row, plane 0..N in sequence.
    // Used by Amiga hardware display (copper DMA), most demos, and games.
    // Memory: [row0_plane0][row0_plane1]...[row0_planeN][row1_plane0]...
    interleaved,
};

// ---------------------------------------------------------------------------
// Bitplane data — the encoded result
// ---------------------------------------------------------------------------

struct BitplaneData {
    std::vector<std::uint8_t> data;     // raw bitplane bytes
    std::size_t width{};                // image width in pixels
    std::size_t height{};               // image height in pixels
    std::size_t depth{};                // number of bitplanes
    std::size_t bytes_per_row{};        // bytes per row per plane (width/8, word-aligned)
    Layout layout{Layout::interleaved};

    // Total size: depth * height * bytes_per_row
    [[nodiscard]] std::size_t total_bytes() const noexcept {
        return depth * height * bytes_per_row;
    }

    // Offset to a specific row of a specific plane
    [[nodiscard]] std::size_t plane_row_offset(std::size_t plane,
                                                std::size_t row) const noexcept {
        if (layout == Layout::interleaved) {
            // row * (depth * bytes_per_row) + plane * bytes_per_row
            return row * depth * bytes_per_row + plane * bytes_per_row;
        }
        // standard: plane * (height * bytes_per_row) + row * bytes_per_row
        return plane * height * bytes_per_row + row * bytes_per_row;
    }
};

// ---------------------------------------------------------------------------
// Encode indexed image to bitplane data
//
// pixel_indices: row-major array of palette indices (width * height entries).
//                Each value must be < (1 << depth).
// width, height: image dimensions. Width is rounded up to next multiple of 16
//                for word alignment (standard Amiga requirement).
// depth:         number of bitplanes (1-8).
// layout:        interleaved or standard ordering.
// ---------------------------------------------------------------------------

Result<BitplaneData> encode(const std::vector<std::uint8_t>& pixel_indices,
                            std::size_t width, std::size_t height,
                            std::size_t depth,
                            Layout layout = Layout::interleaved);

// ---------------------------------------------------------------------------
// Decode bitplane data back to indexed pixels
//
// Returns a vector of palette indices (width * height entries).
// ---------------------------------------------------------------------------

Result<std::vector<std::uint8_t>> decode(const BitplaneData& planes);

// ---------------------------------------------------------------------------
// Convert an Image + Palette into bitplane data.
// Each pixel is mapped to nearest palette color, then encoded.
// Returns the bitplane data and the palette indices used.
// ---------------------------------------------------------------------------

struct EncodeResult {
    BitplaneData planes;
    std::vector<std::uint8_t> indices;      // per-pixel palette index
    std::vector<Color3f> used_palette;       // the palette colors actually used
    float total_error{};                     // sum of perceptual error
};

Result<EncodeResult> encode_image(const Image& image,
                                  const Palette& palette,
                                  std::size_t depth,
                                  Layout layout = Layout::interleaved);

// ---------------------------------------------------------------------------
// Render bitplane data back to an Image for preview
// ---------------------------------------------------------------------------

Result<Image> render(const BitplaneData& planes,
                     std::span<const Color3f> palette);

} // namespace png2amiga::bitplane
