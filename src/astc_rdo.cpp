// ASTC rate-distortion optimisation (--rdo). See astc_rdo.hpp.
//
// The pass walks the finished block grid in stream order, twice. For
// every block it builds a set of replacement candidates, scores each
// one with
//
//     J = D + lambda * R
//
// and keeps the lowest.
//
// D is the encoder's own block error (OKLab squared distance to the
// per-texel target the main search used), plus the error of the
// quadrant means and of the block mean, plus a charge for a large rise
// over the encoder's block (see "The pass" below).
//
// R is the cost in bits of the 16 block bytes under an LZ77 model of
// the bytes already written: real matches against the preceding output
// (hash chains, repeat offsets, a match that runs on from the previous
// block) and an adaptive order-0 cost for literals. zstd is the target
// compressor (KTX2 supercompression), so the model follows the zstd
// sequence layout.
//
// Candidates of a block:
//   - the block the main search chose;
//   - for every donor (a recently written block, a block of the rows
//     above, in the second pass also a block that follows): the donor
//     itself; the donor's configuration and endpoints with the weights
//     solved again for this block; the donor's configuration and weights
//     with the endpoints solved again; and the donor's configuration
//     alone with both solved again.
// A solve keeps the block mode, partition and endpoint formats of the
// donor, so the reused bits stay on the same bit positions and whole
// bytes repeat.
//
// The file holds its own ASTC block codec (symbolic decode / encode and
// the texel paint). It shares no code with astc.cpp and none with
// astcenc.

#include "astc_rdo.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace png2amiga::astc::rdo {

