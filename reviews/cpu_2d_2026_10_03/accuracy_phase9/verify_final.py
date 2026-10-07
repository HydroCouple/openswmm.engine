from pathlib import Path
import json,hashlib,math,subprocess,re
p=Path(__file__).resolve().parent;root=p.parents[2];manifest=json.loads((p/'baseline_manifest.json').read_text());sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest();source_hashes={}
for f in manifest:
 actual=(root/f).read_text()
 if f.startswith('tests/'):expected=(p/'test_sources/test_2d_cpu_correctness.cpp').read_text()
 else:expected=(p/'src_selected'/f).read_text()
 if f.endswith('ExplicitInertialSolver.hpp'):
  expected=expected.replace('public:\n    const std::vector<double>& reviewQx() const { return qcx_; }\n    const std::vector<double>& reviewQy() const { return qcy_; }\nprivate:','private:',1)
 assert actual==expected,f;source_hashes[f]=sha(root/f)
# Verify the compiled full-engine snapshot differs from retained source only
# in review-only accessors or comments, never numerical implementation.
def tokens(s):return ''.join(re.sub(r'//[^\n]*','',re.sub(r'/\*.*?\*/','',s,flags=re.S)).split())
for n in ['ExplicitInertialSolver.cpp','SweKernels.hpp']:
 assert tokens((root/'src/engine/2d/solver'/n).read_text())==tokens((p/'engine_src_selected/src/engine/2d/solver'/n).read_text())
reg=json.loads((p/'regressions_selected.json').read_text());assert len(reg)==15 and all(r['code']==0 for r in reg);nt=sum(int(r['counts']['tests'])for r in reg);skip=sum(int(r['counts']['skipped'])for r in reg);assert(nt,skip)==(170,1)
for f,n in [('equivalence_results.json',18),('optimized_confirmation.json',18)]:
 a=json.loads((p/f).read_text());assert len(a)==n and all(not r['non_timing_differences']and r['history_identical']for r in a)
counts={};water=0
for f,n in [('screen.json',126),('joint_results.json',36),('stencil_results.json',54),('ablation_results.json',54),('confirmation.json',36),('stress_results.json',18),('mesh_results.json',84),('accuracy_cost_results.json',4)]:
 a=json.loads((p/f).read_text());assert len(a)==n,(f,len(a));counts[f]=len(a)
 for r in a:
  assert 'error'not in r,r;assert r['min_depth']>=0 and math.isfinite(r['relative_l1']) and abs(r['volume_relative_error'])<1e-11,r;water=max(water,abs(r['volume_relative_error']))
counts.update({'equivalence_results.json':18,'optimized_confirmation.json':18});assert sum(counts.values())==448
perf=json.loads((p/'performance_results.json').read_text());assert len(perf)==74;summary=json.loads((p/'performance_summary.json').read_text());assert len(summary)==9
for r in summary:
 assert r['pairs']==(5 if r['comparison']=='same_mesh'else 3)
 if r['comparison']=='accuracy_cost':assert r['selected']['relative_l1']<r['baseline']['relative_l1']
files=['REPORT.md','comparison.png','comparison.svg','retained.patch','screen.json','joint_results.json','stencil_results.json','ablation_results.json','confirmation.json','optimized_confirmation.json','stress_results.json','mesh_results.json','accuracy_cost_results.json','equivalence_results.json','regressions_selected.json','baseline_new_tests.xml','performance_results.json','performance_summary.json','energy_results.json','budget_events.json']
out=dict(committed=False,baseline_commit=(p/'baseline_commit.txt').read_text().strip(),retained_files=source_hashes,tests=nt,passed=nt-skip,skipped=skip,analytical_executions=sum(counts.values()),analytical_counts=counts,max_relative_water_error_including_rejected_prototypes=water,measured_timing_runs=len(perf),discarded_warmup_runs=18,compiled_tokens_match=True,artifacts={f:sha(p/f)for f in files},binaries={str(x.relative_to(p)):sha(x)for x in [p/'build_selected/review',p/'build_selected/performance',p/'build_selected/libopenswmm.engine.selected.dylib',p/'build_selected/test_engine_2d_cpu_correctness']})
(p/'final_verification.json').write_text(json.dumps(out,indent=2));subprocess.run(['git','diff','--check','--',*manifest],cwd=root,check=True)
print(json.dumps({k:out[k]for k in ['committed','passed','skipped','analytical_executions','measured_timing_runs','compiled_tokens_match']},indent=2))
