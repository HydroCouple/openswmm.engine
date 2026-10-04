from pathlib import Path
import shutil
p=Path(__file__).resolve().parent;q=p.parent/'accuracy_phase8';f='src/engine/2d/solver/ExplicitInertialSolver.cpp';kf='src/engine/2d/solver/SweKernels.hpp'
for name,parent,pressure,positive in [('centered','baseline',True,False),('shore_centered','selected',True,False),('connected_centered','connected',True,False),('shore_positive','selected',True,True),('connected_positive','connected',True,True),('positive_oldsource','selected',False,True)]:
 shutil.copytree(p/'src_baseline',p/f'src_{name}',dirs_exist_ok=True)
 if parent!='baseline':shutil.copy(q/f'src_{parent}'/f,p/f'src_{name}'/f)
 s=(p/f'src_{name}'/f).read_text()
 if positive:
  needle='        gex_[depth_offset + i] = phi[3] * gx[3]; gey_[depth_offset + i] = phi[3] * gy[3];'
  new='''        // Review prototype: limit the linear depth polynomial BEFORE
        // hydrostatic reconstruction, including physical boundary faces.
        double positive_scale = 1.0;
        for (int kk = 0; kk < nvc; ++kk) {
            const int va = mesh_->cell_vertex(i, kk);
            const int vb = mesh_->cell_vertex(i, (kk + 1) % nvc);
            const double ax = .5 * (mesh_->vx[va] + mesh_->vx[vb]) - mesh_->tri_cx[i];
            const double ay = .5 * (mesh_->vy[va] + mesh_->vy[vb]) - mesh_->tri_cy[i];
            const double dh = phi[3] * (gx[3] * ax + gy[3] * ay);
            if (dh < -hi) positive_scale = std::min(positive_scale, -hi / dh);
        }
        phi[3] *= positive_scale;
'''+needle
  assert needle in s;s=s.replace(needle,new)
 (p/f'src_{name}'/f).write_text(s)
 if pressure:
  k=(p/f'src_{name}'/kf).read_text();start=k.index('    const double hLf = (etaLf - zL');end=k.index('    corrL_x =',start)
  k=k[:start]+'''    const double hLf = std::max(0.0, etaLf - zLf);
    const double hRf = std::max(0.0, etaRf - zRf);
    // Hydrostatic face correction plus a centered within-cell bed source.
    const double cL = 0.5 * kGravity * (L.h * L.h - hLf * hLf
        - (hL_cell + hLf) * (zLf - zL));
    const double cR = 0.5 * kGravity * (R.h * R.h - hRf * hRf
        - (hR_cell + hRf) * (zRf - zR));
'''+k[end:];(p/f'src_{name}'/kf).write_text(k)
