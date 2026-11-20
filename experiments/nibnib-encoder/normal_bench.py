#!/usr/bin/env python3
"""Compare normal mode using two builds of the small native encoder harness."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess

import numpy as np
from PIL import Image

p = argparse.ArgumentParser()
p.add_argument('--before', type=Path, required=True)
p.add_argument('--after', type=Path, required=True)
p.add_argument('--out', type=Path, default=Path('/tmp/nibnib-normal'))
p.add_argument('--results', type=Path, default=Path('experiments/nibnib-encoder/normal-results.json'))
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('check', 'tools/check_nibnib.py')
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)
check.test_image(a.out / 'gradient.png', 96, 72)
cases = [('examples/macaw.jpg', 160, 100), ('examples/logo.png', 160, 100),
         ('examples/macaw.jpg', 320, 200), ('examples/makena.jpg', 320, 200),
         ('examples/jungle.webp', 320, 200), ('examples/shooter.png', 160, 100),
         ('examples/brick.png', 160, 100), (str(a.out / 'gradient.png'), 96, 72)]
rows = []
for filename, w, h in cases:
    row = {'image': Path(filename).name, 'width': w, 'height': h}
    for label, binary in [('before', a.before), ('after', a.after)]:
        prefix = a.out / f'{Path(filename).stem}{w}-{label}'
        result = subprocess.run([str(binary.resolve()), filename, str(prefix), str(w), str(h)],
                                capture_output=True, text=True, check=True)
        row[label] = json.loads(result.stdout)
        decoded = check.decode(prefix.with_suffix('.nibnib').read_bytes())
        preview = np.asarray(Image.open(prefix.with_suffix('.png')).convert('RGB'))
        if not np.array_equal(decoded, preview):
            raise ValueError(f'decoder mismatch: {prefix}')
    row['s2_gain'] = round(row['after']['s2'] - row['before']['s2'], 4)
    row['speedup'] = round(row['before']['seconds'] / row['after']['seconds'], 3)
    if row['before']['bytes'] != row['after']['bytes']:
        raise ValueError('file size changed')
    rows.append(row)
    print(json.dumps(row), flush=True)
    a.results.write_text(json.dumps({'settings': {'dither': 'floyd-steinberg', 'strength': 0.76,
                        'error_clamp': 0.12, 'resize': 'Lanczos-3'},
                        'note': 'Sequential native runs; independent decoding checked for every output.',
                        'results': rows}, indent=2) + '\n')
