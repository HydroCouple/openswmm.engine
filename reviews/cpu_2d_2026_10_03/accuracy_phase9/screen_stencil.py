from pathlib import Path
import subprocess,json,csv,os,math
p=Path(__file__).resolve().parent;rows=[]
jobs=[(c,64,q,0,3)for c in ['radial','planar']for q in [0,1]]+[(c,32,q,.15,3)for c in ['radial','planar']for q in [0,1,2]]+[(c,64,q,0,20 if c.startswith('lake')else 2)for c in ['lake_wet','lake_dry','ritter','stoker']for q in [0,1]]
for v in ['shore_conditioned','connected_conditioned','connected_first_velocity']:
 if v!='baseline':subprocess.run(['python3',str(p/'build_variant.py'),v],check=True)
 for case,n,quad,skew,duration in jobs:
  name=f'{case}_{n}_{quad}_{skew}_{v}';history=p/'histories'/f'{name}.csv'
  try:
   r=subprocess.run([str(p/f'build_{v}/review'),case,str(n),'1','2','1',str(quad),str(duration)],env={**os.environ,'REVIEW_HISTORY':str(history),'REVIEW_SKEW':str(skew)},capture_output=True,text=True,check=True,timeout=60)
   row=json.loads(r.stdout);h=[{k:float(v)for k,v in x.items()}for x in csv.DictReader(history.open())];end=h[-1];first=h[0]
   row.update(variant=v,skew=skew,energy_ratio=end['energy']/first['energy'],centroid_radius=math.hypot(end['cx'],end['cy']),centroid_phase=math.atan2(end['cy'],end['cx']),max_speed=max(x['max_speed']for x in h),mean_relative_l1=sum(x['relative_l1']for x in h)/len(h));rows.append(row)
   print(case,n,quad,skew,v,'L1',round(row['relative_l1'],5),'maxV',round(row['max_speed'],4),'steps',row['steps'],flush=True)
  except Exception as e:rows.append(dict(variant=v,case=case,nx=n,quad=quad,skew=skew,error=str(e)));print('FAIL',v,case,quad,str(e),flush=True)
  (p/'stencil_results.json').write_text(json.dumps(rows,indent=2))
