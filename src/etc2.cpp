// ETC2 RGB8 encoder + decoder.
//
// Decoder ported line-by-line from Ericsson's etcdec.cxx reference
// (https://github.com/Ericsson/ETCPACK). Re-expressed against a single
// uint64_t block representation (the reference splits it into two uint32
// "parts"); behaviour is byte-identical, just less arithmetic to track.
//
// Per-mode encoders are still stubs in this file — the real per-block
// encoders (planar via LSQ, T/H via small search, ETC1 individual/diff
// via beam over base + modifier-table) ship in subsequent commits.
// For now encode_image still routes everything through a block-mean
// "passthrough" encoder so the end-to-end CLI dispatch stays exercised.
//
// Spec reference: Khronos Data Format Spec §13 (ETC2 RGB8); reference
// implementation at github.com/Ericsson/ETCPACK/blob/master/source/etcdec.cxx.

#include "etc2.hpp"

#include <algorithm>
#include <cstring>

namespace png2amiga::etc2 {

// ---------------------------------------------------------------------------
// Tables (verbatim from etcdec.cxx)
// ---------------------------------------------------------------------------

namespace {

// ETC1 modifier tables. The reference indexes this [16][4] (each row
// doubled) so that `compressParams[table << 1][selector]` works after
// the encoder packs `table` as 3 bits then implicit shift. We store
// the 8 unique rows and shift in our access pattern instead.
constexpr int kModifier[8][4] = {
    {-8,   -2,   2,   8},
    {-17,  -5,   5,   17},
    {-29,  -9,   9,   29},
    {-42,  -13,  13,  42},
    {-60,  -18,  18,  60},
    {-80,  -24,  24,  80},
    {-106, -33,  33,  106},
    {-183, -47,  47,  183},
};

// T-mode and H-mode distance tables (both happen to be identical
// values; named separately to track which mode picks which entry).
constexpr int kDistanceT[8] = {3, 6, 11, 16, 23, 32, 41, 64};
constexpr int kDistanceH[8] = {3, 6, 11, 16, 23, 32, 41, 64};

// (MSB, LSB) → modifier-table index. From the ETC1 spec:
//   00 → 2 (small positive)
//   01 → 3 (large positive)
//   10 → 1 (small negative)
//   11 → 0 (large negative)
constexpr int kUnscramble[4] = {2, 3, 1, 0};

// ---------------------------------------------------------------------------
// Bit helpers
// ---------------------------------------------------------------------------
//
// `block_u64` packs the 8-byte block as big-endian uint64 (MSB = byte 0
// bit 7). All bit-position constants in the reference impl are relative
// to bit 63 = MSB of byte 0, so we keep that convention.

constexpr std::uint64_t pack_be(const Block& blk) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | std::uint64_t(blk[std::size_t(i)]);
    }
    return v;
}

// Get `size` bits ending at `hi` (inclusive) in `v` (treated as 64 bits,
// MSB = bit 63). Mirrors GETBITSHIGH / GETBITS from the reference.
constexpr std::uint32_t getbits(std::uint64_t v, int size, int hi) {
    int shift = hi - size + 1;
    return std::uint32_t((v >> shift) & ((std::uint64_t(1) << size) - 1));
}

// Sign-extend `v` from `bits` bits to 32-bit signed.
constexpr int sign_extend(std::uint32_t v, int bits) {
    int shift = 32 - bits;
    return (int(v) << shift) >> shift;
}

constexpr std::uint8_t clamp_u8(int v) {
    return std::uint8_t(std::clamp(v, 0, 255));
}

// 4-bit nibble → 8-bit replicated (0xN → 0xNN).
constexpr std::uint8_t expand4(std::uint32_t v) {
    return std::uint8_t((v << 4) | v);
}

// 5-bit → 8-bit: (x << 3) | (x >> 2).
constexpr std::uint8_t expand5(std::uint32_t v) {
    return std::uint8_t((v << 3) | (v >> 2));
}

// 6-bit → 8-bit: (x << 2) | (x >> 4).
constexpr std::uint8_t expand6(std::uint32_t v) {
    return std::uint8_t((v << 2) | (v >> 4));
}

// 7-bit → 8-bit: (x << 1) | (x >> 6).
constexpr std::uint8_t expand7(std::uint32_t v) {
    return std::uint8_t((v << 1) | (v >> 6));
}

