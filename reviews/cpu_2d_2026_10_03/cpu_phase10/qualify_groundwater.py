from pathlib import Path
import shutil,re,subprocess,json,os
p=Path(__file__).resolve().parent;rows=[]
for closure in ['flat','vfr']:
 old=p/f'engine_decks/two_way_groundwater_swe2_{closure}';archive=p/'unsupported_decks'/old.name;archive.parent.mkdir(exist_ok=True)
 if old.exists():shutil.move(str(old),str(archive))
 for v in ['baseline','selected']:
  out=p/'engine_runs'/f'unsupported_{closure}_{v}';r=subprocess.run(['python3',str(p/'run_engine.py'),str(p/f'build_{v}/libopenswmm.engine.{v}.dylib'),str(archive/'model.inp'),str(out)],capture_output=True,text=True,env={**os.environ,'OPENSWMM_2D_BACKEND':'cpu'})
  expected='groundwater is not advanced by the RK2 surface route';assert r.returncode!=0 and expected in r.stderr
  rows.append(dict(closure=closure,variant=v,rejected_as_expected=True,error=expected));(out/'process.log').write_text(r.stdout+r.stderr)
 new=p/f'engine_decks/two_way_groundwater_swe1_{closure}';new.mkdir(exist_ok=True);s=(archive/'model.inp').read_text().replace('RECONSTRUCTION_ORDER 2','RECONSTRUCTION_ORDER 1');(new/'model.inp').write_text(s)
(p/'unsupported_groundwater.json').write_text(json.dumps(rows,indent=2))
f=p/'check_models.py';s=f.read_text().replace("q=p;rows=[]", "q=p;result_file=p/('bellinge_results.json' if 'bellinge_10min_swe2' in sys.argv else 'model_results.json');rows=json.loads(result_file.read_text()) if len(sys.argv)>1 and result_file.exists() else []\nif len(sys.argv)>1: rows=[r for r in rows if r['case'] not in sys.argv[1:]]")
s=s.replace("if len(sys.argv)>1 and deck.name not in sys.argv[1:]:continue", "if len(sys.argv)>1 and deck.name not in sys.argv[1:]:continue\n if len(sys.argv)==1 and 'bellinge' in deck.name:continue")
s=s.replace("if 'swe2' in deck.name: assert row", "if '_swe' in deck.name: assert row").replace("(p/('bellinge_results.json' if len(sys.argv)>1 else 'model_results.json')).write_text", "result_file.write_text")
f.write_text(s)
