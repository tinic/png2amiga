# Refactor plan — main.cpp / api.cpp / preview pipeline consolidation

Drafted 2026-04-28 after merging `fix/lace-vertical-dither` (commit
`02b5a58`). Pre-existing commit base: `b0a812e` (v1.22.0).

## Why refactor

Current state (after 5 merged fixes for hires-lace + CAP correctness)
exposes structural duplication that made the bug-hunt harder than it
should have been:

- **CHeaderOptions construction** appears at ~10 sites in `main.cpp`,
  each with near-identical boilerplate (`hires`, `interlace`, `aga`,
  `dpf`, `fade_in`, `copper_changes`, `copper_changes_per_line`,
  `symbol_name`, `scap_*`). When a new field is added (`dual_cap_lists`
  earlier this session), every site has to be touched.
- **Pipeline duplication** between `main.cpp` (CLI) and `api.cpp`
  (WASM). Both run the same encoder, both build a preview, both pick
  an output writer based on extension, both reach `cheader::*` etc.
  When `api.cpp` and `main.cpp` diverge silently, bugs creep in.
- **Preview rendering exists in three places** with different code:
  `show_terminal_preview` (terminal iTerm2 inline), `save_preview` (PNG
  on disk), and `result.rendered` (WASM return). Each can pick up a
  fix in the encoder pipeline at a different rate.
- **Chipset resolution** has at least two entry points:
  `effective_chipset(config)` in `main.cpp` and `resolve_chipset(s,
  mode)` in `api.cpp`. They mostly agree but have drifted on edge
  cases (depth-implied AGA on hires-lace, etc.).

## Consolidation targets

### 1. `Pipeline` runner (highest priority)

Single function, consumed by both `main.cpp` and `api.cpp`:

```cpp
struct PipelineInput {
    Image source;
    api::Options opts;
};

struct PipelineResult {
    bitplane::BitplaneData planes;
    std::vector<Color3f> palette;
    amiga::Mode mode;
    amiga::Chipset chipset;
    bool hires, interlace, dpf, has_transparency;
    std::vector<bool> transparency_mask;

    // CAP / SCAP per-row data when applicable
    std::optional<copper::CopperResult> copper;
    std::optional<scap::ScapResult> scap;

    // Preview already quantized + capped to match chip output
    Image preview;
};

Result<PipelineResult> run_pipeline(const PipelineInput&);
```

`main.cpp` and `api.cpp` become thin wrappers around `run_pipeline` +
output-format dispatch (.png, .iff, .h, .cpp, .raw).

### 2. `make_ch_opts(result, output_path, config_or_options)`

Single CHeaderOptions builder. Replaces the ~10 repeated inline
constructions in `main.cpp`. Lives in `api.cpp` (or a new
`pipeline.cpp`).

### 3. Preview quantize-cap-scale stage

After the encoder runs, the preview goes through a single canonical
transformation. Split into three substeps so each commit lands cleanly:

- **3a — display-rescale stage.** `scale_for_display(image, mode,
  hires, interlace)` + `scale_mask_for_display(...)` collapse the per-mode
  PAR / integer-scale rules used by `save_preview()` and
  `show_terminal_preview()`. **DONE** in commit `ebcc01e`.
- **3b — render dispatch.** Single `pipeline::render_preview(...)`
  helper that picks the right per-mode renderer
  (`bitplane::render` / `ham::render_ham` / `ham::render_ham_copper` /
  `copper::render_copper_capped`) from PipelineResult fields. The five
  current inline call sites (3 in main.cpp, 2-3 in api.cpp) all call
  into it.
- **3c — PipelineResult.rendered as canonical source.** After 3b,
  `run_pipeline` already populates `result.rendered` via the dispatch.
  Migrate main.cpp's branches to consume that field instead of
  re-rendering inline.

Goal: any future preview correctness fix — including the deferred OCS
preview-vs-chip bug below — lands in **one** place.

### 4. `Chipset chipset = resolve(...)` — single source

Pick one of `effective_chipset` / `resolve_chipset`, delete the other,
make sure both `main.cpp` and `api.cpp` go through the survivor.

### 5. CLI argument plumbing

