#pragma once

#include "amiga.hpp"
#include "bitplane.hpp"
#include "types.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace png2amiga::iff {

// ---------------------------------------------------------------------------
// IFF ILBM (Interleaved Bitmap) output
//
// IFF is the standard Amiga interchange format. ILBM is the image sub-type.
// Structure:
//   FORM ILBM
//     BMHD  — Bitmap Header (width, height, depth, compression, etc.)
//     CMAP  — Color Map (palette, 3 bytes per color: R, G, B)
//     CAMG  — Amiga viewport mode (HAM, EHB, hires, interlace flags)
//     BODY  — Bitplane data (interleaved, optionally ByteRun1 compressed)
//
// References:
//   - "EA IFF 85" standard
//   - Amiga ROM Kernel Reference Manual: Devices (Appendix A)
// ---------------------------------------------------------------------------

// Compression method for BODY chunk
enum class Compression : std::uint8_t {
    none = 0,           // Uncompressed
    byterun1 = 1,       // PackBits / ByteRun1 (standard IFF compression)
};

// ---------------------------------------------------------------------------
// IFF generation options
// ---------------------------------------------------------------------------

struct IffOptions {
    Compression compression = Compression::byterun1;
    bool interlace = false;         // set LACE bit in CAMG
    bool has_transparency = false;  // BMHD masking=2, transparentColor=0

    // Copper palette: per-scanline palettes written as custom COPL chunk.
    // If non-empty, a COPL chunk is appended after CAMG.
    // Each inner vector has num_colors entries (linear RGB).
    const std::vector<std::vector<Color3f>>* scanline_palettes = nullptr;
};

// ---------------------------------------------------------------------------
// Write IFF ILBM from bitplane data + palette
//
// planes:  encoded bitplane data (must be interleaved layout)
// palette: color palette (size must be <= 2^depth)
// mode:    Amiga graphics mode (for CAMG chunk)
// options: compression, video standard
//
// Returns IFF file bytes.
// ---------------------------------------------------------------------------

Result<std::vector<std::uint8_t>> write_ilbm(
    const bitplane::BitplaneData& planes,
    std::span<const Color3f> palette,
    amiga::Mode mode,
    const IffOptions& options = {});

// ---------------------------------------------------------------------------
// Write IFF ILBM to file
// ---------------------------------------------------------------------------

Result<void> save_ilbm(std::string_view path,
                       const bitplane::BitplaneData& planes,
                       std::span<const Color3f> palette,
                       amiga::Mode mode,
                       const IffOptions& options = {});

} // namespace png2amiga::iff
