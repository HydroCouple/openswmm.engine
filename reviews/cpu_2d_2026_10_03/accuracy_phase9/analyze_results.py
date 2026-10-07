from pathlib import Path
import json,csv,math,statistics
p=Path(__file__).resolve().parent
read=lambda f:json.loads((p/f).read_text())
# Mechanical energy relative to the same-volume discrete lake at rest.
energy=[]
for case in ['radial','planar']:
 for quad in [0,1]:
  fields=p/'fields'/f'mesh_{case}_64_{quad}_2_1_0_selected.csv';cells=[{k:float(v)for k,v in r.items()}for r in csv.DictReader(fields.open())]
  baseline=[{k:float(v)for k,v in r.items()}for r in csv.DictReader((p/'histories'/f'{case}_64_{quad}_0_baseline.csv').open())];volume=baseline[0]['volume'];lo=min(r['bed']+.1 for r in cells);hi=max(r['bed']+.1 for r in cells)+1
  for _ in range(70):
   level=(lo+hi)/2;v=sum(r['area']*max(0,level-r['bed']-.1)for r in cells)
   if v>volume:hi=level
   else:lo=level
  level=(lo+hi)/2;eq=sum(9.80665*r['area']*(.5*max(0,level-r['bed']-.1)**2+max(0,level-r['bed']-.1)*(r['bed']+.1))for r in cells)
  for v,file in [('baseline',f'{case}_64_{quad}_0_baseline.csv'),('selected',f'mesh_{case}_64_{quad}_2_1_0_selected.csv')]:
   hist=[{k:float(v)for k,v in r.items()}for r in csv.DictReader((p/'histories'/file).open())];a=hist[0];b=hist[-1];energy.append(dict(case=case,quad=quad,variant=v,equilibrium_energy=eq,initial_energy=a['energy'],final_energy=b['energy'],oscillatory_energy_ratio=(b['energy']-eq)/(a['energy']-eq)))
(p/'energy_results.json').write_text(json.dumps(energy,indent=2))
perf=read('performance_results.json');out=[]
for key in dict.fromkeys((r['comparison'],r['case'],r['quad'],r['order'])for r in perf):
 group=[r for r in perf if tuple(r[k]for k in ['comparison','case','quad','order'])==key];pairs={}
 for r in group:pairs.setdefault(r['repeat'],{})[r['variant']]=r
 assert all(len(x)==2 for x in pairs.values());ratios={m:[x['selected'][m]/x['baseline'][m]for x in pairs.values()]for m in ['cpu_seconds','seconds']};row=dict(comparison=key[0],case=key[1],quad=key[2],order=key[3],pairs=len(pairs),median_paired_cpu_ratio=statistics.median(ratios['cpu_seconds']),median_paired_wall_ratio=statistics.median(ratios['seconds']),paired_cpu_ratio_range=[min(ratios['cpu_seconds']),max(ratios['cpu_seconds'])]);
 for v in ['baseline','selected']:
  a=[r for r in group if r['variant']==v];row[v]=dict(nx=a[0]['nx'],cells=a[0]['cells'],cpu_seconds=statistics.median(r['cpu_seconds']for r in a),seconds=statistics.median(r['seconds']for r in a),relative_l1=a[0]['relative_l1'],steps=a[0]['steps'])
 out.append(row)
(p/'performance_summary.json').write_text(json.dumps(out,indent=2))
print('Analysis complete',flush=True)
