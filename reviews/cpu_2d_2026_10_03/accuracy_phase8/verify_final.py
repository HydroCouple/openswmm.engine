from pathlib import Path
import hashlib,json,math,subprocess
p=Path(__file__).resolve().parent;root=p.parents[2];manifest=json.loads((p/'baseline_manifest.json').read_text());test='tests/unit/engine/test_2d_cpu_correctness.cpp'
def sha(f):return hashlib.sha256(f.read_bytes()).hexdigest()
for f,h in manifest.items():
 if f!=test:assert sha(root/f)==h,f
assert sha(root/test)==sha(p/'test_sources/test_2d_cpu_correctness.cpp')
assert sha(p/'baseline_test.cpp')==manifest[test]
reg=json.loads((p/'regression_results.json').read_text());assert len(reg)==15 and all(r['code']==0 for r in reg)
nt=sum(int(r['counts']['tests'])for r in reg);sk=sum(int(r['counts']['skipped'])for r in reg);assert(nt,sk)==(168,1)
neg=json.loads((p/'final_test_results.json').read_text());assert neg[0]['code']==0 and all(r['code']!=0 and r['counts']['failures']=='1'for r in neg[1:])
count=0;worst=0
for file,n in [('screen.json',60),('connected_results.json',24),('confirmation.json',24),('diagnosis.json',12),('mesh_results.json',84)]:
 a=json.loads((p/file).read_text());assert len(a)==n;count+=n
 for r in a:
  assert 'error'not in r,r
  assert r['min_depth']>=0 and math.isfinite(r['relative_l1']) and abs(r['volume_relative_error'])<1e-11
  worst=max(worst,abs(r['volume_relative_error']))
files=['REPORT.md','comparison.png','comparison.svg','retained.patch','regression_results.json','final_test_results.json','screen.json','connected_results.json','confirmation.json','diagnosis.json','mesh_results.json','energy_results.json','performance_results.json']
metadata=dict(committed=False,solver_changes_retained=False,retained_changes=['Extend planar Thacker velocity check','Add distorted/mixed radial Thacker velocity regression'],baseline_commit=(p/'baseline_commit.txt').read_text().strip(),production_solver_hashes_unchanged={f:h for f,h in manifest.items()if f!=test},test_sha256=sha(root/test),analytical_runs=count,max_relative_water_error=worst,tests=nt,passed=nt-sk,skipped=sk,negative_controls='Both intended regressions fail the rejected prototypes',artifacts={f:sha(p/f)for f in files})
(p/'final_verification.json').write_text(json.dumps(metadata,indent=2));print(json.dumps({k:v for k,v in metadata.items()if k not in ['artifacts','production_solver_hashes_unchanged']},indent=2))
subprocess.run(['git','diff','--check','--',test],cwd=root,check=True)
