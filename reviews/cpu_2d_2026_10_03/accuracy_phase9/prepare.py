from pathlib import Path
import shutil,json,hashlib,subprocess
root=Path.cwd();p=root/'reviews/cpu_2d_2026_10_03/accuracy_phase9';q=p.parent/'accuracy_phase8';p.mkdir(exist_ok=True)
for name in ['histories','fields']: (p/name).mkdir(exist_ok=True)
files=list(json.loads((q/'baseline_manifest.json').read_text()))
(p/'baseline_manifest.json').write_text(json.dumps({f:hashlib.sha256((root/f).read_bytes()).hexdigest()for f in files},indent=2))
(p/'baseline.diff').write_text(subprocess.check_output(['git','diff','HEAD','--',*files],text=True));(p/'baseline_commit.txt').write_text(subprocess.check_output(['git','rev-parse','HEAD'],text=True))
shutil.copytree(q/'src_baseline',p/'src_baseline',dirs_exist_ok=True)
for f in files:
 dest=p/'src_baseline'/f;dest.parent.mkdir(exist_ok=True,parents=True);dest.write_bytes((root/f).read_bytes())
h=p/'src_baseline/src/engine/2d/solver/ExplicitInertialSolver.hpp';h.write_text(h.read_text().replace('private:', 'public:\n    const std::vector<double>& reviewQx() const { return qcx_; }\n    const std::vector<double>& reviewQy() const { return qcy_; }\nprivate:',1))
for f in ['harness.cpp','build.py','build_variant.py']:shutil.copy(q/f,p/f)
shutil.copy(__file__,p/'prepare.py');(p/'.gitignore').write_text('/src_*/\n/build_*/\n/engine_src_*/\n/histories/\n/fields/\n')
