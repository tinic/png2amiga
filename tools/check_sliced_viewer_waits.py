#!/usr/bin/env python3
"""Check that generated sliced viewers defer next-row writes past visible pixels.

Inspect both interlace field loops and reject the old early line-255 special case.
This validates the exported viewer contract, not cycle-accurate Amiga execution.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--cli', required=True)
p.add_argument('--in', dest='source', required=True)
p.add_argument('--lace', action='store_true')
p.add_argument('extra', nargs=argparse.REMAINDER)
a = p.parse_args()
extra = a.extra[1:] if a.extra[:1] == ['--'] else a.extra
with tempfile.TemporaryDirectory() as directory:
    out = Path(directory) / 'viewer.cpp'
    subprocess.run([a.cli, '--sliced', '--width', '320', '--height',
                    '448' if a.lace else '224', *extra, a.source, str(out)],
                   check=True, capture_output=True)
    text = out.read_text()
    labels = ['// Per-scanline copper palette changes (end of previous line)']
    if a.lace:
        labels.append('// Field 2 per-scanline palette changes')
    for label in labels:
        start = text.index('{', text.index('for (int y = 1;', text.index(label)))
        depth = 1
        end = start + 1
        while depth:
            depth += (text[end] == '{') - (text[end] == '}')
            end += 1
        loop = text[start:end]
        waits = re.findall(r'\(\(line & 0xFF\) << 8\) \| (0x[0-9A-Fa-f]+)', loop)
        assert waits, 'Missing per-line vertical/horizontal WAIT'
        minimum = 0xE2 if a.lace else 0xE0
        assert all(int(w, 16) & 0xFE >= minimum for w in waits), 'Palette reset begins too early'
        assert not re.search(r'\*cl2?\+\+\s*=\s*0xFFDF\b', loop, re.I), 'Line 255 resets too early'
print('PASS: palette reset waits stay beyond the visible right edge in every field')
