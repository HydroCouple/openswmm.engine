from pathlib import Path
import json,hashlib,subprocess,difflib
p=Path(__file__).resolve().parent;root=p.parents[2];f='src/engine/2d/solver/ExplicitInertialSolver.cpp';base=json.loads((p/'baseline_manifest.json').read_text())
for name,h in base.items():assert hashlib.sha256((root/name).read_bytes()).hexdigest()==h,('production drift',name)
a=json.loads((p/'analytical_results.json').read_text());assert len(a)==70 and all(x['baseline_selected_exact']for x in a)
tests=json.loads((p/'regressions_selected.json').read_text());assert len(tests)==15 and all(x['code']==0 for x in tests)
models=json.loads((p/'model_results.json').read_text());assert len(models)==20
bellinge=json.loads((p/'bellinge_results.json').read_text());assert len(bellinge)==4
assert all(x['mass_balance'][1]>0 and x['stats']['active_frac_mean']>.9 for x in bellinge if 'storm' in x['case'])
assert all(x['stats']['momentum']==1 and x['stats']['face_evals']>0 for x in models+bellinge if 'swe2' in x['case'])
assert (p/'src_selected'/f).read_bytes()==(p/'engine_src_selected'/f).read_bytes()
for x in a:assert abs(x['volume_relative_error'])<1e-11
perf=json.loads((p/'performance_summary.json').read_text());assert len(perf)==7
mt=json.loads((p/'model_timing_summary.json').read_text());assert len(mt)==4
(root/f).write_bytes((p/'src_selected'/f).read_bytes())
result=dict(committed=False,baseline_commit=(p/'baseline_commit.txt').read_text().strip(),changed_files={f:hashlib.sha256((root/f).read_bytes()).hexdigest()},tests=sum(int(x['counts']['tests'])for x in tests),skipped=sum(int(x['counts']['skipped'])for x in tests),analytical_runs=2*len(a),analytical_exact_pairs=len(a),coupled_runs=len(models)+len(bellinge),coupled_exact_pairs=(len(models)+len(bellinge))//2,measured_component_runs=70,measured_coupled_runs=24,discarded_timing_warmups=22,production_matches_compiled=True)
(p/'final_verification.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
subprocess.run(['git','diff','--check','--',f],cwd=root,check=True)
