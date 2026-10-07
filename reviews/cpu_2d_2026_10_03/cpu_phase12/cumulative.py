from pathlib import Path
import subprocess,json,os,random,hashlib,re,statistics
p=Path(__file__).resolve().parent;rows=[];rng=random.Random(121204);summary=[]
for ns in [0,8,32]:
 deck=p/f'cumulative{ns}.inp'
 if ns==0:
  text=(p/'cumulative8.inp').read_text()
  for section in ['POLLUTANTS','2D_INITIAL_QUALITY']:
   text=re.sub(r'(?m)^\['+section+r'\][\s\S]*?(?=^\[|\Z)','',text)
  deck.write_text(text)
 ref=None
 for repeat in range(-1,3):
  vs=['start','selected'];rng.shuffle(vs);dirs={}
  for v in vs:
   out=p/'engine_runs'/f'cumulative{ns}'/f'{v}_{repeat}';dirs[v]=out
   r=subprocess.run(['python3',str(p/'run_engine.py'),str(p/f'build_{v}/libopenswmm.engine.{v}.dylib'),str(deck),str(out)],env={**os.environ,'OPENSWMM_2D_BACKEND':'cpu'},capture_output=True,text=True,timeout=600)
   out.mkdir(parents=True,exist_ok=True);(out/'process.log').write_text(r.stdout+r.stderr);r.check_returncode()
   row=json.loads(next(x for x in r.stdout.splitlines()if x.startswith('{')))
   report='\n'.join(x for x in (out/'model.rpt').read_text(encoding='latin-1').splitlines()if not re.match(r'\s*(Analysis begun on:|Analysis ended on:|Total elapsed time:)',x));row['report_hash']=hashlib.sha256(report.encode()).hexdigest()
   state={k:x for k,x in row.items()if k not in ['cpu_seconds','init_seconds','run_seconds','total_seconds','peak_rss_bytes']}
   if ref is None:ref=state
   assert state==ref,(ns,v,'state mismatch',state,ref)
   row.update(species=ns,variant=v,repeat=repeat,load_average=os.getloadavg());rows.append(row);(p/'cumulative_results.json').write_text(json.dumps(rows,indent=2))
  files=list(dirs['start'].rglob('*.h5'));assert files
  for f in files:
   r=subprocess.run(['/opt/homebrew/opt/hdf5/bin/h5diff',str(f),str(dirs['selected']/f.relative_to(dirs['start']))],capture_output=True,text=True);assert r.returncode==0,(r.stdout,r.stderr)
  print('cumulative',ns,repeat,'exact state/report/HDF5',flush=True)
 rr=[r for r in rows if r['species']==ns and r['repeat']>=0]
 ratios=[next(r['cpu_seconds']for r in rr if r['repeat']==i and r['variant']=='selected')/next(r['cpu_seconds']for r in rr if r['repeat']==i and r['variant']=='start')for i in range(3)]
 summary.append(dict(species=ns,paired_cpu_ratios=ratios,median_cpu_ratio=statistics.median(ratios),start_cpu=statistics.median(r['cpu_seconds']for r in rr if r['variant']=='start'),selected_cpu=statistics.median(r['cpu_seconds']for r in rr if r['variant']=='selected')))
 (p/'cumulative_summary.json').write_text(json.dumps(summary,indent=2))
