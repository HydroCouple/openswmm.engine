from pathlib import Path
import json,os,subprocess,sys,resource,random,statistics as S
R=Path(__file__).resolve().parent;out=R/sys.argv[1];out.mkdir(exist_ok=False);rows=[];rng=random.Random(20261004)
cases=[('free',4,t,120) for t in (1,4,8)]+[('free',16,4,120),('transition',4,4,30),('chain',4,4,300)]
for repeat in range(int(sys.argv[2])):
 order=cases[:];rng.shuffle(order)
 for case,cells,t,duration in order:
  labels=['baseline_A','baseline_B','candidate'];rng.shuffle(labels)
  for label in labels:
   lib='baseline' if label.startswith('baseline') else label;name=f'{case}_c{cells}_t{t}_r{repeat}_{label}';env={k:v for k,v in os.environ.items() if not k.startswith(('OMP_','KMP_','OPENSWMM_','SWMM_','DYLD_','FV_TEST_'))};env.update(OMP_NUM_THREADS=str(t),OMP_THREAD_LIMIT=str(t),OMP_DYNAMIC='FALSE',OMP_WAIT_POLICY='PASSIVE',KMP_BLOCKTIME='0');before=resource.getrusage(resource.RUSAGE_CHILDREN)
   cp=subprocess.run([sys.executable,str(R/'fixture.py'),lib,case,str(cells),str(t),str(duration),str(out/name)],env=env,capture_output=True,text=True,timeout=180);(out/(name+'.log')).write_text(cp.stdout+cp.stderr);assert cp.returncode==0,(name,cp.stderr[-2000:]);after=resource.getrusage(resource.RUSAGE_CHILDREN);row=json.loads((out/name/'result.json').read_text());row.update(case=case,cells=cells,threads=t,label=label,name=name,repeat=repeat,process_cpu=after.ru_utime+after.ru_stime-before.ru_utime-before.ru_stime)
   ref=next((x for x in rows if (x['case'],x['cells'])==(case,cells)),None)
   if ref:assert all(row[k]==ref[k] for k in ('output_sha256','step_times_sha256','substeps','fluxes','macros')),name
   rows.append(row);(out/'results.json').write_text(json.dumps(rows,indent=2));print(name,row['engine_step_seconds'],row['process_cpu'],flush=True)
summary=[]
for case,cells,t,duration in cases:
 xs=[x for x in rows if (x['case'],x['cells'],x['threads'])==(case,cells,t)];b=[x['engine_step_seconds'] for x in xs if x['label'].startswith('baseline')];c=[x['engine_step_seconds'] for x in xs if x['label']=='candidate'];pairs=[]
 for repeat in range(int(sys.argv[2])):
  v={x['label']:x['engine_step_seconds'] for x in xs if x['repeat']==repeat};pairs.append(dict(repeat=repeat,**v,ratio=v['candidate']/S.mean([v['baseline_A'],v['baseline_B']]),control_ratio=max(v['baseline_A'],v['baseline_B'])/min(v['baseline_A'],v['baseline_B'])))
 summary.append(dict(case=case,cells=cells,threads=t,baseline_median=S.median(b),candidate_median=S.median(c),percent_change=100*(S.median(c)/S.median(b)-1),baseline_range=[min(b),max(b)],candidate_range=[min(c),max(c)],pairs=pairs))
(out/'summary.json').write_text(json.dumps(summary,indent=2))
