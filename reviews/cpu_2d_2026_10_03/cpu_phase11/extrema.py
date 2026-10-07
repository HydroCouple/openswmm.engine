from pathlib import Path
import shutil
p=Path(__file__).resolve().parent;cpp=Path('src/engine/2d/solver/ExplicitInertialSolver.cpp');s=(p/'src_baseline'/cpp).read_text();start=s.index('void ExplicitInertialSolver::computeLimitedGradientsSwe()')
a=s.index('        for (int p = begin; p < end; ++p) {\n            if (!(connected',s.index('        } else {',start));b=s.index('        // Connected interior faces',a)
s=s[:a]+'''        // The smallest limiter ratio comes from the largest positive or
        // most negative extrapolation. Accumulate those extrema before
        // dividing, retaining the same rounded face extrapolations.
        double dmin[4] = {}, dmax[4] = {};
        for (int p = begin; p < end; ++p) {
            if (!(connected & (1u << (p - begin)))) continue;
            const double ax = ed.cell_arm_x[p], ay = ed.cell_arm_y[p];
            for (int m = 0; m < 4; ++m) {
                const double wf = wi[m] + gx[m] * ax + gy[m] * ay;
                const double d = wf - wi[m];
                dmin[m] = std::min(dmin[m], d);
                dmax[m] = std::max(dmax[m], d);
            }
        }
        for (int m = 0; m < 4; ++m) {
            const double upper = wmax[m] - wi[m], lower = wmin[m] - wi[m];
            if (dmax[m] > 1.0e-14 && upper < dmax[m])
                phi[m] = std::min(phi[m], upper / dmax[m]);
            if (dmin[m] < -1.0e-14 && lower > dmin[m])
                phi[m] = std::min(phi[m], lower / dmin[m]);
        }
'''+s[b:]
shutil.copytree(p/'src_baseline',p/'src_extrema',dirs_exist_ok=True);(p/'src_extrema'/cpp).write_text(s)
