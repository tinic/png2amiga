// Real bare-metal Amiga implementation for pa::Screen + pa::blitter.
// Mirrors the vscode-amiga-debug template's TakeSystem / FreeSystem dance:
// the engine takes over the machine on Screen::open, runs its own copper +
// BPL DMA, and hands the system back on Screen::close.
//
// Scope intentionally narrow:
//   - Lores single-playfield screen at 320xH, interleaved bitplanes in CHIP RAM
//   - Copper list: ddfstrt/ddfstop/diwstrt/diwstop, bplcon0/1/2, bpl1mod/2mod,
//     BPLxxPT, end
//   - Palette via direct COLOR00..COLORxx writes (no per-line copper changes)
//   - A→D plane_copy through the blitter, cookie_cut likewise with A/B/C/D
//   - mouse_left() via CIAA PRA bit 6
// The host build of the same file compiles as a plain memcpy/memset fallback
// so concept tests run on the dev machine.

#include "pa/blitter.hpp"
#include "pa/screen.hpp"
#include "pa/types.hpp"

#if defined(__mc68000__) || defined(__m68k__)
  #define PA_ON_AMIGA 1
#else
  #define PA_ON_AMIGA 0
#endif

#if PA_ON_AMIGA

extern "C" {
#include <exec/execbase.h>
#include <exec/memory.h>
#include <graphics/gfxbase.h>
#include <graphics/view.h>
#include <hardware/custom.h>
#include <hardware/dmabits.h>
#include <hardware/blit.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>

void* memcpy(void*, const void*, unsigned long);
void* memset(void*, int, unsigned long);
void memclr(void*, unsigned long);
}

// The SDK's inline library-call macros (OpenLibrary, AllocMem, ...) reference
// `SysBase`, `GfxBase`, `DOSBase` as global symbols by name, so they must be
// defined at namespace scope with exactly those identifiers.
extern "C" {
struct ExecBase*   SysBase = nullptr;
struct GfxBase*    GfxBase = nullptr;
struct DosLibrary* DOSBase = nullptr;
}

namespace {

volatile struct Custom* g_custom =
    reinterpret_cast<volatile struct Custom*>(0xDFF000);

// Saved hardware state (restored on Screen::close).
struct SavedSystem {
    UWORD adkcon{};
    UWORD intena{};
    UWORD dmacon{};
    struct View* active_view{};
    APTR  saved_irq{};
    APTR  vbr{};
};

// Engine-allocated resources for one open Screen.
//
// bpl_hi_slot / bpl_lo_slot point into the copper list at the *value* word
// of each BPLxxPT hi/lo MOVE instruction (i.e. the second of the two words
// in each copper entry). flip() patches these in place.
//
// Double-buffered: `bitmap[0]` and `bitmap[1]` are two equal-sized CHIP RAM
// allocations. `front_idx` is which buffer the display reads; the other is
// the draw target returned by Screen::back(). flip() waits for VBL, swaps,
// and applies any pending scroll in a single atomic step.
struct ScreenImpl {
    pa::u8*    bitmap[2]{};
    pa::u32    bitmap_bytes{};
    pa::u16    bitmap_bpr{};
    pa::u8     front_idx{0};
    UWORD*     copper{};
    pa::u32    copper_bytes{};
    SavedSystem saved{};
    bool       system_taken{};

    UWORD*     bpl_hi_slot[8]{};
    UWORD*     bpl_lo_slot[8]{};
    UWORD*     bplcon1_slot{};
    pa::u8     depth{};

    pa::u32    pending_scroll_x{};
    pa::u32    pending_scroll_y{};

