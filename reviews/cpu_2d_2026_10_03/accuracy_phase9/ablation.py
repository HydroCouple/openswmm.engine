from pathlib import Path
import shutil
p=Path(__file__).resolve().parent;f='src/engine/2d/solver/ExplicitInertialSolver.cpp';k='src/engine/2d/solver/SweKernels.hpp'
for v in ['velocity_unconditioned','velocity_oldsource','velocity_unlimited_depth']:
 shutil.copytree(p/'src_connected_first_velocity',p/f'src_{v}',dirs_exist_ok=True);s=(p/f'src_{v}'/f).read_text()
 if v=='velocity_unconditioned':s=s.replace('det <= 1e-2 *','det <= 1e-12 *')
 if v=='velocity_oldsource':shutil.copy(p/'src_baseline'/k,p/f'src_{v}'/k)
 if v=='velocity_unlimited_depth':s=s.replace('phi[3] *= positive_scale;','phi[3] *= 1.0;').replace('phi[0] *= positive_scale;','phi[0] *= 1.0;')
 (p/f'src_{v}'/f).write_text(s)
s=(p/'screen.py').read_text().replace("['baseline','centered','shore_centered','connected_centered','shore_positive','connected_positive','positive_oldsource']","['velocity_unconditioned','velocity_oldsource','velocity_unlimited_depth']").replace("'screen.json'","'ablation_results.json'");(p/'screen_ablation.py').write_text(s)
