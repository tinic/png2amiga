#!/usr/bin/env bash
# A/B bench: experiments/gpu_quantize/quant vs pngquant on N DIV2K images.
# Prints a compact TSV (image, our_psnr, our_s2, pq_psnr, pq_s2, dpsnr, ds2)
# plus mean deltas at the end.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

INPUT_DIR="${INPUT_DIR:-${HOME}/png2amiga-testset/DIV2K_train_HR}"
N="${N:-10}"
COLORS="${COLORS:-256}"
RESTARTS="${RESTARTS:-32}"
ITERS="${ITERS:-20}"

PNGQUANT="$(command -v pngquant)"
PNG2AMIGA="$REPO_ROOT/build/png2amiga"
QUANT="$REPO_ROOT/experiments/gpu_quantize/quant"

OUT="$(mktemp -d -t gpuq.XXXXXX)"
echo "out: $OUT"
echo "config: N=$N colors=$COLORS restarts=$RESTARTS iters=$ITERS"
printf "image\tour_psnr\tour_s2\tpq_psnr\tpq_s2\tdpsnr\tds2\n"

sum_dpsnr=0
sum_ds2=0
count=0

for f in $(find "$INPUT_DIR" -name '*.png' | sort | head -n "$N"); do
    base=$(basename "$f" .png)
    # Our quantizer.
    "$QUANT" "$f" "$OUT/${base}_us.png" \
        --colors "$COLORS" --restarts "$RESTARTS" --iters "$ITERS" \
        ${SCOLORQ:+--scolorq "$SCOLORQ"} \
        > /dev/null 2>&1
    # pngquant (no FS dither for quantizer-only A/B).
    "$PNGQUANT" --speed 1 --nofs --output "$OUT/${base}_pq.png" \
        "$COLORS" "$f" 2> /dev/null

    # Score both via png2amiga --score-vs.
    our_score=$("$PNG2AMIGA" --score-vs "$f" "$OUT/${base}_us.png" 2>&1 \
        | awk '/scored, PSNR:/ {gsub(/[,]/,""); print $4, $7}')
    pq_score=$("$PNG2AMIGA" --score-vs "$f" "$OUT/${base}_pq.png" 2>&1 \
        | awk '/scored, PSNR:/ {gsub(/[,]/,""); print $4, $7}')
    our_psnr=$(echo "$our_score" | awk '{print $1}')
    our_s2=$(  echo "$our_score" | awk '{print $2}')
    pq_psnr=$( echo "$pq_score"  | awk '{print $1}')
    pq_s2=$(   echo "$pq_score"  | awk '{print $2}')
    dpsnr=$(awk -v a="$our_psnr" -v b="$pq_psnr" 'BEGIN{printf "%.2f",a-b}')
    ds2=$(  awk -v a="$our_s2"   -v b="$pq_s2"   'BEGIN{printf "%.2f",a-b}')

    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
        "$base" "$our_psnr" "$our_s2" "$pq_psnr" "$pq_s2" "$dpsnr" "$ds2"
    sum_dpsnr=$(awk -v a="$sum_dpsnr" -v b="$dpsnr" 'BEGIN{printf "%.4f",a+b}')
    sum_ds2=$(  awk -v a="$sum_ds2"   -v b="$ds2"   'BEGIN{printf "%.4f",a+b}')
    count=$((count + 1))
done

if [[ $count -gt 0 ]]; then
    mean_dpsnr=$(awk -v s="$sum_dpsnr" -v n="$count" 'BEGIN{printf "%.2f",s/n}')
    mean_ds2=$(  awk -v s="$sum_ds2"   -v n="$count" 'BEGIN{printf "%.2f",s/n}')
    echo "----"
    printf "MEAN over %d images: ΔPSNR=%s dB  ΔS2=%s\n" \
        "$count" "$mean_dpsnr" "$mean_ds2"
fi
