from pathlib import Path
import subprocess,json,os,hashlib,re,math,sys
p=Path(__file__).resolve().parent;q=p;result_file=p/('bellinge_results.json' if any('bellinge' in x for x in sys.argv[1:]) else 'model_results.json');rows=json.loads(result_file.read_text()) if len(sys.argv)>1 and result_file.exists() else []
if len(sys.argv)>1: rows=[r for r in rows if r['case'] not in sys.argv[1:]]
for deck in sorted((p/'engine_decks').iterdir()):
 if len(sys.argv)>1 and deck.name not in sys.argv[1:]:continue
 if len(sys.argv)==1 and 'bellinge' in deck.name:continue
 ref=None;dirs=[]
 for v in ['baseline','selected']:
  out=p/'engine_runs'/deck.name/v;dirs.append(out)
  result=subprocess.run(['python3',str(p/'run_engine.py'),str(q/f'build_{v}/libopenswmm.engine.{v}.dylib'),str(deck/'model.inp'),str(out)],env={**os.environ,'OPENSWMM_2D_BACKEND':'cpu'},capture_output=True,text=True,timeout=300)
  out.mkdir(exist_ok=True,parents=True);(out/'process.log').write_text(result.stdout+result.stderr);result.check_returncode()
  if 'swe2' in deck.name: assert 'using first order' not in result.stderr
  row=json.loads(next(x for x in result.stdout.splitlines()if x.startswith('{')))
  if '_swe' in deck.name: assert row['stats']['momentum']==1 and row['stats']['lts_tiers']==1 
  if 'swe2' in deck.name: assert row['stats']['face_evals']>0
  report='\n'.join(x for x in (out/'model.rpt').read_text(encoding='latin-1').splitlines()if not re.match(r'\s*(Analysis begun on:|Analysis ended on:|Total elapsed time:)',x))
  row['report_hash']=hashlib.sha256(report.encode()).hexdigest()
  state={k:x for k,x in row.items()if k not in ['cpu_seconds','init_seconds','run_seconds','total_seconds','peak_rss_bytes']}
  if ref is None:ref=state
  assert state==ref,(deck.name,v,'state/report mismatch')
  gw=row['groundwater'];assert gw['active']==('groundwater' in deck.name or 'dunne' in deck.name),(deck.name,gw)
  if gw['active']:
   assert abs(gw['residual'])<1e-8*max(1,abs(gw['ledger'][8])),gw
   for terms in gw['species']:assert abs(terms[12])<1e-8*max(1,abs(terms[0]),abs(terms[2])),terms
  row.update(case=deck.name,variant=v);rows.append(row)
 h5=[]
 for f in dirs[0].rglob('*.h5'):
  g=dirs[1]/f.relative_to(dirs[0]);assert g.exists(),g
  diff=subprocess.run(['/opt/homebrew/opt/hdf5/bin/h5diff',str(f),str(g)],capture_output=True,text=True)
  (dirs[1]/'h5diff.log').write_text(diff.stdout+diff.stderr);assert diff.returncode==0,(f,diff.stdout,diff.stderr)
  h5.append(str(f.relative_to(dirs[0])))
 if 'parking_lot' in deck.name or 'surface_' in deck.name or 'bellinge' in deck.name:assert h5,(deck.name,'missing HDF5')
 rows[-1]['hdf5_exact']=h5
 result_file.write_text(json.dumps(rows,indent=2));print(deck.name,'exact state/report/HDF5',h5,flush=True)
print('PASS',len(rows),'complete model runs',flush=True)
