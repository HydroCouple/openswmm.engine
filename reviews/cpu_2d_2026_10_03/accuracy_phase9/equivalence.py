from pathlib import Path
import json,subprocess,csv,os,math
p=Path(__file__).resolve().parent;expected=[r for r in json.loads((p/'stencil_results.json').read_text())if r['variant']=='connected_first_velocity'];rows=[]
for old in expected:
 case=old['case'];n=old['nx'];quad=old['quad'];skew=old['skew'];hist=p/'histories'/f'optimized_{case}_{n}_{quad}_{skew}.csv'
 r=subprocess.run([str(p/'build_selected/review'),case,str(n),'1','2','1',str(quad),str(old['duration'])],env={**os.environ,'REVIEW_HISTORY':str(hist),'REVIEW_SKEW':str(skew)},capture_output=True,text=True,check=True,timeout=180);row=json.loads(r.stdout)
 diffs={k:[old[k],row[k]]for k in row if k!='seconds'and row[k]!=old[k]}
 original=p/'histories'/f'{case}_{n}_{quad}_{skew}_connected_first_velocity.csv';same=original.read_bytes()==hist.read_bytes();rows.append(dict(case=case,nx=n,quad=quad,skew=skew,non_timing_differences=diffs,history_identical=same));print(case,n,quad,'metrics',diffs,'history_identical',same,flush=True)
 (p/'equivalence_results.json').write_text(json.dumps(rows,indent=2))
assert all(not r['non_timing_differences']and r['history_identical']for r in rows)