    // Hardware sprite 0: 1-pixel-wide opaque column pinned at screen x=0,
    // full display height, in COLOR17 (set to black in the copper). Masks
    // the residual left-edge BPLCON1-delay artifact. Sprites 1..7 point at
    // the 2-word terminator appended to this buffer (free "null sprite").
    UWORD*     sprite_data{};
    pa::u32    sprite_bytes{};
};

// ---------------------------------------------------------------------------
// Stable-interrupt helpers (copied from template main.c style).
// ---------------------------------------------------------------------------

__attribute__((interrupt)) void SupervisorGetVBR() {
    __asm__ volatile(".short 0x4e7a, 0x0801");   // movec.l vbr, d0
}

APTR GetVBR() {
    APTR vbr = nullptr;
    if (SysBase->AttnFlags & AFF_68010) {
        vbr = reinterpret_cast<APTR>(Supervisor(
            reinterpret_cast<ULONG (*)()>(SupervisorGetVBR)));
    }
    return vbr;
}

inline void WaitBlt() {
    UWORD tst = *(volatile UWORD*)&g_custom->dmaconr;
    (void)tst;
    while (*(volatile UWORD*)&g_custom->dmaconr & (1 << 14)) {}
}

inline void WaitVbl() {
    while (true) {
        volatile ULONG vpos = *(volatile ULONG*)0xDFF004;
        vpos &= 0x1ff00;
        if (vpos != (311 << 8)) break;
    }
    while (true) {
        volatile ULONG vpos = *(volatile ULONG*)0xDFF004;
        vpos &= 0x1ff00;
        if (vpos == (311 << 8)) break;
    }
}

void TakeSystem(SavedSystem& s) {
    Forbid();
    s.adkcon = g_custom->adkconr;
    s.intena = g_custom->intenar;
    s.dmacon = g_custom->dmaconr;
    s.active_view = GfxBase->ActiView;

    LoadView(nullptr);
    WaitTOF();
    WaitTOF();
    WaitVbl();
    WaitVbl();

    OwnBlitter();
    WaitBlit();
    Disable();

    g_custom->intena = 0x7fff;
    g_custom->intreq = 0x7fff;
    g_custom->dmacon = 0x7fff;

    for (int i = 0; i < 32; ++i) g_custom->color[i] = 0;

    WaitVbl();
    WaitVbl();

    s.vbr = GetVBR();
    s.saved_irq = *reinterpret_cast<volatile APTR*>(
        reinterpret_cast<UBYTE*>(s.vbr) + 0x6c);
}

void FreeSystem(const SavedSystem& s) {
    WaitVbl();
    WaitBlit();
    g_custom->intena = 0x7fff;
    g_custom->intreq = 0x7fff;
    g_custom->dmacon = 0x7fff;

    *reinterpret_cast<volatile APTR*>(
        reinterpret_cast<UBYTE*>(s.vbr) + 0x6c) = s.saved_irq;

    g_custom->cop1lc = reinterpret_cast<ULONG>(GfxBase->copinit);
    g_custom->cop2lc = reinterpret_cast<ULONG>(GfxBase->LOFlist);
    g_custom->copjmp1 = 0x7fff;

    g_custom->intena = s.intena | 0x8000;
    g_custom->dmacon = s.dmacon | 0x8000;
    g_custom->adkcon = s.adkcon | 0x8000;

    WaitBlit();
    DisownBlitter();
    Enable();

    LoadView(s.active_view);
    WaitTOF();
    WaitTOF();

    Permit();
}

// Copper-list emission helpers. Each writes a register MOVE (reg, value).
UWORD* cop_move(UWORD* p, UWORD reg_offset, UWORD value) {
    *p++ = reg_offset;
    *p++ = value;
    return p;
}

// Offsets into struct Custom. Using __builtin_offsetof keeps the SDK types
// the single source of truth; the compiler folds these to constants.
#define O(m) (UWORD)__builtin_offsetof(struct Custom, m)

// Build copper with runtime-patchable BPLxxPT / BPLCON1 slots. On return,
// impl's bpl_hi_slot/bpl_lo_slot/bplcon1_slot point at the VALUE words of
// each corresponding MOVE instruction, so set_view() can patch them in
// place without rebuilding the list.
UWORD* build_copper(UWORD* p, pa::u16 screen_w, pa::u16 bitmap_w,
                    pa::u16 height, pa::u8 depth,
                    pa::u8* bitmap, ScreenImpl* impl) {
    constexpr UWORD x0 = 129;
    constexpr UWORD y0 = 44;
    UWORD xstop = x0 + screen_w;
    UWORD ystop = y0 + height;
    // DDFSTRT = $30 = 48, one word earlier than the standard $38. Fetches
    // 21 words per scanline; the first feeds the BPLCON1 delay region
    // (screen -16..-1), so the canvas carries one slack word per plane per
    // row for that position. Tested stable on FS-UAE.
    UWORD fw = (x0 >> 1) - 16;

    // AmigaOS order: display window → fetch window → BPLCONs → BPLMODs →
    // BPLxxPT. BPLCON0 goes early (with BPU set, which enables bitplane
    // DMA). That's safe here because the copper runs entirely within VBL;
    // by the time the beam reaches DIWSTRT_Y=44 and DMA can fire, every
    // BPLxxPT is already latched.
    p = cop_move(p, O(diwstrt), (UWORD)(x0 + (y0 << 8)));
    p = cop_move(p, O(diwstop), (UWORD)((xstop - 256) + ((ystop - 256) << 8)));
    p = cop_move(p, O(ddfstrt), fw);
    // DDFSTOP = (x0>>1)-8 + (screen_w/16 - 1)*8 = 208 = $D0 for 320 lores.
    p = cop_move(p, O(ddfstop), (UWORD)(((x0 >> 1) - 8) + (((screen_w >> 4) - 1) << 3)));

    p = cop_move(p, O(bplcon0),
        (UWORD)((0u << 10) | (1u << 9) | ((UWORD)depth << 12)));
    p = cop_move(p, O(bplcon1), 0);
    impl->bplcon1_slot = p - 1;
    p = cop_move(p, O(bplcon2), (UWORD)0x0024);

    // 21-word fetch per plane per scanline (42 bytes). phys_bpr includes
    // one slack word (+2 bytes) for the off-screen fetch at screen -16..-1.
    // Modulo = phys_bpr*depth - 42.
    UWORD phys_bpr = (UWORD)(bitmap_w / 8) + 2u;
    UWORD modulo = (UWORD)(phys_bpr * depth - 42);
    p = cop_move(p, O(bpl1mod), modulo);
    p = cop_move(p, O(bpl2mod), modulo);

    for (pa::u8 i = 0; i < depth; ++i) {
        ULONG addr = reinterpret_cast<ULONG>(bitmap + (pa::u32)i * phys_bpr);
        UWORD reg_hi = (UWORD)(O(bplpt[0]) + (UWORD)i * (UWORD)sizeof(APTR));
        UWORD reg_lo = (UWORD)(reg_hi + 2);
        p = cop_move(p, reg_hi, (UWORD)(addr >> 16));
        impl->bpl_hi_slot[i] = p - 1;
        p = cop_move(p, reg_lo, (UWORD)(addr & 0xFFFF));
        impl->bpl_lo_slot[i] = p - 1;
    }

    // Sprites: sprite 0 is the 1-pixel left-edge mask; sprites 1..7 point at
    // the 2-word terminator tail of the sprite-0 buffer so they're disabled
    // without needing a separate allocation. COLOR17 is forced to black; the
    // palette loader uses color 0..(2^depth - 1) so COLOR17 is only touched
    // here when depth >= 5, and we always want it black regardless.
    // TEMP: sprite 0 disabled (all SPRxPT → null terminator) to verify
    // whether the latency=16 formula alone eliminates the left-edge flicker.
    ULONG sprN_addr = reinterpret_cast<ULONG>(impl->sprite_data) +
                      impl->sprite_bytes - 4u;
    for (int i = 0; i < 8; ++i) {
        ULONG a = sprN_addr;
        UWORD reg_hi = (UWORD)(O(sprpt[0]) + (UWORD)i * (UWORD)sizeof(APTR));
        UWORD reg_lo = (UWORD)(reg_hi + 2);
        p = cop_move(p, reg_hi, (UWORD)(a >> 16));
        p = cop_move(p, reg_lo, (UWORD)(a & 0xFFFF));
    }
    p = cop_move(p, O(color[17]), 0x0000);  // mask column is black

    *p++ = 0xFFFF;
    *p++ = 0xFFFE;
    return p;
}

#undef O

} // namespace

