#!/usr/bin/env bash
# Plain EHB (32 base + 32 half-brite, OCS 12-bit) A/B:
# default ocs-bruteforce vs gpu-restart (snapped to OCS post-Lloyd).
# Shows whether the OKLab + parallel-restart approach helps on a
# discrete 4096-color gamut. Hypothesis: probably not (continuous
# centroids snap suboptimally), but actually testing it.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PNG2AMIGA=$REPO_ROOT/build/png2amiga
DIV2K=${DIV2K:-${HOME}/png2amiga-testset/DIV2K_train_HR}
N=${N:-20}
OUT=$(mktemp -d -t ehb_bench.XXXXXX)
RESULTS=$OUT/results.tsv
echo "out=$OUT"
printf "image\tocs_psnr\tocs_s2\tgpu_psnr\tgpu_s2\tdpsnr\tds2\n" > "$RESULTS"

# Deterministic 20-image sample from DIV2K (every 40th of 800).
mapfile -t SAMPLE < <(ls "$DIV2K" | sort | awk "NR % $((800 / N)) == 1" | head -n "$N")
echo "sample size: ${#SAMPLE[@]}"

run_score() {
    local img=$1 quantizer=$2
    # Plain EHB hardcodes PNN; --quantizer is ignored there. Use
    # --mode lores --depth 5 --chipset ocs which is the same
    # quantization problem (pick 32 OCS-12bit colors) and respects
    # the --quantizer flag via main.cpp's quantize_palette dispatch.
    "$PNG2AMIGA" --mode lores --depth 5 --width 320 --chipset ocs \
        --quantizer "$quantizer" \
        "$DIV2K/$img" "$OUT/${quantizer}_${img%.png}.iff" 2>&1 \
        | awk '/PSNR:/ {gsub(/[,]/,""); for(i=1;i<=NF;++i) if($i=="PSNR:") {print $(i+1), $(i+4); exit}}'
}

for img in "${SAMPLE[@]}"; do
    base=${img%.png}
    ocs_score=$(run_score "$img" ocs-bruteforce)
    gpu_score=$(run_score "$img" gpu-restart)
    ocs_psnr=$(echo "$ocs_score" | awk '{print $1}')
    ocs_s2=$(  echo "$ocs_score" | awk '{print $2}')
    gpu_psnr=$(echo "$gpu_score" | awk '{print $1}')
    gpu_s2=$(  echo "$gpu_score" | awk '{print $2}')
    dpsnr=$(awk -v a="$gpu_psnr" -v b="$ocs_psnr" 'BEGIN{printf "%.2f",a-b}')
    ds2=$(  awk -v a="$gpu_s2"   -v b="$ocs_s2"   'BEGIN{printf "%.2f",a-b}')
    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
        "$base" "$ocs_psnr" "$ocs_s2" "$gpu_psnr" "$gpu_s2" "$dpsnr" "$ds2" \
        | tee -a "$RESULTS"
done

echo
gawk -F'\t' 'NR > 1 {
    nP++; sP += $6; pArr[nP] = $6
    nS++; sS += $7; sArr[nS] = $7
    if ($6 > 0) wP++
    if ($7 > 0) wS++
}
END {
    if (nS == 0) exit
    asort(pArr); asort(sArr)
    med_p = (nP%2==1) ? pArr[(nP+1)/2] : (pArr[nP/2]+pArr[nP/2+1])/2
    med_s = (nS%2==1) ? sArr[(nS+1)/2] : (sArr[nS/2]+sArr[nS/2+1])/2
    printf "\nMEAN over %d images:\n  ΔPSNR = %+.2f dB (median %+.2f, wins %d/%d)\n  ΔS2   = %+.2f    (median %+.2f, wins %d/%d)\n", \
        nS, sP/nP, med_p, wP+0, nP, sS/nS, med_s, wS+0, nS
}' "$RESULTS"
