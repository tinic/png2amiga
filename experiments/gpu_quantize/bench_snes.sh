#!/usr/bin/env bash
# SNES Mode 7 256-color A/B: median-cut (former default) vs
# gpu-restart (new default when Metal is up). SNES gamut is
# BGR555 (5 bits/channel = 32768 colors total); 256 of 32K is
# 0.78% pick density, so snap-collapse risk is low — gpu-restart
# might transfer well from AGA continuous-RGB.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PNG2AMIGA=$REPO_ROOT/build/png2amiga
DIV2K=${DIV2K:-${HOME}/png2amiga-testset/DIV2K_train_HR}
N=${N:-20}
OUT=$(mktemp -d -t snes_bench.XXXXXX)
RESULTS=$OUT/results.tsv
echo "out=$OUT"
printf "image\tmc_psnr\tmc_s2\tgpu_psnr\tgpu_s2\tdpsnr\tds2\n" > "$RESULTS"

mapfile -t SAMPLE < <(ls "$DIV2K" | sort | awk "NR % $((800 / N)) == 1" | head -n "$N")
echo "sample size: ${#SAMPLE[@]}"

# We A/B by toggling the snes_io.cpp default. Easier path: run
# both binaries (current default = gpu-restart) but force the
# alternate via two separate builds — too heavy. Instead we
# bench the *current* gpu-restart against `--quantizer median-cut`
# IF that flag flows through SNES. It doesn't (snes_io.cpp:290
# is hardcoded), so we directly compare:
#   binary-as-built (gpu-restart)  vs  one-shot disable
# by setting METAL=0 envvar to force runtime fallback.
# (Without explicit env support, simpler: bench the new default
# only and compare against the prior commit's median-cut numbers
# captured here for reference.)
#
# Implementation: assume the binary is built with gpu-restart
# default. Run snes-mode7-256 once per image, capture PSNR/S2.
# Then run again forcing CPU fallback by setting an env var that
# we'll add to MetalContext (TODO).
#
# Quick path: A/B by RE-BUILDING with the alternate default and
# RE-RUNNING. Drive that from a separate harness; for now this
# script just runs gpu-restart and prints results so we can
# eyeball them against the median-cut numbers we'd see from the
# prior commit.

run_score() {
    local img=$1 var=$2  # var unused in single-binary mode
    "$PNG2AMIGA" --mode snes-mode7-256 --width 256 \
        "$DIV2K/$img" "$OUT/${var}_${img%.png}.bin" 2>&1 \
        | awk '/PSNR:/ {gsub(/[,]/,""); for(i=1;i<=NF;++i) if($i=="PSNR:") {print $(i+1), $(i+4); exit}}'
}

for img in "${SAMPLE[@]}"; do
    base=${img%.png}
    score=$(run_score "$img" gpu)
    psnr=$(echo "$score" | awk '{print $1}')
    s2=$(  echo "$score" | awk '{print $2}')
    printf "%s\t%s\t%s\n" "$base" "$psnr" "$s2" | tee -a "$OUT/gpu_only.tsv"
done
