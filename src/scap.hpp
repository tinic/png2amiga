#pragma once

#include "amiga.hpp"
#include "bitplane.hpp"
#include "dither.hpp"
#include "ham.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace png2amiga::scap {

// ---------------------------------------------------------------------------
// SCAP — Super Copper-Augmented Palette, dual-playfield edition.
//
// Plain CAP (copper.cpp) emits per-scanline COLORxx writes only at the top
// of each line, before bitplane DMA resumes. SCAP extends that by allowing
// additional COLORxx MOVEs at fixed mid-line horizontal slot positions
// chosen so the copper still gets DMA cycles even while bitplane fetch is
// active.
//
// Why DPF: standard 6-plane lores only frees 2 slots per 16-pixel block
// (slots 7 and 8) for the copper, and most of those go to memory refresh
// and sprite DMA — there's almost no mid-line bandwidth left. With DPF
// the upper PF2 colors (regs 8-15 OCS / 16-31 AGA) are precisely what we
// want to swap mid-line: a 3-plane PF2 has only 8 colors per row, and
// SCAP buys us a per-strip palette-bank refresh. AGA DPF (4-plane PF2,
// 16 colors) gets the same treatment but with a different bandwidth
// budget — handled by separate slot tables.
//
// Slot positions are bpp-dependent (more planes ⇒ tighter bandwidth ⇒
// fewer/later slots) but constant across all scanlines, so they live in
// hardcoded tables that are populated empirically via Probe A.
// ---------------------------------------------------------------------------

// One mid-line MOVE opportunity, in lores 320-pixel display coordinates.
//
// pixel_x   Horizontal pixel where the COLORxx MOVE lands. NOTE: still TBD
//           whether this is "at x" or "after x" — Probe D will pin it down.
//           Step 1 calibration writes raw HPOS values; pixel_x is filled in
//           later from observed transitions.
// hpos      Raw HPOS comparison value used in the per-line WAIT.
// capacity  Number of MOVEs that can be safely chained at this slot before
//           the next bitplane fetch resumes. Probe B fills this in.
// One real-MOVE position in the SCAP chain. Strip s covers display pixels
// in [slots[s-1].pixel_x .. slots[s].pixel_x) and uses the palette state
// produced by all MOVEs slots[0..s-1]. pixel_x is only used by the
// planner to determine which pixels belong to which strip; the actual
// hardware MOVE position is determined by its index in the chain (each
// MOVE takes ~4 CCK absent BPL DMA stalls).
struct ScapSlot {
    int pixel_x{};
};

struct ScapSlotTable {
    int total_planes{};         // 6 = OCS DPF, 8 = AGA DPF (deferred)
    int line_gate_hpos{};       // WAIT(hp=this, vp=N) opens the SCAP chain
    int end_of_line_hpos{};     // WAIT(hp=this, vp=N) closes the chain
    std::vector<ScapSlot> slots;
};

