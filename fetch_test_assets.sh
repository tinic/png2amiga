#!/usr/bin/env bash
# Build a test corpus for the Amiga asset packer from public GitHub repos.
# Idempotent — already-downloaded files are skipped.
#
# Layout under ./test_assets/:
#   aligned/        16-px-aligned, Amiga-friendly inputs (clean path)
#     turrican2/      tilesets + sprite strips
#     ninja/          16-px character strips + tilesets
#     tilengine/      SNES/Genesis reference sets
#     lpc/            32/64-px grids, large palette
#   unaligned/      non-aligned / full-color (packer + quantize stress)
#     hurrican/       diverse sizes, mixed alignment
#     superpowers/    caveman, NPCs, FX
#     opensurge/      sonic-style mix

# No -e: individual fetch failures shouldn't abort the whole corpus build.
set -u

ROOT="${ROOT:-$(pwd)/test_assets}"
mkdir -p "$ROOT"

fetch() {
  local url="$1" out="$2"
  [ -f "$out" ] && { return 0; }
  mkdir -p "$(dirname "$out")"
  if curl -sfL -o "$out.tmp" "$url" 2>/dev/null && [ -s "$out.tmp" ]; then
    mv "$out.tmp" "$out"
  else
    rm -f "$out.tmp"
    echo "FAIL $url" >&2
    return 1
  fi
}