namespace {

using u8 = std::uint8_t;
using color_space::OKLab;

constexpr int kMaxTexels = 144;
constexpr float kInf = std::numeric_limits<float>::infinity();

// ---------------------------------------------------------------------------
// Format tables (ASTC specification).
// ---------------------------------------------------------------------------

struct QuantDesc {
    int levels, bits, trits, quints;
};
constexpr QuantDesc kQuant[21] = {
    {2, 1, 0, 0},   {3, 0, 1, 0},   {4, 2, 0, 0},   {5, 0, 0, 1},   {6, 1, 1, 0},
    {8, 3, 0, 0},   {10, 1, 0, 1},  {12, 2, 1, 0},  {16, 4, 0, 0},  {20, 2, 0, 1},
    {24, 3, 1, 0},  {32, 5, 0, 0},  {40, 3, 0, 1},  {48, 4, 1, 0},  {64, 6, 0, 0},
    {80, 4, 0, 1},  {96, 5, 1, 0},  {128, 7, 0, 0}, {160, 5, 0, 1}, {192, 6, 1, 0},
    {256, 8, 0, 0},
};
constexpr int kMinEndpointQuant = 4;  // QUANT_6

constexpr int ise_bits(int count, int qi) {
    const QuantDesc& q = kQuant[qi];
    int b = count * q.bits;
    if (q.trits) b += (count * 8 + 4) / 5;
    if (q.quints) b += (count * 7 + 2) / 3;
    return b;
}

// Colour endpoint unquantisation of an ISE value (spec C.2.13).
constexpr int color_unquant(int qi, int stored) {
    const QuantDesc& q = kQuant[qi];
    const int n = q.bits;
    if (!q.trits && !q.quints) {
        int r = 0, have = 0;
        while (have < 8) {
            r = (r << n) | stored;
            have += n;
        }
        return r >> (have - 8);
    }
    const int m = stored & ((1 << n) - 1);
    const int D = stored >> n;
    const int a = m & 1, b = (m >> 1) & 1, c = (m >> 2) & 1;
    const int d = (m >> 3) & 1, e = (m >> 4) & 1, f = (m >> 5) & 1;
    const int A = a ? 0x1FF : 0;
    int B = 0, C = 0;
    if (q.trits) {
        constexpr int kC[7] = {0, 204, 93, 44, 22, 11, 5};
        C = kC[n];
        if (n == 2) B = b * 0x116;
        if (n == 3) B = c * 0x10A + b * 0x85;
        if (n == 4) B = d * 0x104 + c * 0x82 + b * 0x41;
        if (n == 5) B = e * 0x102 + d * 0x81 + c * 0x40 + b * 0x20;
        if (n == 6) B = f * 0x101 + e * 0x80 + d * 0x40 + c * 0x20 + b * 0x10;
    } else {
        constexpr int kC[6] = {0, 113, 54, 26, 13, 6};
        C = kC[n];
        if (n == 2) B = b * 0x10C;
        if (n == 3) B = c * 0x105 + b * 0x82;
        if (n == 4) B = d * 0x102 + c * 0x81 + b * 0x40;
        if (n == 5) B = e * 0x101 + d * 0x80 + c * 0x40 + b * 0x20;
    }
    int T = D * C + B;
    T ^= A;
    return (A & 0x80) | (T >> 2);
}

// Weight unquantisation of an ISE value to 0..64 (spec C.2.17).
constexpr int weight_unquant(int qi, int stored) {
    const QuantDesc& q = kQuant[qi];
    const int n = q.bits;
    int T = 0;
    if (!q.trits && !q.quints) {
        int r = 0, have = 0;
        while (have < 6) {
            r = (r << n) | stored;
            have += n;
        }
        T = r >> (have - 6);
    } else if (n == 0) {
        T = q.trits ? stored * 32 : stored * 16;
        if (T > 63) T = 63;
        if (q.quints && stored == 3) T = 47;
    } else {
        const int m = stored & ((1 << n) - 1);
        const int D = stored >> n;
        const int a = m & 1, b = (m >> 1) & 1, c = (m >> 2) & 1;
        const int A = a ? 0x7F : 0;
        int B = 0, C = 0;
        if (q.trits) {
            constexpr int kC[4] = {0, 50, 23, 11};
            C = kC[n];
            if (n == 2) B = b * 0x45;
            if (n == 3) B = c * 0x42 + b * 0x21;
        } else {
            constexpr int kC[3] = {0, 28, 13};
            C = kC[n];
            if (n == 2) B = b * 0x42;
        }
        T = D * C + B;
        T ^= A;
        T = (A & 0x20) | (T >> 2);
    }
    if (T > 32) T += 1;
    return T;
}
static_assert(weight_unquant(4, 3) == 52 && weight_unquant(7, 2) == 17 &&
              weight_unquant(10, 2) == 8 && weight_unquant(9, 2) == 16 &&
              weight_unquant(3, 3) == 48 && weight_unquant(1, 2) == 64);

struct Tables {
    u8 trit[256][5]{};
    u8 quint[128][3]{};
    u8 trit_enc[243]{};
    u8 quint_enc[125]{};
    // Colour endpoints, per quant index: ISE value <-> value rank.
    u8 c_unq[21][256]{};     // ISE value -> 8-bit value
    u8 c_rank[21][256]{};    // ISE value -> rank (ascending value)
    u8 c_stored[21][256]{};  // rank -> ISE value
    u8 c_near[21][256]{};    // 8-bit value -> ISE value of the nearest level
    // Weights, per quant index 0..11.
    u8 w_rank[12][32]{};    // ISE value -> rank
    u8 w_stored[12][32]{};  // rank -> ISE value
    u8 w_val[12][32]{};     // rank -> 0..64
    u8 w_near[12][65]{};    // 0..64 -> nearest rank
};

Tables build_tables() {
    Tables t;
    for (int T = 0; T < 256; ++T) {
        int v[5];
        int C;
        if (((T >> 2) & 7) == 7) {
            C = ((T >> 5) << 2) | (T & 3);
            v[4] = v[3] = 2;
        } else {
            C = T & 0x1F;
            if (((T >> 5) & 3) == 3) {
                v[4] = 2;
                v[3] = (T >> 7) & 1;
            } else {
                v[4] = (T >> 7) & 1;
                v[3] = (T >> 5) & 3;
            }
        }
        if ((C & 3) == 3) {
            v[2] = 2;
            v[1] = (C >> 4) & 1;
            v[0] = (((C >> 3) & 1) << 1) | (((C >> 2) & 1) & ~((C >> 3) & 1));
        } else if (((C >> 2) & 3) == 3) {
            v[2] = 2;
            v[1] = 2;
            v[0] = C & 3;
        } else {
            v[2] = (C >> 4) & 1;
            v[1] = (C >> 2) & 3;
            v[0] = (((C >> 1) & 1) << 1) | ((C & 1) & ~((C >> 1) & 1));
        }
        for (int i = 0; i < 5; ++i) t.trit[T][i] = u8(v[i]);
        const int idx = v[0] + 3 * v[1] + 9 * v[2] + 27 * v[3] + 81 * v[4];
        t.trit_enc[idx] = u8(T);  // several T decode alike: the highest is canonical
    }
    for (int Q = 0; Q < 128; ++Q) {
        int v[3];
        if (((Q >> 1) & 3) == 3 && ((Q >> 5) & 3) == 0) {
            v[2] = ((Q & 1) << 2) | ((((Q >> 4) & 1) & ~(Q & 1)) << 1) |
                   (((Q >> 3) & 1) & ~(Q & 1));
            v[1] = v[0] = 4;
        } else {
            int C;
            if (((Q >> 1) & 3) == 3) {
                v[2] = 4;
                C = (((Q >> 3) & 3) << 3) | ((~(Q >> 5) & 3) << 1) | (Q & 1);
            } else {
                v[2] = (Q >> 5) & 3;
                C = Q & 0x1F;
            }
            if ((C & 7) == 5) {
                v[1] = 4;
                v[0] = (C >> 3) & 3;
            } else {
                v[1] = (C >> 3) & 3;
                v[0] = C & 7;
            }
        }
        for (int i = 0; i < 3; ++i) t.quint[Q][i] = u8(v[i]);
        const int idx = v[0] + 5 * v[1] + 25 * v[2];
        if (v[0] < 5 && v[1] < 5 && v[2] < 5) t.quint_enc[idx] = u8(Q);
    }
    for (int qi = kMinEndpointQuant; qi < 21; ++qi) {
        const int n = kQuant[qi].levels;
        int order[256];
        for (int s = 0; s < n; ++s) {
            t.c_unq[qi][s] = u8(color_unquant(qi, s));
            order[s] = s;
        }
        std::sort(order, order + n,
                  [&](int a, int b) { return t.c_unq[qi][a] < t.c_unq[qi][b]; });
        for (int r = 0; r < n; ++r) {
            t.c_stored[qi][r] = u8(order[r]);
            t.c_rank[qi][order[r]] = u8(r);
        }
        for (int v = 0; v < 256; ++v) {
            int best = 0;
            for (int s = 1; s < n; ++s)
                if (std::abs(v - int(t.c_unq[qi][s])) < std::abs(v - int(t.c_unq[qi][best])))
                    best = s;
            t.c_near[qi][v] = u8(best);
        }
    }
    for (int qi = 0; qi < 12; ++qi) {
        const int n = kQuant[qi].levels;
        int order[32], val[32];
        for (int s = 0; s < n; ++s) {
            val[s] = weight_unquant(qi, s);
            order[s] = s;
        }
        std::sort(order, order + n, [&](int a, int b) { return val[a] < val[b]; });
        for (int r = 0; r < n; ++r) {
            t.w_stored[qi][r] = u8(order[r]);
            t.w_rank[qi][order[r]] = u8(r);
            t.w_val[qi][r] = u8(val[order[r]]);
        }
        for (int v = 0; v <= 64; ++v) {
            int best = 0;
            for (int r = 1; r < n; ++r)
                if (std::abs(v - int(t.w_val[qi][r])) < std::abs(v - int(t.w_val[qi][best])))
                    best = r;
            t.w_near[qi][v] = u8(best);
        }
    }
    return t;
}

const Tables& tables() {
    static const Tables t = build_tables();
    return t;
}

// Block mode -> weight grid, weight quant, weight bits (spec C.2.10).
bool decode_block_mode(int bm, int& xw, int& yw, int& wqi, int& wb, bool& dual) {
    int base_q = (bm >> 4) & 1;
    int H = (bm >> 9) & 1;
    int D = (bm >> 10) & 1;
    const int A = (bm >> 5) & 3;
    if ((bm & 3) != 0) {
        base_q |= (bm & 3) << 1;
        int B = (bm >> 7) & 3;
        switch ((bm >> 2) & 3) {
            case 0: xw = B + 4; yw = A + 2; break;
            case 1: xw = B + 8; yw = A + 2; break;
            case 2: xw = A + 2; yw = B + 8; break;
            default:
                B &= 1;
                if (bm & 0x100) {
                    xw = B + 2;
                    yw = A + 2;
                } else {
                    xw = A + 2;
                    yw = B + 6;
                }
                break;
        }
    } else {
        base_q |= ((bm >> 2) & 3) << 1;
        if (((bm >> 2) & 3) == 0) return false;
        const int B = (bm >> 9) & 3;
        switch ((bm >> 7) & 3) {
            case 0: xw = 12; yw = A + 2; break;
            case 1: xw = A + 2; yw = 12; break;
            case 2:
                xw = A + 6;
                yw = B + 6;
                D = 0;
                H = 0;
                break;
            default: {
                const int sub = (bm >> 5) & 3;
                if (sub == 0) {
                    xw = 6;
                    yw = 10;
                } else if (sub == 1) {
                    xw = 10;
                    yw = 6;
                } else {
                    return false;
                }
                break;
            }
        }
    }
    dual = D != 0;
    wqi = (base_q - 2) + 6 * H;
    if (wqi < 0 || wqi > 11) return false;
    const int count = xw * yw * (dual ? 2 : 1);
    if (count > 64) return false;
    wb = ise_bits(count, wqi);
    return wb >= 24 && wb <= 96;
}

// Partition of a texel (spec C.2.21).
int select_partition(int seed, int x, int y, int partition_count, bool small_block) {
    if (small_block) {
        x <<= 1;
        y <<= 1;
    }
    seed += (partition_count - 1) * 1024;
    auto rnum = std::uint32_t(seed);
    rnum ^= rnum >> 15;
    rnum *= 0xEEDE0891u;
    rnum ^= rnum >> 5;
    rnum += rnum << 16;
    rnum ^= rnum >> 7;
    rnum ^= rnum >> 3;
    rnum ^= rnum << 6;
    rnum ^= rnum >> 17;
    std::uint32_t s[8];
    for (int i = 0; i < 8; ++i) {
        s[i] = (rnum >> (4 * i)) & 0xF;
        s[i] *= s[i];
    }
    int sh1, sh2;
    if (seed & 1) {
        sh1 = (seed & 2) ? 4 : 5;
        sh2 = (partition_count == 3) ? 6 : 5;
    } else {
        sh1 = (partition_count == 3) ? 6 : 5;
        sh2 = (seed & 2) ? 4 : 5;
    }
    for (int i = 0; i < 8; ++i) s[i] >>= (i & 1) ? sh2 : sh1;
    int a = int(s[0]) * x + int(s[1]) * y + int(rnum >> 14);
    int b = int(s[2]) * x + int(s[3]) * y + int(rnum >> 10);
    int c = int(s[4]) * x + int(s[5]) * y + int(rnum >> 6);
    int d = int(s[6]) * x + int(s[7]) * y + int(rnum >> 2);
    a &= 0x3F;
    b &= 0x3F;
    c &= 0x3F;
    d &= 0x3F;
    if (partition_count <= 3) d = 0;
    if (partition_count <= 2) c = 0;
    if (a >= b && a >= c && a >= d) return 0;
    if (b >= c && b >= d) return 1;
    if (c >= d) return 2;
    return 3;
}

// ---------------------------------------------------------------------------
// Bit access and integer sequence encoding (spec C.2.12).
// ---------------------------------------------------------------------------

inline int get_bits(const u8* blk, int off, int n) {
    int v = 0;
    for (int i = 0; i < n; ++i) v |= ((blk[(off + i) >> 3] >> ((off + i) & 7)) & 1) << i;
    return v;
}
inline void put_bits(u8* blk, int off, int n, int v) {
    for (int i = 0; i < n; ++i)
        if ((v >> i) & 1) blk[(off + i) >> 3] = u8(blk[(off + i) >> 3] | (1 << ((off + i) & 7)));
}
// The weight stream runs downwards from bit 127.
inline int get_bits_rev(const u8* blk, int off, int n) {
    int v = 0;
    for (int i = 0; i < n; ++i) {
        const int p = 127 - (off + i);
        v |= ((blk[p >> 3] >> (p & 7)) & 1) << i;
    }
    return v;
}
inline void put_bits_rev(u8* blk, int off, int n, int v) {
    for (int i = 0; i < n; ++i)
        if ((v >> i) & 1) {
            const int p = 127 - (off + i);
            blk[p >> 3] = u8(blk[p >> 3] | (1 << (p & 7)));
        }
}

// Bit pieces of one trit / quint group, in stream order: value index or
// -1 for a piece of the packed T / Q word, then the bit count (-1 = the
// value's own bit count).
struct IsePiece {
    int value, shift, bits;
};
constexpr IsePiece kTritPieces[10] = {{0, 0, -1}, {-1, 0, 2}, {1, 0, -1}, {-1, 2, 2}, {2, 0, -1},
                                      {-1, 4, 1}, {3, 0, -1}, {-1, 5, 2}, {4, 0, -1}, {-1, 7, 1}};
constexpr IsePiece kQuintPieces[6] = {{0, 0, -1}, {-1, 0, 3}, {1, 0, -1},
                                      {-1, 3, 2}, {2, 0, -1}, {-1, 5, 2}};

template <typename Get>
void ise_decode(Get get, int count, int qi, u8* out) {
    const QuantDesc& q = kQuant[qi];
    const int total = ise_bits(count, qi);
    int pos = 0;
    auto take = [&](int n) {
        const int m = std::min(n, total - pos);
        const int v = m > 0 ? get(pos, m) : 0;
        pos += n;
        return v;
    };
    if (!q.trits && !q.quints) {
        for (int i = 0; i < count; ++i) out[i] = u8(take(q.bits));
        return;
    }
    const Tables& t = tables();
    const int group = q.trits ? 5 : 3;
    const IsePiece* pieces = q.trits ? kTritPieces : kQuintPieces;
    const int npieces = q.trits ? 10 : 6;
    for (int base = 0; base < count; base += group) {
        int m[5] = {}, packed = 0;
        for (int k = 0; k < npieces; ++k) {
            const IsePiece& p = pieces[k];
            if (p.value >= 0) m[p.value] = take(q.bits);
            else packed |= take(p.bits) << p.shift;
        }
        for (int i = 0; i < group && base + i < count; ++i) {
            const int hi = q.trits ? t.trit[packed][i] : t.quint[packed][i];
            out[base + i] = u8((hi << q.bits) | m[i]);
        }
    }
}

template <typename Put>
void ise_encode(Put put, const u8* in, int count, int qi) {
    const QuantDesc& q = kQuant[qi];
    const int total = ise_bits(count, qi);
    int pos = 0;
    auto give = [&](int n, int v) {
        const int m = std::min(n, total - pos);
        if (m > 0) put(pos, m, v & ((1 << m) - 1));
        pos += n;
    };
    if (!q.trits && !q.quints) {
        for (int i = 0; i < count; ++i) give(q.bits, in[i]);
        return;
    }
    const Tables& t = tables();
    const int group = q.trits ? 5 : 3;
    const IsePiece* pieces = q.trits ? kTritPieces : kQuintPieces;
    const int npieces = q.trits ? 10 : 6;
    for (int base = 0; base < count; base += group) {
        int m[5] = {}, hi[5] = {};
        for (int i = 0; i < group && base + i < count; ++i) {
            m[i] = in[base + i] & ((1 << q.bits) - 1);
            hi[i] = in[base + i] >> q.bits;
        }
        const int packed = q.trits
            ? t.trit_enc[hi[0] + 3 * hi[1] + 9 * hi[2] + 27 * hi[3] + 81 * hi[4]]
            : t.quint_enc[hi[0] + 5 * hi[1] + 25 * hi[2]];
        for (int k = 0; k < npieces; ++k) {
            const IsePiece& p = pieces[k];
            if (p.value >= 0) give(q.bits, m[p.value]);
            else give(p.bits, packed >> p.shift);
        }
    }
}

// ---------------------------------------------------------------------------
// Symbolic block.
// ---------------------------------------------------------------------------

struct Sym {
    bool ok = false;     // an LDR RGB block this pass can decode and rebuild
    int bm = 0;          // 11-bit block mode
    int gw = 0, gh = 0;  // weight grid
    int wqi = 0, wb = 0; // weight quant index, weight bits
    bool dual = false;
    int plane2 = 0;      // component on the second weight plane
    int np = 1;          // partitions
    int pi = 0;          // partition index
    bool mixed = false;  // per-partition CEM encoding
    int cem[4] = {};
    int qi = 0;          // endpoint quant index
    int nvals = 0;       // endpoint integers
    u8 ev[18] = {};      // endpoint ISE values
    u8 w[64] = {};       // weight ISE values (planes interleaved)
};

inline int cem_values(int cem) { return 2 * (cem / 4 + 1); }
inline int ep_offset(const Sym& s) { return s.np == 1 ? 17 : 29; }
inline int weight_count(const Sym& s) { return s.gw * s.gh * (s.dual ? 2 : 1); }

// Fill qi / nvals from the layout. False if the layout is not legal.
bool derive_endpoint_quant(Sym& s) {
    s.nvals = 0;
    for (int p = 0; p < s.np; ++p) s.nvals += cem_values(s.cem[p]);
    if (s.nvals > 18) return false;
    int below = 128 - s.wb;
    if (s.mixed) below -= 3 * s.np - 4;
    if (s.dual) below -= 2;
    const int avail = below - ep_offset(s);
    s.qi = -1;
    for (int qi = 20; qi >= kMinEndpointQuant; --qi)
        if (ise_bits(s.nvals, qi) <= avail) {
            s.qi = qi;
            break;
        }
    return s.qi >= 0;
}

Sym decode_sym(const Block& blk, int W, int H) {
    Sym s;
    const u8* b = blk.data();
    s.bm = get_bits(b, 0, 11);
    if ((s.bm & 0x1FF) == 0x1FC) return s;  // void extent
    if (!decode_block_mode(s.bm, s.gw, s.gh, s.wqi, s.wb, s.dual)) return s;
    if (s.gw > W || s.gh > H) return s;
    s.np = get_bits(b, 11, 2) + 1;
    if (s.dual && s.np == 4) return s;
    if (s.np == 1) {
        s.cem[0] = get_bits(b, 13, 4);
    } else {
        s.pi = get_bits(b, 13, 10);
        const int f = get_bits(b, 23, 6);
        if ((f & 3) == 0) {
            for (int p = 0; p < s.np; ++p) s.cem[p] = f >> 2;
        } else {
            s.mixed = true;
            const int high = 3 * s.np - 4;
            const int enc = f | (get_bits(b, 128 - s.wb - high, high) << 6);
            const int base = (f & 3) - 1;
            for (int p = 0; p < s.np; ++p) {
                const int c = (enc >> (2 + p)) & 1;
                const int m = (enc >> (2 + s.np + 2 * p)) & 3;
                s.cem[p] = (base + c) * 4 + m;
            }
        }
    }
    for (int p = 0; p < s.np; ++p) {
        const int c = s.cem[p];
        if (c != 0 && c != 1 && c != 6 && c != 8 && c != 9) return s;  // HDR or alpha
    }
    if (!derive_endpoint_quant(s)) return s;
    if (s.dual) {
        s.plane2 = get_bits(b, 128 - s.wb - (s.mixed ? 3 * s.np - 4 : 0) - 2, 2);
        if (s.plane2 == 3) return s;
    }
    const int eo = ep_offset(s);
    ise_decode([&](int off, int n) { return get_bits(b, eo + off, n); }, s.nvals, s.qi, s.ev);
    ise_decode([&](int off, int n) { return get_bits_rev(b, off, n); }, weight_count(s), s.wqi,
               s.w);
    for (int i = 0; i < s.nvals; ++i)
        if (s.ev[i] >= kQuant[s.qi].levels) return s;
    for (int i = 0; i < weight_count(s); ++i)
        if (s.w[i] >= kQuant[s.wqi].levels) return s;
    s.ok = true;
    return s;
}

Block encode_sym(const Sym& s) {
    Block blk{};
    u8* b = blk.data();
    put_bits(b, 0, 11, s.bm);
    put_bits(b, 11, 2, s.np - 1);
    if (s.np == 1) {
        put_bits(b, 13, 4, s.cem[0]);
    } else {
        put_bits(b, 13, 10, s.pi);
        if (!s.mixed) {
            put_bits(b, 23, 6, s.cem[0] << 2);
        } else {
            int base = 3;
            for (int p = 0; p < s.np; ++p) base = std::min(base, s.cem[p] >> 2);
            int enc = base + 1;
            for (int p = 0; p < s.np; ++p) {
                enc |= ((s.cem[p] >> 2) - base) << (2 + p);
                enc |= (s.cem[p] & 3) << (2 + s.np + 2 * p);
            }
            const int high = 3 * s.np - 4;
            put_bits(b, 23, 6, enc & 0x3F);
            put_bits(b, 128 - s.wb - high, high, enc >> 6);
        }
    }
    if (s.dual) put_bits(b, 128 - s.wb - (s.mixed ? 3 * s.np - 4 : 0) - 2, 2, s.plane2);
    const int eo = ep_offset(s);
    ise_encode([&](int off, int n, int v) { put_bits(b, eo + off, n, v); }, s.ev, s.nvals, s.qi);
    ise_encode([&](int off, int n, int v) { put_bits_rev(b, off, n, v); }, s.w, weight_count(s),
               s.wqi);
    return blk;
}

// Endpoint pair of one partition from unquantised values (spec C.2.14).
inline void blue_contract(int c[3]) {
    c[0] = (c[0] + c[2]) >> 1;
    c[1] = (c[1] + c[2]) >> 1;
}
inline void bit_transfer_signed(int& a, int& b) {
    b >>= 1;
    b |= a & 0x80;
    a >>= 1;
    a &= 0x3F;
    if (a & 0x20) a -= 0x40;
}
void decode_endpoints(int cem, const int* v, int e0[3], int e1[3]) {
    switch (cem) {
        case 0:
            for (int ch = 0; ch < 3; ++ch) {
                e0[ch] = v[0];
                e1[ch] = v[1];
            }
            break;
        case 1: {
            const int l0 = (v[0] >> 2) | (v[1] & 0xC0);
            const int l1 = std::min(l0 + (v[1] & 0x3F), 255);
            for (int ch = 0; ch < 3; ++ch) {
                e0[ch] = l0;
                e1[ch] = l1;
            }
            break;
        }
        case 6:
            for (int ch = 0; ch < 3; ++ch) {
                e0[ch] = (v[ch] * v[3]) >> 8;
                e1[ch] = v[ch];
            }
            break;
        case 8: {
            int a[3] = {v[0], v[2], v[4]}, b[3] = {v[1], v[3], v[5]};
            if (a[0] + a[1] + a[2] > b[0] + b[1] + b[2]) {
                blue_contract(a);
                blue_contract(b);
                for (int ch = 0; ch < 3; ++ch) {
                    e0[ch] = b[ch];
                    e1[ch] = a[ch];
                }
            } else {
                for (int ch = 0; ch < 3; ++ch) {
                    e0[ch] = a[ch];
                    e1[ch] = b[ch];
                }
            }
            break;
        }
        default: {  // 9
            int base[3], end[3], dsum = 0;
            for (int ch = 0; ch < 3; ++ch) {
                int b0 = v[2 * ch], d = v[2 * ch + 1];
                bit_transfer_signed(d, b0);
                base[ch] = b0;
                end[ch] = b0 + d;
                dsum += d;
            }
            if (dsum < 0) {
                blue_contract(base);
                blue_contract(end);
                for (int ch = 0; ch < 3; ++ch) {
                    e0[ch] = end[ch];
                    e1[ch] = base[ch];
                }
            } else {
                for (int ch = 0; ch < 3; ++ch) {
                    e0[ch] = base[ch];
                    e1[ch] = end[ch];
                }
            }
            break;
        }
    }
    for (int ch = 0; ch < 3; ++ch) {
        e0[ch] = std::clamp(e0[ch], 0, 255);
        e1[ch] = std::clamp(e1[ch], 0, 255);
    }
}

// ---------------------------------------------------------------------------
// Footprint context: weight infill maps and partition tables.
// ---------------------------------------------------------------------------

struct GridMap {
    int gw = 0, gh = 0, gn = 0;
    bool identity = false;
    u8 tg[kMaxTexels][4]{};  // grid points of a texel
    u8 tc[kMaxTexels][4]{};  // their contributions (sum 16)
    std::vector<std::uint16_t> goff;  // per grid point: texel list offsets
    std::vector<u8> gtex;
    std::vector<u8> gcoef;
    std::vector<float> gnorm;  // 1 / sum of contributions
};

struct Ctx {
    int W = 0, H = 0, TN = 0;
    std::unique_ptr<GridMap> maps[13][13];
    std::vector<u8> part[3];     // 2..4 partitions: 1024 x TN
    std::vector<u8> part_ok[3];
    u8 zeros[kMaxTexels] = {};

