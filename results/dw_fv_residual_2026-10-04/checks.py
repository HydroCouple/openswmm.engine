from pathlib import Path
import json,os,subprocess,re,sys
R=Path(__file__).resolve().parent;out=R/'checks';out.mkdir(exist_ok=False);rows=[]
cases=['free','transition','pressure','transition_tpa','order2','reverse_mixed','dry','gate','lts','lts_transition']
for case in cases:
 for t in (1,4,8):
  for label in ('baseline','candidate'):
   name=f'{case}_t{t}_{label}';env={k:v for k,v in os.environ.items() if not k.startswith(('OMP_','KMP_','OPENSWMM_','SWMM_','DYLD_'))};env.update(OMP_NUM_THREADS=str(t),OMP_THREAD_LIMIT=str(t),OMP_DYNAMIC='FALSE',OMP_WAIT_POLICY='PASSIVE',KMP_BLOCKTIME='0',OPENSWMM_PERF='1');cp=subprocess.run([sys.executable,str(R/'fixture.py'),label,case,'4',str(t),'5',str(out/name)],env=env,capture_output=True,text=True,timeout=180);(out/(name+'.log')).write_text(cp.stdout+cp.stderr);assert cp.returncode==0,(name,cp.stderr[-2000:])
   row=json.loads((out/name/'result.json').read_text());ps=re.findall(r'\[PERF-FV\] ([^\n]+)',cp.stderr);assert len(ps)==1;profile={k:float(v) for k,v in re.findall(r'([\w.]+)=([0-9.e+-]+)',ps[0])};row.update(case=case,threads=t,label=label,name=name,profile=profile)
   ref=next((x for x in rows if x['case']==case),None)
   if ref:
    assert all(row[k]==ref[k] for k in ('output_sha256','step_times_sha256','substeps','fluxes','macros')),name
    for k,v in profile.items():
     if k.startswith('n.'):
      if k in ('n.area','n.width','n.i1'):assert v<=ref['profile'][k],(name,k)
      else:assert v==ref['profile'][k],(name,k,v,ref['profile'][k])
   rows.append(row);(out/'results.json').write_text(json.dumps(rows,indent=2));print(name,'exact',flush=True)
