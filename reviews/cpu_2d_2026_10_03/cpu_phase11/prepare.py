from pathlib import Path
import json,shutil,hashlib,subprocess,re
p=Path(__file__).resolve().parent;q=p.parent/'cpu_phase10';root=p.parents[2];cpp='src/engine/2d/solver/ExplicitInertialSolver.cpp'
files=[cpp,'src/engine/2d/solver/ExplicitInertialSolver.hpp','src/engine/2d/solver/SweKernels.hpp','src/engine/2d/data/SolverOptions2D.hpp','tests/unit/engine/test_2d_cpu_correctness.cpp']
(p/'baseline_manifest.json').write_text(json.dumps({f:hashlib.sha256((root/f).read_bytes()).hexdigest()for f in files},indent=2));(p/'baseline_commit.txt').write_bytes(subprocess.check_output(['git','rev-parse','HEAD'],cwd=root))
(p/'.gitignore').write_text('/src_*/\n/build_*/\n/engine_src_*/\n/histories/\n/fields/\n/engine_runs/\n/engine_decks/\n/__pycache__/\n')
for f in ['harness.cpp','performance.cpp','profiler.hpp','build.py','build_variant.py','build_performance.py','build_engine.py','check_suites.py','check_analytical.py','run_engine.py','check_models.py','measure.py']:
 shutil.copy(q/f,p/f)
shutil.copytree(q/'src_selected',p/'src_baseline')
for f in [cpp,'src/engine/2d/solver/SweKernels.hpp']:shutil.copy(root/f,p/'src_baseline'/f)
shutil.copytree(q/'test_sources',p/'test_sources');shutil.copytree(q/'engine_decks',p/'engine_decks')
shutil.copytree(p/'src_baseline',p/'src_profile');s=(p/'src_profile'/cpp).read_text()
for name in ['settleAccumulators','lazySourcesOnly','syncAndRebuild','refreshDt0','fireFacesInertial','fireFacesSwe','fireFacesDiffusive','fireCellsImpl','refreshDiffusiveSlopes','computeLimitedGradientsSwe','runRk2Step','advance']:
 s,n=re.subn(r'(\bExplicitInertialSolver::'+name+r'\([^)]*\)\s*\{)',lambda m:m[0]+'\n    review_profile::Timer review_timer("'+name+'");',s);assert n==1
(p/'src_profile'/cpp).write_text('#include "'+str(p/'profiler.hpp')+'"\n'+s)
