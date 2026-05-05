#!/usr/bin/env bash
# Full quality bench for the GPU quantizer experiment.
# Sweeps K ∈ {4, 8, 16, 32, 64, 128, 256} across DIV2K-100 + Kodak-24,
# A/B vs pngquant; output is a per-K aggregate showing mean / median /
# p10 / p90 ΔPSNR + ΔS2 (us − pngquant). The 100-image DIV2K sample is
# deterministic (sorted, every Nth file) so re-runs compare cleanly.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

DIV2K_DIR="${DIV2K_DIR:-${HOME}/png2amiga-testset/DIV2K_train_HR}"
KODAK_DIR="${KODAK_DIR:-${HOME}/png2amiga-testset/Kodak}"
DIV2K_N="${DIV2K_N:-100}"           # 100 randomly-sampled (deterministic)
KS="${KS:-4 8 16 32 64 128 256}"    # palette sizes to sweep
RESTARTS="${RESTARTS:-32}"
ITERS="${ITERS:-20}"
JOBS="${JOBS:-1}"                    # GPU contention => keep 1 by default
OUT_DIR="${OUT_DIR:-${HOME}/png2amiga-testset/gpu_quant_bench_$(date +%Y%m%d_%H%M%S)}"

PNGQUANT="$(command -v pngquant)"
PNG2AMIGA="$REPO_ROOT/build/png2amiga"
QUANT="$REPO_ROOT/experiments/gpu_quantize/quant"

if [[ ! -x "$QUANT" ]]; then
    echo "Building quant..." >&2
    "$REPO_ROOT/experiments/gpu_quantize/build.sh" > /dev/null
fi
if [[ ! -x "$PNG2AMIGA" ]]; then
    echo "Building png2amiga (release)..." >&2
    cmake --build "$REPO_ROOT/build" --target png2amiga --parallel > /dev/null
fi

mkdir -p "$OUT_DIR"
echo "OUT_DIR=$OUT_DIR" >&2

