# --tile demo sources

Each input here is mathematically periodic (`src[W-1] ≈ src[0]` and the
analogous Y constraint), so the *source* tiles seamlessly by
construction. The interesting question is whether the **dithered**
output also tiles seamlessly — that's the part `--tile`'s 3×3
pre-replicate is supposed to fix.

## Sources

| File | What it stresses |
|---|---|
| `ramp_horizontal.png` | Smooth horizontal sine (R/G/B at 60° phase offsets). Pure gradient — no high-frequency content for FS to lock onto. |
| `ramp_vertical.png` | Same content rotated 90°. FS scans rows left→right, so vertical stripes don't propagate any cross-tile state — the FS state at column 0 is identical to the state at column W. Easy case. |
| `ramp_diagonal.png` | Periodic diagonal sweep. Stresses both axes simultaneously. |
| `diamond.png` | 1-diamond per tile, edges land on tile boundaries. The diamond outline must stay continuous when the result is repeated. |
| `plasma.png` | Three superposed sine fields, photoreal-ish smooth texture. |
| `stripes.png` | Sharp 8-pixel-wide stripes. Discrete transitions kick FS state into different basins each time, so cross-tile FS sync is the hardest. |

Sources are 128×128. Re-generate (or change the size) via
`gen_sources.py`.

## Running the comparison

```bash
./examples/tile/compare.sh
```

Outputs land in `examples/tile/out/`:
- `<name>_raw.png`     — encoded with no `--tile`.
- `<name>_tile.png`    — encoded with `--tile`.
- `<name>_compare.png` — both above tiled 3×3 side-by-side, with labels.

Look at the vertical tile boundaries (and horizontal). On a clean
seamless tile the boundary should be invisible. On a broken tile a
faint or strong seam line shows up.

## What you'll see

The naive "replicate 3×3, dither, crop centre" approach is the simplest
possible attempt at FS-tile synchronisation. It works partially:

- **`ramp_vertical`**: FS scans rows left→right. With horizontal stripes
  the FS state at column 0 is the same as at column W (no row-to-row
  drift), so the dither pattern lines up. `--tile` and raw both produce
  ~indistinguishable seams.
- **`stripes`**: source has sharp, hard transitions on the seam. FS
  has nowhere to converge to. `--tile` ties raw — the seam is dictated
  by the source, not the dither.
- **`diamond`** / **`ramp_horizontal`**: source seams are exactly zero
  (mathematical wraparound). The dither's FS state at col W and col 0
  may not match because of accumulated row-to-row error from the warmup
  region. In practice `--tile` neither helps nor hurts here.
- **`ramp_diagonal`** / **`plasma`**: smooth periodic content.
  Empirically `--tile` can produce a slightly *worse* seam than raw
  because the FS state arriving at column W of the centre after warmup
  isn't actually periodic — the row-to-row error term doesn't have a
  W-period.

So the simple 3×3 trick is a **heuristic**, not a guarantee. It's a
no-cost option for textured / photoreal sources where the dithering
artefacts are already swamped by source content variation. For smooth
gradients (the cases here) it doesn't reliably eliminate the seam.

## If you want a stronger guarantee

Two extensions on top of the current `--tile` baseline that would
actually close the loop:

1. **Wrap-around Floyd–Steinberg.** Modify the FS pass to treat the
   row as a circular buffer: when the dither head reaches column W,
   feed the right-edge errors back to column 0. One pass, no warmup
   region needed, mathematically guaranteed seamless. Requires either
   modifying `dither::apply` to take a `wrap_x` flag or wrapping the
   dither call.
2. **Iterative settling.** Run `--tile` once. Take the centre crop.
   Replicate it 3×3 again, dither, crop. Repeat until the seam metric
   stabilises (typically 2–3 passes).

Neither is implemented yet — the demos here exist to make the gap
visible so you can decide whether the cost is worth chasing.