// ---------------------------------------------------------------------------
// Pixel-selector decoding
// ---------------------------------------------------------------------------
//
// The low 32 bits of the block hold selector indices for the 16 pixels.
// Bits 31..16 are pixel-MSBs (one bit per pixel); bits 15..0 are
// pixel-LSBs. Pixel order is column-major from (x=0, y=0): bit 0 is
// (0,0), bit 1 is (0,1), ..., bit 15 is (3,3).

inline int selector_index_at(std::uint64_t v, int x, int y) {
    int pos = x * 4 + y;
    int msb = int((v >> (16 + pos)) & 1u);
    int lsb = int((v >> pos) & 1u);
    return kUnscramble[(msb << 1) | lsb];
}

// ---------------------------------------------------------------------------
// ETC1 individual / differential decoder
// ---------------------------------------------------------------------------
//
// One block has two sub-blocks; the flip bit picks horizontal (top/bottom)
// or vertical (left/right) split. Each sub-block has its own base color +
// modifier-table index; per-pixel selector picks the offset within
// the table's 4-value row.

void decode_etc1(std::uint64_t v, std::uint8_t out[16 * 3]) {
    bool diff = getbits(v, 1, 33) != 0;
    bool flip = getbits(v, 1, 32) != 0;

    std::uint8_t base[2][3]{};

    if (diff) {
        // Differential: 5-bit base + 3-bit signed delta per channel.
        int r5 = int(getbits(v, 5, 63));
        int g5 = int(getbits(v, 5, 55));
        int b5 = int(getbits(v, 5, 47));
        int dr = sign_extend(getbits(v, 3, 58), 3);
        int dg = sign_extend(getbits(v, 3, 50), 3);
        int db = sign_extend(getbits(v, 3, 42), 3);
        base[0][0] = expand5(std::uint32_t(r5));
        base[0][1] = expand5(std::uint32_t(g5));
        base[0][2] = expand5(std::uint32_t(b5));
        base[1][0] = expand5(std::uint32_t(r5 + dr));
        base[1][1] = expand5(std::uint32_t(g5 + dg));
        base[1][2] = expand5(std::uint32_t(b5 + db));
    } else {
        // Individual: 4-bit base per channel for each sub-block.
        base[0][0] = expand4(getbits(v, 4, 63));
        base[0][1] = expand4(getbits(v, 4, 55));
        base[0][2] = expand4(getbits(v, 4, 47));
        base[1][0] = expand4(getbits(v, 4, 59));
        base[1][1] = expand4(getbits(v, 4, 51));
        base[1][2] = expand4(getbits(v, 4, 43));
    }

    int table[2];
    table[0] = int(getbits(v, 3, 39));
    table[1] = int(getbits(v, 3, 36));

    // Sub-block 0 covers half the pixels, sub-block 1 the other half.
    // flip=0: vertical split  — left 2 columns are sub0, right 2 are sub1
    // flip=1: horizontal split — top 2 rows are sub0, bottom 2 are sub1
    for (int x = 0; x < 4; ++x) {
        for (int y = 0; y < 4; ++y) {
            int sub = flip ? (y >= 2 ? 1 : 0) : (x >= 2 ? 1 : 0);
            int s = selector_index_at(v, x, y);
            int mod = kModifier[table[sub]][s];
            int p = (y * 4 + x) * 3;
            out[p + 0] = clamp_u8(int(base[sub][0]) + mod);
            out[p + 1] = clamp_u8(int(base[sub][1]) + mod);
            out[p + 2] = clamp_u8(int(base[sub][2]) + mod);
        }
    }
}

// ---------------------------------------------------------------------------
// T-mode decoder
// ---------------------------------------------------------------------------
//
// Two 4-bit-per-channel base colors C0, C1 plus a 3-bit distance index d.
// Per pixel index 0..3 picks one of:
//   0 → C0
//   1 → C1 + d
//   2 → C1
//   3 → C1 − d
//
// Per-pixel index = (msb_bit << 1) | lsb_bit at the column-major pixel
// position — same layout as ETC1, but NOT unscrambled (raw 0..3).

