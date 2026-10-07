from pathlib import Path
import shutil
p=Path(__file__).resolve().parent;f='src/engine/2d/solver/ExplicitInertialSolver.cpp'
for v in ['connected','connected_face']:
 shutil.copytree(p/'src_shore_wetlimit',p/f'src_{v}',dirs_exist_ok=True)
 s=(p/f'src_{v}'/f).read_text()
 s=s.replace('if (ei - sill <= dry || ej - sill <= dry) { ok = false; break; }','if (ei - sill <= dry || ej - sill <= dry) { shore = true; continue; }')
 needle='if (state_->depth[j] <= dry || !cell_active_[j]) continue;'
 s=s.replace(needle,needle+'''
            const double sill = std::max(ei - hi, state_->head[j] - state_->depth[j]);
            if (ei - sill <= dry || state_->head[j] - sill <= dry) continue;''')
 if v=='connected_face':
  start=s.index('void ExplicitInertialSolver::fireFacesSwe');end=s.index('void ExplicitInertialSolver::fireFacesDiffusive',start)
  face=s[start:end].replace('if (so) {','''const double cell_sill = std::max(state_->head[a] - state_->depth[a], state_->head[b] - state_->depth[b]);
        const bool blocked = state_->head[a] - cell_sill <= dry || state_->head[b] - cell_sill <= dry;
        if (so && !blocked) {''',1)
  s=s[:start]+face+s[end:]
 (p/f'src_{v}'/f).write_text(s)