// kScap6bplOcs — OCS DPF, 6 bitplanes, lores 320 px display.
//
// Per-line copper structure (CAP-style, palette resets every line):
//   1. 8 base-palette MOVEs (write COLOR08..15 to the entry palette).
//      Fires in the horizontal blank between the previous line's
//      end-of-line WAIT and this line's line-gate WAIT.
//   2. WAIT(hp=line_gate_hpos, vp=44+y) — gates the chain to the
//      visible area.
//   3. 20 SCAP MOVEs back-to-back (no fillers) — the planner picks
//      (reg, colour) per slot to greedy-minimise OKLab error in the
//      upcoming strip. Strip s covers pixels [slots[s-1].pixel_x ..
//      slots[s].pixel_x).
//   4. WAIT(hp=end_of_line_hpos, vp=44+y) — releases the copper to
//      the next line's section.
//
// Strip widths are 16 lores px (slots evenly spaced 16..320). MOVE
// positions on the bus depend on BPL DMA contention; in 6-plane lores
// each MOVE takes ~4-12 CCK, so 20 MOVEs cover most of the visible
// area. Right edge may stay on the final palette state if the chain
// finishes before HPOS=DDFSTOP.
constexpr int kOCS = 0;
inline const ScapSlotTable kScap6bplOcs{
    /*total_planes=*/6,
    /*line_gate_hpos=*/0x38,        // Was 0x3C; pulled 8 HPOS (= 16 lores
                                    // px) earlier to fit the new preload
                                    // MOVE (slot 0 at pixel_x = 0) before
                                    // the existing slot at pixel 8. Each
                                    // MOVE takes ~4 CCK under DPF DMA
                                    // contention, so 8 HPOS of headroom
                                    // is enough for the extra preload.
                                    // ADJUST via --scap-debug if hw
                                    // landings differ.
    /*end_of_line_hpos=*/0xDD,
    /*slots=*/{
        // pixel_x — first pixel of the strip this slot's MOVE applies to.
        //
        // Slot 0 ("preload") at pixel_x = 0: lands during HBLANK tail /
        // start of active raster, so its swap takes effect for the
        // first 8 active-area pixels. Without it the chain's first real
        // MOVE is at x=8 and pixels 0..7 stay on CAP's entry palette,
        // producing a visible discontinuity at the x=8 boundary on
        // continuous-tone content. The planner uses this slot like any
        // other; if the beam doesn't want a swap the filler MOVE writes
        // COLOR31 (unread on OCS DPF 3+3) and costs nothing.
        // ADJUST THE LANDING POSITION via --scap-debug if the actual
        // hw timing puts it past x=0.
        {  0+kOCS},
        // Existing slots — shifted by −8 from the naive {16,32,…,320}
        // grid because under 6-plane DPF DMA contention the first real
        // MOVE in this segment of the chain lands at lores x = 8, and
        // subsequent MOVEs follow at 16-px spacing.
        { 8+kOCS}, { 24-1+kOCS}, { 40+kOCS}, { 56-1+kOCS}, { 72-1+kOCS}, { 88+kOCS}, {104+kOCS}, {120+kOCS},
        {136-1+kOCS}, {152+kOCS}, {168+kOCS}, {184+kOCS}, {200+kOCS}, {216-1+kOCS}, {232+kOCS}, {248-1+kOCS},
        {264-1+kOCS}, {280+kOCS}, {296-1+kOCS},
    }
};

// kScap6bplEhb — OCS EHB, 6 bitplanes, lores 320 px display.
//
// Same DMA shape as 6-plane DPF (6 plane fetches per 16 lores px) but
// EHB has no PF2OF / DBLPF combiner machinery, so the actual MOVE
// landing positions on the bus differ from DPF in subtle ways. This
// table starts at the clean naive 16-px grid (8, 24, 40, ..., 296) —
// no per-slot offset corrections yet. Calibrate empirically with
// --scap-debug + the stripe tuner: each visible black/white transition
// position is the actual MOVE landing for that slot. Adjust below.
inline const ScapSlotTable kScap6bplEhb{
    /*total_planes=*/6,
    /*line_gate_hpos=*/0x3C,
    /*end_of_line_hpos=*/0xDD,
    /*slots=*/{
        {  8-1}, { 24-1}, { 40-1}, { 56-1}, { 72-1}, { 88-1}, {104-1}, {120-1},
        {136-1}, {152-1}, {168-1}, {184-1}, {200-1}, {216-1}, {232-1}, {248-1},
        {264-1}, {280-1}, {296-1},
    }
};

// Deferred — populated when we run an AGA probe.
inline const ScapSlotTable kScap8bplAga{
    /*total_planes=*/8,
    /*line_gate_hpos=*/0x40,
    /*end_of_line_hpos=*/0xDD,
    /*slots=*/{},
};

inline const ScapSlotTable& scap_table_for(int total_planes) {
    return (total_planes == 8) ? kScap8bplAga : kScap6bplOcs;
}

// ---------------------------------------------------------------------------
// One copper instruction emitted into the per-line list.
// ---------------------------------------------------------------------------
enum class ScapOpKind : std::uint8_t {
    kMove,
    kWait,
};

struct ScapMove {
    ScapOpKind kind{ScapOpKind::kMove};
    // For kMove:
    std::uint8_t reg{};       // palette register 0..31 (translated to COLORxx)
    std::uint16_t rgb_ocs{};  // 12-bit color packed as 0x0RGB
    // For kWait:
    std::uint8_t hpos{};      // horizontal compare (raw byte; bit 0 ignored)
    std::uint8_t vpos{};      // vertical compare (image scanline 0..255 typical)
    // Provenance / debug:
    int slot_index{-1};       // index into ScapSlotTable.slots, or -1
};

// ---------------------------------------------------------------------------
// Result of an SCAP encode (planner output) OR a probe synthesis.
// ---------------------------------------------------------------------------
struct ScapResult {
    bitplane::BitplaneData planes;

