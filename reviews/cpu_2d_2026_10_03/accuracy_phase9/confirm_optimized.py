from pathlib import Path
import json,subprocess,csv,os
p=Path(__file__).resolve().parent;rows=[]
for old in json.loads((p/'confirmation.json').read_text()):
 if old['variant']=='baseline':continue
 case=old['case'];n=old['nx'];quad=old['quad'];skew=old['skew'];periods=int(old['duration']);hist=p/'histories'/f'optimized_confirm_{case}_{n}_{quad}_{skew}_{periods}.csv'
 r=subprocess.run([str(p/'build_selected/review'),case,str(n),'1','2','1',str(quad),str(periods)],env={**os.environ,'REVIEW_HISTORY':str(hist),'REVIEW_SKEW':str(skew)},capture_output=True,text=True,check=True,timeout=240);row=json.loads(r.stdout);diff={k:[old[k],row[k]]for k in row if k!='seconds'and row[k]!=old[k]};original=p/'histories'/f'confirm_{case}_{n}_{quad}_{skew}_{periods}_connected_first_velocity.csv';same=hist.read_bytes()==original.read_bytes();rows.append(dict(case=case,nx=n,quad=quad,skew=skew,periods=periods,non_timing_differences=diff,history_identical=same));(p/'optimized_confirmation.json').write_text(json.dumps(rows,indent=2));print(case,n,quad,skew,periods,diff,same,flush=True)
assert all(not r['non_timing_differences']and r['history_identical']for r in rows)
