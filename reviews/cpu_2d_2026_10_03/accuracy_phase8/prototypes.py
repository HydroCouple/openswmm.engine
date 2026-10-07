from pathlib import Path
import shutil
p=Path(__file__).resolve().parent;f='src/engine/2d/solver/ExplicitInertialSolver.cpp';base=(p/'src_baseline'/f).read_text()
start=base.index('void ExplicitInertialSolver::computeLimitedGradientsSwe()');end=base.index('// SSP-RK2',start)
grad=base[start:end]
for name in ['unlimited','shore_ls','shore_wetlimit','all_ls']:
 s=grad
 if name=='unlimited':
  s=s.replace('phi[m] = std::min(phi[m], swe::bjLimiter(wi[m], wf, wmin[m], wmax[m]));','phi[m] = 1.0; // Diagnostic only: no limiter in otherwise admissible cells.')
 else:
  s=s.replace('int nfaces = 0;', '''int nfaces = 0;
        bool shore = false;
        double xx = 0.0, xy = 0.0, yy = 0.0;
        double bx[4] = {}, by[4] = {};''')
  s=s.replace('if (hj <= dry || !cell_active_[j]) { ok = false; break; }','if (hj <= dry || !cell_active_[j]) { shore = true; continue; }')
  s=s.replace('++nfaces;', '''const double dx = mesh_->tri_cx[j] - mesh_->tri_cx[i];
            const double dy = mesh_->tri_cy[j] - mesh_->tri_cy[i];
            const double wt = 1.0 / (dx * dx + dy * dy);
            xx += wt * dx * dx; xy += wt * dx * dy; yy += wt * dy * dy;
            const double wi[4] = {ei, ui, vi, hi};
            for (int m = 0; m < 4; ++m) {
                bx[m] += wt * dx * (wj[m] - wi[m]);
                by[m] += wt * dy * (wj[m] - wi[m]);
            }
            ++nfaces;''')
  old='for (int m = 0; m < 4; ++m) { gx[m] *= inv_a; gy[m] *= inv_a; }'
  s=s.replace(old,old+'''
        if (SHORE_CONDITION) {
            const double det = xx * yy - xy * xy;
            if (nfaces < 2 || det <= 1e-12 * (xx + yy) * (xx + yy)) continue;
            for (int m = 0; m < 4; ++m) {
                gx[m] = (yy * bx[m] - xy * by[m]) / det;
                gy[m] = (xx * by[m] - xy * bx[m]) / det;
            }
        }'''.replace('SHORE_CONDITION','true' if name=='all_ls' else 'shore'))
  if name=='shore_wetlimit':
   s=s.replace('const double ax = ed.cell_arm_x[p], ay = ed.cell_arm_y[p];','''const int e = ed.cell_edge[p];
            const int j = ed.cL[e] == i ? ed.cR[e] : ed.cL[e];
            if (state_->depth[j] <= dry || !cell_active_[j]) continue;
            const double ax = ed.cell_arm_x[p], ay = ed.cell_arm_y[p];''')
 shutil.copytree(p/'src_baseline',p/f'src_{name}',dirs_exist_ok=True)
 (p/f'src_{name}'/f).write_text(base[:start]+s+base[end:])
