from pathlib import Path
import shutil,json,hashlib,subprocess
root=Path.cwd();p=root/'reviews/cpu_2d_2026_10_03/cpu_phase12';p.mkdir(exist_ok=True);q=p.parent/'cpu_phase10';a=p.parent/'accuracy_phase7'
for f in ['.gitignore','build.py','build_variant.py','build_performance.py','run_engine.py','performance.cpp','harness.cpp','check_suites.py']:
 shutil.copy(q/f,p/f)
shutil.copytree(q/'test_sources',p/'test_sources',dirs_exist_ok=True)
manifest={}
for v,origin in [('baseline',p.parent/'accuracy_phase9/src_baseline'),('selected',q/'src_selected')]:
 shutil.copytree(origin,p/f'src_{v}',dirs_exist_ok=True)
for v,origin in [('start',p.parent/'cpu_phase6/src_start'),('selected',q/'engine_src_selected')]:
 src=p/f'engine_src_{v}';shutil.copytree(a/'engine_src_selected',src,dirs_exist_ok=True);(p/f'build_{v}').mkdir(exist_ok=True)
 for f in ['ExplicitInertialSolver.cpp','ExplicitInertialSolver.hpp','SweKernels.hpp']:
  rel=Path('src/engine/2d/solver')/f;shutil.copy(origin/rel,src/rel)
  manifest[f'{v}/{rel}']=hashlib.sha256((src/rel).read_bytes()).hexdigest()
manifest['production']=json.loads((p.parent/'cpu_phase11/final_verification.json').read_text())['production_files_unchanged']
for f,h in manifest['production'].items():assert hashlib.sha256((root/f).read_bytes()).hexdigest()==h,f
(p/'source_manifest.json').write_text(json.dumps(manifest,indent=2))
script=(q/'build_engine.py').read_text();start=script.index("src=p/f'engine_src_{v}'");end=script.index("out=p/f'build_{v}'")
script=script[:start]+"src=p/f'engine_src_{v}';\n"+script[end:]
# Compile focused tests only for the retained implementation; the old solver intentionally fails new tests.
script=script.replace("cmds=json.loads((q/'test_commands.json').read_text());", "if v=='start':\n (p/f'engine_commands_{v}.json').write_text(json.dumps(commands,indent=2));sys.exit(0)\ncmds=json.loads((q/'test_commands.json').read_text());")
(p/'build_engine.py').write_text(script)
(p/'build_baseline').mkdir(exist_ok=True)
for ns in [8,32]:
 text=(p.parent/f'cpu_phase6/cumulative{ns}.inp').read_text().replace('REPORT_2D NO','REPORT_2D YES\nOUTPUT_FILE surface.h5')
 (p/f'cumulative{ns}.inp').write_text(text)
with (p/'.gitignore').open('a') as f:f.write('/cumulative*.inp\n')

snapshot={}
for v in ['baseline','selected']:
 for f in ['ExplicitInertialSolver.cpp','ExplicitInertialSolver.hpp','SweKernels.hpp']:
  path=p/f'src_{v}/src/engine/2d/solver/{f}';snapshot[str(path.relative_to(p))]=hashlib.sha256(path.read_bytes()).hexdigest()
(p/'native_manifest.json').write_text(json.dumps(snapshot,indent=2))
r=subprocess.run(['diff','-u',str(p/'engine_src_start/src/engine/2d/solver/ExplicitInertialSolver.hpp'),str(p/'engine_src_selected/src/engine/2d/solver/ExplicitInertialSolver.hpp')],capture_output=True,text=True)
(p/'header_layout_review.diff').write_text(r.stdout)
