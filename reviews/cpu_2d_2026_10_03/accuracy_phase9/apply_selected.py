from pathlib import Path
import json,hashlib,difflib
p=Path(__file__).resolve().parent;root=p.parents[2];manifest=json.loads((p/'baseline_manifest.json').read_text());patch=[]
for f,h in manifest.items():assert hashlib.sha256((root/f).read_bytes()).hexdigest()==h,f+' changed during phase 9'
for f in manifest:
 old=(root/f).read_text()
 if f.startswith('tests/'):
  new=(p/'test_sources/test_2d_cpu_correctness.cpp').read_text()
 else:new=(p/'src_selected'/f).read_text()
 if f.endswith('ExplicitInertialSolver.hpp'):
  getters='public:\n    const std::vector<double>& reviewQx() const { return qcx_; }\n    const std::vector<double>& reviewQy() const { return qcy_; }\nprivate:'
  assert getters in new;new=new.replace(getters,'private:',1)
 if old==new:continue
 assert 'reviewQx'not in new and 'reviewQy'not in new
 patch.extend(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/'+f,tofile='b/'+f));(root/f).write_text(new)
(p/'retained.patch').write_text(''.join(patch))
