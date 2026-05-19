#!/usr/bin/env bash
# Compare png2amiga vs astcenc -medium on synthetic content patterns to
# isolate where the S2 gap lives. All images are 240x240 (20 cols x 20
# rows of 12x12 blocks) so the patterns can be aligned with block grid.
set -eu

ASTCENC=/tmp/astcenc-build/Source/astcenc-neon
P2A=/Users/turo/png2amiga/build/png2amiga
K2P=/Users/turo/png2amiga/build/ktx2_to_png

WD=/tmp/astc_synth
mkdir -p "$WD"

W=240
H=240

# Generate synthetic test images via ImageMagick.
generate() {
    case "$1" in
    smooth-h)  # smooth horizontal gradient
        magick -size ${W}x${H} gradient:black-white "$WD/$1.png" ;;
    smooth-v)  # smooth vertical gradient
        magick -size ${W}x${H} gradient:black-white -rotate 90 "$WD/$1.png" ;;
    smooth-diag)  # diagonal gradient (anti-aligned with 12x12)
        magick -size ${W}x${H} radial-gradient:black-white "$WD/$1.png" ;;
    sharp-h-aligned)  # sharp horizontal edge at y=120 (block-aligned)
        magick -size ${W}x${H} canvas:black -fill white -draw "rectangle 0,0 ${W},119" "$WD/$1.png" ;;
    sharp-h-mid)  # sharp horizontal edge at y=125 (inside block row 10)
        magick -size ${W}x${H} canvas:black -fill white -draw "rectangle 0,0 ${W},125" "$WD/$1.png" ;;
    sharp-v-aligned)  # sharp vertical edge at x=120 (block-aligned)
        magick -size ${W}x${H} canvas:black -fill white -draw "rectangle 0,0 119,${H}" "$WD/$1.png" ;;
    sharp-v-mid)  # sharp vertical edge at x=125 (inside block col 10)
        magick -size ${W}x${H} canvas:black -fill white -draw "rectangle 0,0 125,${H}" "$WD/$1.png" ;;
    checker)  # 1-pixel checker (worst case for any block compressor)
        magick -size ${W}x${H} pattern:checkerboard -monochrome "$WD/$1.png" ;;
    checker12)  # 12-pixel checker (block-aligned)
        magick -size 12x12 pattern:checkerboard -monochrome -scale 2000% -extent ${W}x${H} "$WD/$1.png" ;;
    rgb-stripes)  # vertical RGB color stripes 20 px wide
        magick -size 20x${H} xc:red \
            \( -size 20x${H} xc:green \) \
            \( -size 20x${H} xc:blue \) +append \
            -resize ${W}x${H}! "$WD/$1.png" ;;
    skin-tone)  # narrow-gamut skin-tone gradient (chroma-poor)
        magick -size ${W}x${H} gradient:'#ffd5b4'-'#5a3a2a' "$WD/$1.png" ;;
    bimodal)  # 4 quadrants of distinct colors (tests 2-partition)
        magick -size 120x120 canvas:'#ff0000' \
            \( -size 120x120 canvas:'#00ff00' \) +append \
            \( -size 120x120 canvas:'#0000ff' \
               -size 120x120 canvas:'#ffff00' +append \) -append \
            "$WD/$1.png" ;;
    noise)  # uniform random noise
        magick -size ${W}x${H} xc: +noise random "$WD/$1.png" ;;
    lo-freq)  # smoothly varying multicolor (test endpoint flexibility on smooth multi-hue)
        magick -size ${W}x${H} gradient:'#ff8040'-'#4080ff' "$WD/$1.png" ;;
    hi-freq)  # 3-pixel checker with 3 colors (red/green/blue)
        magick -size 6x6 \( -size 2x2 xc:red -size 2x2 xc:green +append \
                          -size 2x2 xc:blue -size 2x2 xc:yellow +append \) \
            -append -scale 4000% -extent ${W}x${H} "$WD/$1.png" ;;
    mixed)  # smooth gradient with overlaid horizontal lines every 6 px
        magick -size ${W}x${H} gradient:black-white \
            \( -size ${W}x${H} pattern:horizontal2 -evaluate multiply 0.3 \
               -compose lighten \) -composite "$WD/$1.png" ;;
    multi-color)  # 4 distinct color quadrants with smooth blend at edges
        magick -size ${W}x${H} radial-gradient:'#ff4040-#404040' \
            \( -size ${W}x${H} radial-gradient:'#40ff40-#404040' -compose lighten -composite \) \
            \( -size ${W}x${H} radial-gradient:'#4040ff-#404040' -compose lighten -composite \) \
            "$WD/$1.png" ;;
    foliage)  # green-dominated mid-freq texture, simulates real-image foliage
        magick -size ${W}x${H} plasma:'#3a6b29-#7fa658' "$WD/$1.png" ;;
    hair-like)  # vertical 1-2 pixel thin lines on grey bg (simulates hair strands)
        magick -size ${W}x${H} xc:grey50 -fill black \
            -draw "line 30,0 30,${H} line 60,0 60,${H} line 90,0 90,${H} line 120,0 120,${H} \
                   line 150,0 150,${H} line 180,0 180,${H} line 210,0 210,${H} \
                   line 33,0 33,${H} line 63,0 63,${H} line 95,0 96,${H}" \
            "$WD/$1.png" ;;
    *) echo "unknown: $1"; return 1 ;;
    esac
}