void decode_t(std::uint64_t v, std::uint8_t out[16 * 3]) {
    int r0a = int(getbits(v, 2, 60));
    int r0b = int(getbits(v, 2, 57));
    int r0_4 = (r0a << 2) | r0b;
    int g0_4 = int(getbits(v, 4, 55));
    int b0_4 = int(getbits(v, 4, 51));
    int r1_4 = int(getbits(v, 4, 47));
    int g1_4 = int(getbits(v, 4, 43));
    int b1_4 = int(getbits(v, 4, 39));
    int da = int(getbits(v, 2, 35));
    int db = int(getbits(v, 1, 32));
    int d_idx = (da << 1) | db;

    std::uint8_t c0[3] = {expand4(std::uint32_t(r0_4)),
                          expand4(std::uint32_t(g0_4)),
                          expand4(std::uint32_t(b0_4))};
    std::uint8_t c1[3] = {expand4(std::uint32_t(r1_4)),
                          expand4(std::uint32_t(g1_4)),
                          expand4(std::uint32_t(b1_4))};
    int d = kDistanceT[d_idx];

    std::uint8_t paint[4][3];
    paint[0][0] = c0[0];
    paint[0][1] = c0[1];
    paint[0][2] = c0[2];
    paint[1][0] = clamp_u8(int(c1[0]) + d);
    paint[1][1] = clamp_u8(int(c1[1]) + d);
    paint[1][2] = clamp_u8(int(c1[2]) + d);
    paint[2][0] = c1[0];
    paint[2][1] = c1[1];
    paint[2][2] = c1[2];
    paint[3][0] = clamp_u8(int(c1[0]) - d);
    paint[3][1] = clamp_u8(int(c1[1]) - d);
    paint[3][2] = clamp_u8(int(c1[2]) - d);

    for (int x = 0; x < 4; ++x) {
        for (int y = 0; y < 4; ++y) {
            int pos = x * 4 + y;
            int msb = int((v >> (16 + pos)) & 1u);
            int lsb = int((v >> pos) & 1u);
            int idx = (msb << 1) | lsb;
            int p = (y * 4 + x) * 3;
            out[p + 0] = paint[idx][0];
            out[p + 1] = paint[idx][1];
            out[p + 2] = paint[idx][2];
        }
    }
}

// ---------------------------------------------------------------------------
// H-mode decoder
// ---------------------------------------------------------------------------
//
// Two 4-bit-per-channel base colors. Paint colors:
//   0 → C0 + d
//   1 → C0 − d
//   2 → C1 + d
//   3 → C1 − d
//
// Distance index reconstruction: top 2 bits from the block, low bit from
// the lexicographic comparison of the two packed 12-bit base colors.

