#pragma once

// Blitter hot-path helpers. Defined inline in the header so the compiler can
// inline them at every callsite (LTO isn't reliably producing IR on the
// current m68k-amiga-elf-gcc 14 bundle — -flto silently emits normal objects).
// With inlining, the 308 tile blits per frame collapse to a tight
// register-write loop.

#include "screen.hpp"
#include "types.hpp"

#if defined(__mc68000__) || defined(__m68k__)
  #define PA_BLITTER_ON_AMIGA 1
#else
  #define PA_BLITTER_ON_AMIGA 0
#endif

#if PA_BLITTER_ON_AMIGA
extern "C" {
#include <hardware/blit.h>
#include <hardware/custom.h>
#include <hardware/dmabits.h>
}
#else
#include <cstring>
#endif

namespace pa::blitter {

#if PA_BLITTER_ON_AMIGA

[[gnu::always_inline]] inline void wait_idle() noexcept {
    auto* c = reinterpret_cast<volatile struct Custom*>(0xDFF000);
    UWORD tst = *(volatile UWORD*)&c->dmaconr;   // a1000 DMACONR read-twice guard
    (void)tst;
    while (*(volatile UWORD*)&c->dmaconr & (1 << 14)) {}
}

// Plane-copy A→D through the blitter. Interleaved source and destination.
// The body is the single BLTxxx register sequence; with inlining there's no
// call overhead or spill, and gcc can schedule the constant-width modulo
// computations against caller arithmetic.
[[gnu::always_inline]] inline
void plane_copy(BitMap& dst,
                const u16* src, u16 src_bpr, u16 /*src_plane_words*/,
                u16 dst_word_x, u16 dst_y,
                u16 w_words, u16 h,
                u8 depth) noexcept
{
    if (!dst.data || !src) return;
    auto* custom = reinterpret_cast<volatile struct Custom*>(0xDFF000);

    u8* dst_ptr = dst.data +
        static_cast<u32>(dst_y) * dst.bpr * depth +
        static_cast<u32>(dst_word_x) * 2u;

    wait_idle();
    custom->bltapt  = const_cast<u16*>(src);
    custom->bltdpt  = dst_ptr;
    custom->bltamod = static_cast<UWORD>(src_bpr - w_words * 2);
    custom->bltdmod = static_cast<UWORD>(dst.bpr - w_words * 2);
    custom->bltcon0 = static_cast<UWORD>(SRCA | DEST | 0xF0);   // D = A
    custom->bltcon1 = 0;
    custom->bltafwm = 0xFFFF;
    custom->bltalwm = 0xFFFF;
    custom->bltsize = static_cast<UWORD>((h * depth) << HSIZEBITS) | w_words;
}

[[gnu::always_inline]] inline
void cookie_cut(BitMap& dst,
                const u16* src, const u16* mask,
                u16 src_bpr, u16 /*src_plane_words*/,
                u16 dst_pixel_x, u16 dst_y,
                u16 w_pixels, u16 h,
                u8 depth) noexcept
{
    if (!dst.data || !src || !mask) return;
    auto* custom = reinterpret_cast<volatile struct Custom*>(0xDFF000);

    u16 w_words = static_cast<u16>((w_pixels + 15) / 16);
    u16 xshift  = static_cast<u16>(dst_pixel_x & 15);
    u16 word_x  = static_cast<u16>(dst_pixel_x / 16);
    u8* dst_ptr = dst.data +
        static_cast<u32>(dst_y) * dst.bpr * depth +
        static_cast<u32>(word_x) * 2u;

    wait_idle();
    custom->bltapt  = const_cast<u16*>(mask);
    custom->bltbpt  = const_cast<u16*>(src);
    custom->bltcpt  = dst_ptr;
    custom->bltdpt  = dst_ptr;
    custom->bltamod = static_cast<UWORD>(src_bpr - w_words * 2);
    custom->bltbmod = static_cast<UWORD>(src_bpr - w_words * 2);
    custom->bltcmod = static_cast<UWORD>(dst.bpr - w_words * 2);
    custom->bltdmod = static_cast<UWORD>(dst.bpr - w_words * 2);
    custom->bltcon0 = static_cast<UWORD>(SRCA | SRCB | SRCC | DEST | 0xCA |
                                        (xshift << ASHIFTSHIFT));
    custom->bltcon1 = static_cast<UWORD>(xshift << BSHIFTSHIFT);
    custom->bltafwm = 0xFFFF;
    custom->bltalwm = 0xFFFF;
    custom->bltsize = static_cast<UWORD>((h * depth) << HSIZEBITS) | w_words;
}

#else   // Host fallback — used by smoke tests only.

inline void wait_idle() noexcept {}

inline void plane_copy(BitMap& dst,
                       const u16* src, u16 src_bpr, u16 src_plane_words,
                       u16 dst_word_x, u16 dst_y,
                       u16 w_words, u16 h,
                       u8 depth) noexcept
{
    if (!dst.data || !src) return;
    for (unsigned p = 0; p < depth; ++p) {
        const u16* sp = src + static_cast<std::size_t>(p) * src_plane_words;
        for (unsigned y = 0; y < h; ++y) {
            u16* dp = reinterpret_cast<u16*>(
                dst.data +
                (static_cast<std::size_t>(dst_y + y) * dst.depth + p) * dst.bpr +
                static_cast<std::size_t>(dst_word_x) * 2u);
            const u16* sr = sp + static_cast<std::size_t>(y) * (src_bpr / 2);
            std::memcpy(dp, sr, static_cast<std::size_t>(w_words) * 2u);
        }
    }
}

inline void cookie_cut(BitMap& dst,
                       const u16* src, const u16* mask,
                       u16 src_bpr, u16 src_plane_words,
                       u16 dst_pixel_x, u16 dst_y,
                       u16 w_pixels, u16 h,
                       u8 depth) noexcept
{
    (void)mask;
    plane_copy(dst, src, src_bpr, src_plane_words,
               static_cast<u16>(dst_pixel_x / 16), dst_y,
               static_cast<u16>((w_pixels + 15) / 16), h, depth);
}

#endif

} // namespace pa::blitter
