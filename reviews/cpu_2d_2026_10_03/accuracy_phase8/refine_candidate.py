from pathlib import Path
import shutil
p=Path(__file__).resolve().parent;f='src/engine/2d/solver/ExplicitInertialSolver.cpp';shutil.copytree(p/'src_selected',p/'src_fastshore',dirs_exist_ok=True);s=(p/'src_fastshore'/f).read_text()
a=s.index('void ExplicitInertialSolver::computeLimitedGradientsSwe()');b=s.index('// SSP-RK2',a);g=s[a:b]
g=g.replace('''            const int e = ed.cell_edge[p];
            const int j = ed.cL[e] == i ? ed.cR[e] : ed.cL[e];
            if (state_->depth[j] <= dry || !cell_active_[j]) continue;
            const double ax''','''            if (shore) {
                const int e = ed.cell_edge[p];
                const int j = ed.cL[e] == i ? ed.cR[e] : ed.cL[e];
                if (state_->depth[j] <= dry || !cell_active_[j]) continue;
            }
            const double ax''')
g=g.replace('''            // neighbour by a dry sill. Such a face needs the same first-
            // order fallback as a dry neighbour. Extrapolating a surface''','''            // neighbour by a dry sill. Keep the whole cell first order in
            // that case, even with other wet neighbours. Extrapolating a surface''')
g=g.replace('''        if (shore) {
            double xx''','''        if (shore) {
            // A dry cell supplies neither a meaningful water surface nor a
            // velocity. Fit the available wet neighbours instead; two
            // independent directions are needed for a 2D gradient. Keep the
            // interior Green-Gauss path free of least-squares arithmetic.
            double xx''')
start=g.index('            const double dx = mesh_->tri_cx[j]');end=g.index('            }\n            const double det',start)
block=g[start:end];block=block.replace('            const double wi[4] = {ei, ui, vi, hi};\n','');block='\n'.join('    '+line for line in block.split('\n')[:-1])+'\n';g=g[:start]+block+g[end:]
g=g.replace('''        for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
            if (shore)''','''        // At a shoreline, enforce wet-neighbour bounds on the wet faces.
        // Dry-side depth is clipped by hydrostatic face reconstruction.
        for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
            if (shore)''')
s=s[:a]+g+s[b:];(p/'src_fastshore'/f).write_text(s)
