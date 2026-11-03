#!/usr/bin/env python3
"""Build the seed ablation against an existing native Ninja build."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--build', type=Path, default=Path('build'))
p.add_argument('--out', type=Path, default=Path('/tmp/nibnib-encoder'))
p.add_argument('--source', choices=['sweep', 'encode'], default='sweep')
p.add_argument('--revision', help='use src/nibnib.cpp from this Git revision')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
build, out = a.build.resolve(), a.out.resolve()
out.mkdir(parents=True, exist_ok=True)
entries = json.loads((build / 'compile_commands.json').read_text())
entry = next(e for e in entries if e['file'].endswith('/src/nibnib.cpp')
             and 'CMakeFiles/png2amiga.dir/' in e['command'])
args = shlex.split(entry['command'])
obj = out / (a.source + '.o')
args[args.index('-o') + 1] = str(obj)
args[-1] = str(root / f'experiments/nibnib-encoder/{a.source}.cpp')
if a.revision:
    baseline = out / 'baseline-nibnib.cpp'
    baseline.write_bytes(subprocess.check_output(['git', 'show', f'{a.revision}:src/nibnib.cpp'], cwd=root))
    args.insert(1, f'-DNIBNIB_SOURCE="{baseline}"')
subprocess.run(args, cwd=build, check=True)
commands = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', 'png2amiga'], text=True)
link = shlex.split(commands.splitlines()[-1].split(' && ')[1])
link.remove('CMakeFiles/png2amiga.dir/src/main.cpp.o')
link[link.index('CMakeFiles/png2amiga.dir/src/nibnib.cpp.o')] = str(obj)
binary = out / ('seed-sweep' if a.source == 'sweep' else 'normal-encode')
link[link.index('-o') + 1] = str(binary)
subprocess.run(link, cwd=build, check=True)
print(binary)