    const GridMap& map(int gw, int gh) {
        auto& slot = maps[gw][gh];
        if (slot) return *slot;
        slot = std::make_unique<GridMap>();
        GridMap& m = *slot;
        m.gw = gw;
        m.gh = gh;
        m.gn = gw * gh;
        const int sx = (1024 + W / 2) / (W - 1);
        const int sy = (1024 + H / 2) / (H - 1);
        std::vector<std::vector<std::pair<int, int>>> lists(static_cast<std::size_t>(m.gn));
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const int cx = (sx * x * (gw - 1) + 32) >> 6;
                const int cy = (sy * y * (gh - 1) + 32) >> 6;
                const int fx = cx & 0xF, fy = cy & 0xF;
                const int ix = cx >> 4, iy = cy >> 4;
                int q0 = iy * gw + ix, q1 = q0 + 1, q2 = q0 + gw, q3 = q2 + 1;
                if (ix >= gw - 1) {
                    q1 = q0;
                    q3 = q2;
                }
                if (iy >= gh - 1) {
                    q2 = q0;
                    q3 = q1;
                }
                const int w3 = (fx * fy + 8) >> 4;
                const int q[4] = {q0, q1, q2, q3};
                const int w[4] = {16 - fx - fy + w3, fx - w3, fy - w3, w3};
                const int j = y * W + x;
                for (int k = 0; k < 4; ++k) {
                    m.tg[j][k] = u8(q[k]);
                    m.tc[j][k] = u8(w[k]);
                    if (w[k] == 0) continue;
                    auto& l = lists[static_cast<std::size_t>(q[k])];
                    if (!l.empty() && l.back().first == j) l.back().second += w[k];
                    else l.emplace_back(j, w[k]);
                }
            }
        }
        m.identity = m.gn == TN;
        m.goff.assign(static_cast<std::size_t>(m.gn) + 1, 0);
        m.gnorm.assign(static_cast<std::size_t>(m.gn), 0.f);
        for (int g = 0; g < m.gn; ++g) {
            const auto& l = lists[static_cast<std::size_t>(g)];
            if (l.size() != 1 || l[0].second != 16) m.identity = false;
            int sum = 0;
            for (const auto& [j, c] : l) {
                m.gtex.push_back(u8(j));
                m.gcoef.push_back(u8(c));
                sum += c;
            }
            m.gnorm[static_cast<std::size_t>(g)] = sum > 0 ? 1.f / float(sum) : 0.f;
            m.goff[static_cast<std::size_t>(g) + 1] = std::uint16_t(m.gtex.size());
        }
        return m;
    }

    const u8* partition(int np, int pi) {
        if (np == 1) return zeros;
        auto& tab = part[np - 2];
        auto& ok = part_ok[np - 2];
        const auto tn = static_cast<std::size_t>(TN);
        if (tab.empty()) {
            tab.assign(1024 * tn, 0);
            ok.assign(1024, 0);
        }
        u8* row = &tab[static_cast<std::size_t>(pi) * tn];
        if (!ok[static_cast<std::size_t>(pi)]) {
            ok[static_cast<std::size_t>(pi)] = 1;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    row[y * W + x] = u8(select_partition(pi, x, y, np, TN < 31));
        }
        return row;
    }
};

