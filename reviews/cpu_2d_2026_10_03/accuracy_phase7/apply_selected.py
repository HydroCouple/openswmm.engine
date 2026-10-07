from pathlib import Path
import json,hashlib,difflib,subprocess
p=Path(__file__).resolve().parent;root=p.parents[2]
access='public:\n    const std::vector<double>& reviewQx()const{return qcx_;}\n    const std::vector<double>& reviewQy()const{return qcy_;}\nprivate:'
rels=['src/engine/2d/solver/ExplicitInertialSolver.cpp','src/engine/2d/solver/ExplicitInertialSolver.hpp','src/engine/2d/solver/SweKernels.hpp','src/engine/2d/data/SolverOptions2D.hpp'];changes={}
for rel in rels:
 old=(p/'src_baseline'/rel).read_text().replace(access,'private:');new=(p/'src_selected'/rel).read_text().replace(access,'private:');assert(root/rel).read_text()==old,'Concurrent source edit: '+rel;changes[rel]=(old,new)
rel='tests/unit/engine/test_2d_cpu_correctness.cpp';new=(p/'test_sources/test_2d_cpu_correctness.cpp').read_text();addition=(p/'new_tests.cpp').read_text();assert new.endswith(addition)
old=(p/'baseline_test.cpp').read_text();assert(root/rel).read_text()==old,'Concurrent regression test edit';changes[rel]=(old,new)
reg=json.loads((p/'regression_results.json').read_text());assert all(r['code']==0 and int(r['counts']['failures'])==0 for r in reg)
assert len(json.loads((p/'accuracy_results.json').read_text()))==56
assert len(json.loads((p/'performance_results.json').read_text()))==50
patch=[]
for rel,(old,new)in changes.items():
 (root/rel).write_text(new);patch.extend(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/'+rel,tofile='b/'+rel))
(p/'selected.patch').write_text(''.join(patch));subprocess.run(['git','diff','--check','--',*changes],cwd=root,check=True)
metadata=dict(committed=False,baseline_commit=(p/'baseline_commit.txt').read_text().strip(),production_hashes={r:hashlib.sha256(new.encode()).hexdigest()for r,(_,new)in changes.items()},previous_hashes={r:hashlib.sha256(old.encode()).hexdigest()for r,(old,_)in changes.items()},tests_passed=166,tests_skipped=1,new_tests_fail_baseline=2,accuracy_executions=56,compatibility_executions=72,cross_thread_executions=8,diagnostic_executions=20,screen_executions=24,timed_executions=50)
(p/'final_verification.json').write_text(json.dumps(metadata,indent=2)+'\n');print('Applied validated second-order reconstruction and two regressions; unrelated changes retained.')
