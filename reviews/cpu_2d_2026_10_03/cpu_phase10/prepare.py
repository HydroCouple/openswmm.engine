from pathlib import Path
import json,shutil,hashlib,subprocess,re
p=Path(__file__).resolve().parent;q=p.parent/'accuracy_phase9';root=p.parents[2]
files=list(json.loads((q/'final_verification.json').read_text())['retained_files'])
(p/'baseline_manifest.json').write_text(json.dumps({f:hashlib.sha256((root/f).read_bytes()).hexdigest()for f in files},indent=2))
(p/'baseline_commit.txt').write_bytes(subprocess.check_output(['git','rev-parse','HEAD'],cwd=root))
(p/'.gitignore').write_text('/src_*/\n/build_*/\n/engine_src_*/\n/histories/\n/fields/\n/engine_runs/\n')
for f in ['harness.cpp','performance.cpp','build.py','build_variant.py','prepare_performance.py','build_engine.py','check_suites.py']:
 shutil.copy(q/f,p/f)
shutil.copy(p.parent/'profiler.hpp',p/'profiler.hpp')
shutil.copytree(q/'src_selected',p/'src_baseline')
cpp=Path('src/engine/2d/solver/ExplicitInertialSolver.cpp')
# Freeze the current production implementation, retaining only review accessors in its header.
for f in files[:4]:
 if not f.endswith('ExplicitInertialSolver.hpp'):shutil.copy(root/f,p/'src_baseline'/f)
(p/'test_sources').mkdir(exist_ok=True);shutil.copy(root/files[-1],p/'test_sources'/Path(files[-1]).name)
s=(p/'src_baseline'/cpp).read_text()
shutil.copytree(p/'src_baseline',p/'src_profile')
names=['settleAccumulators','lazySourcesOnly','syncAndRebuild','refreshDt0','fireFacesInertial','fireFacesSwe','fireFacesDiffusive','fireCellsImpl','refreshDiffusiveSlopes','computeLimitedGradientsSwe','runRk2Step','advance']
for name in names:
 s,n=re.subn(r'(\bExplicitInertialSolver::'+name+r'\([^)]*\)\s*\{)',lambda m:m[0]+'\n    review_profile::Timer review_timer("'+name+'");',s);assert n==1
s='#include "'+str(p/'profiler.hpp')+'"\n'+s
(p/'src_profile'/cpp).write_text(s)
shutil.copytree(p/'src_baseline',p/'src_direct')
s=(p/'src_baseline'/cpp).read_text();a=s.index('            double axa = 0.0, aya = 0.0, axb = 0.0, ayb = 0.0;');b=s.index('            const double ua =',a)
s=s[:a]+'''            // These are the same arms stored by InertialEdges::build().
            // Read the face midpoint directly instead of searching both CSR rows.
            const double axa = ed.mx[e] - mesh_->tri_cx[a];
            const double aya = ed.my[e] - mesh_->tri_cy[a];
            const double axb = ed.mx[e] - mesh_->tri_cx[b];
            const double ayb = ed.my[e] - mesh_->tri_cy[b];
'''+s[b:];(p/'src_direct'/cpp).write_text(s)
