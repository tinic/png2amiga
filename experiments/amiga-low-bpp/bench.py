#!/usr/bin/env python3
import argparse,concurrent.futures,json,re,subprocess,time,html
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--before',default='build/amiga-low-bpp/baseline');p.add_argument('--after',default='build/png2amiga');p.add_argument('--out',default='build/amiga-low-bpp/gallery');p.add_argument('--only',nargs='*');p.add_argument('--methods',nargs='+',default=['opt-checker']);p.add_argument('--modes',nargs='+',default=['lores','hires','lores-lace','hires-lace']);a=p.parse_args()
root=Path(a.out);root.mkdir(parents=True,exist_ok=True)
images=[f for f in sorted(Path('examples').iterdir()) if f.suffix.lower() in ['.png','.jpg','.webp'] and (not a.only or f.stem in a.only)]
def run(case):
 f,mode,depth,method=case;key=f'{mode}-d{depth}-{method}-{f.stem}';folder=root/key;folder.mkdir(exist_ok=True)
 report={'input':str(f),'mode':mode,'depth':depth,'dither':method,'directory':key}
 for arm,binary in [('before',a.before),('after',a.after)]:
  start=time.monotonic();r=subprocess.run([binary,'--mode',mode,'--depth',str(depth),'--dither',method,str(f),str(folder/f'{arm}.png')],capture_output=True,text=True)
  (folder/f'{arm}.log').write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(key+': '+r.stderr)
  matches=re.findall(r'S2:\s*(-?\d+(?:\.\d+)?)',r.stdout)
  if not matches:raise RuntimeError(key+': no S2')
  report[arm]={'s2':float(matches[-1]),'seconds':time.monotonic()-start}
 report['gain']=report['after']['s2']-report['before']['s2'];print(key,f'{report["gain"]:+.2f}',flush=True)
 (folder/'results.json').write_text(json.dumps(report,indent=2));return report
cases=[(f,mode,d,m) for mode in a.modes for d in [2,3] for m in a.methods for f in images]
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:rows=list(pool.map(run,cases))
(root/'results.json').write_text(json.dumps(rows,indent=2))
summary=[]
for mode in a.modes:
 for d in [2,3]:
  for method in a.methods:
   rr=[r for r in rows if (r['mode'],r['depth'],r['dither'])==(mode,d,method)]
   summary.append({'mode':mode,'depth':d,'dither':method,'count':len(rr),'mean_gain':sum(r['gain'] for r in rr)/len(rr),'minimum_gain':min(r['gain'] for r in rr)})
(root/'summary.json').write_text(json.dumps(summary,indent=2));print(summary)
page='''<!doctype html><meta charset="utf-8"><title>Amiga low-bpp fitting</title><style>body{background:#151719;color:#ddd;font:16px system-ui;margin:24px}a{color:#9cf}.row{display:grid;grid-template-columns:repeat(2,1fr);gap:12px}figure{margin:0}img{width:100%;image-rendering:pixelated}h2{margin-top:36px}figcaption{padding:8px;background:#282b30}</style><h1>Amiga · 4 and 8 colors</h1><p>Previous encoder → automatic best of original, single-color fitting, and paired fitting. Same CLI settings, source preprocessing, dither, and hardware limits. The lores baseline already includes paired fitting; hires/interlace baselines do not.</p>'''
page+='<nav>'+ ' · '.join(f'<a href="#{g["mode"]}-{g["depth"]}-{g["dither"]}">{g["mode"]} {2**g["depth"]} colors</a>' for g in summary)+'</nav>'
for group in summary:
 page+=f'<h2 id="{group["mode"]}-{group["depth"]}-{group["dither"]}">{group["mode"]} · {2**group["depth"]} colors · {group["dither"]} · mean {group["mean_gain"]:+.2f} S2</h2>'
 for row in rows:
  if any(row[k]!=group[k] for k in ['mode','depth','dither']):continue
  page+=f'<h3>{html.escape(Path(row["input"]).stem)} · {row["gain"]:+.2f}</h3><div class="row">'
  for arm in ['before','after']:page+=f'<figure><figcaption>{arm} · S2 {row[arm]["s2"]:.2f}</figcaption><img loading="lazy" src="{row["directory"]}/{arm}.png"></figure>'
  page+='</div>'
(root/'comparison.html').write_text(page)
