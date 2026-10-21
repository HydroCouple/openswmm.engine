// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#pragma once
#include "network_cases.hpp"

namespace fvnetwork {
inline Channel makeStorage(const std::string& name,int cells=8,double speed=660.0) {
    auto ch=make(name=="storage_loop" ? "loop" : name=="storage_reversed" ? "reversed" : "tee",cells,speed);
    auto& m=ch.mesh;
    for (int j=0;j<m.n_nodes();++j) {
        if (j!=1 && !(name=="storage_loop" && j==3) && name!="storage_closed") continue;
        m.node_kind[j]=kNodeStorage; m.node_full_depth[j]=8.0;
        m.node_vol_off[j]=static_cast<int>(m.node_vol_tbl.size());
        m.node_vol_dmax[j]=8.0; m.node_vol_atop[j]=10.0+1.8*j;
        for (int i=0;i<kNodeVolSamples;++i) {
            const double d=8.0*i/(kNodeVolSamples-1);
            m.node_vol_tbl.push_back((2.0+j)*d+(.25+.05*j)*d*d);
        }
    }
    return ch;
}
// Test-only interpolation; the engine consumes the exported flattened curve.
inline double curveVolume(const NetworkMeshData& m,int j,double head) {
    const double d=head-m.node_invert[j], step=m.node_vol_dmax[j]/(kNodeVolSamples-1);
    const int i=std::min(static_cast<int>(d/step),kNodeVolSamples-2);
    const auto off=m.node_vol_off[j]+i;
    if (d==m.node_vol_dmax[j]) return m.node_vol_tbl[m.node_vol_off[j]+kNodeVolSamples-1];
    return m.node_vol_tbl[off]+(m.node_vol_tbl[off+1]-m.node_vol_tbl[off])*((d-i*step)/step);
}
inline S::Problem storageProblem(Channel& ch,const std::string& name,bool moving=false) {
    auto p=problem(ch,name,moving); const auto& m=ch.mesh;
    p.old_node_volume.assign(m.n_nodes(),0.0);
    p.node_lateral.assign(m.n_nodes(),0.0); p.cell_lateral.assign(ch.n,0.0);
    for (int j=0;j<m.n_nodes();++j) {
        if (m.node_kind[j]==kNodeStorage) {
            if (moving) p.initial_head[j]=6.0-.0005;
            p.old_node_volume[j]=curveVolume(m,j,p.initial_head[j]);
            if (moving) p.node_lateral[j]=j==1 ? .7 : -.2;
        } else if (moving && m.node_kind[j]==kNodeJunction) p.node_lateral[j]=.1;
    }
    if (moving) p.cell_lateral[ch.n/2]=.02;
    return p;
}
} // namespace fvnetwork
