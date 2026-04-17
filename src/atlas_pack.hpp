#pragma once

#include "bitplane.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace png2amiga::atlas_pack {

// ---------------------------------------------------------------------------
// Input to the atlas packer — one named PNG-on-disk entry.
// ---------------------------------------------------------------------------

struct Input {
    std::string name;       // identifier used in the emitted AtlasEntry (sanitized externally)
    std::string path;       // filesystem path
};

// ---------------------------------------------------------------------------
// Packer options.
//
// page_w / page_h define the chip-RAM-friendly bitmap page size. Inputs are
// sorted by height descending and placed with shelf packing — simple and
// gives >90% utilization on typical sprite mixes. When an input doesn't fit
// in the current page it overflows to a new page.
// ---------------------------------------------------------------------------

struct Options {
    std::size_t depth = 5;
    std::size_t page_w = 320;          // must be multiple of 16
    std::size_t page_h = 256;
    bitplane::Layout layout = bitplane::Layout::interleaved;

    bool emit_mask = false;
    float alpha_threshold = 0.5f;
    bool reserve_color0 = true;
};

// ---------------------------------------------------------------------------
// Atlas placement entry — fully describes where to find a named rect in the
// packed pages.
//
// x is guaranteed to be 16-aligned (blitter A-source alignment). w is the
// logical visible width; w_padded is the 16-aligned storage width. The BOB
// blit reads w_padded pixels per row but only the first w are meaningful.
// ---------------------------------------------------------------------------

struct Entry {
    std::string name;
    std::uint8_t page{};
    std::uint16_t x{};        // 16-px aligned within its page
    std::uint16_t y{};
    std::uint16_t w{};        // logical width
    std::uint16_t h{};
    std::uint16_t w_padded{}; // 16-aligned storage width
};

struct PackResult {
    // pages[p] = one page's bitplane blob. All pages have the same dimensions
    // and layout so callers can index them uniformly.
    std::vector<bitplane::BitplaneData> pages;
    // masks[p] = companion single-plane mask if emit_mask. Empty otherwise.
    std::vector<std::vector<std::uint8_t>> masks;
    std::vector<Entry> entries;
    std::vector<Color3f> palette;
    std::size_t page_w{};
    std::size_t page_h{};
    std::size_t page_bpr{};                // bytes per row per plane
    std::size_t page_bytes{};              // total bytes per page (depth * h * bpr)
    std::size_t page_mask_bytes{};         // bytes per mask page (h * bpr)
    std::size_t depth{};
    bool has_mask{};
    // Packing stats (informational).
    float utilization{};                   // sum of entry pixels / (num_pages * page_w * page_h)
};

// ---------------------------------------------------------------------------
// Pack a set of PNG inputs into N chip-RAM-sized bitmap pages.
// All inputs must share a palette that fits within 2^depth colors; the
// packer aggregates unique visible colors across all inputs.
// ---------------------------------------------------------------------------

Result<PackResult> pack_atlas(const std::vector<Input>& inputs,
                              const Options& opts);

} // namespace png2amiga::atlas_pack
