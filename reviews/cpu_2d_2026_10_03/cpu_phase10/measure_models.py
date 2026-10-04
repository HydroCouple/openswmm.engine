from pathlib import Path
import subprocess,os,json,random,statistics
p=Path(__file__).resolve().parent;rows=[];rng=random.Random(10401)
for case in ['parking_lot_swe2_flat','surface_wet_swe2_t1','surface_wet_swe2_t4','bellinge_10min_swe2']:
 for repeat in range(-1,3):
  vs=['baseline','selected'];rng.shuffle(vs);pair=[]
  for v in vs:
   out=p/'engine_runs'/f'timing_{case}_{repeat}_{v}'
   result=subprocess.run(['python3',str(p/'run_engine.py'),str(p/f'build_{v}/libopenswmm.engine.{v}.dylib'),str(p/'engine_decks'/case/'model.inp'),str(out)],env={**os.environ,'OPENSWMM_2D_BACKEND':'cpu'},capture_output=True,text=True,check=True,timeout=300)
   row=json.loads(next(x for x in result.stdout.splitlines()if x.startswith('{')));pair.append({k:x for k,x in row.items()if k not in ['cpu_seconds','init_seconds','run_seconds','total_seconds','peak_rss_bytes']});row.update(case=case,variant=v,repeat=repeat,load_average=os.getloadavg());rows.append(row);(p/'model_timings.json').write_text(json.dumps(rows,indent=2))
  assert pair[0]==pair[1];print(case,repeat,flush=True)
summary=[]
for case in sorted(set(r['case']for r in rows)):
 rr=[r for r in rows if r['case']==case and r['repeat']>=0]
 ratios=[next(r['cpu_seconds']for r in rr if r['repeat']==i and r['variant']=='selected')/next(r['cpu_seconds']for r in rr if r['repeat']==i and r['variant']=='baseline')for i in range(3)]
 summary.append(dict(case=case,median_cpu_ratio=statistics.median(ratios),paired_ratios=ratios,baseline_cpu=statistics.median(r['cpu_seconds']for r in rr if r['variant']=='baseline'),selected_cpu=statistics.median(r['cpu_seconds']for r in rr if r['variant']=='selected')))
(p/'model_timing_summary.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2),flush=True)
