from pathlib import Path
import shutil
p=Path(__file__).resolve().parent;cpp=Path('src/engine/2d/solver/ExplicitInertialSolver.cpp')
shutil.copytree(p/'src_direct',p/'src_cached',dirs_exist_ok=True)
f=p/'src_cached'/cpp;s=f.read_text();s=s.replace('gux_.assign(un, 0.0); guy_.assign(un, 0.0);','gux_.assign(2 * un, 0.0); guy_.assign(2 * un, 0.0);')
a=s.index('void ExplicitInertialSolver::computeLimitedGradientsSwe()');b=s.index('\nvoid ExplicitInertialSolver::runRk2Step',a);frag=s[a:b]
old='#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)\n    for (int k = 0; k < na; ++k) {'
new='''// Cache each active cell's velocity once per RK stage. Both gradient
    // neighbours and face states reuse these divisions. The implicit barrier
    // completes the cache before any neighbour reads it.
#pragma omp parallel num_threads(opts_->num_threads)
    {
#pragma omp for schedule(static)
    for (int k = 0; k < na; ++k) {
        const int i = active_cells_[static_cast<std::size_t>(k)];
        const double h = state_->depth[i];
        gux_[depth_offset + i] = h > dry ? qcx_[i] / h : 0.0;
        guy_[depth_offset + i] = h > dry ? qcy_[i] / h : 0.0;
    }
#pragma omp for schedule(static)
    for (int k = 0; k < na; ++k) {'''
assert frag.count(old)==1;frag=frag.replace(old,new).replace('const double ui = qcx_[i] / hi, vi = qcy_[i] / hi;','const double ui = gux_[depth_offset + i], vi = guy_[depth_offset + i];').replace('const double uj = qcx_[j] / hj, vj = qcy_[j] / hj;','const double uj = gux_[depth_offset + j], vj = guy_[depth_offset + j];')
# Close the parallel region before the function's closing brace.
pos=frag.rfind('\n}');frag=frag[:pos]+'\n    }'+frag[pos:];s=s[:a]+frag+s[b:]
s=s.replace('const double ua = (ha > dry) ? qcx_[a] / ha : 0.0, va = (ha > dry) ? qcy_[a] / ha : 0.0;','const double ua = gux_[depth_offset + a], va = guy_[depth_offset + a];').replace('const double ub = (hb > dry) ? qcx_[b] / hb : 0.0, vb = (hb > dry) ? qcy_[b] / hb : 0.0;','const double ub = gux_[depth_offset + b], vb = guy_[depth_offset + b];')
f.write_text(s)
