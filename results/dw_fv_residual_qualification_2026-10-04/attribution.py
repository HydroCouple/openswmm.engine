from pathlib import Path
import sys,json,subprocess,os,random,statistics,collections
Q=Path(__file__).resolve().parent;R=Q.parent/'dw_fv_residual_2026-10-04';sys.path.insert(0,str(R))
import fixture,network_screen as net
if len(sys.argv)>1 and sys.argv[1]=='worker':
 _,_,label,case,cells,t,out=sys.argv;cells=int(cells);t=int(t);net.ROOT=Q;net.deck=lambda *a,**kw:fixture.deck(case,cells,t,60);net.worker(label,'FV',cells,t,60,False,out);sys.exit()
out=Q/'attribution';out.mkdir(exist_ok=False);rows=[];rng=random.Random(421)
for repeat in range(2):
 configs=[(case,c,t) for case,c in [('free',4),('free',16),('rest',4)] for t in (1,4,8)];rng.shuffle(configs)
 for case,c,t in configs:
  labels=['baseline','candidate'];rng.shuffle(labels)
  ref=None
  for label in labels:
   name=f'{case}_c{c}_t{t}_r{repeat}_{label}';env={k:v for k,v in os.environ.items() if not k.startswith(('OMP_','KMP_','OPENSWMM_','SWMM_','DYLD_','FV_'))};env.update(OMP_NUM_THREADS=str(t),OMP_THREAD_LIMIT=str(t),OMP_DYNAMIC='FALSE',OMP_WAIT_POLICY='PASSIVE',KMP_BLOCKTIME='0')
   cp=subprocess.run([sys.executable,__file__,'worker','attribution_'+label,case,str(c),str(t),str(out/name)],env=env,capture_output=True,text=True,timeout=180);(out/(name+'.log')).write_text(cp.stdout+cp.stderr);assert cp.returncode==0,(name,cp.stderr[-1000:])
   result=json.loads((out/name/'result.json').read_text());sig=tuple(result[k] for k in ('output_sha256','step_times_sha256','substeps','fluxes','macros'))
   if ref is None:ref=sig
   else:assert sig==ref,name
   samples=[list(map(float,l.split()[1:])) for l in cp.stderr.splitlines() if l.startswith('NODETEAM ')]
   assert samples and all(x[1]==t and x[0]==x[7]==256 for x in samples),name
   row=dict(name=name,case=case,cells=c,threads=t,repeat=repeat,label=label,regions=len(samples),team_elapsed=sum(x[2] for x in samples),loop_thread_cpu=sum(x[3] for x in samples),node_busy_wall=sum(x[4] for x in samples),sum_max_worker_busy=sum(x[5] for x in samples),sum_max_barrier_wait=sum(x[6] for x in samples),node_solves=sum(x[7] for x in samples),result=result)
   rows.append(row);(out/'results.json').write_text(json.dumps(rows,indent=2));print(name,round(row['team_elapsed'],4),round(row['loop_thread_cpu'],4),flush=True)
summary=[]
for case,c in [('free',4),('free',16),('rest',4)]:
 for t in (1,4,8):
  for label in ('baseline','candidate'):
   rr=[x for x in rows if (x['case'],x['cells'],x['threads'],x['label'])==(case,c,t,label)]
   summary.append(dict(case=case,cells=c,threads=t,label=label,**{k:statistics.median(x[k] for x in rr) for k in ('team_elapsed','loop_thread_cpu','node_busy_wall','sum_max_worker_busy','sum_max_barrier_wait','node_solves','regions')}))
(out/'summary.json').write_text(json.dumps(summary,indent=2))