// ---------------------------------------------------------------------------
// Block target and the working state of one candidate.
// ---------------------------------------------------------------------------

// What a block is scored against: the OKLab targets of the main search,
// and the source colours (the start values of the solvers use them).
struct Target {
    OKLab lab[kMaxTexels];
    u8 rgb[kMaxTexels][3];
};

// A symbolic block with its decoded endpoints, weight ranks and per-texel
// infill sums.
struct Work {
    Sym s;
    const GridMap* gm = nullptr;
    const u8* part = nullptr;
    int e0[4][3]{}, e1[4][3]{};
    u8 wr[2][64]{};            // weight ranks per plane
    int wsum[2][kMaxTexels]{};  // infill sums (x16) per plane
};

inline int infill(int wsum) { return (wsum + 8) >> 4; }
inline int ep_base(const Sym& s, int p) {
    int o = 0;
    for (int q = 0; q < p; ++q) o += cem_values(s.cem[q]);
    return o;
}

void decode_partition_endpoints(Work& w, int p) {
    const Tables& t = tables();
    const int o = ep_base(w.s, p);
    int v[8];
    for (int i = 0; i < cem_values(w.s.cem[p]); ++i) v[i] = t.c_unq[w.s.qi][w.s.ev[o + i]];
    decode_endpoints(w.s.cem[p], v, w.e0[p], w.e1[p]);
}