namespace pa {

Screen::~Screen() { close(); }

bool Screen::open(const ScreenOptions& opts) {
    if (opened_) return false;

    SysBase = *reinterpret_cast<struct ExecBase**>(4UL);
    g_custom  = reinterpret_cast<volatile struct Custom*>(0xDFF000);

    GfxBase = reinterpret_cast<struct GfxBase*>(
        OpenLibrary(reinterpret_cast<CONST_STRPTR>("graphics.library"), 0));
    if (!GfxBase) return false;
    DOSBase = reinterpret_cast<struct DosLibrary*>(
        OpenLibrary(reinterpret_cast<CONST_STRPTR>("dos.library"), 0));
    if (!DOSBase) { CloseLibrary(reinterpret_cast<struct Library*>(GfxBase)); return false; }

    // All stack locals up front so the single fail-path doesn't cross any
    // variable initializations (GCC rejects goto past initialized vars).
    u16 bmw = opts.bitmap_width  ? opts.bitmap_width  : opts.width;
    u16 bmh = opts.bitmap_height ? opts.bitmap_height : opts.height;
    if (bmw < opts.width)  bmw = opts.width;
    if (bmh < opts.height) bmh = opts.height;
    if (bmw & 15) bmw = static_cast<u16>((bmw + 15) & ~u16{15});

    auto* impl = reinterpret_cast<ScreenImpl*>(
        AllocMem(sizeof(ScreenImpl), MEMF_ANY | MEMF_CLEAR));
    if (!impl) {
        CloseLibrary(reinterpret_cast<struct Library*>(DOSBase));
        CloseLibrary(reinterpret_cast<struct Library*>(GfxBase));
        GfxBase = nullptr; DOSBase = nullptr;
        return false;
    }
    impl_ = impl;

    // Physical row stride includes one "slack word" (2 bytes) per plane per
    // row. Callers never see the slack: their BitMap.data points 2 bytes past
    // the allocated plane base, so word-x = 0 lands on the first visible pixel
    // word. The slack absorbs the extra word per scanline that DDFSTRT -= 8
    // prefetches, so the BPLCON1 delay region reflects real canvas content
    // instead of color 0 / previous-line tail.
    u16 phys_bpr = static_cast<u16>(bmw / 8) + u16{2};

    back_.width  = opts.width;
    back_.height = opts.height;
    back_.depth  = opts.depth;
    back_.bpr    = phys_bpr;
    back_.plane_bytes = static_cast<u32>(back_.bpr) * back_.height;

    bitmap_.width  = bmw;
    bitmap_.height = bmh;
    bitmap_.depth  = opts.depth;
    bitmap_.bpr    = phys_bpr;
    bitmap_.plane_bytes = static_cast<u32>(bitmap_.bpr) * bmh;
    impl->bitmap_bpr = phys_bpr;
    impl->depth      = opts.depth;
    impl->bitmap_bytes = bitmap_.plane_bytes * opts.depth;
    impl->bitmap[0] = reinterpret_cast<u8*>(
        AllocMem(impl->bitmap_bytes, MEMF_CHIP | MEMF_CLEAR));
    impl->bitmap[1] = reinterpret_cast<u8*>(
        AllocMem(impl->bitmap_bytes, MEMF_CHIP | MEMF_CLEAR));
    if (!impl->bitmap[0] || !impl->bitmap[1]) {
        if (impl->bitmap[0]) FreeMem(impl->bitmap[0], impl->bitmap_bytes);
        if (impl->bitmap[1]) FreeMem(impl->bitmap[1], impl->bitmap_bytes);
        FreeMem(impl, sizeof(ScreenImpl)); impl_ = nullptr;
        CloseLibrary(reinterpret_cast<struct Library*>(DOSBase));
        CloseLibrary(reinterpret_cast<struct Library*>(GfxBase));
        GfxBase = nullptr; DOSBase = nullptr;
        return false;
    }
    // Display `bitmap[0]` initially; caller draws into `bitmap[1]` (= back).
    // Caller-facing .data skips the leading slack word of each plane's row.
    impl->front_idx = 0;
    bitmap_.data = impl->bitmap[1] + 2;
    back_ = bitmap_;                  // back() is an alias for the draw target
    front_ = bitmap_;                 // front_ exposed for callers who want it
    front_.data = impl->bitmap[0] + 2;
    screen_width_ = opts.width;

    impl->copper_bytes = 512;
    impl->copper = reinterpret_cast<UWORD*>(
        AllocMem(impl->copper_bytes, MEMF_CHIP | MEMF_CLEAR));
    if (!impl->copper) {
        FreeMem(impl->bitmap[0], impl->bitmap_bytes);
        FreeMem(impl->bitmap[1], impl->bitmap_bytes);
        FreeMem(impl, sizeof(ScreenImpl)); impl_ = nullptr;
        CloseLibrary(reinterpret_cast<struct Library*>(DOSBase));
        CloseLibrary(reinterpret_cast<struct Library*>(GfxBase));
        GfxBase = nullptr; DOSBase = nullptr;
        return false;
    }

    // Sprite-0 left-edge mask: one solid 1-pixel-wide column in COLOR17, full
    // display height, plus a 2-word terminator shared by sprites 1..7.
    // Layout: [POS][CTL][planeA[0]][planeB[0]]..[planeA[H-1]][planeB[H-1]][0][0]
    constexpr UWORD kVStart = 44;
    // Sprites have one color-clock more pipeline latency than bitplanes, so
    // HSTART = DIWSTRT_X - 1 puts the sprite's leftmost pixel on screen x=0.
    constexpr UWORD kHStart = 128;
    u16 vstop = static_cast<u16>(kVStart + opts.height);
    impl->sprite_bytes = static_cast<u32>((2u + 2u * opts.height + 2u) * 2u);
    impl->sprite_data  = reinterpret_cast<UWORD*>(
        AllocMem(impl->sprite_bytes, MEMF_CHIP | MEMF_CLEAR));
    if (!impl->sprite_data) {
        FreeMem(impl->copper, impl->copper_bytes);
        FreeMem(impl->bitmap[0], impl->bitmap_bytes);
        FreeMem(impl->bitmap[1], impl->bitmap_bytes);
        FreeMem(impl, sizeof(ScreenImpl)); impl_ = nullptr;
        CloseLibrary(reinterpret_cast<struct Library*>(DOSBase));
        CloseLibrary(reinterpret_cast<struct Library*>(GfxBase));
        GfxBase = nullptr; DOSBase = nullptr;
        return false;
    }
    impl->sprite_data[0] = static_cast<UWORD>(
        ((kVStart & 0xFFu) << 8) | ((kHStart >> 1) & 0xFFu));
    impl->sprite_data[1] = static_cast<UWORD>(
        ((vstop & 0xFFu) << 8) |
        (((kVStart >> 8) & 1u) << 2) |
        (((vstop   >> 8) & 1u) << 1) |
        (kHStart & 1u));
    for (u16 i = 0; i < opts.height; ++i) {
        impl->sprite_data[2u + i * 2u + 0u] = 0x8000;  // planeA: leftmost 1 px opaque
        impl->sprite_data[2u + i * 2u + 1u] = 0x0000;  // planeB: zeros → color 01 = COLOR17
    }
    // Last two words stay 0 (MEMF_CLEAR) — acts as the sprite terminator
    // and as the "null sprite" address shared by SPR1..SPR7.

    TakeSystem(impl->saved);
    impl->system_taken = true;

    build_copper(impl->copper, opts.width, bmw, opts.height, opts.depth,
                 impl->bitmap[0], impl);
    (void)bmh;

    g_custom->cop1lc = reinterpret_cast<ULONG>(impl->copper);
    g_custom->dmacon = DMAF_BLITTER;
    g_custom->copjmp1 = 0x7FFF;
    g_custom->dmacon = DMAF_SETCLR | DMAF_MASTER | DMAF_RASTER |
                       DMAF_COPPER  | DMAF_BLITTER | DMAF_SPRITE;

    opened_ = true;
    return true;
}

void Screen::close() noexcept {
    if (!opened_) return;
    auto* impl = reinterpret_cast<ScreenImpl*>(impl_);
    if (impl) {
        if (impl->system_taken) FreeSystem(impl->saved);
        if (impl->bitmap[0]) FreeMem(impl->bitmap[0], impl->bitmap_bytes);
        if (impl->bitmap[1]) FreeMem(impl->bitmap[1], impl->bitmap_bytes);
        if (impl->copper) FreeMem(impl->copper, impl->copper_bytes);
        if (impl->sprite_data) FreeMem(impl->sprite_data, impl->sprite_bytes);
        FreeMem(impl, sizeof(ScreenImpl));
    }
    if (DOSBase) CloseLibrary(reinterpret_cast<struct Library*>(DOSBase));
    if (GfxBase) CloseLibrary(reinterpret_cast<struct Library*>(GfxBase));
    GfxBase = nullptr; DOSBase = nullptr;
    impl_ = nullptr;
    opened_ = false;
    front_ = BitMap{};
    back_  = BitMap{};
    bitmap_ = BitMap{};
}

void Screen::set_view(u32 scroll_x, u32 scroll_y) noexcept {
    if (!opened_) return;
    auto* impl = reinterpret_cast<ScreenImpl*>(impl_);
    if (!impl) return;
    // Double-buffered: copper is repointed on flip(), not here. Just latch
    // the intended viewport — the next flip() applies it atomically after
    // VBL on the freshly-drawn buffer.
    impl->pending_scroll_x = scroll_x;
    impl->pending_scroll_y = scroll_y;
}

void Screen::set_palette_ocs(std::span<const u16> entries) noexcept {
    // Direct-register writes into COLOR00..COLOR31. No DMA can re-stomp these
    // because the copper list doesn't include per-line color changes.
    auto n = entries.size();
    if (n > 32) n = 32;
    for (std::size_t i = 0; i < n; ++i) g_custom->color[i] = entries[i];
}

void Screen::set_palette_aga(std::span<const u32>) noexcept {
    // TODO: BPLCON3 bank/LOCT dance. OCS-only for the first slice.
}

void Screen::flip() noexcept {
    auto* impl = reinterpret_cast<ScreenImpl*>(impl_);
    if (!impl) { ++frames_; return; }

    // Patch FIRST, WaitVbl SECOND. Copper re-executes at top of VBL; if we
    // patch after WaitVbl returns we race the copper's BPLxxPT MOVEs and may
    // lose the update for that frame (manifests as a 1-frame Y stutter since
    // Y has no sub-pixel smoothing to mask it). Mid-frame patching is safe
    // because BPLxxPT is only latched by bitplane DMA at scanline start off
    // COPPC — the current frame is unaffected; next copper restart sees us.
    u8  new_front_idx = impl->front_idx ^ 1;
    u8* new_front     = impl->bitmap[new_front_idx];

    // DDFSTRT=$30, latency 18 with 1 slack word per row. At scroll_x=0,
    // BPLxxPT = physical word 0 (slack), delay = 0 (slack off-screen at
    // -16..-1, caller word 0 at 0..15). General formula:
    //     bplpt_pw = (scroll_x + 16) >> 4
    //     D        = bplpt_pw * 16 - 1 - scroll_x
    u32 bplpt_pw = (impl->pending_scroll_x + 16u) >> 4;
    u32 byte_x   = bplpt_pw * 2u;
    u32 row_off  = impl->pending_scroll_y * impl->bitmap_bpr * impl->depth;
    for (u8 i = 0; i < impl->depth; ++i) {
        ULONG addr = reinterpret_cast<ULONG>(
            new_front + row_off +
            static_cast<u32>(i) * impl->bitmap_bpr +
            byte_x);
        *impl->bpl_hi_slot[i] = static_cast<UWORD>(addr >> 16);
        *impl->bpl_lo_slot[i] = static_cast<UWORD>(addr & 0xFFFF);
    }
    u16 delay = static_cast<u16>(bplpt_pw * 16u - 1u - impl->pending_scroll_x);
    *impl->bplcon1_slot = static_cast<UWORD>((delay << 4) | delay);

    WaitVbl();   // sync to frame boundary; copper will pick up patched values

    // Complete the logical swap. Caller-facing .data skips the slack word.
    impl->front_idx = new_front_idx;
    back_.data   = impl->bitmap[impl->front_idx ^ 1] + 2;
    bitmap_.data = back_.data;
    front_.data  = impl->bitmap[impl->front_idx] + 2;

    ++frames_;
}

bool Screen::mouse_left() noexcept {
    return (*(volatile UBYTE*)0xBFE001 & 0x40) == 0;
}

bool Screen::mouse_right() noexcept {
    return (*(volatile UWORD*)0xDFF016 & (1 << 10)) == 0;
}

void Screen::set_bg_color(u16 ocs_rgb) noexcept {
    g_custom->color[0] = ocs_rgb;
}

} // namespace pa

