from pathlib import Path
import subprocess,json,csv,os,math
p=Path(__file__).resolve().parent;rows=[]
for n,periods in [(32,3),(128,3),(64,10)]:
 for case in ['radial','planar']:
  for quad in [0,1]:
   for v in ['baseline','shore_wetlimit']:
    history=p/'histories'/f'confirm_{case}_{n}_{quad}_{periods}_{v}.csv'
    r=subprocess.run([str(p/f'build_{v}/review'),case,str(n),'1','2','1',str(quad),str(periods)],env={**os.environ,'REVIEW_HISTORY':str(history)},capture_output=True,text=True,check=True,timeout=240)
    row=json.loads(r.stdout);h=[{k:float(v)for k,v in x.items()}for x in csv.DictReader(history.open())];end=h[-1];first=h[0]
    row.update(variant=v,energy_ratio=end['energy']/first['energy'],centroid_radius=math.hypot(end['cx'],end['cy']),centroid_phase=math.atan2(end['cy'],end['cx']),max_speed=max(x['max_speed']for x in h),shore_area=end['shore_area'],velocity_l2=end['velocity_l2'],mean_relative_l1=sum(x['relative_l1']for x in h)/len(h));rows.append(row)
    print(case,n,quad,periods,v,'L1',round(row['relative_l1'],4),'maxV',round(row['max_speed'],4),'meanL1',round(row['mean_relative_l1'],4),flush=True)
    (p/'confirmation.json').write_text(json.dumps(rows,indent=2))
