from pathlib import Path
import json,os,subprocess,re,sys,resource
R=Path(__file__).resolve().parent;out=R/'baseline_cost';out.mkdir(exist_ok=False);rows=[]
for case,duration in [('free',120),('transition',10)]:
 for t in (1,4):
  name=f'{case}_t{t}';env={k:v for k,v in os.environ.items() if not k.startswith(('OMP_','KMP_','OPENSWMM_','SWMM_','DYLD_'))};env.update(OMP_NUM_THREADS=str(t),OMP_THREAD_LIMIT=str(t),OMP_DYNAMIC='FALSE',OMP_WAIT_POLICY='PASSIVE',KMP_BLOCKTIME='0',OPENSWMM_PERF='1');before=resource.getrusage(resource.RUSAGE_CHILDREN)
  cp=subprocess.run([sys.executable,str(R/'fixture.py'),'baseline',case,'4',str(t),str(duration),str(out/name)],env=env,capture_output=True,text=True,timeout=180);(out/(name+'.log')).write_text(cp.stdout+cp.stderr);assert cp.returncode==0,(name,cp.stderr[-2000:]);after=resource.getrusage(resource.RUSAGE_CHILDREN)
  row=json.loads((out/name/'result.json').read_text());p=re.findall(r'\[PERF-FV\] ([^\n]+)',cp.stderr);assert len(p)==1;row.update(case=case,threads=t,process_cpu=after.ru_utime+after.ru_stime-before.ru_utime-before.ru_stime,profile={k:float(v) for k,v in re.findall(r'([\w.]+)=([0-9.e+-]+)',p[0])});ref=next((x for x in rows if x['case']==case),None)
  if ref:assert all(row[k]==ref[k] for k in ('output_sha256','step_times_sha256','substeps','fluxes'))
  rows.append(row);(out/'results.json').write_text(json.dumps(rows,indent=2));print(case,t,row['profile']['step'],row['profile']['nodesolve'],flush=True)
