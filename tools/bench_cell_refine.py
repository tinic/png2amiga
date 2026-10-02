#!/usr/bin/env python3
"""Compare default vs --cell-refine; save S2, timing, previews, and an HTML gallery."""
import argparse
import html
import json
from pathlib import Path
import re
import subprocess
import time

MODES = ['thomson-to7-320x16', 'cga-text80x200', 'cga-text80x100',
         'cga-text80x50', 'cga-text80x25', 'cga-text40x200', 'cga-text40x100',
         'thomson-to8-320x16', 'c64-afli', 'c64-hires', 'ted-hires', 'c64-petscii',
         'c64-multicolor', 'c64-fli', 'ted-multicolor',
         'c64-charset-hires', 'c64-charset-multicolor']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', type=Path, default=Path('build/png2amiga'))
    parser.add_argument('--out', type=Path, default=Path('build/cell-refine/final'))
    parser.add_argument('--examples', type=Path, default=Path('examples'))
    parser.add_argument('--modes', nargs='+', choices=MODES, default=MODES)
    parser.add_argument('--only', nargs='+', help='Source stems to include')
    parser.add_argument('--dither', help='Override the initial dither for both variants')
    parser.add_argument('--resume', action='store_true', help='Reuse completed rows in the output directory')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    sources = sorted(p for p in args.examples.iterdir()
                     if p.suffix.lower() in {'.png', '.jpg', '.jpeg', '.webp'}
                     and (not args.only or p.stem in args.only))
    if not sources:
        parser.error('No source images matched')
    result_path = args.out / 'results.json'
    rows = json.loads(result_path.read_text()) if args.resume and result_path.exists() else []
    if any(r.get('dither') != args.dither for r in rows):
        parser.error('Existing results use another dither; choose a separate output directory')
    completed = {(r['mode'], r['image']) for r in rows}
    for mode in args.modes:
        for source in sources:
            if (mode, source.name) in completed:
                continue
            row = dict(mode=mode, image=source.name, dither=args.dither)
            for label, flags in [('before', []), ('after', ['--cell-refine'])]:
                name = f'{mode}-{source.stem}-{label}.png'
                start = time.perf_counter()
                result = subprocess.run([str(args.cli.resolve()), '--mode', mode, *flags,
                                         *(['--dither', args.dither] if args.dither else []),
                                         str(source), str(args.out / name)],
                                        capture_output=True, text=True, check=True)
                elapsed = time.perf_counter() - start
                log = result.stdout + result.stderr
                (args.out / name).with_suffix('.log').write_text(log)
                score = float(re.findall(r'S2:\s*(-?\d+(?:\.\d+)?)', log)[-1])
                row[label] = dict(s2=score, seconds=round(elapsed, 3), png=name)
            row['delta'] = round(row['after']['s2'] - row['before']['s2'], 2)
            rows.append(row)
            (args.out / 'results.json').write_text(json.dumps(rows, indent=2) + '\n')
            print(f'{mode:22} {source.name:24} {row["delta"]:+.2f}', flush=True)
    summaries, sections = [], []
    for mode in args.modes:
        group = [r for r in rows if r['mode'] == mode]
        before = sum(r['before']['s2'] for r in group) / len(group)
        after = sum(r['after']['s2'] for r in group) / len(group)
        summaries.append(dict(mode=mode, count=len(group), before=before, after=after,
                              delta=after-before, wins=sum(r['delta'] > 0 for r in group),
                              ties=sum(r['delta'] == 0 for r in group),
                              losses=sum(r['delta'] < 0 for r in group)))
        sections.append(f'<h2 id="{mode}">{html.escape(mode)}: mean S2 {before:.2f} → {after:.2f}</h2>')
        for row in group:
            sections.append(f'<h3>{html.escape(row["image"])} · {row["delta"]:+.2f} S2</h3><div class="pair">')
            for label in ['before', 'after']:
                item = row[label]
                sections.append(f'<figure><figcaption>{label} · S2 {item["s2"]:.2f}</figcaption>'
                                f'<img loading="lazy" src="{html.escape(item["png"], quote=True)}"></figure>')
            sections.append('</div>')
    (args.out / 'summary.json').write_text(json.dumps(summaries, indent=2) + '\n')
    settings = ('default settings' if not args.dither else
                f'--dither {html.escape(args.dither)} for both variants')
    note = ('<p>PETSCII chooses ROM glyphs directly and ignores the dither setting; '
            'its results are included for reference.</p>'
            if args.dither and 'c64-petscii' in args.modes else '')
    (args.out / 'comparison.html').write_text('''<!doctype html><meta charset="utf-8">
<title>Cell refinement: all modes</title><style>
body{background:#17191c;color:#eee;font:16px/1.5 system-ui;max-width:1400px;margin:32px auto;padding:0 20px}
.pair{display:grid;grid-template-columns:1fr 1fr;gap:16px}figure{margin:0}img{width:100%;image-rendering:pixelated}
h2{margin-top:64px}h3{margin-top:32px}@media(max-width:760px){.pair{grid-template-columns:1fr}}
</style><h1>Cross-cell color and pattern refinement</h1>
<p>Without vs with --cell-refine, native mode dimensions; ''' + settings + '''.
S2 is measured in hardware pixel space. Previews use the normal display aspect ratio.
The opt-in pass retains the original image when its final S2 does not improve.
A higher S2 does not guarantee a preferable halftone texture.</p>''' + '<p>C64 multicolor uses the corrected shared-background baseline; its old preview allowed colors the exported hardware data could not display. FLI/AFLI include the actual light-gray left edge of the bundled displayers, verified in PAL VICE.</p>' + note + '<nav>' + ' · '.join(f'<a href="#{m}">{m}</a>' for m in args.modes) + '</nav>' + ''.join(sections))
    print(json.dumps(summaries, indent=2))


if __name__ == '__main__':
    main()