void decode_h(std::uint64_t v, std::uint8_t out[16 * 3]) {
    // Per the reference (after unstuff58bits): R0 is at bits 57..54 (4),
    // G0 at 53..50, B0 at 49..46, R1 at 45..42, G1 at 41..38, B1 at 37..34,
    // distance HI 2 bits at 33..32. unstuff58 rearranges the original
    // bits — we apply the rearrangement inline via getbits on the
    // original 64-bit value.

    // Unstuffed layout extraction (re-packed in flight):
    // From etcdec.cxx unstuff58bits + decompressBlockTHUMB58Hc:
    //   colorsRGB444[0][R] = bits 57..54
    //   colorsRGB444[0][G] = bits 53..50
    //   colorsRGB444[0][B] = bits 49..46
    //   colorsRGB444[1][R] = bits 45..42
    //   colorsRGB444[1][G] = bits 41..38
    //   colorsRGB444[1][B] = bits 37..34
    // …but those bits in the ORIGINAL block come from unstuff58bits:
    //   part0 = original bits 62..56  → mapped to bits 57..51
    //   part1 = original bits 52..51  → mapped to bits 50..49
    //   part2 = original bits 49..34  → mapped to bits 48..33
    //   part3 = original bit  32     → mapped to bit 32
    // Compose the unstuffed value, then extract the H fields from it.
    std::uint64_t u = 0;
    {
        std::uint64_t p0 = (v >> (62 - 7 + 1)) & ((std::uint64_t(1) << 7) - 1);
        std::uint64_t p1 = (v >> (52 - 2 + 1)) & ((std::uint64_t(1) << 2) - 1);
        std::uint64_t p2 = (v >> (49 - 16 + 1)) & ((std::uint64_t(1) << 16) - 1);
        std::uint64_t p3 = (v >> (32 - 1 + 1)) & 1u;
        u |= p0 << (57 - 7 + 1);
        u |= p1 << (50 - 2 + 1);
        u |= p2 << (48 - 16 + 1);
        u |= p3 << 32;
        u |= v & ((std::uint64_t(1) << 32) - 1);  // low 32 bits = selectors
    }

    int r0_4 = int(getbits(u, 4, 57));
    int g0_4 = int(getbits(u, 4, 53));
    int b0_4 = int(getbits(u, 4, 49));
    int r1_4 = int(getbits(u, 4, 45));
    int g1_4 = int(getbits(u, 4, 41));
    int b1_4 = int(getbits(u, 4, 37));
    int d_hi = int(getbits(u, 2, 33));
    int d_lo = 0;
    std::uint32_t col0 = (std::uint32_t(r0_4) << 8) | (std::uint32_t(g0_4) << 4) |
                         std::uint32_t(b0_4);
    std::uint32_t col1 = (std::uint32_t(r1_4) << 8) | (std::uint32_t(g1_4) << 4) |
                         std::uint32_t(b1_4);
    if (col0 >= col1) d_lo = 1;
    int d_idx = (d_hi << 1) | d_lo;
    int d = kDistanceH[d_idx];

    std::uint8_t c0[3] = {expand4(std::uint32_t(r0_4)),
                          expand4(std::uint32_t(g0_4)),
                          expand4(std::uint32_t(b0_4))};
    std::uint8_t c1[3] = {expand4(std::uint32_t(r1_4)),
                          expand4(std::uint32_t(g1_4)),
                          expand4(std::uint32_t(b1_4))};
    std::uint8_t paint[4][3];
    for (int ch = 0; ch < 3; ++ch) {
        paint[0][ch] = clamp_u8(int(c0[ch]) + d);
        paint[1][ch] = clamp_u8(int(c0[ch]) - d);
        paint[2][ch] = clamp_u8(int(c1[ch]) + d);
        paint[3][ch] = clamp_u8(int(c1[ch]) - d);
    }

    for (int x = 0; x < 4; ++x) {
        for (int y = 0; y < 4; ++y) {
            int pos = x * 4 + y;
            int msb = int((u >> (16 + pos)) & 1u);
            int lsb = int((u >> pos) & 1u);
            int idx = (msb << 1) | lsb;
            int p = (y * 4 + x) * 3;
            out[p + 0] = paint[idx][0];
            out[p + 1] = paint[idx][1];
            out[p + 2] = paint[idx][2];
        }
    }
}

// ---------------------------------------------------------------------------
// Planar decoder
// ---------------------------------------------------------------------------
//
// Three colors O (origin), H (horizontal), V (vertical), each
// in R6:G7:B6 precision. Per-pixel:
//   pixel[ch] = clamp(0, ((x*(H[ch]-O[ch]) + y*(V[ch]-O[ch]) + 4*O[ch] + 2) >> 2), 255)

