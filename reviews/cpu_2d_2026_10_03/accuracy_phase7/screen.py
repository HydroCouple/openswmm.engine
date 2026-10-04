from pathlib import Path
import subprocess,json,csv,os,math
p=Path(__file__).resolve().parent;rows=[]
for case in ['radial','planar','lake_wet','lake_dry','ritter','stoker']:
 for quad in [0,1]:
  for v in ['baseline','recon']:
   history=p/'histories'/f'screen_{case}_{quad}_{v}.csv';duration='3'if case in ['radial','planar']else '2'if case in ['ritter','stoker']else '20'
   r=subprocess.run([str(p/f'build_{v}/review'),case,'64','1','2','1',str(quad),duration],env={**os.environ,'REVIEW_HISTORY':str(history)},capture_output=True,text=True,check=True,timeout=180)
   row=json.loads(r.stdout);h=[{k:float(v)for k,v in x.items()}for x in csv.DictReader(history.open())];end=h[-1];first=h[0]
   row.update(variant=v,energy_ratio=end['energy']/first['energy'],centroid_radius=math.hypot(end['cx'],end['cy']),centroid_phase=math.atan2(end['cy'],end['cx']),max_speed=max(x['max_speed']for x in h),shore_area=end['shore_area'],velocity_l2=end['velocity_l2']);rows.append(row)
   print(case,quad,v,'L1',round(row['relative_l1'],5),'E',round(row['energy_ratio'],5),'phase',round(row['centroid_phase'],4),'maxV',round(row['max_speed'],4),'steps',row['steps'],flush=True)
   (p/'screen.json').write_text(json.dumps(rows,indent=2))
