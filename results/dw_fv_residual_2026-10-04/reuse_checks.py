from pathlib import Path
import json,sys,os,subprocess,collections,hashlib
R=Path(__file__).resolve().parent
cases=[('pressure',4,1,'1'),('free',16,4,'0'),('reverse_mixed',4,8,'1'),('transition_tpa',4,1,'1')]
if len(sys.argv)>1:
 import fixture
 label=sys.argv[1];out=R/'reuse_checks'/label;out.mkdir(parents=True,exist_ok=False);results=[]
 for i,(case,cells,t,mixed) in enumerate(cases):
  os.environ['OPENSWMM_FV_MIXED_WAVE']=mixed;print(f'SEQUENCE {i}',file=sys.stderr,flush=True)
  fixture.net.deck=lambda *a,**kw:fixture.deck(case,cells,t,2)
  fixture.net.worker('diagnostic_'+label,'FV',cells,t,2,False,out/str(i));results.append(json.loads((out/str(i)/'result.json').read_text()))
 (out/'results.json').write_text(json.dumps(results,indent=2));sys.exit()
out=R/'reuse_checks';out.mkdir(exist_ok=False);ref=None;rows=[]
for label in ('baseline','candidate'):
 env={k:v for k,v in os.environ.items() if not k.startswith(('OMP_','KMP_','OPENSWMM_','SWMM_','DYLD_','FV_TEST_'))};env.update(OMP_NUM_THREADS='8',OMP_THREAD_LIMIT='8',OMP_DYNAMIC='FALSE',OMP_WAIT_POLICY='PASSIVE',KMP_BLOCKTIME='0',OPENSWMM_FV_RESIDUAL_AUDIT='1',OPENSWMM_FV_OMP_MIN_NODES='1',FV_TEST_GROUPS='8')
 cp=subprocess.run([sys.executable,__file__,label],env=env,capture_output=True,text=True,timeout=180);(out/(label+'.log')).write_text(cp.stdout+cp.stderr);assert cp.returncode==0,cp.stderr[-2000:];seq=-1;trace=collections.defaultdict(list);teams=collections.defaultdict(set)
 for line in cp.stderr.splitlines():
  if line.startswith('SEQUENCE '):seq=int(line.split()[1])
  elif line.startswith('TEAM '):teams[seq].add(int(line.split()[2]))
  elif line.startswith(('I ','R ','E ')):trace[(seq,int(line.split()[1]))].append(line)
 assert [teams[i] for i in range(4)]==[{1},{4},{8},{1}],teams
 sums=json.loads((out/label/'results.json').read_text());sig=({str(k):hashlib.sha256('\n'.join(v).encode()).hexdigest() for k,v in trace.items()},[[x[k] for k in ('output_sha256','step_times_sha256','substeps','fluxes','macros')] for x in sums])
 if ref is None:ref=sig
 else:assert sig==ref
 rows.append(dict(label=label,observed_teams={i:sorted(v) for i,v in teams.items()},trace_records=sum(len(v) for v in trace.values()),trace_hashes=sig[0]));(out/'results.json').write_text(json.dumps(rows,indent=2))
print('success: exact across four successive engines per process')
