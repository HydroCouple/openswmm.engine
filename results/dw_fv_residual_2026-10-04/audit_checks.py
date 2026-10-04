from pathlib import Path
import json,os,subprocess,sys,hashlib,collections,re
R=Path(__file__).resolve().parent;out=R/'audit_checks';out.mkdir(exist_ok=False);rows=[]
for case in ['free','transition','pressure','transition_tpa','order2','reverse_mixed','dry','gate','lts','lts_transition']:
 ref=None
 for label,t in [('baseline',1),('candidate',1),('candidate',4),('candidate',8)]:
  name=f'{case}_{label}_t{t}';env={k:v for k,v in os.environ.items() if not k.startswith(('OMP_','KMP_','OPENSWMM_','SWMM_','DYLD_','FV_TEST_'))};env.update(OMP_NUM_THREADS=str(t),OMP_THREAD_LIMIT=str(t),OMP_DYNAMIC='FALSE',OMP_WAIT_POLICY='PASSIVE',KMP_BLOCKTIME='0',OPENSWMM_FV_RESIDUAL_AUDIT='1',OPENSWMM_FV_OMP_MIN_NODES='1',FV_TEST_GROUPS='8')
  cp=subprocess.run([sys.executable,str(R/'fixture.py'),'diagnostic_'+label,case,'4',str(t),'5',str(out/name)],env=env,capture_output=True,text=True,timeout=180);(out/(name+'.log')).write_text(cp.stdout+cp.stderr);assert cp.returncode==0,(name,cp.stderr[-2000:])
  trace=collections.defaultdict(list);teams=set();nr=ni=ne=0
  for l in cp.stderr.splitlines():
   if l.startswith('TEAM '):teams.add(int(l.split()[2]))
   elif l.startswith(('I ','R ','E ')):
    trace[int(l.split()[1])].append(l);nr+=l.startswith('R ');ni+=l.startswith('I ');ne+=l.startswith('E ')
  assert trace and teams=={t},(name,teams)
  result=json.loads((out/name/'result.json').read_text());sig=({k:hashlib.sha256('\n'.join(v).encode()).hexdigest() for k,v in trace.items()},result['output_sha256'],result['step_times_sha256'])
  if ref is None:ref=sig
  else:assert sig==ref,name
  row=dict(name=name,case=case,label=label,threads=t,observed_node_teams=sorted(teams),trial_inputs=ni,residuals=nr,final_faces=ne,node_trace_hashes=sig[0],output_sha256=sig[1],step_times_sha256=sig[2]);rows.append(row);(out/'results.json').write_text(json.dumps(rows,indent=2));print(name,'exact',ni,nr,ne,flush=True)