void recompute_wsum(Work& w) {
    const Tables& t = tables();
    const GridMap& gm = *w.gm;
    const int planes = w.s.dual ? 2 : 1;
    for (int pl = 0; pl < planes; ++pl)
        for (int j = 0; j < kMaxTexels; ++j) w.wsum[pl][j] = 0;
    for (int pl = 0; pl < planes; ++pl)
        for (int g = 0; g < gm.gn; ++g) {
            const int val = t.w_val[w.s.wqi][w.wr[pl][g]];
            for (auto i = gm.goff[static_cast<std::size_t>(g)];
                 i < gm.goff[static_cast<std::size_t>(g) + 1]; ++i)
                w.wsum[pl][gm.gtex[i]] += int(gm.gcoef[i]) * val;
        }
}

void load_work(Work& w, const Sym& s, Ctx& ctx) {
    const Tables& t = tables();
    w.s = s;
    w.gm = &ctx.map(s.gw, s.gh);
    w.part = ctx.partition(s.np, s.pi);
    for (int p = 0; p < s.np; ++p) decode_partition_endpoints(w, p);
    const int planes = s.dual ? 2 : 1;
    for (int g = 0; g < w.gm->gn; ++g)
        for (int pl = 0; pl < planes; ++pl)
            w.wr[pl][g] = t.w_rank[s.wqi][s.w[g * planes + pl]];
    recompute_wsum(w);
}

void store_weights(Work& w) {
    const Tables& t = tables();
    const int planes = w.s.dual ? 2 : 1;
    for (int g = 0; g < w.gm->gn; ++g)
        for (int pl = 0; pl < planes; ++pl)
            w.s.w[g * planes + pl] = t.w_stored[w.s.wqi][w.wr[pl][g]];
}

inline void texel_rgb(const Work& w, int j, int ws0, int ws1, u8 rgb[3]) {
    const int p = w.part[j];
    for (int ch = 0; ch < 3; ++ch) {
        const int wi = infill((w.s.dual && ch == w.s.plane2) ? ws1 : ws0);
        rgb[ch] = u8(((64 - wi) * w.e0[p][ch] + wi * w.e1[p][ch] + 32) >> 6);
    }
}

inline float lab_dist(const OKLab& a, const OKLab& b) {
    const float dL = a.L - b.L, da = a.a - b.a, db = a.b - b.b;
    return dL * dL + da * da + db * db;
}

void paint(const Work& w, int tn, u8 out[][3]) {
    for (int j = 0; j < tn; ++j) texel_rgb(w, j, w.wsum[0][j], w.wsum[1][j], out[j]);
}

// ---------------------------------------------------------------------------
// Solvers.
// ---------------------------------------------------------------------------

// Coordinate descent on the weight ranks. col(j, ws0, ws1) is the decoded
// OKLab colour of texel j for the given infill sums. A grid point scans
// every level when the grid is 1:1 or the quant is coarse, and +-2 ranks
// otherwise.
template <typename ColFn>
void descend_weights(Work& w, const Target& t, int tn, ColFn col) {
    const Tables& tb = tables();
    const GridMap& gm = *w.gm;
    const int wl = kQuant[w.s.wqi].levels;
    const u8* val = tb.w_val[w.s.wqi];
    const int planes = w.s.dual ? 2 : 1;
    const bool full = gm.identity || wl <= 8;
    float terr[kMaxTexels];
    auto err = [&](int j, int ws0, int ws1) { return lab_dist(t.lab[j], col(j, ws0, ws1)); };
    for (int j = 0; j < tn; ++j) terr[j] = err(j, w.wsum[0][j], w.wsum[1][j]);
    for (int sweep = 0; sweep < 8; ++sweep) {
        bool improved = false;
        for (int pl = 0; pl < planes; ++pl) {
            for (int g = 0; g < gm.gn; ++g) {
                const int cur = w.wr[pl][g];
                const auto lo = gm.goff[static_cast<std::size_t>(g)];
                const auto hi = gm.goff[static_cast<std::size_t>(g) + 1];
                const int r0 = full ? 0 : std::max(cur - 2, 0);
                const int r1 = full ? wl - 1 : std::min(cur + 2, wl - 1);
                float best_d = -1e-9f;
                int best_r = -1;
                for (int r = r0; r <= r1; ++r) {
                    if (r == cur) continue;
                    const int dr = int(val[r]) - int(val[cur]);
                    float delta = 0.f;
                    for (auto i = lo; i < hi; ++i) {
                        const int j = gm.gtex[i];
                        const int d = int(gm.gcoef[i]) * dr;
                        delta += err(j, w.wsum[0][j] + (pl == 0 ? d : 0),
                                     w.wsum[1][j] + (pl == 1 ? d : 0)) - terr[j];
                    }
                    if (delta < best_d) {
                        best_d = delta;
                        best_r = r;
                    }
                }
                if (best_r < 0) continue;
                const int dr = int(val[best_r]) - int(val[cur]);
                for (auto i = lo; i < hi; ++i) {
                    const int j = gm.gtex[i];
                    w.wsum[pl][j] += int(gm.gcoef[i]) * dr;
                    terr[j] = err(j, w.wsum[0][j], w.wsum[1][j]);
                }
                w.wr[pl][g] = u8(best_r);
                improved = true;
            }
        }
        if (!improved || (gm.identity && planes == 1)) break;
    }
}

// Best weights for the endpoints in `w`.
void solve_weights(Work& w, const Target& t, int tn) {
    const Tables& tb = tables();
    const GridMap& gm = *w.gm;
    const Sym& s = w.s;
    const u8* near = tb.w_near[s.wqi];
    float ideal[2][kMaxTexels];
    if (!s.dual) {
        // One 65-entry OKLab palette per partition.
        OKLab pal[4][65];
        for (int p = 0; p < s.np; ++p)
            for (int k = 0; k <= 64; ++k) {
                u8 c[3];
                for (int ch = 0; ch < 3; ++ch)
                    c[ch] = u8(((64 - k) * w.e0[p][ch] + k * w.e1[p][ch] + 32) >> 6);
                pal[p][k] = color_space::srgb8_to_oklab(c[0], c[1], c[2]);
            }
        if (!gm.identity) {
            for (int j = 0; j < tn; ++j) {
                const OKLab* pp = pal[w.part[j]];
                float be = kInf;
                int bk = 0;
                for (int k = 0; k <= 64; k += 2) {
                    const float e = lab_dist(t.lab[j], pp[k]);
                    if (e < be) {
                        be = e;
                        bk = k;
                    }
                }
                ideal[0][j] = float(bk);
            }
            for (int g = 0; g < gm.gn; ++g) {
                float acc = 0.f;
                for (auto i = gm.goff[static_cast<std::size_t>(g)];
                     i < gm.goff[static_cast<std::size_t>(g) + 1]; ++i)
                    acc += float(gm.gcoef[i]) * ideal[0][gm.gtex[i]];
                w.wr[0][g] = near[std::clamp(
                    int(acc * gm.gnorm[static_cast<std::size_t>(g)] + 0.5f), 0, 64)];
            }
            recompute_wsum(w);
        }
        descend_weights(w, t, tn,
                        [&](int j, int ws0, int) { return pal[w.part[j]][infill(ws0)]; });
        return;
    }
    // Dual plane: project the source colour on each plane's axis.
    for (int j = 0; j < tn; ++j) {
        const int p = w.part[j];
        float dot = 0.f, len = 0.f;
        ideal[1][j] = 0.f;
        for (int ch = 0; ch < 3; ++ch) {
            const auto d = float(w.e1[p][ch] - w.e0[p][ch]);
            const float x = float(t.rgb[j][ch]) - float(w.e0[p][ch]);
            if (ch == s.plane2) {
                ideal[1][j] = d != 0.f ? std::clamp(x / d, 0.f, 1.f) * 64.f : 0.f;
            } else {
                dot += x * d;
                len += d * d;
            }
        }
        ideal[0][j] = len > 0.f ? std::clamp(dot / len, 0.f, 1.f) * 64.f : 0.f;
    }
    for (int pl = 0; pl < 2; ++pl)
        for (int g = 0; g < gm.gn; ++g) {
            float acc = 0.f;
            for (auto i = gm.goff[static_cast<std::size_t>(g)];
                 i < gm.goff[static_cast<std::size_t>(g) + 1]; ++i)
                acc += float(gm.gcoef[i]) * ideal[pl][gm.gtex[i]];
            w.wr[pl][g] =
                near[std::clamp(int(acc * gm.gnorm[static_cast<std::size_t>(g)] + 0.5f), 0, 64)];
        }
    recompute_wsum(w);
    descend_weights(w, t, tn, [&](int j, int ws0, int ws1) {
        u8 rgb[3];
        texel_rgb(w, j, ws0, ws1, rgb);
        return color_space::srgb8_to_oklab(rgb[0], rgb[1], rgb[2]);
    });
}

