#!/usr/bin/env bash
# Build the exported GNU C++/68000 viewer into an absolute TOS PRG.
set -euo pipefail
root="$(cd "$(dirname "$0")" && pwd)"
input="${1:?Usage: ./build-atari.sh picture.cpp [picture.exe]}"
output="${2:-${input%.*}.exe}"
prefix="${M68K_PREFIX:-$root/third_party/vscode-amiga-debug/bin/darwin/opt/bin/m68k-amiga-elf-}"
if [[ ! -x "${prefix}gcc" ]]; then
  echo 'Set M68K_PREFIX to a GNU m68k toolchain prefix (for example m68k-elf-).' >&2
  exit 1
fi
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
"${prefix}gcc" -m68000 -fno-exceptions -fno-rtti -c "$input" -o "$tmp/viewer.o"
"${prefix}ld" -Ttext=0 --oformat=binary "$tmp/viewer.o" -o "$tmp/viewer.bin"
python3 - "$tmp/viewer.bin" "$output" <<'PY'
import pathlib,struct,sys
b=pathlib.Path(sys.argv[1]).read_bytes()
pathlib.Path(sys.argv[2]).write_bytes(struct.pack('>H6IH',0x601a,len(b),0,33024,0,0,0,1)+b)
print(f'{sys.argv[2]}: {len(b)+28} bytes (Atari TOS executable)')
PY
