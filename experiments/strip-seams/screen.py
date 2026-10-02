"""Screen the guard-aware planning/refinement ablation.

Requires src/strips.cpp from prototype.patch (relative to commit 8158ac7e),
with build/png2amiga rebuilt, and the unmodified executable saved as
build/strip-seams/baseline-cli. The final production encoder intentionally
has no experiment environment switches. Uses tools/bench_strips.py.
"""
import subprocess,os,concurrent.futures
from pathlib import Path
root=Path('build/strip-seams')
def run(mode):
 for name,env in [('baseline',{}),('guard',{'PNG2AMIGA_STRIP_GUARD':'1'}),('fit',{'PNG2AMIGA_STRIP_GUARD_FIT':'1'}),('both',{'PNG2AMIGA_STRIP_GUARD':'1','PNG2AMIGA_STRIP_GUARD_FIT':'1'})]:
  out=root/(mode+'-'+name)
  cmd=['python3','tools/bench_strips.py','--mode',mode,'--cli',str(root/'baseline-cli') if name=='baseline' else 'build/png2amiga','--out',str(out),'--only','maui','grungy','fantasy','photo','macaw','ramps']
  if name!='baseline':cmd+=['--baseline',str(root/(mode+'-baseline')/'results.json')]
  p=subprocess.run(cmd,env={**os.environ,**env},capture_output=True,text=True)
  print(mode,name,p.stdout,p.stderr,flush=True)
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:list(pool.map(run,['dpf','ehb']))
