from pathlib import Path
import subprocess,json,csv,os,math
p=Path(__file__).resolve().parent;rows=[]
for case in ['radial','planar']:
 for quad in [0,1]:
  fields=p/'fields'/f'equilibrium_{case}_{quad}.csv';r=subprocess.run([str(p/'build_baseline/review'),case,'64','1','2','1',str(quad),'0'],env={**os.environ,'ANALYTICAL_FIELDS':str(fields)},capture_output=True,text=True,check=True)
  cells=[{k:float(v)for k,v in r.items()}for r in csv.DictReader(fields.open())];volume=sum(r['area']*r['depth']for r in cells);lo=min(r['bed']+.1 for r in cells);hi=max(r['bed']+.1+r['depth']for r in cells)+1
  for _ in range(80):
   level=(lo+hi)/2;vol=sum(r['area']*max(0,level-r['bed']-.1)for r in cells)
   if vol>volume:hi=level
   else:lo=level
  level=(lo+hi)/2;energy=0
  for r in cells:
   z=r['bed']+.1;h=max(0,level-z);energy+=9.80665*r['area']*(.5*h*h+h*z)
  for v in ['baseline','shore_wetlimit']:
   hist=[{k:float(v)for k,v in r.items()}for r in csv.DictReader((p/'histories'/f'screen_{case}_{quad}_{v}.csv').open())]
   ratio=(hist[-1]['energy']-energy)/(hist[0]['energy']-energy);rows.append(dict(case=case,quad=quad,variant=v,equilibrium_energy=energy,initial_energy=hist[0]['energy'],final_energy=hist[-1]['energy'],oscillatory_energy_ratio=ratio));print(case,quad,v,ratio,flush=True)
(p/'energy_results.json').write_text(json.dumps(rows,indent=2))