// Blitter hot-path (plane_copy/cookie_cut/wait_idle) is defined inline in
// pa/blitter.hpp. Leaving out-of-line definitions here would collide.
#if 0
namespace pa::blitter {

void plane_copy(BitMap& dst,
                const u16* src, u16 src_bpr, u16 /*src_plane_words*/,
                u16 dst_word_x, u16 dst_y,
                u16 w_words, u16 h,
                u8 depth) noexcept
{
    if (!dst.data || !src) return;

    // Both src and dst are interleaved — that means adjacent blit-rows are
    // always exactly one plane-stride apart, whether crossing plane→plane
    // within a visual row or wrapping plane N→plane 0 on the next visual
    // row. So bltdmod = bpr - (w_words*2), NOT bpr*depth - (w_words*2).
    u8* dst_ptr = dst.data +
        (std::size_t)dst_y * dst.bpr * depth +
        (std::size_t)dst_word_x * 2;

    WaitBlt();
    g_custom->bltapt  = const_cast<u16*>(src);
    g_custom->bltdpt  = dst_ptr;
    g_custom->bltamod = (UWORD)(src_bpr - w_words * 2);
    g_custom->bltdmod = (UWORD)(dst.bpr - w_words * 2);
    g_custom->bltcon0 = (UWORD)(SRCA | DEST | 0xF0);   // D = A
    g_custom->bltcon1 = 0;
    g_custom->bltafwm = 0xFFFF;
    g_custom->bltalwm = 0xFFFF;
    g_custom->bltsize = (UWORD)((h * depth) << HSIZEBITS) | w_words;
}

void cookie_cut(BitMap& dst,
                const u16* src, const u16* mask,
                u16 src_bpr, u16 /*src_plane_words*/,
                u16 dst_pixel_x, u16 dst_y,
                u16 w_pixels, u16 h,
                u8 depth) noexcept
{
    if (!dst.data || !src || !mask) return;

    // D = (A AND B) OR (NOT A AND C)  — minterm 0xCA
    u16 w_words = (u16)((w_pixels + 15) / 16);
    u16 xshift = (u16)(dst_pixel_x & 15);
    u16 word_x = (u16)(dst_pixel_x / 16);
    u8* dst_ptr = dst.data +
        (std::size_t)dst_y * dst.bpr * depth +
        (std::size_t)word_x * 2;

    WaitBlt();
    g_custom->bltapt  = const_cast<u16*>(mask);
    g_custom->bltbpt  = const_cast<u16*>(src);
    g_custom->bltcpt  = dst_ptr;
    g_custom->bltdpt  = dst_ptr;
    g_custom->bltamod = (UWORD)(src_bpr - w_words * 2);  // mask is single-plane
    g_custom->bltbmod = (UWORD)(src_bpr - w_words * 2);  // source interleaved — see TODO
    g_custom->bltcmod = (UWORD)(dst.bpr * depth - w_words * 2);
    g_custom->bltdmod = (UWORD)(dst.bpr * depth - w_words * 2);
    g_custom->bltcon0 = (UWORD)(SRCA | SRCB | SRCC | DEST | 0xCA |
                                (xshift << ASHIFTSHIFT));
    g_custom->bltcon1 = (UWORD)(xshift << BSHIFTSHIFT);
    g_custom->bltafwm = 0xFFFF;
    g_custom->bltalwm = 0xFFFF;
    g_custom->bltsize = (UWORD)((h * depth) << HSIZEBITS) | w_words;
}

void wait_idle() noexcept { WaitBlt(); }

} // namespace pa::blitter
#endif   // out-of-line blitter — kept for reference only