// Error of the texels of partition p. Stops once the sum reaches
// `limit`: the caller only asks whether the error is below it.
float partition_err(const Work& w, const Target& t, int tn, int p, float limit) {
    float e = 0.f;
    for (int j = 0; j < tn; ++j) {
        if (w.part[j] != p) continue;
        u8 rgb[3];
        texel_rgb(w, j, w.wsum[0][j], w.wsum[1][j], rgb);
        e += lab_dist(t.lab[j], color_space::srgb8_to_oklab(rgb[0], rgb[1], rgb[2]));
        if (e >= limit) break;
    }
    return e;
}

// Least-squares start for the endpoints of partition p under the
// current weights (CEM 0, 6 and 8). False = no start for this CEM.
bool lsq_start(const Work& w, const Target& t, int tn, int p, u8 out[8]) {
    const Tables& tb = tables();
    const Sym& s = w.s;
    const int cem = s.cem[p];
    if (cem != 0 && cem != 6 && cem != 8) return false;
    float A[3], B[3];
    for (int ch = 0; ch < 3; ++ch) {
        const int pl = (s.dual && ch == s.plane2) ? 1 : 0;
        float a00 = 0.f, a01 = 0.f, a11 = 0.f, b0 = 0.f, b1 = 0.f, sum = 0.f;
        int n = 0;
        for (int j = 0; j < tn; ++j) {
            if (w.part[j] != p) continue;
            const float w1 = float(infill(w.wsum[pl][j])) * (1.f / 64.f);
            const float w0 = 1.f - w1;
            const auto x = float(t.rgb[j][ch]);
            a00 += w0 * w0;
            a01 += w0 * w1;
            a11 += w1 * w1;
            b0 += w0 * x;
            b1 += w1 * x;
            sum += x;
            ++n;
        }
        if (n == 0) return false;
        const float det = a00 * a11 - a01 * a01;
        if (std::abs(det) < 1e-4f) {
            A[ch] = B[ch] = sum / float(n);
        } else {
            A[ch] = std::clamp((a11 * b0 - a01 * b1) / det, 0.f, 255.f);
            B[ch] = std::clamp((a00 * b1 - a01 * b0) / det, 0.f, 255.f);
        }
    }
    const u8* near = tb.c_near[s.qi];
    auto q = [&](float x) { return near[std::clamp(int(x + 0.5f), 0, 255)]; };
    if (cem == 0) {
        out[0] = q((A[0] + A[1] + A[2]) / 3.f);
        out[1] = q((B[0] + B[1] + B[2]) / 3.f);
    } else if (cem == 6) {
        float num = 0.f, den = 0.f;
        for (int ch = 0; ch < 3; ++ch) {
            out[ch] = q(B[ch]);
            num += A[ch] * B[ch];
            den += B[ch] * B[ch];
        }
        out[3] = q(den > 0.f ? std::clamp(256.f * num / den, 0.f, 255.f) : 0.f);
    } else {
        if (A[0] + A[1] + A[2] <= B[0] + B[1] + B[2]) {
            for (int ch = 0; ch < 3; ++ch) {
                out[2 * ch] = q(A[ch]);
                out[2 * ch + 1] = q(B[ch]);
            }
        } else {
            // The decoder swaps the pair and applies blue contraction.
            for (int ch = 0; ch < 3; ++ch) {
                const float a = ch == 2 ? A[2] : 2.f * A[ch] - A[2];
                const float b = ch == 2 ? B[2] : 2.f * B[ch] - B[2];
                if (a < 0.f || a > 255.f || b < 0.f || b > 255.f) return false;
                out[2 * ch] = q(b);
                out[2 * ch + 1] = q(a);
            }
        }
    }
    return true;
}

// Best endpoints for the weights in `w`: per partition, the better of
// the current values and a least-squares start, then a coordinate
// descent on the quantised values against the exact decode.
void solve_endpoints(Work& w, const Target& t, int tn) {
    const Tables& tb = tables();
    Sym& s = w.s;
    const int levels = kQuant[s.qi].levels;
    for (int p = 0; p < s.np; ++p) {
        const int o = ep_base(s, p);
        const int nv = cem_values(s.cem[p]);
        float best = partition_err(w, t, tn, p, kInf);
        u8 start[8];
        if (lsq_start(w, t, tn, p, start)) {
            u8 keep[8];
            std::memcpy(keep, &s.ev[o], static_cast<std::size_t>(nv));
            std::memcpy(&s.ev[o], start, static_cast<std::size_t>(nv));
            decode_partition_endpoints(w, p);
            const float e = partition_err(w, t, tn, p, best);
            if (e < best) {
                best = e;
            } else {
                std::memcpy(&s.ev[o], keep, static_cast<std::size_t>(nv));
                decode_partition_endpoints(w, p);
            }
        }
        for (int sweep = 0; sweep < 4; ++sweep) {
            bool improved = false;
            for (int i = 0; i < nv; ++i) {
                const u8 cur = s.ev[o + i];
                const int r = tb.c_rank[s.qi][cur];
                u8 best_v = cur;
                for (int d = -2; d <= 2; ++d) {
                    if (d == 0 || r + d < 0 || r + d >= levels) continue;
                    s.ev[o + i] = tb.c_stored[s.qi][r + d];
                    decode_partition_endpoints(w, p);
                    const float e = partition_err(w, t, tn, p, best);
                    if (e < best) {
                        best = e;
                        best_v = s.ev[o + i];
                    }
                }
                s.ev[o + i] = best_v;
                if (best_v != cur) improved = true;
            }
            if (!improved) break;
        }
        decode_partition_endpoints(w, p);
    }
}

// ---------------------------------------------------------------------------
// LZ77 rate model of the output stream.
// ---------------------------------------------------------------------------

struct Parse {
    float bits = 0.f;
    std::uint32_t rep[3] = {};   // repeat offsets after the block
    bool open = false;           // the block ends inside a match
    std::uint16_t lit_mask = 0;  // bytes coded as literals
};

// Costs in bits, after the zstd sequence layout: every match is one
// sequence (literal-length, match-length and offset code); a new offset
// adds log2(offset) raw bits, a repeat offset none.
constexpr float kSeqBits = 5.5f;     // literal-length + match-length codes
constexpr float kOffsetBits = 2.f;   // offset code of a new offset
constexpr float kRepBits[3] = {1.f, 2.f, 2.5f};
constexpr float kExtendBits = 0.3f;  // a match that runs on from the previous block
constexpr int kChainDepth = 32;      // hash chain positions tried per start

class Lz {
public:
    void init(std::size_t total_bytes, const u8* prior, std::size_t prior_n) {
        buf_.assign(total_bytes + 64, 0);
        head_.assign(std::size_t(1) << kHashBits, -1);
        prev_.assign(total_bytes + 64, -1);
        n_ = 0;
        rep_[0] = 1;
        rep_[1] = 4;
        rep_[2] = 8;
        open_ = false;
        // Literal statistics start from the bytes of the unoptimised stream.
        for (auto& v : cnt_) v = 1;
        tot_ = 256;
        std::uint32_t hist[256] = {};
        for (std::size_t i = 0; i < prior_n; ++i) ++hist[prior[i]];
        if (prior_n > 0)
            for (int b = 0; b < 256; ++b) {
                const auto add = std::uint32_t(std::uint64_t(hist[b]) * 4096u / prior_n);
                cnt_[b] += add;
                tot_ += add;
            }
        refresh();
    }

    // Cost in bits of the 16 bytes `blk` appended to the stream.
    float eval(const u8* blk, Parse* out) {
        std::memcpy(&buf_[n_], blk, 16);
        return eval_at(n_, rep_, open_, out);
    }

    // Cost of the block `next` when it follows `blk` (parsed as `bp`).
    float eval_next(const u8* blk, const Parse& bp, const u8* next) {
        std::memcpy(&buf_[n_], blk, 16);
        std::memcpy(&buf_[n_ + 16], next, 16);
        return eval_at(n_ + 16, bp.rep, bp.open, nullptr);
    }