Currently `Config` (in `main.cpp`) and `api::Options` are nearly
identical structs that drift independently. Either:
- Make `Config` a strict superset of `api::Options` and embed it; or
- Make `main.cpp` build `api::Options` directly and skip `Config`.

The latter forces `api::Options` to be the canonical schema.

## Open / deferred issue

**Preview vs hardware OCS color discrepancy** — even after merging fix
#5 (`render_copper_capped` with `palette::quantize_to_ocs`), the user
reports the preview shows a smoother gradient than the hardware does
(this is reproducible on the saved PNG, not just terminal display, so
not iTerm2 bilinear smoothing). Hypotheses to investigate **after**
the refactor:

- The vertical palette dither alternation pattern visible to the user
  on hardware is NOT being faithfully reproduced in the
  preview-quantize step.
- There's a render path I missed that doesn't go through
  `render_copper_capped` (current grep shows no other callers, so
  likely not, but easier to verify after consolidation).
- `cheader.cpp:lace_rebuild` may differ subtly from what
  `render_copper_capped` emulates (e.g. order of operations, what
  counts as "previous same-field row" for a given y).

The refactor consolidates preview rendering to one site, which makes
this investigation cheaper.

## Out of scope for the refactor

- Encoder algorithm changes (CAP planner, vertical dither logic,
  lace_rebuild in cheader). All those live in `copper.cpp` and
  `cheader.cpp` and stay where they are.
- Output format changes (.iff, .h, .cpp, .raw, .png). These can stay
  as thin per-extension dispatch functions, just consuming the
  unified `PipelineResult`.
- Web frontend (`web/`). The WASM binding will benefit from the
  pipeline consolidation but doesn't itself need restructuring.

## Suggested order of work

1. Define `PipelineInput` / `PipelineResult` / `run_pipeline`. Land
   alongside the existing duplicated paths (don't delete yet).
   **DONE:** `pipeline::PipelineResult` and `pipeline::run_pipeline`
   live in `src/pipeline.hpp`. `api.cpp` keeps the workhorse body;
   `pipeline.cpp` is a one-line forwarder.
2. Migrate `api.cpp::convert_viewer` to call `run_pipeline`. Verify
   WASM still works. **DONE** as a side effect of step 1 — the
   `convert_*` family already shares the single `run_pipeline`
   workhorse; the `pipeline::` forwarder exists for outside callers.
3. Migrate `main.cpp` CAP/SCAP/HAM/plain paths one at a time. Verify
   each via existing ctest cases. **PARTIAL:** `make_api_options(Config)`
   builder landed (commit `af28eec`) so any future branch migration
   has a one-line Config → api::Options translation. Branches still
   pending migration (each its own commit + ctest sweep):
   - Plain lores/hires standard `.iff`/`.h`/`.cpp` fallback
     (main.cpp ~5240-5500).
   - EHB without copper (main.cpp ~4150-4280).
   - EHB + CAP (main.cpp ~4080-4330).
   - HAM standard / HAM + CAP (main.cpp ~3870-4060).
   - CAP standard (lores/hires + copper) (main.cpp ~4500-4820).
   - SCAP probe + production (main.cpp ~3030-3085, ~4830-5005).
4. Delete the now-orphan duplicated code in `main.cpp` and `api.cpp`.
   **BLOCKED on #3 finishing** — nothing yet orphaned.
5. Tackle preview-OCS-discrepancy bug with the consolidated preview
   path as the only relevant code.

## Test coverage

ctest currently has 78 cases (smoke tests for each output format).
The refactor must maintain 100% pass rate at each step. Add new tests
for the unified `run_pipeline` surface as it lands.

## Pre-refactor state — keepers from this session

For reference if context is lost:

1. `MEMF_CHIP | MEMF_CLEAR` on copper allocs (cheader.cpp).
2. File-scope `VblHandler` replacing captureless lambda (cheader.cpp).
3. cl1 row-0 + cl2 row-1 CAP seeds for lace+CAP (cheader.cpp).
4. Field-segregated vertical palette dither (copper.cpp:757,
   `run_pass(start, stride)`).
5. `render_copper_capped` with optional 12-bit OCS quantization
   (copper.cpp + copper.hpp), called from main.cpp and api.cpp.

All five live on `main` after merge `02b5a58`. They are independently
correct; the refactor doesn't change their behavior, just consolidates
their callers.
