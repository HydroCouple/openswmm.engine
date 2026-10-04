from pathlib import Path
import subprocess,json,os,re,hashlib,resource,random,statistics
p=Path(__file__).resolve().parent;d=p/'engine_decks/gradient_probe';d.mkdir(exist_ok=True)
s=(p/'engine_decks/surface_wet_swe2_t1/model.inp').read_text();s=re.sub(r'^END_TIME .*$', 'END_TIME 00:00:20',s,flags=re.M);s=re.sub(r'^REPORT_STEP .*$', 'REPORT_STEP 00:00:05',s,flags=re.M);(d/'model.inp').write_text(s)
(p/'gradient_probe_provenance.json').write_text(json.dumps(dict(source='surface_wet_swe2_t1',cells=32768,threads=1,duration_seconds=20,report_step_seconds=5,sha256=hashlib.sha256(s.encode()).hexdigest()),indent=2))
rows=[];rng=random.Random(11033)
for repeat in range(-1,3):
 vs=['baseline','extrema'];rng.shuffle(vs);states=[];outs={}
 for v in vs:
  out=p/'engine_runs'/f'gradient_probe_{repeat}_{v}';outs[v]=out
  r=subprocess.run(['python3',str(p/'run_engine.py'),str(p/f'build_{v}/libopenswmm.engine.{v}.dylib'),str(d/'model.inp'),str(out)],env={**os.environ,'OPENSWMM_2D_BACKEND':'cpu'},capture_output=True,text=True,timeout=300);out.mkdir(exist_ok=True,parents=True);(out/'process.log').write_text(r.stdout+r.stderr);r.check_returncode()
  row=json.loads(next(x for x in r.stdout.splitlines()if x.startswith('{')));report='\n'.join(x for x in (out/'model.rpt').read_text(encoding='latin-1').splitlines()if not re.match(r'\s*(Analysis begun on:|Analysis ended on:|Total elapsed time:)',x));row['report_hash']=hashlib.sha256(report.encode()).hexdigest()
  state={k:x for k,x in row.items()if k not in ['cpu_seconds','init_seconds','run_seconds','total_seconds','peak_rss_bytes']};states.append(state);row.update(variant=v,repeat=repeat,load=os.getloadavg());rows.append(row)
 assert states[0]==states[1],(repeat,states)
 a=outs['baseline']/'surface.h5';b=outs['extrema']/'surface.h5';r=subprocess.run(['/opt/homebrew/opt/hdf5/bin/h5diff',str(a),str(b)],capture_output=True,text=True);assert r.returncode==0,(r.stdout,r.stderr)
 (p/'coupled_probe.json').write_text(json.dumps(rows,indent=2));print(repeat,'exact state/report/HDF5',flush=True)
ratios=[next(r['cpu_seconds']for r in rows if r['repeat']==i and r['variant']=='extrema')/next(r['cpu_seconds']for r in rows if r['repeat']==i and r['variant']=='baseline')for i in range(3)]
summary=dict(median_cpu_ratio=statistics.median(ratios),ratios=ratios,baseline_cpu=statistics.median(r['cpu_seconds']for r in rows if r['repeat']>=0 and r['variant']=='baseline'),extrema_cpu=statistics.median(r['cpu_seconds']for r in rows if r['repeat']>=0 and r['variant']=='extrema'))
(p/'coupled_probe_summary.json').write_text(json.dumps(summary,indent=2));print(summary,flush=True)