    void commit(const u8* blk, const Parse& p) {
        std::memcpy(&buf_[n_], blk, 16);
        const std::size_t old = n_;
        n_ += 16;
        for (std::size_t i = old >= 2 ? old - 2 : 0; i + 3 <= n_; ++i) {
            const std::uint32_t h = hash3(&buf_[i]);
            prev_[i] = head_[h];
            head_[h] = int(i);
        }
        std::memcpy(rep_, p.rep, sizeof(rep_));
        open_ = p.open;
        for (int i = 0; i < 16; ++i)
            if ((p.lit_mask >> i) & 1) {
                ++cnt_[blk[i]];
                ++tot_;
            }
        if (((n_ >> 4) & 127) == 0) refresh();
    }

private:
    float eval_at(std::size_t pos0, const std::uint32_t* rep, bool open, Parse* out) {
        struct Node {
            float cost = kInf;
            std::uint32_t rep[3] = {};
            std::uint32_t off = 0;  // 0 = reached by a literal
            u8 from = 0;
        };
        Node dp[17];
        dp[0].cost = 0.f;
        std::memcpy(dp[0].rep, rep, sizeof(dp[0].rep));
        const u8* cur = &buf_[pos0];
        for (int i = 0; i < 16; ++i) {
            const Node& a = dp[i];
            // Literal.
            {
                const float cost = a.cost + lit_[cur[i]];
                if (cost < dp[i + 1].cost) {
                    dp[i + 1] = a;
                    dp[i + 1].cost = cost;
                    dp[i + 1].off = 0;
                    dp[i + 1].from = u8(i);
                }
            }
            const int room = 16 - i;
            if (room < 3) continue;
            const std::size_t at = pos0 + std::size_t(i);
            auto try_src = [&](std::size_t src) {
                const u8* sp = &buf_[src];
                if (sp[0] != cur[i] || sp[1] != cur[i + 1] || sp[2] != cur[i + 2]) return;
                int len = 3;
                while (len < room && sp[len] == cur[i + len]) ++len;
                const auto off = std::uint32_t(at - src);
                float mc;
                if (i == 0 && open && off == a.rep[0]) mc = kExtendBits;
                else if (off == a.rep[0]) mc = kSeqBits + kRepBits[0];
                else if (off == a.rep[1]) mc = kSeqBits + kRepBits[1];
                else if (off == a.rep[2]) mc = kSeqBits + kRepBits[2];
                else mc = kSeqBits + kOffsetBits + float(std::bit_width(off) - 1);
                const float cost = a.cost + mc;
                for (int L = len; L >= 3; --L) {
                    Node& d = dp[i + L];
                    if (cost >= d.cost) continue;
                    d.cost = cost;
                    d.off = off;
                    d.from = u8(i);
                    if (off == a.rep[0]) {
                        std::memcpy(d.rep, a.rep, sizeof(d.rep));
                    } else if (off == a.rep[1]) {
                        d.rep[0] = a.rep[1];
                        d.rep[1] = a.rep[0];
                        d.rep[2] = a.rep[2];
                    } else {
                        d.rep[0] = off;
                        d.rep[1] = a.rep[0];
                        d.rep[2] = a.rep[1];
                    }
                }
            };
            // Repeat offsets.
            for (const std::uint32_t r : a.rep)
                if (r <= at) try_src(at - r);
            // Sources too recent for the hash chains.
            for (std::size_t src = n_ >= 2 ? n_ - 2 : 0; src < at; ++src) try_src(src);
            // Hash chains over the committed stream.
            int p = head_[hash3(cur + i)];
            for (int d = 0; d < kChainDepth && p >= 0; ++d) {
                try_src(std::size_t(p));
                p = prev_[std::size_t(p)];
            }
        }
        if (out != nullptr) {
            out->bits = dp[16].cost;
            std::memcpy(out->rep, dp[16].rep, sizeof(out->rep));
            out->open = dp[16].off != 0;
            out->lit_mask = 0;
            for (int at = 16; at > 0; at = dp[at].from)
                if (dp[at].off == 0) out->lit_mask = std::uint16_t(out->lit_mask | (1u << (at - 1)));
        }
        return dp[16].cost;
    }

    static constexpr int kHashBits = 17;
    static std::uint32_t hash3(const u8* p) {
        const std::uint32_t v = std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
                                (std::uint32_t(p[2]) << 16);
        return (v * 2654435761u) >> (32 - kHashBits);
    }
    void refresh() {
        for (int b = 0; b < 256; ++b)
            lit_[b] = std::clamp(std::log2(float(tot_) / float(cnt_[b])), 1.f, 11.f);
    }

    std::vector<u8> buf_;
    std::vector<int> head_, prev_;
    std::size_t n_ = 0;
    std::uint32_t rep_[3] = {1, 4, 8};
    bool open_ = false;
    std::uint32_t cnt_[256] = {};
    std::uint32_t tot_ = 0;
    float lit_[256] = {};
};

// ---------------------------------------------------------------------------
// The pass.
// ---------------------------------------------------------------------------

// Donors of a block: the blocks before it in the stream, the rows above,
// and in the second pass the blocks after it as the first pass left them.
constexpr int kDonorsBefore = 8;
constexpr int kDonorsUp = 2;    // row above, +-2 columns
constexpr int kDonorsUp2 = 1;   // two rows above, +-1 column
constexpr int kDonorsAfter = 2;
constexpr int kDonorsDown = 1;  // row below, +-1 column
constexpr int kPasses = 2;

// Distortion. SSIMULACRA2 compares the images at several scales, so an
// error that survives averaging costs more than its share of the squared
// error. A candidate is scored with the texel error plus the error of
// its quadrant means and of its block mean, each as a sum over texels.
constexpr float kQuadWeight = 1.f;
constexpr float kMeanWeight = 1.f;
// A block must not pay for its bits with a large rise of its own error:
// the rise over the encoder's block is charged again, in proportion to
// rise / (base score + floor). The floor is kRiseFloor mean base scores.
constexpr float kRiseWeight = 3.f;
constexpr float kRiseFloor = 0.25f;

// Strength scale. lambda (score units per bit) grows with the square of
// the strength: the size falls roughly with the logarithm of lambda, so
// a linear scale would spend most of 1..100 on the far end of the curve.
// lambda also follows the square root of the image's mean base score
// (plus a floor of kScoreFloor per texel). With a fixed lambda a 4x4
// image, with its small block errors, loses far more than an 8x8 image;
// with a lambda in proportion to the mean score it loses far less. The
// square root puts 4x4, 6x6 and 8x8 near the same size change and the
// same SSIMULACRA2 change for a given strength (Kodak, 4 images).
// kLambdaMax is lambda / sqrt(mean score) at strength 100.
constexpr float kLambdaMax = 0.028f;
constexpr float kScoreFloor = 1e-7f;

// Score of the block in `w`; d receives the plain block error.
float score_block(const Work& w, const Target& t, int W, int H, float& d) {
    float e = 0.f;
    float m[4][3] = {};
    int n[4] = {};
    for (int j = 0; j < W * H; ++j) {
        u8 rgb[3];
        texel_rgb(w, j, w.wsum[0][j], w.wsum[1][j], rgb);
        const OKLab c = color_space::srgb8_to_oklab(rgb[0], rgb[1], rgb[2]);
        const float dv[3] = {t.lab[j].L - c.L, t.lab[j].a - c.a, t.lab[j].b - c.b};
        e += dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2];
        const int q = ((j / W) * 2 / H) * 2 + ((j % W) * 2 / W);
        for (int k = 0; k < 3; ++k) m[q][k] += dv[k];
        ++n[q];
    }
    float mean = 0.f, quad = 0.f;
    for (int k = 0; k < 3; ++k) {
        const float s = m[0][k] + m[1][k] + m[2][k] + m[3][k];
        mean += s * s / float(W * H);
        for (int q = 0; q < 4; ++q) quad += m[q][k] * m[q][k] / float(n[q]);
    }
    d = e;
    return e + kQuadWeight * quad + kMeanWeight * mean;
}

struct Cand {
    Block blk{};
    float d = 0.f;      // block error
    float score = 0.f;  // block error + low-frequency terms
};

