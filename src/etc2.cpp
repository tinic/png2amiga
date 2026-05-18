// ETC2 RGB8 encoder + decoder.
//
// Skeleton — wiring + entry points compile, encode_image / decode_block
// currently return placeholder output. Per-mode encoders + the decoder
// land in subsequent commits; this file's structure is set up to make
// each addition additive.
//
// Spec reference: Khronos Data Format Spec §13 (ETC2 RGB8), OpenGL ES 3.0
// Spec §3.8.x / Appendix C.

#include "etc2.hpp"

#include <algorithm>

namespace png2amiga::etc2 {

// ---------------------------------------------------------------------------
// Decoder — STUB (full implementation lands in commit 2)
// ---------------------------------------------------------------------------

void decode_block(const Block& blk, std::uint8_t out[kBlockPixels * 3]) {
    // Placeholder: decode the diff bit + base1 channel as a flat block
    // so smoke tests can verify the wiring runs end-to-end. Subsequent
    // commit replaces this with the full 5-mode dispatch.
    bool diff = (blk[3] & 0x02) != 0;
    std::uint8_t r = diff ? (blk[0] & 0xF8) : ((blk[0] & 0xF0) | (blk[0] >> 4));
    std::uint8_t g = diff ? (blk[1] & 0xF8) : ((blk[1] & 0xF0) | (blk[1] >> 4));
    std::uint8_t b = diff ? (blk[2] & 0xF8) : ((blk[2] & 0xF0) | (blk[2] >> 4));
    for (int i = 0; i < kBlockPixels; ++i) {
        out[i * 3 + 0] = r;
        out[i * 3 + 1] = g;
        out[i * 3 + 2] = b;
    }
}

SubMode classify(const Block& blk) {
    // STUB: real impl checks diff bit + R±dR / G±dG / B±dB ranges.
    bool diff = (blk[3] & 0x02) != 0;
    return diff ? SubMode::etc1_differential : SubMode::etc1_individual;
}

std::vector<std::uint8_t> decode_image(std::span<const Block> blocks, int image_w, int image_h) {
    const auto iw = static_cast<std::size_t>(image_w);
    const auto ih = static_cast<std::size_t>(image_h);
    std::vector<std::uint8_t> out(iw * ih * 3u, 0);
    int bcols = (image_w + kBlockW - 1) / kBlockW;
    int brows = (image_h + kBlockH - 1) / kBlockH;
    std::uint8_t tmp[kBlockPixels * 3];
    for (int by = 0; by < brows; ++by) {
        for (int bx = 0; bx < bcols; ++bx) {
            std::size_t bidx = static_cast<std::size_t>(by) * static_cast<std::size_t>(bcols) +
                               static_cast<std::size_t>(bx);
            if (bidx >= blocks.size()) continue;
            decode_block(blocks[bidx], tmp);
            for (int dy = 0; dy < kBlockH; ++dy) {
                int sy = by * kBlockH + dy;
                if (sy >= image_h) break;
                for (int dx = 0; dx < kBlockW; ++dx) {
                    int sx = bx * kBlockW + dx;
                    if (sx >= image_w) break;
                    std::size_t pix = static_cast<std::size_t>(sy) * iw +
                                      static_cast<std::size_t>(sx);
                    int t = dy * kBlockW + dx;
                    out[pix * 3u + 0u] = tmp[t * 3 + 0];
                    out[pix * 3u + 1u] = tmp[t * 3 + 1];
                    out[pix * 3u + 2u] = tmp[t * 3 + 2];
                }
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Encoder — STUB
// ---------------------------------------------------------------------------
//
// Subsequent commits add per-mode encoders + the per-block picker + the
// block-grid ED. The current path picks a plausible "average colour" base
// per block and emits an ETC1-individual block with all selectors at 0
// (i.e. base + modifier[0]). Output is a valid ETC2 RGB8 stream that
// decodes back to roughly the block mean — useful for proving the KTX2
// container + main.cpp dispatch works end-to-end before the real
// encoder lands.

namespace {

// Quick block-mean → 4-bit-per-channel base color, then pack as an
// ETC1-individual block with flip=0, table1=table2=0, all selectors=0.
// Decoded result = base + modifier_table[0][0] (a small positive nudge).
//
// This is intentionally not competitive — it's the no-search baseline.
Block encode_block_passthrough(std::span<const std::uint8_t> src_rgb, int src_w, int px, int py) {
    int r_sum = 0, g_sum = 0, b_sum = 0, n = 0;
    const auto sw = static_cast<std::size_t>(src_w);
    for (int dy = 0; dy < kBlockH; ++dy) {
        int sy = py + dy;
        for (int dx = 0; dx < kBlockW; ++dx) {
            int sx = px + dx;
            if (sx >= src_w) sx = src_w - 1;
            // Input assumed pre-padded by caller; clamp x defensively only.
            std::size_t i = static_cast<std::size_t>(sy) * sw + static_cast<std::size_t>(sx);
            r_sum += src_rgb[i * 3u + 0u];
            g_sum += src_rgb[i * 3u + 1u];
            b_sum += src_rgb[i * 3u + 2u];
            ++n;
        }
    }
    std::uint8_t r4 = static_cast<std::uint8_t>((r_sum / n) >> 4);
    std::uint8_t g4 = static_cast<std::uint8_t>((g_sum / n) >> 4);
    std::uint8_t b4 = static_cast<std::uint8_t>((b_sum / n) >> 4);

    Block b{};
    // ETC1 individual layout (big-endian, MSB-first):
    //   byte0: R1[7..4] | R2[3..0]
    //   byte1: G1[7..4] | G2[3..0]
    //   byte2: B1[7..4] | B2[3..0]
    //   byte3: table1[7..5] | table2[4..2] | diff[1] | flip[0]
    //   bytes 4..7: pixel selectors (all zero here)
    b[0] = static_cast<std::uint8_t>((r4 << 4) | r4);
    b[1] = static_cast<std::uint8_t>((g4 << 4) | g4);
    b[2] = static_cast<std::uint8_t>((b4 << 4) | b4);
    b[3] = 0x00;  // table1=table2=0, diff=0, flip=0
    b[4] = b[5] = b[6] = b[7] = 0;
    return b;
}

}  // namespace

EncodeResult encode_image(std::span<const std::uint8_t> rgb_srgb8,
                          int image_w,
                          int image_h,
                          const Options& options) {
    (void)options;  // skeleton: knobs unused until per-mode encoders land

    EncodeResult res;
    res.block_cols = (image_w + kBlockW - 1) / kBlockW;
    res.block_rows = (image_h + kBlockH - 1) / kBlockH;
    const auto bcols = static_cast<std::size_t>(res.block_cols);
    res.blocks.assign(bcols * static_cast<std::size_t>(res.block_rows), Block{});

    // Pad input by replicating the last row / column so encode_block_passthrough
    // doesn't read out of bounds. Cheap copy — the real encoder will use
    // block_compress::sample_block which clamps inline.
    std::vector<std::uint8_t> padded;
    int pad_w = res.block_cols * kBlockW;
    int pad_h = res.block_rows * kBlockH;
    const auto pw = static_cast<std::size_t>(pad_w);
    const auto iw = static_cast<std::size_t>(image_w);
    padded.assign(pw * static_cast<std::size_t>(pad_h) * 3u, 0);
    for (int y = 0; y < pad_h; ++y) {
        int sy = std::min(y, image_h - 1);
        for (int x = 0; x < pad_w; ++x) {
            int sx = std::min(x, image_w - 1);
            std::size_t s = static_cast<std::size_t>(sy) * iw + static_cast<std::size_t>(sx);
            std::size_t d = static_cast<std::size_t>(y) * pw + static_cast<std::size_t>(x);
            padded[d * 3u + 0u] = rgb_srgb8[s * 3u + 0u];
            padded[d * 3u + 1u] = rgb_srgb8[s * 3u + 1u];
            padded[d * 3u + 2u] = rgb_srgb8[s * 3u + 2u];
        }
    }

    for (int by = 0; by < res.block_rows; ++by) {
        for (int bx = 0; bx < res.block_cols; ++bx) {
            res.blocks[static_cast<std::size_t>(by) * bcols + static_cast<std::size_t>(bx)] =
                encode_block_passthrough(padded, pad_w, bx * kBlockW, by * kBlockH);
            res.mode_counts[static_cast<std::size_t>(SubMode::etc1_individual)] += 1;
        }
    }

    return res;
}

}  // namespace png2amiga::etc2
