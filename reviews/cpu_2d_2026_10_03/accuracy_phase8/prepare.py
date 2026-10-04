from pathlib import Path
import shutil,json,hashlib,subprocess
root=Path.cwd();p=root/'reviews/cpu_2d_2026_10_03/accuracy_phase8';q=p.parent/'accuracy_phase7';p.mkdir(exist_ok=True)
(p/'histories').mkdir(exist_ok=True)
files=['src/engine/2d/solver/ExplicitInertialSolver.cpp','src/engine/2d/solver/ExplicitInertialSolver.hpp','src/engine/2d/solver/SweKernels.hpp','src/engine/2d/data/SolverOptions2D.hpp','tests/unit/engine/test_2d_cpu_correctness.cpp']
(p/'baseline_manifest.json').write_text(json.dumps({f:hashlib.sha256((root/f).read_bytes()).hexdigest()for f in files},indent=2))
(p/'baseline.diff').write_text(subprocess.check_output(['git','diff','HEAD','--',*files],text=True))
(p/'baseline_commit.txt').write_text(subprocess.check_output(['git','rev-parse','HEAD'],text=True))
shutil.copytree(q/'src_selected',p/'src_baseline',dirs_exist_ok=True)
for f in files[:4]:
 t=p/'src_baseline'/f;t.write_bytes((root/f).read_bytes())
h=p/'src_baseline'/files[1];h.write_text(h.read_text().replace('private:', 'public:\n    const std::vector<double>& reviewQx() const { return qcx_; }\n    const std::vector<double>& reviewQy() const { return qcy_; }\nprivate:',1))
shutil.copy(q/'harness.cpp',p/'harness.cpp')
shutil.copy(q/'build.py',p/'build.py');shutil.copy(q/'build_variant.py',p/'build_variant.py')
shutil.copy(__file__,p/'prepare.py')
