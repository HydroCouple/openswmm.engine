// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#pragma once
#include "../fv_implicit_phase0/network_cases.hpp"
#include "hydraulics/fv/ImplicitNetworkSolver.hpp"

namespace fvnetwork {
using namespace fvtest;
using S=ImplicitNetworkSolver;
inline double nan() { return std::numeric_limits<double>::quiet_NaN(); }
inline Channel make(const std::string& name,int cells,double speed) {
    Channel ch;
    if (name.starts_with("tee") || name.starts_with("loop") || name=="reversed")
        ch=fvbaseline::makeNetwork(name.starts_with("loop") ? "loop_rest" : "tee_rest",cells,speed);
    else ch=makeWalledChannel(circular(3.0),cells,1200.0/cells,
        [&](double x){return name=="bed" || name=="geometry" ? .3*std::sin(x/140.0)+.001*x : 0.0;},.013,speed);
    auto& m=ch.mesh;
    if (name=="geometry") {
        m.geom.resize(3);
        buildGeometry(circular(2.0),false,speed,m.geom[1],2);
        // A supplied face section is authoritative. It need not equal either
        // cell section: the cell-side pressure correction balances that wall.
        buildGeometry(circular(2.5),false,speed,m.geom[2]);
        for (int i=ch.n/2;i<ch.n;++i) m.cell_geom[i]=1;
    }
    m.deriveFaceGeom();
    if (name=="geometry") m.face_geom[ch.n/2]=2;
    if (name=="virtual" || name=="geometry") {
        const int f=ch.n/2;
        m.face_virtual[f]=1; m.face_vj_node[f]=0;
        m.node_invert={m.face_zb[f]}; m.node_kind={kNodeVirtual};
        m.node_area={constants::MIN_SURFAREA}; m.node_can_pond={0}; m.node_vol_off={-1};
        m.node_face_ptr={0,0}; m.node_vj_face={f};
    }
    if (name=="reversed" || name=="virtual") {
        // Reverse the local axes on the second conduit (or half reach). The
        // oriented face graph and physical location remain exactly unchanged.
        const int b=name=="reversed" ? cells : ch.n/2;
        const int end=name=="reversed" ? 2*cells : ch.n;
        for (int i=b;i<end;++i) m.chain_dir[i]*=-1;
        for (int f=0;f<m.n_faces();++f) {
            if (m.face_cl[f]>=b && m.face_cl[f]<end) m.face_dir_l[f]=-1;
            if (m.face_cr[f]>=b && m.face_cr[f]<end) m.face_dir_r[f]=-1;
        }
    }
    if (name=="virtual" || name=="geometry") {
        m.node_full_depth={1.e6}; m.node_ponded_area={0}; m.node_sur_depth={0};
        m.node_vol_dmax={0}; m.node_vol_atop={0};
    }
    ch.state.resize(ch.n,m.n_nodes(),0);
    return ch;
}
inline S::Problem problem(Channel& ch,const std::string& name,bool moving=false) {
    auto& m=ch.mesh;
    S::Problem p;
    p.mesh=&m; p.fixed_head.assign(m.n_nodes(),nan()); p.initial_head.assign(m.n_nodes(),6.0);
    p.old_area.resize(ch.n); p.old_discharge.assign(ch.n,0.0);
    for (int i=0;i<ch.n;++i) {
        double head=6.0;
        if (moving) head+=.01*std::sin(.31*i);
        p.initial_depth.push_back(head-m.cell_zb[i]);
        p.old_area[i]=kernels::areaOfDepth(m.geom[m.cell_geom[i]],p.initial_depth[i]);
    }
    for (int j=0;j<m.n_nodes();++j) if (m.node_kind[j]==kNodeOutfall) {
        p.fixed_head[j]=6.0;
        if (moving && j==0) p.fixed_head[j]=6.01;
        p.initial_head[j]=p.fixed_head[j];
    }
    if (name=="tee_source") {
        p.node_lateral.assign(m.n_nodes(),0.0); p.node_lateral[1]=.1;
        p.cell_lateral.assign(ch.n,0.0); p.cell_lateral[ch.n/2]=.02;
    }
    return p;
}
inline S::Trial trial(const S::Problem& p) {
    S::Trial x;
    x.head=p.initial_head; x.discharge=p.old_discharge;
    for (std::size_t i=0;i<p.old_area.size();++i)
        x.depth.push_back(p.initial_depth.empty() ? kernels::depthOfArea(p.mesh->geom[p.mesh->cell_geom[i]],p.old_area[i]) : p.initial_depth[i]);
    return x;
}
} // namespace fvnetwork