#else   // !PA_ON_AMIGA — host fallback

#include <cstring>
#include <cstddef>

namespace pa {

Screen::~Screen() { close(); }

bool Screen::open(const ScreenOptions& opts) {
    if (opened_) return false;
    u16 bmw = opts.bitmap_width  ? opts.bitmap_width  : opts.width;
    u16 bmh = opts.bitmap_height ? opts.bitmap_height : opts.height;
    if (bmw < opts.width)  bmw = opts.width;
    if (bmh < opts.height) bmh = opts.height;
    back_.width  = opts.width;
    back_.height = opts.height;
    back_.depth  = opts.depth;
    back_.bpr    = static_cast<u16>(((opts.width + 15) / 16) * 2);
    back_.plane_bytes = static_cast<u32>(back_.bpr) * back_.height;
    back_.data   = nullptr;
    bitmap_ = back_;
    bitmap_.width  = bmw;
    bitmap_.height = bmh;
    bitmap_.bpr    = static_cast<u16>(bmw / 8);
    bitmap_.plane_bytes = static_cast<u32>(bitmap_.bpr) * bmh;
    screen_width_ = opts.width;
    front_ = back_;
    opened_ = true;
    return true;
}

void Screen::close() noexcept {
    if (!opened_) return;
    front_ = BitMap{}; back_ = BitMap{}; bitmap_ = BitMap{}; opened_ = false;
}

void Screen::set_palette_ocs(std::span<const u16>) noexcept {}
void Screen::set_palette_aga(std::span<const u32>) noexcept {}
void Screen::flip() noexcept { ++frames_; }
bool Screen::mouse_left()  noexcept { return false; }
bool Screen::mouse_right() noexcept { return false; }
void Screen::set_view(u32, u32) noexcept {}
void Screen::set_bg_color(u16) noexcept {}

} // namespace pa

// Host-side blitter also defined inline in pa/blitter.hpp.

#endif  // PA_ON_AMIGA
