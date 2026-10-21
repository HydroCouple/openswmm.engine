// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#pragma once
#include "storage_cases.hpp"

namespace fvnetwork {
inline Channel makeWet(const std::string& name,int cells=8,double speed=660.0) {
    if (name=="wet_storage") return makeStorage("storage_loop",cells,speed);
    auto ch=make(name=="wet_bed" ? "bed" : name=="wet_geometry" ? "geometry" :
                 name=="wet_virtual" ? "virtual" : name=="wet_loop" || name=="wet_sealed" ? "loop" : "flat",cells,speed);
    if (name=="wet_geometry") {
        // Same bed/section equilibrium spanning a 3-ft free pipe and a 2-ft
        // pressurized two-barrel pipe, with an authoritative 2.5-ft face.
        std::fill(ch.mesh.cell_zb.begin(),ch.mesh.cell_zb.end(),0.0);
        std::fill(ch.mesh.cell_dzdx.begin(),ch.mesh.cell_dzdx.end(),0.0);
        std::fill(ch.mesh.face_zb.begin(),ch.mesh.face_zb.end(),0.0);
    }
    if (name=="wet_sealed") std::fill(ch.mesh.node_sur_depth.begin(),ch.mesh.node_sur_depth.end(),1.0);
    return ch;
}
inline S::Problem wetProblem(Channel& ch,const std::string& name,bool moving=false) {
    auto p=name=="wet_storage" ? storageProblem(ch,"storage_loop",false) : problem(ch,name,false);
    p.regime=S::Regime::BoundedWetSlot;
    const double head=name=="wet_bed" ? 2.0 : name=="wet_geometry" ? 2.4 : name=="wet_fill" ? 2.99 :
                      name=="wet_drain" ? 3.01 : name=="wet_virtual" || name=="wet_storage" ? 2.98 : 2.8;
    p.initial_depth.clear(); p.node_lateral.assign(ch.mesh.n_nodes(),0.0); p.cell_lateral.assign(ch.n,0.0);
    for (int i=0;i<ch.n;++i) {
        const auto& g=ch.mesh.geom[ch.mesh.cell_geom[i]];
        const bool interface=name=="wet_interface" || name=="wet_interface_reverse";
        const double h=interface ? ((i<ch.n/2)==(name=="wet_interface") ? 2.98 : 3.02) :
            head-ch.mesh.cell_zb[i]+(moving && name!="wet_fill" && name!="wet_drain" ? .015*std::sin(.31*i+.2) : 0.0);
        p.initial_depth.push_back(h); p.old_area[i]=kernels::areaOfDepth(g,h);
        if (moving && (name=="wet_fill" || name=="wet_drain"))
            p.cell_lateral[i]=ch.mesh.cell_dx[i]*(kernels::areaOfDepth(g,name=="wet_fill" ? 3.01 : 2.99)-p.old_area[i]);
    }
    for (int j=0;j<ch.mesh.n_nodes();++j) {
        p.initial_head[j]=head;
        if (std::isfinite(p.fixed_head[j])) p.fixed_head[j]=head;
        if (ch.mesh.node_kind[j]==kNodeStorage) {
            p.old_node_volume[j]=curveVolume(ch.mesh,j,head);
            if (moving) p.node_lateral[j]=j==1 ? .3 : -.1;
        } else if (moving && ch.mesh.node_kind[j]==kNodeJunction) p.node_lateral[j]=.005;
    }
    if (moving && name=="wet_storage") p.cell_lateral[ch.n/2]=.005;
    return p;
}
} // namespace fvnetwork
