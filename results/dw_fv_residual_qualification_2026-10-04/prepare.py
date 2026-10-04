from pathlib import Path
import sys,difflib
Q=Path(__file__).resolve().parent;R=Q.parent/'dw_fv_residual_2026-10-04';W=Q.parent/'dw_fv_p0_2026-10-03';p=Path('src/engine/hydraulics/fv/ExplicitFvSolver.cpp')
label=sys.argv[1];s=(R/f'diagnostic_{label}.cpp').read_text()
# Test-only fault injection. One event per fresh process; never timed or shipped.
helper='''namespace {
bool qualificationReject(const char* mode) {
    static bool used = false;
    const char* request=std::getenv("FV_QUALIFY_REJECT");
    if (!used && request && std::strcmp(request,mode)==0) {
        used=true;
        std::fprintf(stderr,"REJECT %s\\n",mode);
        return true;
    }
    return false;
}
}
'''
s='#include <cstring>\n'+s
anchor='void ExplicitFvSolver::computeFaceFlux(';pos=s.index(anchor);s=s[:pos]+helper+s[pos:]
old='const double dt_post = censusDt(anyPressurizedCell());';assert s.count(old)==1
s=s.replace(old,'''double dt_post = censusDt(anyPressurizedCell());
            if (qualificationReject("global")) dt_post = std::min(dt_post, 0.25 * dt);''')
old='if (censusDt() < kStepAcceptRatio * dt0) {';assert s.count(old)==1
s=s.replace(old,'''const double qualification_post = censusDt();
                if (qualificationReject("macro") || qualification_post < kStepAcceptRatio * dt0) {''')
# Trace held-face predicate and special-face fallback at each residual.
a=s.index('    auto residual =');b=s.index('    auto finalize =',a);part=s[a:b]
old='            if (faceIsLive(f)) {';assert part.count(old)==1
part=part.replace(old,'''            if (audit_on && !faceIsLive(f)) std::fprintf(stderr,"H %d %d\\n",n,f);
            if (audit_on && (mesh_->face_culvert[uf]>=0 || mesh_->face_gate[uf]!=0))
                std::fprintf(stderr,"SPECIAL %d %d %d %d\\n",n,f,mesh_->face_culvert[uf],int(mesh_->face_gate[uf]));
'''+old);s=s[:a]+part+s[b:]
# Full cell state after rollback and after every advance, beyond public sampled output.
trace='''    if (std::getenv("OPENSWMM_FV_RESIDUAL_AUDIT")) {
        for (std::size_t c=0;c<state_->cell_a.size();++c)
            std::fprintf(stderr,"CELL %s %zu %a %a %a %a\\n",PHASE,c,state_->cell_a[c],state_->cell_q[c],state_->cell_h[c],cell_u_[c]);
    }
'''
a=s.index('void ExplicitFvSolver::restoreState()');b=s.index('\n}',a)
s=s[:b]+'\n'+trace.replace('PHASE','"restore"')+s[b:]
old='    last_nsteps_ = steps;';assert s.count(old)==1;s=s.replace(old,trace.replace('PHASE','"advance"')+'\n'+old)
(Q/f'{label}.cpp').write_text(s);(Q/f'{label}.patch').write_text(''.join(difflib.unified_diff((R/f'diagnostic_{label}.cpp').read_text().splitlines(True),s.splitlines(True),fromfile='diagnostic',tofile='qualification')))
(W/'source'/p).write_text(s)
