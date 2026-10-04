from pathlib import Path
import subprocess,json,csv,os,math
p=Path(__file__).resolve().parent;rows=[]
jobs=[(case,n,quad,periods)for case in ['radial','planar']for n in [32,64,128]for quad in [0,1]for periods in [1,3]]+[(case,64,quad,10)for case in ['radial','planar']for quad in [0,1]]
for case,n,quad,periods in jobs:
 for v in ['baseline','selected']:
  name=f'{case}_{n}_{quad}_{periods}_{v}';history=p/'histories'/f'{name}.csv'
  r=subprocess.run([str(p/f'build_{v}/review'),case,str(n),'1','2','1',str(quad),str(periods)],env={**os.environ,'REVIEW_HISTORY':str(history)},capture_output=True,text=True,check=True,timeout=300)
  row=json.loads(r.stdout);h=[{k:float(v)for k,v in x.items()}for x in csv.DictReader(history.open())];end=h[-1];first=h[0]
  row.update(variant=v,energy_ratio=end['energy']/first['energy'],centroid_radius=math.hypot(end['cx'],end['cy']),centroid_phase=math.atan2(end['cy'],end['cx']),max_speed=max(x['max_speed']for x in h),shore_area=end['shore_area'],velocity_l2=end['velocity_l2'],history=str(history.relative_to(p)),max_relative_l1=max(x['relative_l1']for x in h),mean_relative_l1=sum(x['relative_l1']for x in h)/len(h));rows.append(row)
  assert row['min_depth']>=0 and abs(row['volume_relative_error'])<1e-11,row
  print(case,n,quad,periods,v,'L1',round(row['relative_l1'],4),'phase',round(row['centroid_phase'],4),flush=True)
  (p/'accuracy_results.json').write_text(json.dumps(rows,indent=2))
print('PASS',len(rows),'runs finite, positive, conserved',flush=True)