printf '%-22s | %5s %6s | %5s %6s | %+7s %+7s\n' \
    'pattern' 'p2a_p' 'p2a_s' 'ref_p' 'ref_s' 'dPSNR' 'dS2'
printf '%s\n' '----------------------+--------------+---------------+----------------'

for P in smooth-h smooth-v smooth-diag \
         sharp-h-aligned sharp-h-mid sharp-v-aligned sharp-v-mid \
         checker checker12 rgb-stripes skin-tone bimodal noise \
         lo-freq hi-freq mixed multi-color foliage hair-like; do
    generate "$P"

    $P2A --mode astc-12x12 "$WD/$P.png" -o "$WD/$P-p2a.ktx2" --no-ktx2-zstd > "$WD/$P-p2a.log" 2>&1
    $K2P "$WD/$P-p2a.ktx2" "$WD/$P-p2a.png" > /dev/null 2>&1
    p2a_p=$($P2A --score-vs "$WD/$P.png" "$WD/$P-p2a.png" 2>&1 | grep -oE 'PSNR: [^ ]+' | awk '{print $2}')
    p2a_s=$($P2A --score-vs "$WD/$P.png" "$WD/$P-p2a.png" 2>&1 | grep -oE 'S2: [^,]+' | awk '{print $2}')
    [[ "$p2a_p" == "inf" ]] && p2a_p="999.99"

    $ASTCENC -ts "$WD/$P.png" "$WD/$P-ref.png" 12x12 -medium > "$WD/$P-ref.log" 2>&1
    ref_p=$($P2A --score-vs "$WD/$P.png" "$WD/$P-ref.png" 2>&1 | grep -oE 'PSNR: [^ ]+' | awk '{print $2}')
    ref_s=$($P2A --score-vs "$WD/$P.png" "$WD/$P-ref.png" 2>&1 | grep -oE 'S2: [^,]+' | awk '{print $2}')
    [[ "$ref_p" == "inf" ]] && ref_p="999.99"

    d_p=$(echo "$p2a_p - $ref_p" | bc)
    d_s=$(echo "$p2a_s - $ref_s" | bc)
    printf '%-22s | %5.2f %6.2f | %5.2f %6.2f | %+7.2f %+7.2f\n' \
        "$P" "$p2a_p" "$p2a_s" "$ref_p" "$ref_s" "$d_p" "$d_s"
done