void run(EncodeResult& res, const Side& side, std::span<const std::uint8_t> rgba, int image_w,
         int image_h, int W, int H, float strength) {
    const int TN = W * H;
    const int cols = res.block_cols, rows = res.block_rows;
    const auto nblocks = res.blocks.size();
    const auto pw = static_cast<std::size_t>(side.pad_w);
    Ctx ctx;
    ctx.W = W;
    ctx.H = H;
    ctx.TN = TN;

    // Target and source texels of a block (replicated at the image edge).
    // False if a source texel is not opaque.
    auto load_target = [&](int bx, int by, Target& tgt) {
        bool opaque = true;
        for (int dy = 0; dy < H; ++dy)
            for (int dx = 0; dx < W; ++dx) {
                const int j = dy * W + dx;
                tgt.lab[j] = side.target[static_cast<std::size_t>(by * H + dy) * pw +
                                         static_cast<std::size_t>(bx * W + dx)];
                const int sx = std::min(bx * W + dx, image_w - 1);
                const int sy = std::min(by * H + dy, image_h - 1);
                const std::uint8_t* px =
                    &rgba[(static_cast<std::size_t>(sy) * static_cast<std::size_t>(image_w) +
                           static_cast<std::size_t>(sx)) * 4u];
                tgt.rgb[j][0] = px[0];
                tgt.rgb[j][1] = px[1];
                tgt.rgb[j][2] = px[2];
                if (px[3] != 255) opaque = false;
            }
        return opaque;
    };

    // The stream is optimised in place: out[d] holds the block of the
    // current pass for d < b and of the previous pass (first pass: the
    // encoder's block) for d >= b. A block the pass cannot rebuild (alpha,
    // HDR, void extent) or whose source has alpha stays as it is.
    std::vector<Block> out(res.blocks.begin(), res.blocks.end());
    std::vector<Sym> syms(nblocks);
    std::vector<float> base_d(nblocks, 0.f), base_score(nblocks, 0.f), cur_d(nblocks, 0.f);
    double score_sum = 0.0;
    std::size_t score_n = 0;
    for (int by = 0; by < rows; ++by)
        for (int bx = 0; bx < cols; ++bx) {
            const auto bi = static_cast<std::size_t>(by * cols + bx);
            Target tgt;
            const bool opaque = load_target(bx, by, tgt);
            const Sym s = decode_sym(out[bi], W, H);
            if (!s.ok || !opaque) continue;
            syms[bi] = s;
            Work w;
            load_work(w, s, ctx);
            base_score[bi] = score_block(w, tgt, W, H, base_d[bi]);
            cur_d[bi] = base_d[bi];
            score_sum += double(base_score[bi]);
            ++score_n;
        }
    if (score_n == 0) return;
    const auto mean_score = float(score_sum / double(score_n));
    const float rise_floor = kRiseFloor * mean_score;
    const float lambda = (strength / 100.f) * (strength / 100.f) * kLambdaMax *
                         std::sqrt(mean_score + kScoreFloor * float(TN));

    std::vector<Cand> cands;
    std::vector<int> donors;
    for (int pass = 0; pass < kPasses; ++pass) {
        Lz lz;
        lz.init(nblocks * 16, reinterpret_cast<const u8*>(out.data()), nblocks * 16);
        for (int by = 0; by < rows; ++by) {
            for (int bx = 0; bx < cols; ++bx) {
                const int b = by * cols + bx;
                const auto bi = static_cast<std::size_t>(b);
                const Block& base = res.blocks[bi];
                Parse parse;
                if (!syms[bi].ok) {
                    lz.eval(base.data(), &parse);
                    lz.commit(base.data(), parse);
                    continue;
                }
                Target tgt;
                load_target(bx, by, tgt);
                cands.clear();
                Work w;
                auto add = [&](const Block& blk, const Work& wk) {
                    for (const Cand& c : cands)
                        if (c.blk == blk) return;
                    Cand c;
                    c.blk = blk;
                    c.score = score_block(wk, tgt, W, H, c.d);
                    cands.push_back(c);
                };
                auto add_work = [&](Work& wk) {
                    store_weights(wk);
                    add(encode_sym(wk.s), wk);
                };
                // The encoder's block, then the block of the previous pass.
                cands.push_back({base, base_d[bi], base_score[bi]});
                if (out[bi] != base) {
                    load_work(w, syms[bi], ctx);
                    add(out[bi], w);
                }
                donors.clear();
                auto donor = [&](int d) {
                    if (d < 0 || d >= int(nblocks) || d == b || !syms[static_cast<std::size_t>(d)].ok)
                        return;
                    for (int e : donors)
                        if (out[static_cast<std::size_t>(e)] == out[static_cast<std::size_t>(d)])
                            return;
                    donors.push_back(d);
                };
                auto donor_row = [&](int row, int reach) {
                    if (row < 0 || row >= rows) return;
                    for (int x = std::max(bx - reach, 0); x <= std::min(bx + reach, cols - 1); ++x)
                        donor(row * cols + x);
                };
                for (int k = 1; k <= kDonorsBefore; ++k) donor(b - k);
                donor_row(by - 1, kDonorsUp);
                donor_row(by - 2, kDonorsUp2);
                if (pass > 0) {
                    for (int k = 1; k <= kDonorsAfter; ++k) donor(b + k);
                    donor_row(by + 1, kDonorsDown);
                }
                for (int d : donors) {
                    load_work(w, syms[static_cast<std::size_t>(d)], ctx);
                    // The donor block itself.
                    add(out[static_cast<std::size_t>(d)], w);
                    // Donor configuration and endpoints, new weights.
                    Work we = w;
                    solve_weights(we, tgt, TN);
                    add_work(we);
                    // Donor configuration alone.
                    solve_endpoints(we, tgt, TN);
                    solve_weights(we, tgt, TN);
                    add_work(we);
                    // Donor configuration and weights, new endpoints.
                    solve_endpoints(w, tgt, TN);
                    add_work(w);
                }
                // Choose. In the second pass the bits of the next block
                // (as the first pass left it) count as well: they depend
                // on what this block offers it to copy.
                const bool with_next = pass > 0 && bi + 1 < nblocks;
                float best_j = kInf;
                std::size_t best = 0;
                for (std::size_t k = 0; k < cands.size(); ++k) {
                    const Cand& c = cands[k];
                    float cost = c.score;
                    if (cost > base_score[bi]) {
                        const float rise = cost - base_score[bi];
                        cost += kRiseWeight * rise * rise / (base_score[bi] + rise_floor);
                    }
                    if (cost >= best_j) continue;
                    Parse pr;
                    float bits = lz.eval(c.blk.data(), &pr);
                    if (with_next) bits += lz.eval_next(c.blk.data(), pr, out[bi + 1].data());
                    const float j = cost + lambda * bits;
                    if (j < best_j) {
                        best_j = j;
                        best = k;
                        parse = pr;
                    }
                }
                const Cand& c = cands[best];
                lz.commit(c.blk.data(), parse);
                cur_d[bi] = c.d;
                if (c.blk != out[bi]) {
                    out[bi] = c.blk;
                    syms[bi] = decode_sym(c.blk, W, H);
                }
            }
        }
    }

    // Write back the blocks and the predicted decode of every changed one.
    const auto iw = static_cast<std::size_t>(image_w);
    double d_change = 0.0;
    for (int by = 0; by < rows; ++by)
        for (int bx = 0; bx < cols; ++bx) {
            const auto bi = static_cast<std::size_t>(by * cols + bx);
            if (out[bi] == res.blocks[bi]) continue;
            res.blocks[bi] = out[bi];
            d_change += double(cur_d[bi]) - double(base_d[bi]);
            Work w;
            load_work(w, syms[bi], ctx);
            u8 px[kMaxTexels][3];
            paint(w, TN, px);
            for (int dy = 0; dy < H && by * H + dy < image_h; ++dy)
                for (int dx = 0; dx < W && bx * W + dx < image_w; ++dx) {
                    const std::size_t o = (static_cast<std::size_t>(by * H + dy) * iw +
                                           static_cast<std::size_t>(bx * W + dx)) * 4u;
                    for (std::size_t ch = 0; ch < 3; ++ch)
                        res.predicted_rgba[o + ch] = px[dy * W + dx][ch];
                    res.predicted_rgba[o + 3] = 255;
                }
        }
    res.total_oklab2_error += float(d_change);
}

}  // namespace

void optimize(EncodeResult& res, const Side& side, std::span<const std::uint8_t> rgba_srgb8,
              int image_w, int image_h, int block_w, int block_h, int strength) {
    if (strength <= 0 || res.blocks.empty()) return;
    run(res, side, rgba_srgb8, image_w, image_h, block_w, block_h, float(strength));
}

}  // namespace png2amiga::astc::rdo
