#!/usr/bin/env bash
# Run an exported Spectrum viewer using Hatari and its bundled EmuTOS.
set -euo pipefail
root="$(cd "$(dirname "$0")" && pwd)"
input="${1:?Usage: ./run-atari.sh picture.cpp|picture.exe|picture.prg [st|ste]}"
machine="${2:-ste}"
if [[ "$input" == *.cpp ]]; then
  "$root/build-atari.sh" "$input"
  input="${input%.*}.exe"
fi
hatari="${HATARI:-$(command -v hatari || true)}"
[[ -n "$hatari" ]] || { echo 'Install Hatari or set HATARI.' >&2; exit 1; }
tos="${HATARI_TOS:-$(dirname "$hatari")/../share/hatari/tos.img}"
[[ -f "$tos" ]] || { echo 'Set HATARI_TOS to your EmuTOS/TOS ROM.' >&2; exit 1; }
# TOS only auto-starts .PRG/.TOS; use a private mounted directory and 8.3 name.
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cp "$input" "$tmp/PICTURE.PRG"
"$hatari" --tos "$tos" --machine "$machine" --cpulevel 0 --cpuclock 8 \
  --monitor rgb --tos-res low --memsize 1 --sound off --confirm-quit false \
  --harddrive "$tmp" --auto 'C:\PICTURE.PRG'
