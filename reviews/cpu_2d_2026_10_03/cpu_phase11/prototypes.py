from pathlib import Path
import shutil
p=Path(__file__).resolve().parent;cpp=Path('src/engine/2d/solver/ExplicitInertialSolver.cpp');baseline=(p/'src_baseline'/cpp).read_text()
old='''                phi[m] = std::min(phi[m], swe::bjLimiter(wi[m], wf, wmin[m], wmax[m]));'''
new='''                // An extrapolation inside the neighbour bounds cannot
                // tighten a limiter already in [0, 1]. Divide only when it can.
                const double d = wf - wi[m];
                if (d > 1.0e-14 && wmax[m] - wi[m] < d)
                    phi[m] = std::min(phi[m], (wmax[m] - wi[m]) / d);
                else if (d < -1.0e-14 && wmin[m] - wi[m] > d)
                    phi[m] = std::min(phi[m], (wmin[m] - wi[m]) / d);'''
assert baseline.count(old)==1
friction='''                swe::frictionUpdate(qx, qy, h_new, mesh_->mannings_n[i], dt_c);'''
friction_new='''                // With zero roughness the positive-depth update is the identity.
                if (mesh_->mannings_n[i] != 0.0)
                    swe::frictionUpdate(qx, qy, h_new, mesh_->mannings_n[i], dt_c);'''
assert baseline.count(friction)==1
clear='''        gex_[depth_offset + i] = gey_[depth_offset + i] = gex_[i] = gey_[i] = gux_[i] = guy_[i] = gvx_[i] = gvy_[i] = 0.0;'''
assert baseline.count(clear)==1
s=baseline.replace(clear,'''        const auto clear_gradients = [&]() {
    '''+clear+'''
        };''')
a=s.index('void ExplicitInertialSolver::computeLimitedGradientsSwe()');b=s.index('\n// SSP-RK2',a);g=s[a:b]
for test in ['hi <= 10.0 * dry','nfaces == 0','nfaces < 2','det <= 1e-2 * (xx + yy) * (xx + yy)']:
 oldtest='if ('+test+') continue;';assert g.count(oldtest)==1;g=g.replace(oldtest,'if ('+test+') { clear_gradients(); continue; }')
s=s[:a]+g+s[b:]
variants=dict(limiter=baseline.replace(old,new),friction=baseline.replace(friction,friction_new),clear=s,combined=s.replace(old,new).replace(friction,friction_new))
for v,s in variants.items():
 shutil.copytree(p/'src_baseline',p/f'src_{v}',dirs_exist_ok=True);(p/f'src_{v}'/cpp).write_text(s)
