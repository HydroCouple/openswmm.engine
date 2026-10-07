from pathlib import Path
import json,hashlib,difflib
p=Path(__file__).resolve().parent;root=p.parents[2];m=json.loads((p/'baseline_manifest.json').read_text())
for f,h in m.items():assert hashlib.sha256((root/f).read_bytes()).hexdigest()==h,f+' changed during review'
f='tests/unit/engine/test_2d_cpu_correctness.cpp';old=(root/f).read_text();new=(p/'test_sources/test_2d_cpu_correctness.cpp').read_text();(p/'baseline_test.cpp').write_text(old)
assert 'SecondOrderShorelineKeepsConnectedWetFaceAccuracy'not in new
assert 'SecondOrderDistortedThackerKeepsShorelineVelocityBounded'in new
(root/f).write_text(new);(p/'retained.patch').write_text(''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/'+f,tofile='b/'+f)))
(p/'.gitignore').write_text('/src_*/\n/build_*/\n/engine_src_*/\n/histories/\n/fields/\n')