    // Frame-start palette (written before the visible area, like CAP base).
    // For DPF this is the full 16-entry (OCS) / 32-entry (AGA) palette
    // already shifted into PF2 upper registers — same convention as the
    // rest of the DPF pipeline.
    std::vector<Color3f> palette;

    // Per-scanline copper ops (WAIT/MOVE pairs). Emitted verbatim by the
    // cheader SCAP block as alternating UWORDs in the copper list.
    std::vector<std::vector<ScapMove>> line_moves;

    // The slot table this result targets (informational; useful for
    // emitting comments and verifying the viewer was generated against the
    // right plane count).
    ScapSlotTable slot_table;

    // Calibration metadata: which probe synthesized this result, if any.
    // Empty string ⇒ this is a real planner output, not a probe.
    std::string probe_label;

    // Stats (planner output only; probes leave these zero).
    float total_error{};
    float avg_changes_per_line{};       // SCAP useful swaps/line
    float avg_total_moves_per_line{};   // CAP diffs + SCAP swaps (real hw load)
    std::size_t max_moves_per_line{};   // worst-case row MOVE count
    float avg_hblank_moves_per_line{};  // MOVEs before line-gate WAIT (hblank)
    std::size_t max_hblank_moves_per_line{};
    float avg_visible_moves_per_line{}; // MOVEs after line-gate WAIT (SCAP)
    std::size_t max_visible_moves_per_line{};

    // Rendered preview produced by the planner. Width × height linear-RGB
    // image where each pixel is looked up against the per-strip palette
    // state active at that column (PF2 colour the hardware would
    // actually display). Empty for probe outputs.
    Image rendered;
};

// ---------------------------------------------------------------------------
// Planner — OCS DPF, lores 320 px.
//
// Quantises the image to 8 base PF2 colours (reg 8..15), then per scanline
// does a greedy slot-by-slot search: at each slot pixel_x boundary, find the
// (reg, colour) MOVE that most reduces OKLab error in the upcoming strip,
// emit it, and continue. Up to slot.capacity MOVEs may be chained per slot.
// The accumulated palette state carries across lines (CAP-style) so swaps
// from the bottom of one line act as the entry palette for the top of the
// next.
//
// Output: 6-plane DPF bitplane data (PF1 zero, PF2 = encoded indices), one
// ScapMove list per scanline (WAIT/MOVE pairs ready for the cheader emitter),
// the entry palette (16 entries — 8 zero PF1 + 8 PF2 base), and a rendered
// preview Image where each pixel reflects the per-strip palette state.
// ---------------------------------------------------------------------------
// debug_overlay: SCAP slot-tuning debug bundle. Enables two changes
// always used together:
//   * Force the 8 base-palette MOVEs at line start to write 0x0000, so
//     each line opens with PF2 regs all black; the 20 SCAP MOVEs then
//     do every visible colour change. Makes it obvious where each
//     MOVE actually lands on real hardware vs. the planner's model.
//   * Paint yellow vertical ruler markers into PF1 at every 4/8/16 px
//     (top-quarter / top-half / full height) so MOVE landing positions
//     can be read off the screen. Also recolours palette[1] to yellow.
// Off (default) for production. Pair with examples/ramps.png as the
// canonical visual test case.
Result<ScapResult> encode_scap_dpf_ocs(const Image& image,
                                       int width,
                                       int height,
                                       bool lock_color0 = true,
                                       const dither::Settings& dither_settings = {},
                                       bool debug_overlay = false,
                                       std::size_t copper_changes_override = 0,
                                       int palette_diversity = 0,
                                       std::function<void(float, std::string_view)>
                                           on_progress = {},
                                       bool enable_best = false,
                                       std::string_view best_metric =
                                           "msssim",
                                       int cap_spread_radius = -1,
                                       float cap_spread_decay = -1.0f,
                                       std::span<const Color3f>
                                           external_palette = {});

