from pathlib import Path
import concurrent.futures,json,subprocess
root=Path('build/to8-pairs/gallery');rows=json.loads((root/'results.json').read_text())
def run(r):
 expected=max(r['single']['s2'],r['paired']['s2'],r['baseline'])
 if r['cell_refine']:
  staged=json.loads((root/r['directory'].replace('cells1','cells0')/'staged.json').read_text())
  expected=max(expected,staged['before']+staged['gain'])
 p=subprocess.run(['build/api_pipeline_smoke','--check-to8-palette',r['input'],r['dither'],str(int(r['cell_refine'])),str(expected)],capture_output=True,text=True)
 print(p.stdout+p.stderr,flush=True)
 if p.returncode:raise RuntimeError(r['directory'])
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:list(pool.map(run,rows))
print('24 production quality and native-memory replay checks passed')
