from pathlib import Path
import sys,difflib
R=Path(__file__).resolve().parent;label=sys.argv[1];p=Path('src/engine/hydraulics/fv/ExplicitFvSolver.cpp');s=(R/f'source_{label}'/p).read_text();s='#include <functional>\n'+s
helper='''namespace {
void auditTrial(int n,int f,double h,const k::FaceState& L,const k::FaceState& R) {
    if (!std::getenv("OPENSWMM_FV_RESIDUAL_AUDIT")) return;
    std::fprintf(stderr,"I %d %d %a %a %a %a %a %a %u %a %a %a %a %a %u\\n",
        n,f,h,L.a,L.q,L.u,L.c,L.i1,unsigned(L.press),R.a,R.q,R.u,R.c,R.i1,unsigned(R.press));
}
}

'''
a=s.index('void ExplicitFvSolver::computeFaceFlux(');s=s[:a]+helper+s[a:]
s=s.replace('        if (mass_only) {\n            double mass', '        if (mass_only) {\n            auditTrial(nd,f,state_->node_head[static_cast<std::size_t>(nd)],L,R);\n            double mass',1)
if label=='candidate':
 s=s.replace('        f_mass_[uf] = cell_left ? k::riemannMassFlux(entry.cell, ghost)', '''        auditTrial(n,f,state_->node_head[un],cell_left?entry.cell:ghost,cell_left?ghost:entry.cell);
        f_mass_[uf] = cell_left ? k::riemannMassFlux(entry.cell, ghost)''',1)
a=s.index('void ExplicitFvSolver::solveAlgebraicNode(');b=s.index('\n}\n',a)+3;part=s[a:b]
anchor='    // Fallback for a degree-1 node'
guard='''    const bool audit_on = std::getenv("OPENSWMM_FV_RESIDUAL_AUDIT") != nullptr;
    int team=1;
#ifdef SWMM_USE_OPENMP
    team=omp_get_num_threads();
#endif
    if (audit_on) std::fprintf(stderr,"TEAM %d %d\\n",n,team);
    struct AuditExit {std::function<void()> run; ~AuditExit(){if(run)run();}};
    AuditExit audit{audit_on ? std::function<void()>([&] {
        for (int p=b;p<e;++p) {
            const int f=mesh_->node_face_idx[static_cast<std::size_t>(p)];
            const auto uf=static_cast<std::size_t>(f);
            const auto& L=f_state_l_[uf];const auto& R=f_state_r_[uf];
            std::fprintf(stderr,"E %d %d %a %a %a %a %a %a %a %a %a %a %u %a %a %a %a %a %u\\n",
                n,f,state_->node_head[un],f_mass_[uf],f_mom_[uf],f_corr_l_[uf],f_corr_r_[uf],
                L.a,L.q,L.u,L.c,L.i1,unsigned(L.press),R.a,R.q,R.u,R.c,R.i1,unsigned(R.press));
        }
    }) : std::function<void()>{}};

'''
assert anchor in part;part=part.replace(anchor,guard+anchor,1);part=part.replace('        return r;','        if(audit_on) std::fprintf(stderr,"R %d %a %a\\n",n,h,r);\n        return r;',1);s=s[:a]+part+s[b:];(R/f'diagnostic_{label}.cpp').write_text(s);(R/f'diagnostic_{label}.patch').write_text(''.join(difflib.unified_diff((R/f'source_{label}'/p).read_text().splitlines(True),s.splitlines(True),fromfile='a/'+str(p),tofile='b/'+str(p))));(R.parent/'dw_fv_p0_2026-10-03/source'/p).write_text(s)