# Fetch every PNG from a GitHub directory via the contents API.
# Usage: fetch_dir OWNER/REPO PATH LOCAL_DEST [LIMIT]
fetch_dir() {
  local repo="$1" path="$2" dest="$3" limit="${4:-0}"
  mkdir -p "$dest"
  local listing
  listing=$(gh api "repos/$repo/contents/$path" 2>/dev/null) || {
    echo "FAIL listing $repo/$path" >&2; return 1
  }
  local count=0
  while IFS=$'\t' read -r name download; do
    [[ "$name" == *.png ]] || continue
    fetch "$download" "$dest/$name"
    count=$((count+1))
    [ "$limit" -gt 0 ] && [ "$count" -ge "$limit" ] && break
  done < <(echo "$listing" | python3 -c "
import sys, json
for x in json.load(sys.stdin):
    if x.get('download_url'):
        print(f\"{x['name']}\t{x['download_url']}\")")
}

echo "== aligned/turrican2 =="
fetch_dir Josef-Friedrich/turrican-clone-assets-collection \
          turrican2/graphics "$ROOT/aligned/turrican2/sprites"
# Tilesets only (skip the giant full-level 1-1.png etc.)
gh api repos/Josef-Friedrich/turrican-clone-assets-collection/contents/turrican2/map 2>/dev/null \
  | python3 -c "
import sys, json
for x in json.load(sys.stdin):
    if x['name'].endswith('_tileset.png'):
        print(f\"{x['name']}\t{x['download_url']}\")" \
  | while IFS=$'\t' read -r name url; do
      fetch "$url" "$ROOT/aligned/turrican2/tilesets/$name"
    done

echo "== aligned/ninja =="
# NinjaAdventure: tilesets from map/, character strips that actually exist
NINJA_TILES=(tileset_floor.png tileset_wall_simple.png)
for f in "${NINJA_TILES[@]}"; do
  fetch "https://raw.githubusercontent.com/pixel-boy/NinjaAdventure/main/content/map/$f" \
        "$ROOT/aligned/ninja/tilesets/$f"
done
for ch in ninja_blue pig samurai_blue samurai_green; do
  fetch "https://raw.githubusercontent.com/pixel-boy/NinjaAdventure/main/content/character/$ch/sprite.png" \
        "$ROOT/aligned/ninja/chars/${ch}.png"
done

echo "== aligned/tilengine =="
# Tilengine has several reference asset sets (sotb = Shadow Of The Beast!)
TILENGINE_FILES=(
  "forest/tileset.png"
  "forest/house.png"
  "forest/tree.png"
  "forest/atlas.png"
  "sonic/Base.png"
  "smw/smw_background.png"
  "sotb/SOTB_bg.png"
  "sotb/SOTB_fg.png"
  "sc4/castle_bg.png"
  "sc4/castle_fg.png"
  "sc4/Simon.png"
)
for f in "${TILENGINE_FILES[@]}"; do
  fetch "https://raw.githubusercontent.com/megamarc/Tilengine/master/samples/assets/$f" \
        "$ROOT/aligned/tilengine/$(basename "$f")"
done

echo "== aligned/lpc =="
# LPC: a handful of terrain + cliff sheets (varied foliage/rock)
LPC_FILES=(
  "Terrain/cliff_summer.png"
  "Terrain/cliff_winter.png"
  "Terrain/cliff_autumn.png"
  "Terrain/flowers.png"
  "Terrain/plants_summer.png"
  "Terrain/Waterfall.png"
)
for f in "${LPC_FILES[@]}"; do
  fetch "https://raw.githubusercontent.com/ElizaWy/LPC/main/$f" \
        "$ROOT/aligned/lpc/$(basename "$f")"
done

echo "== unaligned/hurrican =="
# 30 diverse files from Hurrican textures (small, medium, large)
HURRICAN_FILES=(
  arcshot.png beamsmoke.png beamsmoke2.png Zitronestiel.png blatt.png
  auge.png bigfish.png blauebombe.png bigfishflosseklein.png bigfishflosseoben.png
  bigfishmaul.png blitzbeam.png ballerdrone.png bigfishflossebig.png blitzflash1.png
  bigrocket.png alienexplosion.png blitzstrahl1.png blitzstrahl2.png blauebombe.png
)
for f in "${HURRICAN_FILES[@]}"; do
  fetch "https://raw.githubusercontent.com/HurricanGame/Hurrican/master/Hurrican/data/textures/$f" \
        "$ROOT/unaligned/hurrican/$f"
done

echo "== unaligned/superpowers =="
SUPER_FILES=(
  "characters/playable/caverman.png"
  "characters/npc/blacksmith.png"
  "characters/npc/hunter.png"
  "characters/npc/warrior.png"
  "monsters/dragon-1.png"
  "monsters/tyrannosaurus-1.png"
  "monsters/bat-1.png"
  "monsters/pterodactyl-1.png"
  "font-20x20.png"
)
for f in "${SUPER_FILES[@]}"; do
  fetch "https://raw.githubusercontent.com/sparklinlabs/superpowers-asset-packs/master/prehistoric-platformer/$f" \
        "$ROOT/unaligned/superpowers/$(basename "$f")"
done

echo "== unaligned/opensurge =="
# Using names that actually exist (checked via gh api)
OPENSURGE_FILES=(
  "violet.png" "salamander_boss.png" "neon.png" "surge.png"
  "giant_wolf.png" "tux.png" "hydra.png" "item_boxes.png"
  "charge.png" "bugsy.png"
)
for f in "${OPENSURGE_FILES[@]}"; do
  fetch "https://raw.githubusercontent.com/alemart/opensurge/master/images/$f" \
        "$ROOT/unaligned/opensurge/$f"
done

echo ""
echo "== Summary =="
find "$ROOT" -name '*.png' | wc -l | awk '{print $1" PNGs total"}'
for d in "$ROOT"/*/*/; do
  n=$(find "$d" -name '*.png' | wc -l | tr -d ' ')
  sz=$(du -sh "$d" 2>/dev/null | awk '{print $1}')
  printf "  %-40s %4s PNG  %s\n" "${d#$ROOT/}" "$n" "$sz"
done

echo ""
echo "Manifest: $ROOT/MANIFEST.txt"
{
  echo "# png2amiga asset test corpus"
  echo "# generated $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  find "$ROOT" -name '*.png' | sort | while read -r f; do
    info=$(magick identify -format "%wx%h %k colors" "$f" 2>/dev/null || echo "?")
    printf "%-70s  %s\n" "${f#$ROOT/}" "$info"
  done
} > "$ROOT/MANIFEST.txt"
echo "Done."
