#pragma once

// Engine-side Screen handle. On Amiga, this owns an Intuition/graphics.library
// Screen + BitMap backed by CHIP RAM, along with a custom copper list. On
// host builds, it's a plain heap buffer used for smoke-testing blit logic.
//
// Keep the public surface minimal — resource ownership, a lockable BitMap,
// a palette setter, and a "flip and vsync" step.

#include "chipset.hpp"
#include "types.hpp"

#include <cstddef>
#include <span>

namespace pa {

struct ScreenOptions {
    u16 width          = 320;
    u16 height         = 256;
    u8  depth          = 5;                 // bitplanes; clamped to chipset max
    bool double_buffer = true;
    bool interlace     = false;
    bool hires         = false;             // 640-wide on OCS

    // Canvas size in pixels. Must be >= display dimensions and bitmap_width
    // must be a multiple of 16. When larger than the display, the engine
    // allocates a virtual bitmap and scrolls through it via BPLxxPT +
    // BPLCON1 + plane-pointer advance. 0 = same as display.
    u16 bitmap_width   = 0;
    u16 bitmap_height  = 0;
    // Future: --sprite-pool-size, --pri-copper-slots.
};

// One contiguous bitplane block for a single (logical) frame. On Amiga this
// is the pointer you load into the BPLxxPT registers; the BitMap's row
// stride is `bpr`, and plane i lives at data + i * (depth ≤ stride per frame).
// On host it's just a heap allocation we can inspect in tests.
struct BitMap {
    u8* data{};
    u16 width{};
    u16 height{};
    u8  depth{};
    u16 bpr{};         // bytes per row per plane
    u32 plane_bytes{}; // bpr * height
};

class Screen {
public:
    Screen() = default;
    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;
    Screen(Screen&&) noexcept = default;
    Screen& operator=(Screen&&) noexcept = default;
    ~Screen();

    // Open a new display with the given options. Returns false on failure;
    // the Screen stays in its previously-valid state on failure.
    [[nodiscard]] bool open(const ScreenOptions& opts);

    // Close and release all resources. Safe to call on a never-opened Screen.
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return opened_; }

    // Active back-buffer BitMap — the one the engine draws to this frame.
    // Valid only between open() and close().
    [[nodiscard]] BitMap& back() noexcept { return back_; }
    [[nodiscard]] const BitMap& back() const noexcept { return back_; }

    // Install a palette. Entry format depends on `active_chipset`:
    //   OCS  — std::span<const u16> of 0x0RGB words.
    //   AGA  — std::span<const u32> of 0x00RRGGBB words (3-byte packed).
    void set_palette_ocs(std::span<const u16> entries) noexcept;
    void set_palette_aga(std::span<const u32> entries) noexcept;

    // Flip buffers and wait for the next vblank. On host, a no-op that
    // advances the frame counter.
    void flip() noexcept;

    [[nodiscard]] u32 frame_count() const noexcept { return frames_; }

    // Polled input. Amiga impl reads CIAA PRA bit 6 / PRA bit 10 in JOY1DAT
    // for the right-mouse equivalent. Host impl always returns false.
    [[nodiscard]] static bool mouse_left()  noexcept;
    [[nodiscard]] static bool mouse_right() noexcept;

    // Write the background color register (COLOR00) directly — used for the
    // "raster-bar" CPU/blitter-time diagnostic: set to a bright color at the
    // start of a frame's work, back to black at the end. The colored band
    // visible on the left of the screen shows exactly how far down the
    // raster beam got before the work finished. If the band fills more than
    // a full display, you're missing VBL. OCS format is 0x0RGB.
    void set_bg_color(u16 ocs_rgb) noexcept;

    // Scroll the displayed viewport over a canvas larger than the screen.
    // Patches the copper list's BPLxxPT + BPLCON1 so the next frame's DMA
    // sees the update. Both coordinates are in canvas pixels and must
    // satisfy scroll_x + screen_width <= bitmap_width,
    //          scroll_y + screen_height <= bitmap_height.
    //
    // X is fully sub-word smooth via BPLCON1. Y is scanline-granular on OCS
    // (no sub-scanline hardware scroll; handled via plane-pointer advance
    // by one row of the canvas stride).
    void set_view(u32 scroll_x, u32 scroll_y = 0) noexcept;

    [[nodiscard]] u16 bitmap_width()  const noexcept { return bitmap_.width; }
    [[nodiscard]] u16 bitmap_height() const noexcept { return bitmap_.height; }
    [[nodiscard]] BitMap& bitmap() noexcept { return bitmap_; }
    [[nodiscard]] const BitMap& bitmap() const noexcept { return bitmap_; }

    // Buffer accessors. back() is the buffer the caller is drawing into; it
    // becomes the front at next flip(). front() is currently being displayed.
    // Used for edge-blit patterns that must write to *both* buffers so scroll
    // boundaries reveal valid content regardless of which buffer is live.
    [[nodiscard]] BitMap& back_buffer() noexcept { return back_; }
    [[nodiscard]] BitMap& front_buffer() noexcept { return front_; }

private:
    bool opened_{false};
    BitMap front_{};
    BitMap back_{};
    BitMap bitmap_{};      // possibly wider than `back_` — the scroll canvas
    u16 screen_width_{};
    u32 frames_{};
    void* impl_{nullptr};  // Amiga-side: copper list, library bases, ...
};

} // namespace pa
