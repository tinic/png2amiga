#!/bin/sh
# Build a LoROM that shows one png2amiga Mode 7 frame.
#
#   build.sh frame.bin frame.pal out.sfc     # --mode snes-mode7-256
#   build.sh frame.bin - out.sfc             # --mode snes-mode7-direct
#
# Needs ca65/ld65 (cc65) and python3.
set -e
D=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
cp "$1" "$W/frame.bin"
if [ "$2" = "-" ]; then
    direct=1
    head -c 512 /dev/zero > "$W/frame.pal"
else
    direct=0
    cp "$2" "$W/frame.pal"
fi
(cd "$W" && ca65 --cpu 65816 -D DIRECT=$direct -o view.o "$D/view.s" \
    && ld65 -C "$D/lorom.cfg" -o out.sfc view.o)
# Header checksum, so emulators do not warn.
python3 - "$W/out.sfc" <<'PY'
import sys
p = sys.argv[1]
b = bytearray(open(p, 'rb').read())
b[0x7FDC:0x7FE0] = b'\xff\xff\x00\x00'
s = sum(b) & 0xFFFF
b[0x7FDC:0x7FDE] = (s ^ 0xFFFF).to_bytes(2, 'little')
b[0x7FDE:0x7FE0] = s.to_bytes(2, 'little')
open(p, 'wb').write(b)
PY
cp "$W/out.sfc" "$3"