# Build deterministic 100-image DIV2K sample: every 8th file (800/100=8).
DIV2K_SAMPLE="$OUT_DIR/div2k_sample.txt"
ls "$DIV2K_DIR" | sort | awk "NR % $((800 / DIV2K_N)) == 1" > "$DIV2K_SAMPLE"
DIV2K_COUNT=$(wc -l < "$DIV2K_SAMPLE")
echo "div2k sample: $DIV2K_COUNT images (every $((800 / DIV2K_N))th)" >&2
KODAK_COUNT=$(ls "$KODAK_DIR"/*.png 2>/dev/null | wc -l)
echo "kodak: $KODAK_COUNT images" >&2

# Per-image work: run our quantizer, run pngquant, score both, append.
score_one() {
    local img_path="$1"     # absolute path to source PNG
    local set_label="$2"    # 'div2k' | 'kodak'
    local k="$3"
    local results_tsv="$4"

    local base
    base=$(basename "$img_path" .png)
    local us_out="$OUT_DIR/k${k}_${set_label}_${base}_us.png"
    local pq_out="$OUT_DIR/k${k}_${set_label}_${base}_pq.png"

    # Our quantizer (Stage A — pure Lloyd, no scolorq).
    "$QUANT" "$img_path" "$us_out" \
        --colors "$k" --restarts "$RESTARTS" --iters "$ITERS" \
        > /dev/null 2>&1

    # pngquant: --nofs disables Floyd-Steinberg dither so this is
    # quantizer-only A/B (matches existing harness).
    "$PNGQUANT" --speed 1 --nofs --output "$pq_out" "$k" "$img_path" \
        2> /dev/null

    local us_score pq_score
    us_score=$("$PNG2AMIGA" --score-vs "$img_path" "$us_out" 2>&1 \
        | awk '/scored, PSNR:/ {gsub(/[,]/,""); print $4, $7}')
    pq_score=$("$PNG2AMIGA" --score-vs "$img_path" "$pq_out" 2>&1 \
        | awk '/scored, PSNR:/ {gsub(/[,]/,""); print $4, $7}')

    local us_psnr=$(echo "$us_score" | awk '{print $1}')
    local us_s2=$(  echo "$us_score" | awk '{print $2}')
    local pq_psnr=$(echo "$pq_score" | awk '{print $1}')
    local pq_s2=$(  echo "$pq_score" | awk '{print $2}')

    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
        "$set_label" "$base" "$k" "$us_psnr" "$us_s2" "$pq_psnr" "$pq_s2" \
        >> "$results_tsv"

    # Cleanup intermediate PNGs to save disk.
    rm -f "$us_out" "$pq_out"
}

RESULTS="$OUT_DIR/results.tsv"
if [[ ! -s "$RESULTS" ]]; then
    printf "set\timage\tk\tus_psnr\tus_s2\tpq_psnr\tpq_s2\n" > "$RESULTS"
fi

for K in $KS; do
    echo "=== K=$K ===" >&2
    # DIV2K sample
    while IFS= read -r f; do
        score_one "$DIV2K_DIR/$f" "div2k" "$K" "$RESULTS"
    done < "$DIV2K_SAMPLE"
    # Kodak full
    for f in "$KODAK_DIR"/*.png; do
        [[ -f "$f" ]] || continue
        score_one "$f" "kodak" "$K" "$RESULTS"
    done
    # Per-K stats
    gawk -F'\t' -v k="$K" '
        NR > 1 && $3 == k {
            dpsnr = $4 - $6
            ds2   = $5 - $7
            n_psnr++; sum_psnr += dpsnr; psnrs[n_psnr] = dpsnr
            n_s2++;   sum_s2   += ds2;   ss2[n_s2]   = ds2
            wins_psnr += (dpsnr > 0)
            wins_s2   += (ds2 > 0)
        }
        END {
            if (n_s2 == 0) { print "  (no rows)"; exit }
            asort(psnrs); asort(ss2)
            mean_p = sum_psnr / n_psnr
            mean_s = sum_s2   / n_s2
            med_p  = (n_psnr % 2 == 1) ? psnrs[(n_psnr+1)/2] : (psnrs[n_psnr/2]+psnrs[n_psnr/2+1])/2
            med_s  = (n_s2   % 2 == 1) ? ss2[(n_s2+1)/2]   : (ss2[n_s2/2]+ss2[n_s2/2+1])/2
            p10 = ss2[int(n_s2 * 0.10)]; if (p10 == "") p10 = ss2[1]
            p90 = ss2[int(n_s2 * 0.90)]; if (p90 == "") p90 = ss2[n_s2]
            printf "  K=%d  N=%d  ΔPSNR mean=%+.2f median=%+.2f wins=%d  ΔS2 mean=%+.2f median=%+.2f p10=%+.2f p90=%+.2f wins=%d\n", \
                k, n_s2, mean_p, med_p, wins_psnr, mean_s, med_s, p10, p90, wins_s2
        }
    ' "$RESULTS" >&2
done

echo >&2
echo "=== summary table ===" >&2
echo "K   N    ΔPSNR_mean ΔPSNR_med ΔS2_mean ΔS2_med ΔS2_p10  ΔS2_p90  S2_wins/N" >&2
gawk -F'\t' 'NR > 1 {
    k = $3; dpsnr = $4 - $6; ds2 = $5 - $7
    nP[k]++; sP[k]+=dpsnr; pArr[k][nP[k]] = dpsnr
    nS[k]++; sS[k]+=ds2;   sArr[k][nS[k]] = ds2
    if (dpsnr > 0) wP[k]++
    if (ds2   > 0) wS[k]++
}
END {
    n_keys = asorti(nS, ks_sorted, "@val_num_asc")
    for (i = 1; i <= n_keys; ++i) {
        k = ks_sorted[i] + 0
        # Already in iteration order; just print
    }
    # Collect keys numeric-sorted
    delete ks_sorted
    n = asorti(nS, ks_sorted, "@ind_num_asc")
    for (i = 1; i <= n; ++i) {
        k = ks_sorted[i]
        nn = nS[k]
        if (!nn) continue
        # Sort copies for percentiles
        delete pCopy; delete sCopy
        for (j = 1; j <= nn; ++j) { pCopy[j] = pArr[k][j]; sCopy[j] = sArr[k][j] }
        asort(pCopy); asort(sCopy)
        med_p = (nn % 2 == 1) ? pCopy[(nn+1)/2] : (pCopy[nn/2]+pCopy[nn/2+1])/2
        med_s = (nn % 2 == 1) ? sCopy[(nn+1)/2] : (sCopy[nn/2]+sCopy[nn/2+1])/2
        p10 = sCopy[int(nn*0.10)]; if (p10 == "") p10 = sCopy[1]
        p90 = sCopy[int(nn*0.90)]; if (p90 == "") p90 = sCopy[nn]
        printf "%-3d %-4d %+10.2f %+9.2f %+8.2f %+7.2f %+8.2f %+8.2f %d/%d\n", \
            k, nn, sP[k]/nn, med_p, sS[k]/nn, med_s, p10, p90, wS[k]+0, nn
    }
}' "$RESULTS" | tee "$OUT_DIR/summary.txt" >&2

echo >&2
echo "raw results: $RESULTS" >&2
echo "summary:     $OUT_DIR/summary.txt" >&2
