#!/usr/bin/env python3
import argparse,concurrent.futures,json,subprocess,html
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--binary',default='build/to8-pairs/driver');p.add_argument('--only',nargs='*',default=['maui','makena','grungy','asterix','photo','ramps']);a=p.parse_args()
root=Path('build/to8-pairs/gallery');root.mkdir(parents=True,exist_ok=True)
images=[f for f in sorted(Path('examples').iterdir()) if f.suffix.lower() in ['.png','.jpg','.webp'] and (not a.only or f.stem in a.only)]
def run(case):
 f,d,cells=case;folder=root/f'{d}-cells{cells}-{f.stem}';folder.mkdir(exist_ok=True)
 r=subprocess.run([a.binary,'--experiment-to8-pairs',str(f),str(folder),d,str(cells)],text=True,capture_output=True)
 (folder/'run.log').write_text(r.stdout+r.stderr)
 if r.returncode:raise RuntimeError(str(folder)+': '+r.stderr)
 row=json.loads((folder/'results.json').read_text());row['directory']=folder.name
 row['best_gain']=max(row['single']['gain'],row['paired']['gain'])
 print(folder.name,f'best {row["best_gain"]:+.3f}',flush=True);return row
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:rows=list(pool.map(run,[(f,d,c) for d in ['opt-checker','floyd-steinberg'] for c in [0,1] for f in images]))
(root/'results.json').write_text(json.dumps(rows,indent=2))
page='''<!doctype html><meta charset="utf-8"><title>TO8 palette fitting</title><style>body{background:#151719;color:#ddd;font:16px system-ui;margin:24px}.row{display:grid;grid-template-columns:repeat(3,1fr);gap:12px}figure{margin:0}img{width:100%;image-rendering:pixelated}h2{margin-top:36px}figcaption{padding:8px;background:#282b30}</style><h1>TO8 forme-couleur · palette fitting</h1><p>Current → single-color → paired fitting. Same source, dither and cell-refine setting in each row. Palette colors snap to the TO8 intensity table; every candidate is fully re-encoded with two colors per 8×1 cell. Each search uses two rounds of up to eight encodes. Best-of selection includes the original result. Sources are stretched to 320×200 identically.</p>'''
summary=[]
for d in ['opt-checker','floyd-steinberg']:
 for cells in [False,True]:
  rr=[r for r in rows if r['dither']==d and r['cell_refine']==cells]
  mean=sum(r['best_gain'] for r in rr)/len(rr)
  summary.append({'dither':d,'cell_refine':cells,'count':len(rr),'mean_best_gain':mean,'mean_seconds':sum(r['single']['seconds']+r['paired']['seconds'] for r in rr)/len(rr)})
  page+=f'<h2>{d} · cell-refine {"on" if cells else "off"} · mean best gain {mean:+.2f} S2</h2>'
  for r in rr:
   page+=f'<h3>{html.escape(Path(r["input"]).stem)}</h3><div class="row">'
   for arm in ['before','single','paired']:
    score=r['baseline'] if arm=='before' else r[arm]['s2']
    page+=f'<figure><figcaption>{arm} · S2 {score:.2f}</figcaption><img loading="lazy" src="{r["directory"]}/{arm}.png"></figure>'
   page+='</div>'
(root/'comparison.html').write_text(page);(root/'summary.json').write_text(json.dumps(summary,indent=2));print(summary)