// ---------------------------------------------------------------------------
// SCAP for EHB (Extra Half-Brite, 6 bitplanes, 32 base + 32 hardware-derived
// half-brite colours).
//
// 6bpp EHB has identical BPL DMA bandwidth to 6-plane lores (and to OCS DPF
// 3+3) — same 6 plane fetches per 16 px — so the existing kScap6bplOcs
// slot table transfers directly. The planner swaps the 32 BASE registers;
// each swap implicitly updates the corresponding half-brite sibling
// (hardware halves the sRGB DAC values), so a single MOVE shifts two
// effective palette entries at once. Per-pixel encoding picks among all
// 64 effective entries (bit 5 of the 6-bit index = half-brite toggle).
//
// Output: 6-plane bitplane data, 32-entry base palette (the half-brites
// are reproduced by the hardware and are NOT written to CMAP), and a
// rendered preview Image.
// ---------------------------------------------------------------------------
Result<ScapResult> encode_scap_ehb_ocs(const Image& image,
                                       int width,
                                       int height,
                                       bool lock_color0 = true,
                                       const dither::Settings& dither_settings = {},
                                       std::size_t copper_changes_override = 0,
                                       int palette_diversity = 0,
                                       bool debug_overlay = false,
                                       std::function<void(float, std::string_view)>
                                           on_progress = {},
                                       bool enable_best = false,
                                       std::string_view best_metric =
                                           "msssim",
                                       int cap_spread_radius = -1,
                                       float cap_spread_decay = -1.0f,
                                       std::span<const Color3f>
                                           external_palette = {});

// ---------------------------------------------------------------------------
// SCAP for HAM6 (6 bitplanes — 2 control + 4 data, 16-colour base palette
// with HAM SET / MODIFY ops at pixel granularity).
//
// HAM6 has the same BPL DMA pattern as EHB and OCS DPF (6 plane fetches
// per 16-pixel slot), so the empirically-calibrated kScap6bplEhb slot
// table transfers directly. The encoder mid-line-swaps the 16 BASE
// palette registers on the same 19-slot grid; HAM op selection happens
// per pixel against whichever strip palette is currently active. The
// rolling "previous output colour" HAM state crosses strip boundaries
// unchanged — MODIFY ops apply to the prior colour irrespective of
// palette, SET ops resolve against the new strip palette.
//
// Output: 6-plane bitplane data, 16-entry base palette, per-line SCAP
// MOVE tables, and a rendered preview Image.
//
// v0: single greedy pass, no joint refinement, no best wiring.
// ---------------------------------------------------------------------------
Result<ScapResult> encode_scap_ham6_ocs(const Image& image,
                                        int width,
                                        int height,
                                        bool lock_color0 = true,
                                        const dither::Settings& dither_settings = {},
                                        std::size_t copper_changes_override = 0,
                                        int palette_diversity = 0,
                                        std::function<void(float, std::string_view)>
                                            on_progress = {},
                                        int cap_spread_radius = -1,
                                        float cap_spread_decay = -1.0f,
                                        bool enable_best = false,
                                        std::string_view best_metric =
                                            "msssim",
                                        std::span<const Color3f>
                                            external_palette = {},
                                        ham::HamMetric ham_metric =
                                            ham::HamMetric::oklab2);

// ---------------------------------------------------------------------------
// Probe A — slot discovery sweep for OCS DPF (6-plane).
//
// Builds a 6-plane DPF frame where every pixel = PF2 index 1, i.e. the
// upper-register color reg 9. Frame-start palette: every register set to
// black. Per-line copper ops:
//
//   1. WAIT($00, line)                — gate at top-of-line
//   2. MOVE  COLOR09 = $000           — reset PF2 reg 1 to black
//   3. WAIT(anchor_hpos, line)        — deterministic phase reference
//   4. WAIT(C(y), line)               — swept HPOS, C(y) ∈ [0x00, 0xE3]
//   5. MOVE  COLOR09 = $FFF           — probe to white
//
// Lines where the swept WAIT lands in a bitplane-fetch slot stay black
// (the MOVE never executes before the line ends); lines where it lands
// in a free slot show a black→white transition. The pixel x at which the
// transition occurs maps the HPOS C(y) to a real display column,
// populating the slot table for OCS DPF.
//
// Always emits 6-plane DPF data: plane 0 = all-zero (PF1 LSB), plane 1 =
// all-FF (PF2 LSB → index bit 0), planes 2-5 = zero (PF1/PF2 higher bits
// stay 0). PF1 = transparent, PF2 = solid index 1.
// ---------------------------------------------------------------------------
Result<ScapResult> make_scap_probe_a_dpf_ocs(int width, int height);

// Probes B/C/D — fleshed out once Probe A's slot data is in.
//   B: capacity (chained MOVEs) at each slot.
//   C: AGA-specific bandwidth (8-plane DPF).
//   D: at-x vs after-x pixel mapping disambiguation.
Result<ScapResult> make_scap_probe_b_dpf_ocs(int width, int height);
Result<ScapResult> make_scap_probe_c_dpf_aga(int width, int height);
Result<ScapResult> make_scap_probe_d_dpf_ocs(int width, int height);

} // namespace png2amiga::scap
