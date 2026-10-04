from pathlib import Path
import shutil
p=Path(__file__).resolve().parent;f='src/engine/2d/solver/ExplicitInertialSolver.cpp';shutil.copytree(p/'src_shore_wetlimit',p/'src_selected',dirs_exist_ok=True);s=(p/'src_selected'/f).read_text()
a=s.index('void ExplicitInertialSolver::computeLimitedGradientsSwe()');b=s.index('// SSP-RK2',a);g=s[a:b]
g=g.replace('        double xx = 0.0, xy = 0.0, yy = 0.0;\n        double bx[4] = {}, by[4] = {};\n','')
a2=g.index('            const double dx = mesh_->tri_cx[j]');b2=g.index('            ++nfaces;',a2)
block=g[a2:b2];g=g[:a2]+g[b2:]
g=g.replace('''        if (shore) {
            const double det''','''        if (shore) {
            double xx = 0.0, xy = 0.0, yy = 0.0;
            double bx[4] = {}, by[4] = {};
            for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
                const int e = ed.cell_edge[p];
                const int j = ed.cL[e] == i ? ed.cR[e] : ed.cL[e];
                const double hj = state_->depth[j];
                if (hj <= dry || !cell_active_[j]) continue;
                const double wj[4] = {state_->head[j], qcx_[j] / hj, qcy_[j] / hj, hj};
'''+block+'''            }
            const double det''')
s=s[:a]+g+s[b:];(p/'src_selected'/f).write_text(s)
