#!/usr/bin/env bash
# HAM8 (AGA, 64 base + MODIFY ops) A/B: default PNN vs gpu-restart.
# HAM's base palette objective differs from regular quantization
# (pixels are reached via SET-anchor + MODIFY, not directly), so
# even though gpu-restart wins on plain AGA palette quantization,
# it may not transfer to HAM8 where PNN's Ward's-linkage anchors
# are tuned for the right cost function.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PNG2AMIGA=$REPO_ROOT/build/png2amiga
DIV2K=${DIV2K:-${HOME}/png2amiga-testset/DIV2K_train_HR}
N=${N:-20}
OUT=$(mktemp -d -t ham8_bench.XXXXXX)
RESULTS=$OUT/results.tsv
echo "out=$OUT"
printf "image\tpnn_psnr\tpnn_s2\tgpu_psnr\tgpu_s2\tdpsnr\tds2\n" > "$RESULTS"

mapfile -t SAMPLE < <(ls "$DIV2K" | sort | awk "NR % $((800 / N)) == 1" | head -n "$N")
echo "sample size: ${#SAMPLE[@]}"

run_score() {
    local img=$1 quantizer=$2
    "$PNG2AMIGA" --mode ham8 --width 320 --chipset aga \
        --quantizer "$quantizer" \
        "$DIV2K/$img" "$OUT/${quantizer}_${img%.png}.iff" 2>&1 \
        | awk '/PSNR:/ {gsub(/[,]/,""); for(i=1;i<=NF;++i) if($i=="PSNR:") {print $(i+1), $(i+4); exit}}'
}

for img in "${SAMPLE[@]}"; do
    base=${img%.png}
    pnn_score=$(run_score "$img" pnn)
    gpu_score=$(run_score "$img" gpu-restart)
    pnn_psnr=$(echo "$pnn_score" | awk '{print $1}')
    pnn_s2=$(  echo "$pnn_score" | awk '{print $2}')
    gpu_psnr=$(echo "$gpu_score" | awk '{print $1}')
    gpu_s2=$(  echo "$gpu_score" | awk '{print $2}')
    dpsnr=$(awk -v a="$gpu_psnr" -v b="$pnn_psnr" 'BEGIN{printf "%.2f",a-b}')
    ds2=$(  awk -v a="$gpu_s2"   -v b="$pnn_s2"   'BEGIN{printf "%.2f",a-b}')
    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
        "$base" "$pnn_psnr" "$pnn_s2" "$gpu_psnr" "$gpu_s2" "$dpsnr" "$ds2" \
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
    printf "\nMEAN over %d images (gpu − pnn):\n  ΔPSNR = %+.2f dB (median %+.2f, wins %d/%d)\n  ΔS2   = %+.2f    (median %+.2f, wins %d/%d)\n", \
        nS, sP/nP, med_p, wP+0, nP, sS/nS, med_s, wS+0, nS
}' "$RESULTS"
