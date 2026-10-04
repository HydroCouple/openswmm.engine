from pathlib import Path
import subprocess,json,csv,os,math
p=Path(__file__).resolve().parent;rows=[]
jobs=[(c,n,q,sk,periods)for n,sk,periods in [(32,0,3),(128,0,3),(64,0,10),(64,.15,3)]for c in ['radial','planar']for q in ([0,1,2]if sk else [0,1])]
for case,n,quad,skew,periods in jobs:
 for v in ['baseline','connected_first_velocity']:
  hist=p/'histories'/f'confirm_{case}_{n}_{quad}_{skew}_{periods}_{v}.csv'
  r=subprocess.run([str(p/f'build_{v}/review'),case,str(n),'1','2','1',str(quad),str(periods)],env={**os.environ,'REVIEW_HISTORY':str(hist),'REVIEW_SKEW':str(skew)},capture_output=True,text=True,check=True,timeout=240)
  row=json.loads(r.stdout);h=[{k:float(v)for k,v in x.items()}for x in csv.DictReader(hist.open())];end=h[-1];first=h[0];row.update(variant=v,skew=skew,energy_ratio=end['energy']/first['energy'],centroid_phase=math.atan2(end['cy'],end['cx']),centroid_radius=math.hypot(end['cx'],end['cy']),max_speed=max(x['max_speed']for x in h),mean_relative_l1=sum(x['relative_l1']for x in h)/len(h));rows.append(row);(p/'confirmation.json').write_text(json.dumps(rows,indent=2))
  print(case,n,quad,skew,periods,v,'L1',round(row['relative_l1'],5),'maxV',round(row['max_speed'],4),'phase',round(row['centroid_phase'],4),flush=True)
  assert row['min_depth']>=0 and abs(row['volume_relative_error'])<1e-11
