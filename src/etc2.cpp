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

#include "color_space.hpp"
#include "pipeline.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <thread>

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
// Encoder — per-block search over all 5 sub-modes
// ---------------------------------------------------------------------------
//
// Pipeline per block:
//   1. Sample 16 source pixels (sRGB8 + OKLab pre-computed).
//   2. Encode under each enabled sub-mode → Candidate with err_oklab2
//      (or err_srgb_mse, template-dispatched on Options::metric).
//   3. Argmin across candidates → emit Block.
//
// Sub-mode encoders are deliberately heuristic; refinement / beam /
// block-grid ED layer on later. Bit-level packing mirrors the decoder
// (etcdec.cxx layouts; encoder and decoder share kModifier / kDistanceT
// / kDistanceH / kUnscramble).

namespace {

// kModifier index → (msb, lsb) selector bit pair. Inverse of kUnscramble.
// kUnscramble[(msb<<1)|lsb] = modifier_index, so we invert here once.
constexpr std::array<std::uint8_t, 4> kScramble = []() {
    std::array<std::uint8_t, 4> s{};
    for (int i = 0; i < 4; ++i) {
        s[std::size_t(kUnscramble[i])] = std::uint8_t(i);
    }
    return s;
}();

// Block sample: source pixels in sRGB8 + linear OKLab. Per-block scratch
// the encoders pull from. Layout matches block_compress::BlockSample<4,4>
// — could be unified later, kept local now for compactness.
struct Sample16 {
    std::uint8_t srgb8[16][3];
    color_space::OKLab lab[16];
};

// Load 16 source pixels into the sample buffer, optionally with an OKLab
// shift applied to s.lab[]. The shift is the block-grid ED accumulator
// for this block — added only to the OKLab targets the encoder scores
// against, NOT to s.srgb8 (which feeds the base-color seed). This means
// the encoder still seeds from the true source colour but then picks
// modifiers/selectors that target the shifted OKLab points — exactly
// the FS semantics, but on the block grid.
void load_sample(Sample16& s,
                 std::span<const std::uint8_t> padded_rgb,
                 std::size_t pad_w,
                 int px,
                 int py,
                 color_space::OKLab shift = {0.f, 0.f, 0.f}) {
    for (int dy = 0; dy < 4; ++dy) {
        for (int dx = 0; dx < 4; ++dx) {
            std::size_t idx = (std::size_t(py + dy) * pad_w + std::size_t(px + dx)) * 3u;
            int i = dy * 4 + dx;
            s.srgb8[i][0] = padded_rgb[idx + 0u];
            s.srgb8[i][1] = padded_rgb[idx + 1u];
            s.srgb8[i][2] = padded_rgb[idx + 2u];
            auto raw = color_space::srgb8_to_oklab(s.srgb8[i][0], s.srgb8[i][1], s.srgb8[i][2]);
            s.lab[i].L = raw.L + shift.L;
            s.lab[i].a = raw.a + shift.a;
            s.lab[i].b = raw.b + shift.b;
        }
    }
}

// Score one decoded block against a Sample16 in the chosen metric.
template<block_compress::BlockMetric M>
float score_decoded(const Sample16& s, const std::uint8_t dec[16][3]) {
    if constexpr (M == block_compress::BlockMetric::srgb_mse) {
        int acc = 0;
        for (int i = 0; i < 16; ++i) {
            int dr = int(s.srgb8[i][0]) - int(dec[i][0]);
            int dg = int(s.srgb8[i][1]) - int(dec[i][1]);
            int db = int(s.srgb8[i][2]) - int(dec[i][2]);
            acc += dr * dr + dg * dg + db * db;
        }
        return float(acc) * (1.0f / 65536.0f);
    } else {
        float acc = 0.0f;
        for (int i = 0; i < 16; ++i) {
            color_space::OKLab d = color_space::srgb8_to_oklab(dec[i][0], dec[i][1], dec[i][2]);
            float dL = s.lab[i].L - d.L;
            float da = s.lab[i].a - d.a;
            float db = s.lab[i].b - d.b;
            acc += color_space::fma_dist_sq(dL, da, db);
        }
        return acc;
    }
}

// Sub-block selector — which 8 pixel indices belong to sub-block 0/1
// given the flip bit. flip=0: vertical split (left/right). flip=1:
// horizontal split (top/bottom).
inline void sub_pixel_indices(bool flip, int sub, int out_idx[8]) {
    int n = 0;
    for (int x = 0; x < 4; ++x) {
        for (int y = 0; y < 4; ++y) {
            int s = flip ? (y >= 2 ? 1 : 0) : (x >= 2 ? 1 : 0);
            if (s == sub) out_idx[n++] = y * 4 + x;
        }
    }
}

// Encode an ETC1 SUB-BLOCK: pick (base, table, per-pixel selectors)
// minimizing OKLab² error against the 8 pixels of that sub-block.
//
// `base_seed_srgb8` is the starting base color (e.g. mean of the
// sub-block). For ETC1 individual: 4-bit-replicated (0xN → 0xNN).
// For ETC1 differential: 5-bit-expanded ((x << 3) | (x >> 2)).
// We sweep ±1 nibble around the seed to escape rounding artefacts.
struct SubResult {
    std::uint8_t base[3];           // final 8-bit base (after expansion)
    int base_packed[3];             // 4-bit or 5-bit pre-expansion value
    int table;                      // 0..7
    int selectors[8];               // modifier index 0..3 for each sub-pixel
    int pixel_indices[8];           // which of the 16 source pixels (for later assembly)
    float err;
};

// Inline helpers for the per-(base, table) inner loop.
template<block_compress::BlockMetric M>
inline float pick_selectors_and_score(const Sample16& s,
                                      const int sub_idx[8],
                                      const std::uint8_t base8[3],
                                      int table,
                                      int out_sel[8]) {
    float tot = 0.0f;
    for (int p = 0; p < 8; ++p) {
        int src_i = sub_idx[p];
        float best_e = std::numeric_limits<float>::infinity();
        int best_s = 0;
        for (int s_i = 0; s_i < 4; ++s_i) {
            int m = kModifier[table][s_i];
            std::uint8_t dec[3] = {
                clamp_u8(int(base8[0]) + m),
                clamp_u8(int(base8[1]) + m),
                clamp_u8(int(base8[2]) + m),
            };
            float e;
            if constexpr (M == block_compress::BlockMetric::srgb_mse) {
                int dr2 = int(s.srgb8[src_i][0]) - int(dec[0]);
                int dg2 = int(s.srgb8[src_i][1]) - int(dec[1]);
                int db2 = int(s.srgb8[src_i][2]) - int(dec[2]);
                e = float(dr2 * dr2 + dg2 * dg2 + db2 * db2);
            } else {
                color_space::OKLab d_lab =
                    color_space::srgb8_to_oklab(dec[0], dec[1], dec[2]);
                e = color_space::fma_dist_sq(s.lab[src_i].L - d_lab.L,
                                             s.lab[src_i].a - d_lab.a,
                                             s.lab[src_i].b - d_lab.b);
            }
            if (e < best_e) { best_e = e; best_s = s_i; }
        }
        out_sel[p] = best_s;
        tot += best_e;
    }
    return tot;
}

// Lloyd-style refinement for a fixed table: alternate (1) re-pick
// selectors given current base, (2) re-fit base as the (mean of
// (source − modifier[sel])) snapped to the encodable grid. Converges
// to the local optimum for this table in 3–4 iterations. This is the
// piece etcpak/etc2comp do that we were missing — it closes most of
// the per-sub-block PSNR gap vs jitter-only search.
template<block_compress::BlockMetric M, bool Differential>
inline void refine_lloyd_for_table(const Sample16& s,
                                   const int sub_idx[8],
                                   int table,
                                   int base_packed[3],
                                   int sel[8],
                                   float& err_out,
                                   int packed_max) {
    constexpr int kIters = 4;
    for (int it = 0; it < kIters; ++it) {
        // Re-fit base: pick best base such that
        // sum_p (S[p] - clamp(base + mod[T][sel[p]]))² is minimised.
        // Closed-form ignoring clamp: base_ch = mean(S_ch - mod[T][sel[p]]).
        // Then snap to encodable grid + clamp.
        int sum[3] = {0, 0, 0};
        for (int p = 0; p < 8; ++p) {
            int src_i = sub_idx[p];
            int m = kModifier[table][sel[p]];
            sum[0] += int(s.srgb8[src_i][0]) - m;
            sum[1] += int(s.srgb8[src_i][1]) - m;
            sum[2] += int(s.srgb8[src_i][2]) - m;
        }
        int base8_new[3];
        int base_packed_new[3];
        for (int c = 0; c < 3; ++c) {
            int mean = (sum[c] + 4) >> 3;  // /8 with rounding
            mean = std::clamp(mean, 0, 255);
            if constexpr (Differential) {
                // 5-bit: round to nearest /(8 levels of expansion).
                int q = (mean * 31 + 127) >> 8;  // ≈ mean / (255/31)
                base_packed_new[c] = std::clamp(q, 0, packed_max);
                base8_new[c] = int(expand5(std::uint32_t(base_packed_new[c])));
            } else {
                int q = (mean + 8) >> 4;
                base_packed_new[c] = std::clamp(q, 0, packed_max);
                base8_new[c] = int(expand4(std::uint32_t(base_packed_new[c])));
            }
        }
        // Re-pick selectors with new base, compute new error.
        std::uint8_t base8_u8[3] = {std::uint8_t(base8_new[0]),
                                     std::uint8_t(base8_new[1]),
                                     std::uint8_t(base8_new[2])};
        int new_sel[8];
        float new_err = pick_selectors_and_score<M>(s, sub_idx, base8_u8, table, new_sel);
        if (new_err >= err_out - 1e-6f) break;  // converged
        err_out = new_err;
        base_packed[0] = base_packed_new[0];
        base_packed[1] = base_packed_new[1];
        base_packed[2] = base_packed_new[2];
        for (int i = 0; i < 8; ++i) sel[i] = new_sel[i];
    }
}

template<block_compress::BlockMetric M, bool Differential>
SubResult encode_subblock_etc1(const Sample16& s,
                               const int sub_idx[8],
                               const int base_seed_packed[3]) {
    SubResult best{};
    best.err = std::numeric_limits<float>::infinity();
    constexpr int kPackedMax = Differential ? 31 : 15;

    // Search a small jitter window around the seed (±2 levels per channel).
    // Tight enough to stay cheap; wide enough to escape +/-1 rounding bias.
    // After each (base, table) candidate, run Lloyd-style refinement to
    // converge on the true local optimum for that table.
    constexpr int kJitter = 2;
    for (int dr = -kJitter; dr <= kJitter; ++dr) {
        int br = std::clamp(base_seed_packed[0] + dr, 0, kPackedMax);
        for (int dg = -kJitter; dg <= kJitter; ++dg) {
            int bg = std::clamp(base_seed_packed[1] + dg, 0, kPackedMax);
            for (int dbb = -kJitter; dbb <= kJitter; ++dbb) {
                int bb = std::clamp(base_seed_packed[2] + dbb, 0, kPackedMax);
                std::uint8_t base8[3];
                if constexpr (Differential) {
                    base8[0] = expand5(std::uint32_t(br));
                    base8[1] = expand5(std::uint32_t(bg));
                    base8[2] = expand5(std::uint32_t(bb));
                } else {
                    base8[0] = expand4(std::uint32_t(br));
                    base8[1] = expand4(std::uint32_t(bg));
                    base8[2] = expand4(std::uint32_t(bb));
                }
                for (int t = 0; t < 8; ++t) {
                    int sel[8];
                    float tot = pick_selectors_and_score<M>(s, sub_idx, base8, t, sel);

                    // Lloyd refine: tighten base toward the per-table optimum.
                    int refined_packed[3] = {br, bg, bb};
                    refine_lloyd_for_table<M, Differential>(
                        s, sub_idx, t, refined_packed, sel, tot, kPackedMax);

                    if (tot < best.err) {
                        best.err = tot;
                        if constexpr (Differential) {
                            best.base[0] = expand5(std::uint32_t(refined_packed[0]));
                            best.base[1] = expand5(std::uint32_t(refined_packed[1]));
                            best.base[2] = expand5(std::uint32_t(refined_packed[2]));
                        } else {
                            best.base[0] = expand4(std::uint32_t(refined_packed[0]));
                            best.base[1] = expand4(std::uint32_t(refined_packed[1]));
                            best.base[2] = expand4(std::uint32_t(refined_packed[2]));
                        }
                        best.base_packed[0] = refined_packed[0];
                        best.base_packed[1] = refined_packed[1];
                        best.base_packed[2] = refined_packed[2];
                        best.table = t;
                        for (int i = 0; i < 8; ++i) {
                            best.selectors[i] = sel[i];
                            best.pixel_indices[i] = sub_idx[i];
                        }
                    }
                }
            }
        }
    }
    return best;
}

// Pack two SubResults + flip + diff bits into an ETC1 Block, then
// roundtrip-decode for the picker.
void pack_etc1_block(Block& blk,
                     std::uint8_t dec[16][3],
                     bool diff,
                     bool flip,
                     const SubResult& s0,
                     const SubResult& s1) {
    std::uint64_t v = 0;
    auto put = [&](std::uint64_t val, int size, int hi) {
        int shift = hi - size + 1;
        v |= (val & ((std::uint64_t(1) << size) - 1)) << shift;
    };

    if (!diff) {
        // Individual: 4-bit base per channel × 2 sub-blocks.
        put(std::uint64_t(s0.base_packed[0]), 4, 63);
        put(std::uint64_t(s1.base_packed[0]), 4, 59);
        put(std::uint64_t(s0.base_packed[1]), 4, 55);
        put(std::uint64_t(s1.base_packed[1]), 4, 51);
        put(std::uint64_t(s0.base_packed[2]), 4, 47);
        put(std::uint64_t(s1.base_packed[2]), 4, 43);
    } else {
        // Differential: 5-bit s0 base + 3-bit signed delta to s1 per channel.
        put(std::uint64_t(s0.base_packed[0]), 5, 63);
        put(std::uint64_t(s0.base_packed[1]), 5, 55);
        put(std::uint64_t(s0.base_packed[2]), 5, 47);
        int dr = s1.base_packed[0] - s0.base_packed[0];
        int dg = s1.base_packed[1] - s0.base_packed[1];
        int db = s1.base_packed[2] - s0.base_packed[2];
        put(std::uint64_t(dr & 0x7), 3, 58);
        put(std::uint64_t(dg & 0x7), 3, 50);
        put(std::uint64_t(db & 0x7), 3, 42);
    }
    put(std::uint64_t(s0.table), 3, 39);
    put(std::uint64_t(s1.table), 3, 36);
    put(diff ? 1u : 0u, 1, 33);
    put(flip ? 1u : 0u, 1, 32);

    // Selector bits — column-major (x*4+y) into MSB/LSB halves.
    auto write_selector = [&](const SubResult& sr) {
        for (int p = 0; p < 8; ++p) {
            int src_i = sr.pixel_indices[p];
            int x = src_i % 4, y = src_i / 4;
            int pos = x * 4 + y;
            unsigned scram = unsigned(kScramble[std::size_t(sr.selectors[p])]);
            v |= std::uint64_t((scram >> 1u) & 1u) << (16 + pos);
            v |= std::uint64_t(scram & 1u) << pos;
        }
    };
    write_selector(s0);
    write_selector(s1);

    for (int i = 0; i < 8; ++i) {
        blk[std::size_t(i)] = std::uint8_t((v >> ((7 - i) * 8)) & 0xFFu);
    }

    // Round-trip decode so the picker can score in OKLab.
    std::uint8_t flat[48];
    decode_block(blk, flat);
    for (int i = 0; i < 16; ++i) {
        dec[i][0] = flat[i * 3 + 0];
        dec[i][1] = flat[i * 3 + 1];
        dec[i][2] = flat[i * 3 + 2];
    }
}

template<block_compress::BlockMetric M>
Candidate encode_etc1(const Sample16& s) {
    Candidate best{};
    best.err = std::numeric_limits<float>::infinity();
    best.mode = SubMode::etc1_individual;

    for (int flip = 0; flip < 2; ++flip) {
        int idx0[8], idx1[8];
        sub_pixel_indices(flip != 0, 0, idx0);
        sub_pixel_indices(flip != 0, 1, idx1);

        auto mean_seed = [&](const int (&idx)[8], int packed_max) {
            int sum[3] = {0, 0, 0};
            for (int p : idx) {
                sum[0] += s.srgb8[p][0];
                sum[1] += s.srgb8[p][1];
                sum[2] += s.srgb8[p][2];
            }
            int seed[3];
            for (int c = 0; c < 3; ++c) {
                // round-to-nearest at packed precision
                seed[c] = std::clamp((sum[c] * packed_max + 8 * 255) / (8 * 255), 0, packed_max);
            }
            return std::array<int, 3>{seed[0], seed[1], seed[2]};
        };

        // --- Individual mode ---
        {
            auto seed0 = mean_seed(idx0, 15);
            auto seed1 = mean_seed(idx1, 15);
            auto sub0 = encode_subblock_etc1<M, false>(s, idx0, seed0.data());
            auto sub1 = encode_subblock_etc1<M, false>(s, idx1, seed1.data());
            Block blk;
            std::uint8_t dec[16][3];
            pack_etc1_block(blk, dec, false, flip != 0, sub0, sub1);
            float err = score_decoded<M>(s, dec);
            if (err < best.err) {
                best.err = err;
                best.block = blk;
                std::memcpy(best.decoded, dec, sizeof(dec));
                best.mode = SubMode::etc1_individual;
            }
        }

        // --- Differential mode ---
        {
            auto seed0 = mean_seed(idx0, 31);
            auto seed1 = mean_seed(idx1, 31);
            auto sub0 = encode_subblock_etc1<M, true>(s, idx0, seed0.data());
            // Constrain sub1's base to s0's ±[-4,3] per channel.
            int seed1_constrained[3];
            for (int c = 0; c < 3; ++c) {
                seed1_constrained[c] = std::clamp(seed1[std::size_t(c)],
                                                  sub0.base_packed[c] - 4,
                                                  sub0.base_packed[c] + 3);
            }
            auto sub1 = encode_subblock_etc1<M, true>(s, idx1, seed1_constrained);
            // Final clamp of sub1.base_packed to delta range (encoder may
            // have jittered out of bounds).
            for (int c = 0; c < 3; ++c) {
                sub1.base_packed[c] = std::clamp(sub1.base_packed[c],
                                                  sub0.base_packed[c] - 4,
                                                  sub0.base_packed[c] + 3);
            }
            // Re-pick selectors in sub1 with the clamped base.
            std::uint8_t base8_corr[3] = {
                expand5(std::uint32_t(sub1.base_packed[0])),
                expand5(std::uint32_t(sub1.base_packed[1])),
                expand5(std::uint32_t(sub1.base_packed[2])),
            };
            sub1.base[0] = base8_corr[0];
            sub1.base[1] = base8_corr[1];
            sub1.base[2] = base8_corr[2];
            float err_recheck = 0.0f;
            for (int p = 0; p < 8; ++p) {
                int src_i = sub1.pixel_indices[p];
                float best_e = std::numeric_limits<float>::infinity();
                int best_s = 0;
                for (int s_i = 0; s_i < 4; ++s_i) {
                    int m = kModifier[sub1.table][s_i];
                    std::uint8_t dec_p[3] = {
                        clamp_u8(int(base8_corr[0]) + m),
                        clamp_u8(int(base8_corr[1]) + m),
                        clamp_u8(int(base8_corr[2]) + m),
                    };
                    float e;
                    if constexpr (M == block_compress::BlockMetric::srgb_mse) {
                        int dr = int(s.srgb8[src_i][0]) - int(dec_p[0]);
                        int dg = int(s.srgb8[src_i][1]) - int(dec_p[1]);
                        int db = int(s.srgb8[src_i][2]) - int(dec_p[2]);
                        e = float(dr * dr + dg * dg + db * db);
                    } else {
                        color_space::OKLab d_lab =
                            color_space::srgb8_to_oklab(dec_p[0], dec_p[1], dec_p[2]);
                        e = color_space::fma_dist_sq(s.lab[src_i].L - d_lab.L,
                                                     s.lab[src_i].a - d_lab.a,
                                                     s.lab[src_i].b - d_lab.b);
                    }
                    if (e < best_e) {
                        best_e = e;
                        best_s = s_i;
                    }
                }
                sub1.selectors[p] = best_s;
                err_recheck += best_e;
            }
            sub1.err = err_recheck;
            Block blk;
            std::uint8_t dec[16][3];
            pack_etc1_block(blk, dec, true, flip != 0, sub0, sub1);
            float err = score_decoded<M>(s, dec);
            if (err < best.err) {
                best.err = err;
                best.block = blk;
                std::memcpy(best.decoded, dec, sizeof(dec));
                best.mode = SubMode::etc1_differential;
            }
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Planar encoder
// ---------------------------------------------------------------------------
//
// LSQ-fit a plane I(x,y) = O + (H-O)*x/4 + (V-O)*y/4 to the 16 source
// pixels per channel. The decoder rounds via:
//   pixel = ((x*(H-O) + y*(V-O) + 4*O + 2) >> 2)
// So we're fitting 8-bit O / H / V values then snapping to RGB 6-7-6
// precision (R=6 bits, G=7 bits, B=6 bits).

void planar_lsq(const std::uint8_t pixels[16][3], int OHV[3][3]) {
    // For each channel solve for (O, H-O, V-O) minimising sum of squared
    // errors of pixel(x,y) = O + (H-O)*x/4 + (V-O)*y/4. Closed form:
    //   minimise over (a, b, c): sum_xy (a + b*x + c*y - p[x,y])^2
    // where a = O, b = (H-O)/4, c = (V-O)/4.
    // Normal equations: 16a + 24b + 24c = sum_p, etc.
    for (int ch = 0; ch < 3; ++ch) {
        double sum_p = 0, sum_xp = 0, sum_yp = 0;
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                double p = double(pixels[y * 4 + x][ch]);
                sum_p += p;
                sum_xp += p * x;
                sum_yp += p * y;
            }
        }
        // sum(1) = 16, sum(x) = sum(y) = 24, sum(x²) = sum(y²) = 56,
        // sum(xy) = 36, sum(x*y) = sum_x*sum_y/16... need exact moments.
        // Cleaner: solve by averaging the four corners.
        //   O   ≈ mean over y=0,x=0 region; but with 1 sample noisy.
        // Pragmatic: fit a + b*x + c*y to all 16 pixels via least squares.
        // Moments over 4×4 grid (x, y ∈ {0,1,2,3}):
        //   N = 16, Sx = Sy = 24, Sxx = Syy = 56, Sxy = 36.
        // det = N*Sxx*Syy + 2*Sx*Sy*Sxy - N*Sxy² - Sx²*Syy - Sy²*Sxx
        // Closed-form derived once and pasted:
        constexpr double N = 16.0;
        constexpr double Sx = 24.0;
        constexpr double Sy = 24.0;
        constexpr double Sxx = 56.0;
        constexpr double Syy = 56.0;
        constexpr double Sxy = 36.0;
        double Sp = sum_p, Sxp = sum_xp, Syp = sum_yp;
        // Solve [N Sx Sy; Sx Sxx Sxy; Sy Sxy Syy] [a b c]^T = [Sp Sxp Syp]
        // via Cramer's rule.
        double det = N * (Sxx * Syy - Sxy * Sxy) - Sx * (Sx * Syy - Sxy * Sy) +
                     Sy * (Sx * Sxy - Sxx * Sy);
        double da = Sp * (Sxx * Syy - Sxy * Sxy) - Sx * (Sxp * Syy - Sxy * Syp) +
                    Sy * (Sxp * Sxy - Sxx * Syp);
        double db = N * (Sxp * Syy - Sxy * Syp) - Sp * (Sx * Syy - Sxy * Sy) +
                    Sy * (Sx * Syp - Sxp * Sy);
        double dc = N * (Sxx * Syp - Sxp * Sxy) - Sx * (Sx * Syp - Sxp * Sy) +
                    Sp * (Sx * Sxy - Sxx * Sy);
        double a = da / det;
        double b = db / det;
        double c = dc / det;
        double O_d = a;
        double H_d = O_d + 4.0 * b;
        double V_d = O_d + 4.0 * c;
        OHV[0][ch] = int(std::lround(std::clamp(O_d, 0.0, 255.0)));
        OHV[1][ch] = int(std::lround(std::clamp(H_d, 0.0, 255.0)));
        OHV[2][ch] = int(std::lround(std::clamp(V_d, 0.0, 255.0)));
    }
}

// Snap 8-bit value to N-bit precision via round-to-nearest at the
// bit-replicate quantisation grid.
inline int snap_to_bits(int v, int bits) {
    int max_packed = (1 << bits) - 1;
    int shift = 8 - bits;
    int q = (v + (1 << (shift - 1))) >> shift;
    return std::clamp(q, 0, max_packed);
}

template<block_compress::BlockMetric M>
Candidate encode_planar(const Sample16& s) {
    int OHV[3][3];
    planar_lsq(s.srgb8, OHV);

    // Snap each component to 6/7/6 bits.
    int O_pk[3] = {snap_to_bits(OHV[0][0], 6),
                    snap_to_bits(OHV[0][1], 7),
                    snap_to_bits(OHV[0][2], 6)};
    int H_pk[3] = {snap_to_bits(OHV[1][0], 6),
                    snap_to_bits(OHV[1][1], 7),
                    snap_to_bits(OHV[1][2], 6)};
    int V_pk[3] = {snap_to_bits(OHV[2][0], 6),
                    snap_to_bits(OHV[2][1], 7),
                    snap_to_bits(OHV[2][2], 6)};

    // Pack into 64-bit block per planar bit layout. The decoder reads
    // it post-unstuff57; we write the pre-stuff layout (matches the
    // reference's stuff/unstuff inverse path).
    std::uint64_t v = 0;
    auto put = [&](std::uint64_t val, int size, int hi) {
        int shift = hi - size + 1;
        v |= (val & ((std::uint64_t(1) << size) - 1)) << shift;
    };

    // Stuffed layout (= original block bit layout):
    //   RO  bits 62..57 (6)
    //   GO1 bit 56      (1)
    //   GO2 bits 54..49 (6)
    //   BO1 bit 48      (1)
    //   BO2 bits 45..44 (2)
    //   BO3 bits 43..41 (3)
    //   RH1 bits 42..38 (5)        — overlaps BO3, but stuff order writes
    //                                  RH1 right after BO3 — see reference
    //   RH2 bit 32      (1)
    //   GH  bits 38..32 (7)  ← wait, this overlaps. The actual layout
    //                            sets the diff bit at bit 33 and the
    //                            "blue overflow" trigger forces planar
    //                            mode dispatch.
    //   ...
    // Re-derived from unstuff57: the stuffed positions are
    //   RO  → 62..57   (6)
    //   GO1 → 56       (1)
    //   GO2 → 54..49   (6)
    //   BO1 → 48       (1)
    //   BO2 → 45..44   (2)
    //   BO3 → 43..41   (3)
    //   RH1 → 38..34   (5)
    //   RH2 → 32       (1)
    //   GH  → 31..25   (7)
    //   BH  → 24..19   (6)
    //   RV  → 18..13   (6)
    //   GV  → 12..6    (7)
    //   BV  →  5..0    (6)
    // The diff bit must be at position 33 = 1 (planar requires diffbit=1
    // AND blue-overflow). We set bit 33 = 1; blue overflow naturally
    // arises because the 5-bit "B1+dB" interpretation lands out of range
    // for valid planar payloads.

    int RO = O_pk[0], GO = O_pk[1], BO = O_pk[2];
    int RH = H_pk[0], GH = H_pk[1], BH = H_pk[2];
    int RV = V_pk[0], GV = V_pk[1], BV = V_pk[2];

    put(std::uint64_t(RO), 6, 62);
    put(std::uint64_t((GO >> 6) & 1), 1, 56);
    put(std::uint64_t(GO & 0x3F), 6, 54);
    put(std::uint64_t((BO >> 5) & 1), 1, 48);
    put(std::uint64_t((BO >> 3) & 0x3), 2, 45);
    put(std::uint64_t(BO & 0x7), 3, 43);
    put(std::uint64_t((RH >> 1) & 0x1F), 5, 38);
    put(std::uint64_t(RH & 0x1), 1, 32);
    put(std::uint64_t(GH), 7, 31);
    put(std::uint64_t(BH), 6, 24);
    put(std::uint64_t(RV), 6, 18);
    put(std::uint64_t(GV), 7, 12);
    put(std::uint64_t(BV), 6, 5);
    put(1u, 1, 33);  // diffbit

    Candidate c{};
    for (int i = 0; i < 8; ++i) {
        c.block[std::size_t(i)] = std::uint8_t((v >> ((7 - i) * 8)) & 0xFFu);
    }
    std::uint8_t flat[48];
    decode_block(c.block, flat);
    for (int i = 0; i < 16; ++i) {
        c.decoded[i][0] = flat[i * 3 + 0];
        c.decoded[i][1] = flat[i * 3 + 1];
        c.decoded[i][2] = flat[i * 3 + 2];
    }
    c.err = score_decoded<M>(s, c.decoded);
    c.mode = SubMode::planar;
    return c;
}

// ---------------------------------------------------------------------------
// T-mode encoder
// ---------------------------------------------------------------------------
//
// 2 base colors C0, C1 (each 4-bit per channel = RGB444) + distance index.
// Paint colors: { C0, C1+d, C1, C1-d }. The encoder runs k=2 k-means in
// OKLab on the 16 source pixels, snaps cluster means to 4-bit precision,
// then sweeps the 8 distance values + 2 (C0,C1) orderings.
//
// Dispatch trigger: T-mode is signalled by diff bit = 1 AND R5+dR<0 OR
// R5+dR>31. With R0a (high 2 bits of R0) at raw bits 60..59 and R0b (low
// 2) at raw bits 57..56, the encoder picks raw bits 63..61 (the high
// 3 of R5) and raw bit 58 (sign bit of dR) such that the dispatcher
// always picks T-mode regardless of (R0a, R0b). Cases:
//
//   - R0a + R0b < 4: raw 63..61 = 0, bit58 = 1 → R5+dR < 0 (downward).
//   - else:          raw 63..61 = 7, bit58 = 0 → R5+dR > 31 (upward).
//
// Verified by exhaustive enumeration of R0 ∈ [0,15] × dispatcher rules.

void cluster_k2_oklab(const Sample16& s, std::uint8_t c0[3], std::uint8_t c1[3]) {
    // Initialise with two extreme pixels — pick the pair with largest OKLab
    // distance. Cheap (16² = 256 distances) and avoids local-minimum traps.
    int i0 = 0, i1 = 0;
    float best_d = -1.0f;
    for (int i = 0; i < 16; ++i) {
        for (int j = i + 1; j < 16; ++j) {
            float d = color_space::fma_dist_sq(s.lab[i].L - s.lab[j].L,
                                                s.lab[i].a - s.lab[j].a,
                                                s.lab[i].b - s.lab[j].b);
            if (d > best_d) { best_d = d; i0 = i; i1 = j; }
        }
    }
    color_space::OKLab m0 = s.lab[i0];
    color_space::OKLab m1 = s.lab[i1];

    // Two Lloyd iterations are sufficient on k=2 with good initialisation.
    for (int iter = 0; iter < 4; ++iter) {
        float sum0[3] = {0}, sum1[3] = {0};
        int n0 = 0, n1 = 0;
        for (int i = 0; i < 16; ++i) {
            float d0 = color_space::fma_dist_sq(s.lab[i].L - m0.L,
                                                 s.lab[i].a - m0.a,
                                                 s.lab[i].b - m0.b);
            float d1 = color_space::fma_dist_sq(s.lab[i].L - m1.L,
                                                 s.lab[i].a - m1.a,
                                                 s.lab[i].b - m1.b);
            if (d0 <= d1) {
                sum0[0] += s.srgb8[i][0]; sum0[1] += s.srgb8[i][1]; sum0[2] += s.srgb8[i][2];
                ++n0;
            } else {
                sum1[0] += s.srgb8[i][0]; sum1[1] += s.srgb8[i][1]; sum1[2] += s.srgb8[i][2];
                ++n1;
            }
        }
        if (n0 == 0) { n0 = 1; sum0[0] = s.srgb8[i0][0]; sum0[1] = s.srgb8[i0][1]; sum0[2] = s.srgb8[i0][2]; }
        if (n1 == 0) { n1 = 1; sum1[0] = s.srgb8[i1][0]; sum1[1] = s.srgb8[i1][1]; sum1[2] = s.srgb8[i1][2]; }
        std::uint8_t a0[3] = {std::uint8_t(int(sum0[0] / n0)),
                              std::uint8_t(int(sum0[1] / n0)),
                              std::uint8_t(int(sum0[2] / n0))};
        std::uint8_t a1[3] = {std::uint8_t(int(sum1[0] / n1)),
                              std::uint8_t(int(sum1[1] / n1)),
                              std::uint8_t(int(sum1[2] / n1))};
        m0 = color_space::srgb8_to_oklab(a0[0], a0[1], a0[2]);
        m1 = color_space::srgb8_to_oklab(a1[0], a1[1], a1[2]);
        c0[0] = a0[0]; c0[1] = a0[1]; c0[2] = a0[2];
        c1[0] = a1[0]; c1[1] = a1[1]; c1[2] = a1[2];
    }
}

inline int snap_4bit(int v) { return std::clamp((v + 8) >> 4, 0, 15); }

template<block_compress::BlockMetric M>
Candidate encode_t(const Sample16& s) {
    std::uint8_t c0_8[3], c1_8[3];
    cluster_k2_oklab(s, c0_8, c1_8);

    Candidate best{};
    best.err = std::numeric_limits<float>::infinity();
    best.mode = SubMode::t_mode;

    // Try both orderings of (C0, C1) — paint geometry isn't symmetric.
    for (int swap = 0; swap < 2; ++swap) {
        const std::uint8_t* C0 = swap ? c1_8 : c0_8;
        const std::uint8_t* C1 = swap ? c0_8 : c1_8;
        int R0_4 = snap_4bit(C0[0]);
        int G0_4 = snap_4bit(C0[1]);
        int B0_4 = snap_4bit(C0[2]);
        int R1_4 = snap_4bit(C1[0]);
        int G1_4 = snap_4bit(C1[1]);
        int B1_4 = snap_4bit(C1[2]);
        std::uint8_t C0e[3] = {expand4(std::uint32_t(R0_4)),
                                expand4(std::uint32_t(G0_4)),
                                expand4(std::uint32_t(B0_4))};
        std::uint8_t C1e[3] = {expand4(std::uint32_t(R1_4)),
                                expand4(std::uint32_t(G1_4)),
                                expand4(std::uint32_t(B1_4))};

        for (int d_idx = 0; d_idx < 8; ++d_idx) {
            int d = kDistanceT[d_idx];
            std::uint8_t paint[4][3];
            paint[0][0] = C0e[0]; paint[0][1] = C0e[1]; paint[0][2] = C0e[2];
            for (int ch = 0; ch < 3; ++ch) {
                paint[1][ch] = clamp_u8(int(C1e[ch]) + d);
                paint[2][ch] = C1e[ch];
                paint[3][ch] = clamp_u8(int(C1e[ch]) - d);
            }
            // Per-pixel argmin over 4 paints.
            int sel[16];
            float tot_err = 0.0f;
            for (int i = 0; i < 16; ++i) {
                float be = std::numeric_limits<float>::infinity();
                int bp = 0;
                for (int p = 0; p < 4; ++p) {
                    float e;
                    if constexpr (M == block_compress::BlockMetric::srgb_mse) {
                        int dr = int(s.srgb8[i][0]) - int(paint[p][0]);
                        int dg = int(s.srgb8[i][1]) - int(paint[p][1]);
                        int db = int(s.srgb8[i][2]) - int(paint[p][2]);
                        e = float(dr * dr + dg * dg + db * db);
                    } else {
                        color_space::OKLab plab = color_space::srgb8_to_oklab(
                            paint[p][0], paint[p][1], paint[p][2]);
                        e = color_space::fma_dist_sq(s.lab[i].L - plab.L,
                                                     s.lab[i].a - plab.a,
                                                     s.lab[i].b - plab.b);
                    }
                    if (e < be) { be = e; bp = p; }
                }
                sel[i] = bp;
                tot_err += be;
            }

            // Compare against best so far — pack only if better.
            if (tot_err < best.err) {
                Block blk{};
                std::uint64_t v = 0;
                auto put = [&](std::uint64_t val, int size, int hi) {
                    int shift = hi - size + 1;
                    v |= (val & ((std::uint64_t(1) << size) - 1)) << shift;
                };

                int R0a = (R0_4 >> 2) & 0x3;
                int R0b = R0_4 & 0x3;
                bool downward = (R0a + R0b) < 4;
                std::uint64_t raw_63_61 = downward ? 0 : 7;
                std::uint64_t bit58 = downward ? 1 : 0;

                put(raw_63_61, 3, 63);   // R5 high 3 bits — dispatch helper
                put(std::uint64_t(R0a), 2, 60);  // R0a — bits 60..59
                put(bit58, 1, 58);              // dR sign bit
                put(std::uint64_t(R0b), 2, 57); // R0b — bits 57..56
                put(std::uint64_t(G0_4), 4, 55);
                put(std::uint64_t(B0_4), 4, 51);
                put(std::uint64_t(R1_4), 4, 47);
                put(std::uint64_t(G1_4), 4, 43);
                put(std::uint64_t(B1_4), 4, 39);
                put(std::uint64_t((d_idx >> 1) & 0x3), 2, 35);
                put(std::uint64_t(d_idx & 0x1), 1, 32);
                put(1u, 1, 33);  // diffbit

                // Selector bits (raw 0..3, no unscramble for T-mode).
                for (int idx = 0; idx < 16; ++idx) {
                    int x = idx % 4, y = idx / 4;
                    int pos = x * 4 + y;
                    unsigned sc = unsigned(sel[idx]);
                    v |= std::uint64_t((sc >> 1u) & 1u) << (16 + pos);
                    v |= std::uint64_t(sc & 1u) << pos;
                }
                for (int i = 0; i < 8; ++i) {
                    blk[std::size_t(i)] = std::uint8_t((v >> ((7 - i) * 8)) & 0xFFu);
                }
                // Sanity round-trip via the decoder — guarantees we score
                // against what the GPU actually renders.
                std::uint8_t flat[48];
                decode_block(blk, flat);
                std::uint8_t dec[16][3];
                for (int i = 0; i < 16; ++i) {
                    dec[i][0] = flat[i * 3 + 0];
                    dec[i][1] = flat[i * 3 + 1];
                    dec[i][2] = flat[i * 3 + 2];
                }
                float verified = score_decoded<M>(s, dec);
                if (verified < best.err) {
                    best.err = verified;
                    best.block = blk;
                    std::memcpy(best.decoded, dec, sizeof(dec));
                    best.mode = SubMode::t_mode;
                }
            }
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// H-mode encoder
// ---------------------------------------------------------------------------
//
// 2 base colours C0, C1 (each 4-bit per channel) + 3-bit distance.
// Paint colours: { C0+d, C0-d, C1+d, C1-d }. Distance index's LOW bit is
// IMPLICIT — derived by the decoder from (col0_lex ≥ col1_lex) where
// col_lex is the packed 12-bit (R<<8)|(G<<4)|B value. Encoder must
// arrange (C0,C1) so this matches the wanted dist_lo, swapping the pair
// if not — paint set is symmetric under C0↔C1 (just relabel selectors).
//
// Dispatch trigger: diffbit=1 + R5+dR ∈ [0,31] + G5+dG ∉ [0,31].
// dR is FIXED by the payload (raw 58..56 = G0_4 >> 1, so dR is a function
// of G0_4 alone). raw 63 picks R5 ∈ {R0_4, 16+R0_4} — we pick whichever
// keeps R5+dR in range. G's overflow trigger uses the free bits
// raw 55..53 (G5 high 3) and raw 50 (dG sign); we brute-force the 16
// combinations and pick one that makes G5+dG ∉ [0,31].

template<block_compress::BlockMetric M>
Candidate encode_h(const Sample16& s) {
    std::uint8_t c0_8[3], c1_8[3];
    cluster_k2_oklab(s, c0_8, c1_8);

    Candidate best{};
    best.err = std::numeric_limits<float>::infinity();
    best.mode = SubMode::h_mode;

    int R0_4 = snap_4bit(c0_8[0]);
    int G0_4 = snap_4bit(c0_8[1]);
    int B0_4 = snap_4bit(c0_8[2]);
    int R1_4 = snap_4bit(c1_8[0]);
    int G1_4 = snap_4bit(c1_8[1]);
    int B1_4 = snap_4bit(c1_8[2]);

    auto col_lex = [](int R, int G, int B) { return (R << 8) | (G << 4) | B; };

    for (int d_idx = 0; d_idx < 8; ++d_idx) {
        int dist_lo_wanted = d_idx & 1;
        int dist_hi = d_idx >> 1;
        int Rc0 = R0_4, Gc0 = G0_4, Bc0 = B0_4;
        int Rc1 = R1_4, Gc1 = G1_4, Bc1 = B1_4;
        int lex0 = col_lex(Rc0, Gc0, Bc0);
        int lex1 = col_lex(Rc1, Gc1, Bc1);
        bool need_geq = dist_lo_wanted == 1;
        if ((lex0 >= lex1) != need_geq) {
            // Swap C0 ↔ C1 to match the implicit distance LO bit.
            std::swap(Rc0, Rc1);
            std::swap(Gc0, Gc1);
            std::swap(Bc0, Bc1);
            lex0 = col_lex(Rc0, Gc0, Bc0);
            lex1 = col_lex(Rc1, Gc1, Bc1);
            if ((lex0 >= lex1) != need_geq) continue;  // can't satisfy
        }
        int d = kDistanceH[d_idx];
        std::uint8_t C0e[3] = {expand4(std::uint32_t(Rc0)),
                                expand4(std::uint32_t(Gc0)),
                                expand4(std::uint32_t(Bc0))};
        std::uint8_t C1e[3] = {expand4(std::uint32_t(Rc1)),
                                expand4(std::uint32_t(Gc1)),
                                expand4(std::uint32_t(Bc1))};
        std::uint8_t paint[4][3];
        for (int ch = 0; ch < 3; ++ch) {
            paint[0][ch] = clamp_u8(int(C0e[ch]) + d);
            paint[1][ch] = clamp_u8(int(C0e[ch]) - d);
            paint[2][ch] = clamp_u8(int(C1e[ch]) + d);
            paint[3][ch] = clamp_u8(int(C1e[ch]) - d);
        }
        int sel[16];
        float tot_err = 0.0f;
        for (int i = 0; i < 16; ++i) {
            float be = std::numeric_limits<float>::infinity();
            int bp = 0;
            for (int p = 0; p < 4; ++p) {
                float e;
                if constexpr (M == block_compress::BlockMetric::srgb_mse) {
                    int dr = int(s.srgb8[i][0]) - int(paint[p][0]);
                    int dg = int(s.srgb8[i][1]) - int(paint[p][1]);
                    int db = int(s.srgb8[i][2]) - int(paint[p][2]);
                    e = float(dr * dr + dg * dg + db * db);
                } else {
                    color_space::OKLab plab = color_space::srgb8_to_oklab(
                        paint[p][0], paint[p][1], paint[p][2]);
                    e = color_space::fma_dist_sq(s.lab[i].L - plab.L,
                                                 s.lab[i].a - plab.a,
                                                 s.lab[i].b - plab.b);
                }
                if (e < be) { be = e; bp = p; }
            }
            sel[i] = bp;
            tot_err += be;
        }
        if (tot_err < best.err) {
            // Pack H-mode block. dR is FIXED by G0_4 (raw 58..56 are
            // G0_4's high 3 bits per unstuff58). Free bits: raw 63
            // (chooses R5 ∈ {R0_4, 16+R0_4}, set to keep R safe), and
            // raw 55..53 + raw 50 (4 bits — brute-force which combo
            // overflows G).
            int g0_hi3 = (Gc0 >> 1) & 0x7;
            int dR_signed = (g0_hi3 < 4) ? g0_hi3 : (g0_hi3 - 8);  // sign-extend 3-bit
            int raw63 = (g0_hi3 >= 4) ? 1 : 0;
            int R5 = raw63 * 16 + Rc0;
            int R_test = R5 + dR_signed;
            if (R_test < 0 || R_test > 31) continue;  // shouldn't happen by choice of raw63

            // Per unstuff58:
            //   raw 52       = G0_4 bit 0           (fixed by payload)
            //   raw 51       = B0_4 bit 3           (fixed)
            //   raw 49..47   = B0_4 bits 2..0       (fixed)
            //   raw 48       = B0_4 bit 1           (fixed; overlaps dG bit 0!)
            // dG = sign_extend(raw 50..48, 3) = sign_extend(s ++ B0_4_bit2 ++ B0_4_bit1, 3).
            // So dG's LOW 2 bits are fixed by B0_4; only the sign bit (raw 50)
            // is free. We brute-force (raw 55..53) × (raw 50) = 16 combos.
            int b0_bit3 = (Bc0 >> 3) & 1;
            int b0_bit2 = (Bc0 >> 2) & 1;
            int b0_bit1 = (Bc0 >> 1) & 1;
            // sign=0 → dG = 2·b0_bit2 + b0_bit1 ∈ [0, 3]
            // sign=1 → dG = -4 + 2·b0_bit2 + b0_bit1 ∈ [-4, -1]
            int g0_lo = Gc0 & 1;

            int chosen_free = -1;
            int chosen_sign = -1;
            for (int s_bit = 0; s_bit < 2 && chosen_free < 0; ++s_bit) {
                int dG = (s_bit == 0)
                             ? (2 * b0_bit2 + b0_bit1)
                             : (-4 + 2 * b0_bit2 + b0_bit1);
                for (int F = 0; F < 8; ++F) {
                    int G5 = 4 * F + 2 * g0_lo + b0_bit3;
                    int G_test = G5 + dG;
                    if (G_test < 0 || G_test > 31) {
                        chosen_free = F;
                        chosen_sign = s_bit;
                        break;
                    }
                }
            }
            if (chosen_free < 0) continue;  // no trigger combination works

            std::uint64_t v = 0;
            auto put = [&](std::uint64_t val, int size, int hi) {
                int shift = hi - size + 1;
                v |= (val & ((std::uint64_t(1) << size) - 1)) << shift;
            };
            put(std::uint64_t(raw63), 1, 63);
            put(std::uint64_t(Rc0), 4, 62);                 // R0_4 → raw 62..59
            put(std::uint64_t(g0_hi3), 3, 58);              // G0_4 >> 1 → raw 58..56
            put(std::uint64_t(chosen_free), 3, 55);         // free G5 high → raw 55..53
            put(std::uint64_t(g0_lo), 1, 52);               // G0_4 & 1
            put(std::uint64_t(b0_bit3), 1, 51);             // B0_4 bit 3
            put(std::uint64_t(chosen_sign), 1, 50);         // dG sign bit
            put(std::uint64_t(b0_bit2), 1, 49);             // B0_4 bit 2
            put(std::uint64_t(b0_bit1), 1, 48);             // B0_4 bit 1
            put(std::uint64_t(Bc0 & 1), 1, 47);             // B0_4 bit 0
            put(std::uint64_t(Rc1), 4, 46);                 // R1_4 → raw 46..43
            put(std::uint64_t(Gc1), 4, 42);                 // G1_4 → raw 42..39
            put(std::uint64_t(Bc1), 4, 38);                 // B1_4 → raw 38..35
            put(std::uint64_t((dist_hi >> 1) & 1), 1, 34);  // dist HI bit 1 → raw 34
            put(1u, 1, 33);                                 // diffbit
            put(std::uint64_t(dist_hi & 1), 1, 32);         // dist HI bit 0 → raw 32

            for (int idx = 0; idx < 16; ++idx) {
                int x = idx % 4, y = idx / 4;
                int pos = x * 4 + y;
                unsigned sc = unsigned(sel[idx]);
                v |= std::uint64_t((sc >> 1u) & 1u) << (16 + pos);
                v |= std::uint64_t(sc & 1u) << pos;
            }

            Block blk{};
            for (int i = 0; i < 8; ++i) {
                blk[std::size_t(i)] = std::uint8_t((v >> ((7 - i) * 8)) & 0xFFu);
            }
            // Round-trip verify via decoder (also catches any spec drift).
            std::uint8_t flat[48];
            decode_block(blk, flat);
            if (classify(blk) != SubMode::h_mode) continue;  // dispatch failed
            std::uint8_t dec[16][3];
            for (int i = 0; i < 16; ++i) {
                dec[i][0] = flat[i * 3 + 0];
                dec[i][1] = flat[i * 3 + 1];
                dec[i][2] = flat[i * 3 + 2];
            }
            float verified = score_decoded<M>(s, dec);
            if (verified < best.err) {
                best.err = verified;
                best.block = blk;
                std::memcpy(best.decoded, dec, sizeof(dec));
                best.mode = SubMode::h_mode;
            }
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Per-block picker
// ---------------------------------------------------------------------------

template<block_compress::BlockMetric M>
Candidate encode_block(const Sample16& s, const Options& opts) {
    Candidate best = encode_etc1<M>(s);
    if (opts.effort >= 1) {
        Candidate planar = encode_planar<M>(s);
        if (planar.err < best.err) best = planar;
    }
    // T-mode is opt-in via --etc2-effort 3 — its hard 2-colour
    // quantisation can win OKLab² per block but lose SSIMULACRA2 on
    // continuous-tone content (same pattern as PSNR-vs-S2 tension in
    // project_ham_aware_ed.md). Cartoon / high-contrast tile content
    // gains from it though, so we keep the option.
    if (opts.effort >= 3) {
        Candidate t = encode_t<M>(s);
        if (t.err < best.err) best = t;
        Candidate h = encode_h<M>(s);
        if (h.err < best.err) best = h;
    }
    return best;
}

}  // namespace

EncodeResult encode_image(std::span<const std::uint8_t> rgb_srgb8,
                          int image_w,
                          int image_h,
                          const Options& options) {
    EncodeResult res;
    res.block_cols = (image_w + kBlockW - 1) / kBlockW;
    res.block_rows = (image_h + kBlockH - 1) / kBlockH;
    const auto bcols = static_cast<std::size_t>(res.block_cols);
    res.blocks.assign(bcols * static_cast<std::size_t>(res.block_rows), Block{});

    // Pad source so we don't need per-edge clamps in the inner loop.
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

    // Block-grid error diffusion — the project's main quality wager over
    // existing ETC2 encoders, which treat blocks independently. After each
    // block commits, the per-block mean residual in OKLab is fed through
    // the SAME error-diffusion kernel catalog dither.cpp uses for per-
    // pixel ED. Defaults to Floyd-Steinberg; --dither / Options.block_ed
    // .method picks any of the supported kernels (Atkinson, Stucki,
    // Jarvis, Sierra-Lite).
    //
    // Threading: block rows are partitioned into N_threads strips, each
    // run in parallel via pipeline::parallel_for. Within each strip the
    // ED carry is honoured row-by-row (serpentine flip still applies).
    // ED carry STOPS at strip boundaries — quality cost is small (a few
    // horizontal seams per image, hidden by SSIMULACRA2 spatial blur).
    // Output is deterministic: strips have fixed by-row ranges and each
    // strip's results land at a unique block-index range in res.blocks.
    const bool use_block_ed = options.block_ed.strength > 0.0f;
    auto ed_kernel = dither::error_diffusion_kernel(options.block_ed.method);

    int n_strips = int(std::max(std::thread::hardware_concurrency(), 1u));
    n_strips = std::min(n_strips, res.block_rows);
    if (n_strips < 1) n_strips = 1;
    const int rows_per_strip = (res.block_rows + n_strips - 1) / n_strips;

    std::atomic<float> total_err_atom{0.0f};
    std::array<std::atomic<int>, 5> mode_counts_atom{};

    auto run_one = [&](auto metric_tag) {
        constexpr block_compress::BlockMetric M = decltype(metric_tag)::value;
        pipeline::parallel_for(std::size_t(n_strips), [&](std::size_t strip) {
            const int by_lo = int(strip) * rows_per_strip;
            const int by_hi = std::min(by_lo + rows_per_strip, res.block_rows);
            if (by_lo >= by_hi) return;

            // Per-strip ED carry (small; reset to zero).
            block_compress::BlockGrid<color_space::OKLab> err_carry(
                res.block_cols, by_hi - by_lo);
            for (auto& v : err_carry.as_span()) v = {0.f, 0.f, 0.f};

            Sample16 s{};
            float strip_err = 0.0f;
            std::array<int, 5> strip_mode_counts{};

            for (int by = by_lo; by < by_hi; ++by) {
                const int local_by = by - by_lo;
                bool reverse_row = use_block_ed && options.block_ed.serpentine &&
                                   (local_by & 1);
                int bx_start = reverse_row ? res.block_cols - 1 : 0;
                int bx_end = reverse_row ? -1 : res.block_cols;
                int bx_step = reverse_row ? -1 : 1;
                for (int bx = bx_start; bx != bx_end; bx += bx_step) {
                    color_space::OKLab shift =
                        use_block_ed ? err_carry[bx, local_by]
                                     : color_space::OKLab{0.f, 0.f, 0.f};
                    load_sample(s, padded, pw, bx * kBlockW, by * kBlockH, shift);

                    Candidate c = encode_block<M>(s, options);

                    if (use_block_ed && !ed_kernel.empty()) {
                        float tL = 0.f, ta = 0.f, tb = 0.f;
                        float dL = 0.f, da = 0.f, db = 0.f;
                        for (int i = 0; i < 16; ++i) {
                            tL += s.lab[i].L; ta += s.lab[i].a; tb += s.lab[i].b;
                            auto dec_lab = color_space::srgb8_to_oklab(
                                c.decoded[i][0], c.decoded[i][1], c.decoded[i][2]);
                            dL += dec_lab.L; da += dec_lab.a; db += dec_lab.b;
                        }
                        color_space::OKLab residual{(tL - dL) * (1.f / 16.f),
                                                    (ta - da) * (1.f / 16.f),
                                                    (tb - db) * (1.f / 16.f)};
                        block_compress::propagate_block_residual(
                            err_carry, {bx, local_by}, residual, ed_kernel,
                            options.block_ed, reverse_row);
                    }

                    std::size_t bidx = static_cast<std::size_t>(by) * bcols +
                                       static_cast<std::size_t>(bx);
                    res.blocks[bidx] = c.block;
                    strip_mode_counts[std::size_t(c.mode)] += 1;
                    strip_err += c.err;
                }
            }
            // Merge strip-local counters via atomics.
            for (std::size_t i = 0; i < 5; ++i) {
                mode_counts_atom[i].fetch_add(strip_mode_counts[i],
                                              std::memory_order_relaxed);
            }
            float prev = total_err_atom.load(std::memory_order_relaxed);
            while (!total_err_atom.compare_exchange_weak(
                prev, prev + strip_err, std::memory_order_relaxed)) {
            }
        });
    };
    if (options.metric == block_compress::BlockMetric::srgb_mse) {
        run_one(std::integral_constant<block_compress::BlockMetric,
                                       block_compress::BlockMetric::srgb_mse>{});
    } else {
        run_one(std::integral_constant<block_compress::BlockMetric,
                                       block_compress::BlockMetric::oklab2>{});
    }
    for (std::size_t i = 0; i < 5; ++i) {
        res.mode_counts[i] = mode_counts_atom[i].load(std::memory_order_relaxed);
    }
    res.total_oklab2_error = total_err_atom.load(std::memory_order_relaxed);
    return res;
}

}  // namespace png2amiga::etc2
