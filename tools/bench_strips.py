#!/usr/bin/env python3
"""Benchmark DPF or EHB strips on examples; retain previews, logs, S2, and timings.

Example: python3 tools/bench_strips.py --cli build/png2amiga \
    --out build/strips-quality/trial --baseline build/strips-quality/baseline/results.json
Additional encoder arguments follow -- (e.g. -- --best).
"""
import argparse
import html
import json
import os
from pathlib import Path
import re
import subprocess
import time


def write_report(out, baseline_path, rows, baseline, mode="dpf"):
    mode_label = mode.upper()
    constraints = ("Seven selectable PF2 indices" if mode == "dpf" else "32 base colors and their 32 hardware half-brite colors")
    """Keep the report beside the images so the comparison remains portable."""
    def image_url(path):
        return html.escape(os.path.relpath(path.resolve(), out.resolve()), quote=True)

    cards = []
    table = []
    for row in rows:
        name = row['image']
        if name not in baseline:
            continue
        label = html.escape(name)
        stem = Path(name).stem
        before, after = baseline[name]['s2'], row['s2']
        table.append(f'<tr><td><a href="#{stem}">{label}</a></td>'
                     f'<td>{before:.2f}</td><td>{after:.2f}</td><td>{after-before:+.2f}</td></tr>')
        cards.append(f'<section id="{stem}"><h2>{label} · S2 {after-before:+.2f}</h2>'
                     '<div class="pair">'
                     f'<figure><figcaption>Before · S2 {before:.2f}</figcaption>'
                     f'<img loading="lazy" src="{image_url(baseline_path.parent / (stem + ".png"))}" '
                     f'alt="Before: {label}"></figure>'
                     f'<figure><figcaption>After · S2 {after:.2f}</figcaption>'
                     f'<img loading="lazy" src="{image_url(out / (stem + ".png"))}" '
                     f'alt="After: {label}"></figure></div></section>')
    mean_before = sum(baseline[r['image']]['s2'] for r in rows) / len(rows)
    mean_after = sum(r['s2'] for r in rows) / len(rows)
    document = f'''<!doctype html><html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{mode_label} strips quality comparison</title>
<style>
body{{background:#151719;color:#eceff1;font:16px/1.5 system-ui;margin:32px auto;padding:0 24px;max-width:1340px}}
h1{{font-size:32px}}h2{{font-size:20px;margin-top:40px}}a{{color:#8ed6ff}}
.pair{{display:grid;grid-template-columns:1fr 1fr;gap:20px}}figure{{margin:0}}
img{{width:100%;image-rendering:pixelated}}figcaption{{margin-bottom:8px}}
table{{border-collapse:collapse}}th,td{{padding:5px 22px;text-align:right;border-bottom:1px solid #363a40}}
td:first-child,th:first-child{{text-align:left}}section{{scroll-margin-top:20px}}
@media(max-width:750px){{.pair{{grid-template-columns:1fr}}}}
</style><h1>{mode_label} strips: before and after</h1>
<p>{len(rows)} specimens · 320 × 200 · default Floyd–Steinberg dithering.
Scores are SSIMULACRA2 (higher is better), measured by the encoder against the resized source.</p>
<p><strong>Mean S2: {mean_before:.2f} → {mean_after:.2f} ({mean_after-mean_before:+.2f})</strong></p>
<p>{constraints}; same RGB444 format, visible slot positions, and MOVE budget.</p>
<table><thead><tr><th>Specimen</th><th>Before</th><th>After</th><th>Change</th></tr></thead>
<tbody>{''.join(table)}</tbody></table>{''.join(cards)}</html>'''
    (out / 'comparison.html').write_text(document)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['dpf', 'ehb'], default='dpf')
    parser.add_argument('--cli', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--examples', type=Path, default=Path('examples'))
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--only', nargs='+')
    parser.add_argument('extra', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    extra = args.extra[1:] if args.extra[:1] == ['--'] else args.extra
    args.out.mkdir(parents=True, exist_ok=True)
    baseline = {}
    if args.baseline:
        baseline = {r['image']: r for r in json.loads(args.baseline.read_text())['results']}
    inputs = sorted(p for p in args.examples.iterdir()
                    if p.suffix.lower() in {'.png', '.jpg', '.jpeg', '.webp'}
                    and (not args.only or p.stem in args.only))
    if not inputs:
        parser.error('No input images matched')
    rows = []
    flags = (['--mode', 'lores', '--dpf'] if args.mode == 'dpf' else ['--mode', 'ehb'])
    flags += ['--strips', '--width', '320', '--height', '200']
    for source in inputs:
        output = args.out / (source.stem + '.png')
        command = [str(args.cli.resolve()), *flags, *extra, str(source), str(output)]
        start = time.perf_counter()
        result = subprocess.run(command, text=True, capture_output=True, check=True)
        elapsed = time.perf_counter() - start
        log = result.stdout + result.stderr
        output.with_suffix('.log').write_text(log)
        score = float(re.findall(r'S2:\s*(-?\d+(?:\.\d+)?)', log)[-1])
        row = dict(image=source.name, s2=score, seconds=round(elapsed, 3))
        if source.name in baseline:
            row['delta'] = round(score - baseline[source.name]['s2'], 3)
        rows.append(row)
        delta = f"  delta {row['delta']:+.2f}" if 'delta' in row else ''
        print(f'{source.name:24s} S2 {score:7.2f}{delta}  {elapsed:.3f}s', flush=True)
        (args.out / 'results.json').write_text(json.dumps(
            dict(cli=str(args.cli.resolve()), flags=flags + extra, results=rows), indent=2) + '\n')
    print(f"Mean S2: {sum(r['s2'] for r in rows) / len(rows):.3f}")
    deltas = [r['delta'] for r in rows if 'delta' in r]
    if deltas:
        print(f'Mean delta: {sum(deltas) / len(deltas):+.3f}; '
              f'wins/ties/losses: {sum(d > 0 for d in deltas)}/'
              f'{sum(d == 0 for d in deltas)}/{sum(d < 0 for d in deltas)}')
        if len(deltas) == len(rows):
            write_report(args.out, args.baseline, rows, baseline, args.mode)


if __name__ == '__main__':
    main()
