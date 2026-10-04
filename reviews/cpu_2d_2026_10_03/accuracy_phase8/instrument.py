from pathlib import Path
import shutil
p=Path(__file__).resolve().parent;v='instrumented';f='src/engine/2d/solver/ExplicitInertialSolver.cpp';shutil.copytree(p/'src_baseline',p/f'src_{v}',dirs_exist_ok=True);s=(p/f'src_{v}'/f).read_text()
struct='''
// Review-only counters: this diagnostic executable is restricted to one thread.
struct ReviewCounters {
    double cell[9] = {}, volume[9] = {};
    double faces = 0, capped = 0, raw_export = 0, removed_export = 0;
    void add(int k, double v) { cell[k] += 1; volume[k] += v; }
    ~ReviewCounters() {
        std::fprintf(stderr, "REVIEW {\\\"cell\\\":[");
        for(int k=0;k<9;++k)std::fprintf(stderr,"%s%.17g",k?",":"",cell[k]);
        std::fprintf(stderr,"],\\\"volume\\\":[");
        for(int k=0;k<9;++k)std::fprintf(stderr,"%s%.17g",k?",":"",volume[k]);
        std::fprintf(stderr,"],\\\"faces\\\":%.17g,\\\"capped\\\":%.17g,\\\"raw_export\\\":%.17g,\\\"removed_export\\\":%.17g}\\n",faces,capped,raw_export,removed_export);
    }
} review;
'''
s=s.replace('namespace openswmm::twoD {','namespace openswmm::twoD {\n'+struct,1)
s=s.replace('mesh_  = &mesh;', 'if (opts.num_threads != 1) throw std::runtime_error("Review counters require one thread");\n    mesh_  = &mesh;',1)
a=s.index('void ExplicitInertialSolver::computeLimitedGradientsSwe()');b=s.index('// SSP-RK2',a);g=s[a:b]
g=g.replace('if (hi <= 10.0 * dry) continue;', 'review.add(0, state_->volume[i]);\n        if (hi <= 10.0 * dry) { review.add(1, state_->volume[i]); continue; }')
g=g.replace('{ ok = false; break; }','{ review.add(2, state_->volume[i]); ok = false; break; }',1)
g=g.replace('if (ei - sill <= dry || ej - sill <= dry) { ok = false; break; }','if (ei - sill <= dry || ej - sill <= dry) { review.add(3, state_->volume[i]); ok = false; break; }')
g=g.replace('const double inv_a =', 'review.add(4, state_->volume[i]);\n        const double inv_a =')
g=g.replace('gex_[depth_offset + i] = phi[3]', 'for(int m=0;m<4;++m) if(phi[m] < 1.0-1e-12) review.add(5+m,state_->volume[i]);\n        gex_[depth_offset + i] = phi[3]')
s=s[:a]+g+s[b:]
a=s.index('void ExplicitInertialSolver::fireFacesSwe');b=s.index('void ExplicitInertialSolver::fireFacesDiffusive',a);g=s[a:b]
g=g.replace('const int e = faces[', 'review.faces += 1;\n        const int e = faces[',1)
g=g.replace('if (take > budget) {','review.raw_export += take;\n            if (take > budget) {\n                review.capped += 1; review.removed_export += take-budget;',1)
s=s[:a]+g+s[b:];(p/f'src_{v}'/f).write_text(s)
