#!/usr/bin/env python3
"""
RECOIL round-trip suite.

For one input image, encode it through every (mode, depth, cap, dpf,
chipset) combo png2amiga supports for IFF output, then decode each IFF
back to PNG via Piotr Fusik's RECOIL (https://recoil.sourceforge.net/)
and compare against our own preview PNG.

The two PNGs answer two different questions:
  * expected.png — what png2amiga thinks the IFF will look like
                   (rendered from the same internal state that wrote it).
  * recoil.png   — what an independent decoder makes of the IFF bytes.

A high PSNR between them means the IFF correctly self-describes for
third-party readers (DPaint, ViewTek, IrfanView's IFF plugin, etc. all
share RECOIL's general approach to ILBM + CAMG + PCHG).

Artifacts for each combo land under build/recoil_check/<combo>/:
  in.iff        — IFF written by png2amiga
  expected.png  — png2amiga's own preview render
  recoil.png    — RECOIL's decode of in.iff
  log.txt       — encoder stderr + cmd lines (for failed combos)

Usage:
    python3 tools/recoil_roundtrip.py [--input PATH] [--build DIR]

Exits 0 on full sweep completion, 1 on any encode failure or PSNR < 25 dB,
77 when recoil2png isn't available (ctest interprets 77 as "skip").
Prints a Markdown table to stdout with PSNR per combo.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image


# RECOIL has no Amiga dual-playfield (DPF) rendering — these combos
# decode as flat 6/8-plane indexed images and look wrong, but our IFF
# is correct (DPaint, ViewTek, real Amigas render them fine). Tagged
# XFAIL so the suite's overall pass count isn't dragged down by a
# known third-party limitation.
KNOWN_XFAIL_PREFIXES = ("dpf-",)


COMBOS = [
    # (mode, depth, chipset, extra_flags, label)
    # OCS lores indexed
    ("lores",      1, "ocs", [],                      "lores-d1"),
    ("lores",      3, "ocs", [],                      "lores-d3"),
    ("lores",      5, "ocs", [],                      "lores-d5"),
    ("lores",      5, "ocs", ["--cap"],               "lores-d5-cap"),
    ("lores-lace", 5, "ocs", [],                      "lores-lace-d5"),
    ("lores-lace", 5, "ocs", ["--cap"],               "lores-lace-d5-cap"),
    # OCS hires indexed
    ("hires",      4, "ocs", [],                      "hires-d4"),
    ("hires",      4, "ocs", ["--cap"],               "hires-d4-cap"),
    ("hires-lace", 4, "ocs", [],                      "hires-lace-d4"),
    # AGA lores deep
    ("lores",      6, "aga", [],                      "lores-d6-aga"),
    ("lores",      8, "aga", [],                      "lores-d8-aga"),
    ("lores",      8, "aga", ["--cap"],               "lores-d8-aga-cap"),
    # AGA hires
    ("hires",      8, "aga", [],                      "hires-d8-aga"),
    # OCS DPF (lores only, depth=3, 8 PF2 colours)
    ("lores",      3, "ocs", ["--dpf"],               "dpf-lores-d3"),
    ("lores",      3, "ocs", ["--dpf", "--cap"],      "dpf-lores-d3-cap"),
    ("lores-lace", 3, "ocs", ["--dpf"],               "dpf-lores-lace-d3"),
    # AGA DPF (depth=4, 16 PF2 colours)
    ("lores",      4, "aga", ["--dpf"],               "dpf-lores-d4-aga"),
    ("hires",      4, "aga", ["--dpf"],               "dpf-hires-d4-aga"),
    # HAM6 (OCS / AGA)
    ("ham6",            6, "ocs", [],          "ham6"),
    ("ham6",            6, "ocs", ["--cap"],   "ham6-cap"),
    ("ham6",            6, "ocs", ["--cap", "--cap-best"], "ham6-cap-best"),
    ("ham6-lace",       6, "ocs", [],          "ham6-lace"),
    ("ham6-hires",      6, "aga", [],          "ham6-hires"),
    ("ham6-hires-lace", 6, "aga", [],          "ham6-hires-lace"),
    # HAM8 (AGA only)
    ("ham8",            8, "aga", [],          "ham8"),
    ("ham8",            8, "aga", ["--cap"],   "ham8-cap"),
    ("ham8",            8, "aga", ["--cap", "--cap-best"], "ham8-cap-best"),
    ("ham8-lace",       8, "aga", [],          "ham8-lace"),
    ("ham8-hires",      8, "aga", [],          "ham8-hires"),
    # EHB (OCS, fixed 6 planes)
    ("ehb",      6, "ocs", [],         "ehb"),
    ("ehb",      6, "ocs", ["--cap"],  "ehb-cap"),
    ("ehb-lace", 6, "ocs", [],         "ehb-lace"),
]


def find_recoil():
    for name in ("recoil2png", "recoil-util"):
        p = shutil.which(name)
        if p:
            return p
    # Common install locations
    for app in (
        os.path.expanduser("~/bin/recoil2png"),
        "/usr/local/bin/recoil2png",
        "/opt/homebrew/bin/recoil2png",
        "/Applications/RECOIL.app/Contents/MacOS/recoil2png",
        "/Applications/RECOIL.app/Contents/Resources/recoil2png",
    ):
        if os.path.isfile(app) and os.access(app, os.X_OK):
            return app
    return None


def find_png2amiga():
    here = Path(__file__).resolve().parent.parent
    cand = here / "build" / "png2amiga"
    if cand.exists() and os.access(cand, os.X_OK):
        return str(cand)
    p = shutil.which("png2amiga")
    if p:
        return p
    return None


def psnr_aligned(a_path, b_path):
    """PSNR of two PNGs. Resamples b to a's size if dimensions differ
    (e.g. RECOIL emits at native size while our preview is 2x-scaled)."""
    a = Image.open(a_path).convert("RGB")
    b = Image.open(b_path).convert("RGB")
    if a.size != b.size:
        b = b.resize(a.size, Image.NEAREST)
    aa = np.asarray(a, dtype=np.float64)
    bb = np.asarray(b, dtype=np.float64)
    mse = np.mean((aa - bb) ** 2)
    if mse == 0:
        return float("inf")
    return 20.0 * np.log10(255.0) - 10.0 * np.log10(mse)


def run(cmd, cwd=None, capture=True):
    return subprocess.run(cmd, cwd=cwd,
                          capture_output=capture, text=True, timeout=120)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", default="examples/lovers.jpg",
                    help="source image fed to every combo")
    ap.add_argument("--build", default="build/recoil_check",
                    help="root for per-combo artifact directories")
    ap.add_argument("--threshold", type=float, default=25.0,
                    help="minimum PSNR (dB) to consider a combo passing")
    args = ap.parse_args()

    repo = Path(__file__).resolve().parent.parent
    os.chdir(repo)

    p2a = find_png2amiga()
    if not p2a:
        print("ERROR: png2amiga binary not found. Run "
              "`cmake --build build` first.", file=sys.stderr)
        return 2

    recoil = find_recoil()
    if not recoil:
        print("recoil2png not found in PATH or "
              "/Applications/RECOIL.app/. Skipping RECOIL round-trip.\n\n"
              "Install RECOIL from https://recoil.sourceforge.net/macos.html\n"
              "  1. Download recoil-X.Y.Z-macos.dmg\n"
              "  2. Drag recoil2png from the DMG to /usr/local/bin\n",
              file=sys.stderr)
        return 77  # ctest: skip

    if not Path(args.input).exists():
        print(f"ERROR: input not found: {args.input}", file=sys.stderr)
        return 2

    out_root = Path(args.build)
    out_root.mkdir(parents=True, exist_ok=True)

    rows = []
    fails = 0
    for mode, depth, chipset, extra, label in COMBOS:
        d = out_root / label
        d.mkdir(parents=True, exist_ok=True)
        iff_path = d / "in.iff"
        expected_path = d / "expected.png"
        recoil_path = d / "recoil.png"
        log_path = d / "log.txt"

        flags = ["--quiet", "--mode", mode, "--depth", str(depth),
                 "--chipset", chipset, *extra]

        # Encode to IFF
        cmd_iff = [p2a, *flags, args.input, str(iff_path)]
        r1 = run(cmd_iff)
        # Render our own preview PNG
        cmd_png = [p2a, *flags, args.input, str(expected_path)]
        r2 = run(cmd_png)

        if r1.returncode != 0 or not iff_path.exists():
            log_path.write_text(
                f"$ {' '.join(cmd_iff)}\n{r1.stderr}\n"
                f"$ {' '.join(cmd_png)}\n{r2.stderr}\n")
            rows.append((label, "iff_fail", "—", str(d.resolve().relative_to(repo))))
            fails += 1
            continue
        if r2.returncode != 0 or not expected_path.exists():
            log_path.write_text(f"$ {' '.join(cmd_png)}\n{r2.stderr}\n")
            rows.append((label, "preview_fail", "—", str(d.resolve().relative_to(repo))))
            fails += 1
            continue

        # RECOIL → PNG. recoil2png writes to <input>.png by default; we
        # rename so all combos use the same recoil.png filename.
        cmd_recoil = [recoil, str(iff_path)]
        r3 = run(cmd_recoil)
        recoil_default = iff_path.with_suffix(".png")
        if r3.returncode != 0 or not recoil_default.exists():
            log_path.write_text(
                f"$ {' '.join(cmd_recoil)}\nrc={r3.returncode}\n"
                f"stdout={r3.stdout}\nstderr={r3.stderr}\n")
            rows.append((label, "recoil_fail", "—", str(d.resolve().relative_to(repo))))
            fails += 1
            continue
        recoil_default.rename(recoil_path)

        try:
            p = psnr_aligned(expected_path, recoil_path)
        except Exception as e:
            log_path.write_text(f"PSNR error: {e}\n")
            rows.append((label, "psnr_fail", "—", str(d.resolve().relative_to(repo))))
            fails += 1
            continue

        is_xfail = any(label.startswith(prefix)
                       for prefix in KNOWN_XFAIL_PREFIXES)
        if p >= args.threshold:
            ok = "XPASS" if is_xfail else "PASS"
        else:
            ok = "XFAIL" if is_xfail else "FAIL"
        if p == float("inf"):
            psnr_str = "∞"
        else:
            psnr_str = f"{p:.2f} dB"
        # XPASS / FAIL count as failures; XFAIL is expected (RECOIL
        # limitation — our IFF is fine, viewer just can't render it).
        if ok in ("FAIL", "XPASS"):
            fails += 1
        rows.append((label, ok, psnr_str, str(d.resolve().relative_to(repo))))

    print()
    print(f"# RECOIL round-trip — {args.input}")
    print(f"# recoil2png: {recoil}")
    print(f"# threshold: {args.threshold:.1f} dB")
    print()
    print(f"{'combo':<28}  {'status':<14}  {'psnr':>10}  artifacts")
    print(f"{'-' * 28}  {'-' * 14}  {'-' * 10}  {'-' * 30}")
    for label, status, psnr, dirpath in rows:
        print(f"{label:<28}  {status:<14}  {psnr:>10}  {dirpath}")
    print()
    print(f"Total: {len(rows)} combos, {fails} failure(s).")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
