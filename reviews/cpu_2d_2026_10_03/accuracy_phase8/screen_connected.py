from pathlib import Path
import subprocess,json,csv,os,math
p=Path(__file__).resolve().parent;rows=[]
for v in ['connected','connected_face']:
 if v!='baseline':subprocess.run(['python3',str(p/'build_variant.py'),v],check=True)
 for case in ['radial','planar','lake_wet','lake_dry','ritter','stoker']:
  for quad in [0,1]:
   history=p/'histories'/f'screen_{case}_{quad}_{v}.csv';duration='3'if case in ['radial','planar']else '2'if case in ['ritter','stoker']else '20'
   cmd=[str(p/f'build_{v}/review'),case,'64','1','2','1',str(quad),duration]
   try:
    r=subprocess.run(cmd,env={**os.environ,'REVIEW_HISTORY':str(history)},capture_output=True,text=True,check=True,timeout=90)
    row=json.loads(r.stdout);h=[{k:float(v)for k,v in x.items()}for x in csv.DictReader(history.open())];end=h[-1];first=h[0]
    row.update(variant=v,energy_ratio=end['energy']/first['energy'],centroid_radius=math.hypot(end['cx'],end['cy']),centroid_phase=math.atan2(end['cy'],end['cx']),max_speed=max(x['max_speed']for x in h),shore_area=end['shore_area'],velocity_l2=end['velocity_l2'],mean_relative_l1=sum(x['relative_l1']for x in h)/len(h));rows.append(row)
    print(case,quad,v,'L1',round(row['relative_l1'],5),'E',round(row['energy_ratio'],5),'phase',round(row['centroid_phase'],4),'maxV',round(row['max_speed'],4),'steps',row['steps'],flush=True)
   except Exception as e:
    rows.append(dict(variant=v,case=case,quad=quad,error=str(e)));print('FAIL',v,case,quad,str(e),flush=True)
   (p/'connected_results.json').write_text(json.dumps(rows,indent=2))
