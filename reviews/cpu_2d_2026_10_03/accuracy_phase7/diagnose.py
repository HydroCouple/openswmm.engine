from pathlib import Path
import subprocess,json,csv,os,math
p=Path(__file__).resolve().parent;(p/'histories').mkdir(exist_ok=True);rows=[]
jobs=[(case,64,order,quad,label,env)for case in ['radial','planar']for quad in [0,1]for order,label,env in [(1,'first',{}),(2,'second',{}),(2,'rk2_first_space',{'OPENSWMM_2D_MUSCL_OFF':'1'}),(2,'cfl01',{'REVIEW_CFL':'.1'}),(2,'low_threshold',{'REVIEW_DRY':'1e-10','REVIEW_HMOVE':'1e-9'})]]
for case,n,order,quad,label,env in jobs:
 name=f'{case}_{n}_{quad}_{label}';history=p/'histories'/f'{name}.csv'
 r=subprocess.run([str(p/'build_baseline/review'),case,str(n),'1',str(order),'1',str(quad),'3'],env={**os.environ,**env,'REVIEW_HISTORY':str(history)},capture_output=True,text=True,check=True,timeout=180)
 row=json.loads(r.stdout);h=[{k:float(v)for k,v in x.items()}for x in csv.DictReader(history.open())];end=h[-1];first=h[0]
 row.update(label=label,energy_ratio=end['energy']/first['energy'],centroid_radius=math.hypot(end['cx'],end['cy']),centroid_phase=math.atan2(end['cy'],end['cx']),max_speed=max(x['max_speed']for x in h),shore_area=end['shore_area'],velocity_l2=end['velocity_l2'],history=str(history.relative_to(p)));rows.append(row)
 print(case,quad,label,'L1',round(row['relative_l1'],4),'E',round(row['energy_ratio'],4),'centroid',round(row['centroid_radius'],4),'phase',round(row['centroid_phase'],4),flush=True)
 (p/'diagnosis.json').write_text(json.dumps(rows,indent=2))