void decode_planar(std::uint64_t v, std::uint8_t out[16 * 3]) {
    // Unstuff57bits: rearrange original bits into the layout the planar
    // decoder reads. Composed inline to keep the dispatch in one TU.
    std::uint64_t u = 0;
    auto put = [&](std::uint64_t val, int size, int hi) {
        int shift = hi - size + 1;
        u |= (val & ((std::uint64_t(1) << size) - 1)) << shift;
    };
    auto get = [&](std::uint64_t src, int size, int hi) {
        int shift = hi - size + 1;
        return (src >> shift) & ((std::uint64_t(1) << size) - 1);
    };
    std::uint64_t RO = get(v, 6, 62);
    std::uint64_t GO1 = get(v, 1, 56);
    std::uint64_t GO2 = get(v, 6, 54);
    std::uint64_t BO1 = get(v, 1, 48);
    std::uint64_t BO2 = get(v, 2, 44);
    std::uint64_t BO3 = get(v, 3, 41);
    std::uint64_t RH1 = get(v, 5, 38);
    std::uint64_t RH2 = get(v, 1, 32);
    std::uint64_t GH = get(v, 7, 31);
    std::uint64_t BH = get(v, 6, 24);
    std::uint64_t RV = get(v, 6, 18);
    std::uint64_t GV = get(v, 7, 12);
    std::uint64_t BV = get(v, 6, 5);
    put(RO, 6, 63);
    put(GO1, 1, 57);
    put(GO2, 6, 56);
    put(BO1, 1, 50);
    put(BO2, 2, 49);
    put(BO3, 3, 47);
    put(RH1, 5, 44);
    put(RH2, 1, 39);
    put(GH, 7, 38);
    put(BH, 6, 31);
    put(RV, 6, 25);
    put(GV, 7, 19);
    put(BV, 6, 12);

    int O[3], H[3], V[3];
    O[0] = expand6(getbits(u, 6, 63));
    O[1] = expand7(getbits(u, 7, 57));
    O[2] = expand6(getbits(u, 6, 50));
    H[0] = expand6(getbits(u, 6, 44));
    H[1] = expand7(getbits(u, 7, 38));
    H[2] = expand6(getbits(u, 6, 31));
    V[0] = expand6(getbits(u, 6, 25));
    V[1] = expand7(getbits(u, 7, 19));
    V[2] = expand6(getbits(u, 6, 12));

    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            int p = (y * 4 + x) * 3;
            for (int ch = 0; ch < 3; ++ch) {
                int v_int = (x * (H[ch] - O[ch]) + y * (V[ch] - O[ch]) + 4 * O[ch] + 2) >> 2;
                out[p + ch] = clamp_u8(v_int);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Sub-mode classifier — picks which decoder to dispatch to
// ---------------------------------------------------------------------------

SubMode classify_internal(std::uint64_t v) {
    if (getbits(v, 1, 33) == 0) return SubMode::etc1_individual;
    // Differential: check 5-bit base + signed 3-bit delta overflow per channel.
    int r5 = int(getbits(v, 5, 63));
    int g5 = int(getbits(v, 5, 55));
    int b5 = int(getbits(v, 5, 47));
    int dr = sign_extend(getbits(v, 3, 58), 3);
    int dg = sign_extend(getbits(v, 3, 50), 3);
    int db = sign_extend(getbits(v, 3, 42), 3);
    int rr = r5 + dr;
    int gg = g5 + dg;
    int bb = b5 + db;
    if (rr < 0 || rr > 31) return SubMode::t_mode;
    if (gg < 0 || gg > 31) return SubMode::h_mode;
    if (bb < 0 || bb > 31) return SubMode::planar;
    return SubMode::etc1_differential;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public decoder
// ---------------------------------------------------------------------------

void decode_block(const Block& blk, std::uint8_t out[kBlockPixels * 3]) {
    std::uint64_t v = pack_be(blk);
    switch (classify_internal(v)) {
    case SubMode::etc1_individual:
    case SubMode::etc1_differential:
        decode_etc1(v, out);
        break;
    case SubMode::t_mode:
        decode_t(v, out);
        break;
    case SubMode::h_mode:
        decode_h(v, out);
        break;
    case SubMode::planar:
        decode_planar(v, out);
        break;
    }
}

SubMode classify(const Block& blk) { return classify_internal(pack_be(blk)); }

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
// Encoder — still a stub block-mean baseline (real encoders in next commit)
// ---------------------------------------------------------------------------

namespace {

Block encode_block_passthrough(std::span<const std::uint8_t> src_rgb, int src_w, int px, int py) {
    int r_sum = 0, g_sum = 0, b_sum = 0, n = 0;
    const auto sw = static_cast<std::size_t>(src_w);
    for (int dy = 0; dy < kBlockH; ++dy) {
        int sy = py + dy;
        for (int dx = 0; dx < kBlockW; ++dx) {
            int sx = px + dx;
            if (sx >= src_w) sx = src_w - 1;
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
    b[0] = static_cast<std::uint8_t>((r4 << 4) | r4);
    b[1] = static_cast<std::uint8_t>((g4 << 4) | g4);
    b[2] = static_cast<std::uint8_t>((b4 << 4) | b4);
    b[3] = 0x00;
    b[4] = b[5] = b[6] = b[7] = 0;
    return b;
}

}  // namespace

EncodeResult encode_image(std::span<const std::uint8_t> rgb_srgb8,
                          int image_w,
                          int image_h,
                          const Options& options) {
    (void)options;

    EncodeResult res;
    res.block_cols = (image_w + kBlockW - 1) / kBlockW;
    res.block_rows = (image_h + kBlockH - 1) / kBlockH;
    const auto bcols = static_cast<std::size_t>(res.block_cols);
    res.blocks.assign(bcols * static_cast<std::size_t>(res.block_rows), Block{});

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
