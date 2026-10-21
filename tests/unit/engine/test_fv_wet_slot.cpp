// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include <gtest/gtest.h>
#include <numeric>
#include "../../verification/fv_implicit_phase1/wet_cases.hpp"
#include "hydraulics/fv/ImplicitWetKernels.hpp"

namespace {
using namespace fvnetwork;
namespace w=openswmm::fv::implicit_wet;
std::vector<double> rows(const S::Problem& p,const S::Evaluation& e,const S::Linearization& l) {
    std::vector<double> r;
    for (std::size_t i=0;i<e.volume.size();++i) { r.push_back(e.volume[i]); r.push_back(e.momentum[i]); }
    for (int j:l.nodes) r.push_back(p.mesh->node_kind[j]==kNodeStorage ? e.storage[j] : e.junction[j]);
    return r;
}
void advance(S::Problem& p,const S::Endpoint& e) {
    p.old_area=e.area; p.old_discharge=e.discharge; p.initial_depth=e.depth; p.initial_head=e.head;
    if (!p.old_node_volume.empty()) p.old_node_volume=e.node_volume;
}
double inventory(const S::Problem& p) {
    double v=std::accumulate(p.old_node_volume.begin(),p.old_node_volume.end(),0.0);
    for (int i=0;i<p.mesh->n_cells();++i) v+=p.mesh->cell_dx[i]*p.old_area[i];
    return v;
}
kernels::FaceState primal(const w::State& s) {
    kernels::FaceState p; p.a=s.a.v; p.q=s.q.v; p.u=s.u.v; p.c=s.c.v; p.i1=s.i1.v; p.press=s.press; return p;
}
}

TEST(FvWetSlot, ClosureDerivativesThroughFreeSurfaceTaperAndSlot) {
    auto ch=makeWet("wet_fill"); const auto& c=ch.mesh.geom[0].closure_tbl;
    for (double h:{.1,.8,1.8,2.95,2.99,3.0,3.02,6.0}) {
        SCOPED_TRACE(h);
        const auto e=w::closure(c,w::Jet::seed(h,0)); const auto p=kernels::closureEval(c,h);
        EXPECT_EQ(e.area.v,p.a); EXPECT_EQ(e.width.v,p.t); EXPECT_EQ(e.moment.v,p.i1);
        EXPECT_EQ(e.area.d[0],p.t); EXPECT_EQ(e.moment.d[0],p.a);
        constexpr double eps=1.e-6;
        const auto a=kernels::closureEval(c,h+eps),b=kernels::closureEval(c,h-eps);
        if (h!=3.0) EXPECT_NEAR(e.width.d[0],(a.t-b.t)/(2*eps),2.e-6);
        EXPECT_NEAR(e.area.d[0],h==3.0 ? (a.a-p.a)/eps : (a.a-b.a)/(2*eps),2.e-8);
        EXPECT_NEAR(e.moment.d[0],(a.i1-b.i1)/(2*eps),2.e-8);
    }
}

TEST(FvWetSlot, NativeHllDerivativePrimalMatchesSharedKernelBranches) {
    auto ch=makeWet("wet_fill"); const auto& c=ch.mesh.geom[0].closure_tbl;
    for (double hl:{1.5,2.98,3.02}) for (double hr:{1.7,2.99,3.04})
    for (double u:{-800.,-.5,0.,.7,800.}) for (bool vented:{false,true}) {
        auto l=w::state(c,w::Jet::seed(hl,0),w::Jet::seed(u,1),hl>=3 ? 1 : 0);
        auto r=w::state(c,w::Jet::seed(hr,2),w::Jet::seed(.9*u,3),vented ? 2 : hr>=3 ? 1 : 0);
        const auto a=w::flux(l,r); const auto b=kernels::riemannFlux(primal(l),primal(r));
        EXPECT_EQ(a.mass.v,b.mass); EXPECT_EQ(a.momentum.v,b.mom);
        ASSERT_TRUE(w::finite(a.mass)); ASSERT_TRUE(w::finite(a.momentum));
        constexpr double eps=1.e-6;
        const auto lp=w::state(c,w::Jet(hl+eps),w::Jet(u),l.press),lm=w::state(c,w::Jet(hl-eps),w::Jet(u),l.press);
        const auto plus=kernels::riemannFlux(primal(lp),primal(r)),minus=kernels::riemannFlux(primal(lm),primal(r));
        EXPECT_NEAR(a.mass.d[0],(plus.mass-minus.mass)/(2*eps),2.e-5*std::max(1.,std::fabs(a.mass.d[0])));
        EXPECT_NEAR(a.momentum.d[0],(plus.mom-minus.mom)/(2*eps),2.e-5*std::max(1.,std::fabs(a.momentum.d[0])));
    }
}

TEST(FvWetSlot, RestBalancesBedsBarrelsSharedSectionsVirtualFacesAndStorage) {
    for (const std::string name:{"wet_bed","wet_geometry","wet_virtual","wet_loop","wet_storage","wet_sealed"}) {
        SCOPED_TRACE(name); auto ch=makeWet(name); auto p=wetProblem(ch,name);
        S solver; S::Endpoint e; const auto r=solver.solve(p,1.0,{},e);
        ASSERT_TRUE(r.converged())<<static_cast<int>(r.status)<<' '<<r.scaled_residual;
        EXPECT_EQ(e.depth.size(),static_cast<std::size_t>(ch.n)); EXPECT_EQ(e.area,p.old_area);
        for (int i=0;i<ch.n;++i) EXPECT_NEAR(e.discharge[i],0.0,1.e-12);
        for (int j=0;j<ch.mesh.n_nodes();++j) if (ch.mesh.node_kind[j]==kNodeStorage) EXPECT_EQ(e.node_volume[j],p.old_node_volume[j]);
    }
}

TEST(FvWetSlot, SparseJacobianMatchesPhysicalDirectionalDerivatives) {
    for (const std::string name:{"wet_bed","wet_geometry","wet_virtual","wet_storage","wet_sealed","wet_interface","wet_interface_reverse"}) {
        SCOPED_TRACE(name); auto ch=makeWet(name,4); auto p=wetProblem(ch,name,true); auto x=trial(p);
        // Avoid crown/table/wave-speed ties for this smooth directional oracle.
        for (int i=0;i<ch.n;++i) { x.depth[i]-=.0037; x.discharge[i]=.03*std::cos(.3*i+.2); }
        for (int j=0;j<ch.mesh.n_nodes();++j) if (!std::isfinite(p.fixed_head[j])) x.head[j]-=.0037;
        S::Linearization l; ASSERT_TRUE(S::linearize(p,x,.05,1.e-4,l));
        std::vector<double> v(l.residual.size()),av(v.size());
        for (std::size_t i=0;i<v.size();++i) v[i]=.02*std::sin(.37*i+.2);
        ASSERT_TRUE(l.matrix.apply(v.data(),av.data())); auto a=x,b=x; constexpr double eps=1.e-5;
        for (int i=0;i<ch.n;++i) {
            a.depth[i]+=eps*v[2*i]; b.depth[i]-=eps*v[2*i];
            a.discharge[i]+=eps*v[2*i+1]*l.column_scale[2*i+1]; b.discharge[i]-=eps*v[2*i+1]*l.column_scale[2*i+1];
        }
        for (std::size_t j=0;j<l.nodes.size();++j) {
            a.head[l.nodes[j]]+=eps*v[2*ch.n+j]; b.head[l.nodes[j]]-=eps*v[2*ch.n+j];
        }
        S::Evaluation ea,eb; ASSERT_TRUE(S::residual(p,a,.05,ea)); ASSERT_TRUE(S::residual(p,b,.05,eb));
        const auto ra=rows(p,ea,l),rb=rows(p,eb,l);
        for (std::size_t i=0;i<v.size();++i) EXPECT_NEAR(av[i],(ra[i]-rb[i])/(2*eps*l.row_scale[i]),3.e-6)<<i;
        S::Linearization other; ASSERT_TRUE(S::linearize(p,x,.05,.01,other));
        EXPECT_EQ(l.matrix.value,other.matrix.value); // wet derivatives have no perturbation-size dependence
    }
}

TEST(FvWetSlot, AllNativePathsPreserveWetStatesStorageAndConservation) {
    for (const std::string name:{"wet_bed","wet_geometry","wet_virtual","wet_loop","wet_storage","wet_sealed","wet_fill","wet_drain","wet_interface","wet_interface_reverse"}) {
        S::Endpoint reference;
        for (int mode=0;mode<6;++mode) {
            SCOPED_TRACE(name+" mode="+std::to_string(mode));
            auto ch=makeWet(name); auto p=wetProblem(ch,name,true); const double initial=inventory(p);
            S solver; S::Controls c; c.preconditioner=mode>=3 ? S::Preconditioner::BlockIlu0 : S::Preconditioner::ScalarIlu0;
            c.reuse_linear_workspace=mode>=3;
            if (mode%3) { c.method=S::LinearMethod::CondensedOnly; c.condensation.reduced_gmres_only=true; }
            c.condensation.tree_interface=mode%3==2;
            const double dt=name.starts_with("wet_interface") ? 1.e-5 : .25;
            double external=0.,source=0.; S::Endpoint e;
            for (int step=0;step<4;++step) {
                const auto r=solver.solve(p,dt,c,e);
                ASSERT_TRUE(r.converged())<<static_cast<int>(r.status)<<' '<<r.scaled_residual;
                for (int f=0;f<ch.mesh.n_faces();++f) {
                    const int j=ch.mesh.face_node[f];
                    if (j>=0 && std::isfinite(p.fixed_head[j])) external+=dt*(ch.mesh.face_cl[f]>=0 ? 1 : -1)*e.faces[f].flux.mass;
                }
                source+=dt*(std::accumulate(p.cell_lateral.begin(),p.cell_lateral.end(),0.)+std::accumulate(p.node_lateral.begin(),p.node_lateral.end(),0.));
                advance(p,e); EXPECT_LE(std::fabs(inventory(p)-initial+external-source),1.e-10*initial);
                for (double h:e.depth) EXPECT_GE(h,p.minimum_wet_depth);
            }
            if (name=="wet_fill" || name=="wet_drain") for (double h:e.depth) EXPECT_NEAR(h,name=="wet_fill" ? 3.01 : 2.99,2.e-8);
            if (mode==0) reference=e;
            else {
                for (int i=0;i<ch.n;++i) { EXPECT_NEAR(e.depth[i],reference.depth[i],1.e-8); EXPECT_NEAR(e.discharge[i],reference.discharge[i],1.e-8); }
                for (int j=0;j<ch.mesh.n_nodes();++j) EXPECT_NEAR(e.head[j],reference.head[j],1.e-8);
            }
        }
    }
}

TEST(FvWetSlot, WetSpatialOperatorMatchesExistingFirstOrderFv) {
    for (const std::string name:{"wet_bed","wet_geometry","wet_virtual","wet_interface","wet_interface_reverse"}) {
        SCOPED_TRACE(name); auto ch=makeWet(name,16); auto p=wetProblem(ch,name,true);
        auto x=trial(p); for (int i=0;i<ch.n;++i) p.old_discharge[i]=x.discharge[i]=.1*std::cos(.2*i);
        ch.state.cell_a=p.old_area; ch.state.cell_q=p.old_discharge; ch.state.node_head=p.initial_head;
        auto opts=defaultOptions(); opts.lts=false; opts.pressurized_implicit=false;
        ExplicitFvSolver explicit_solver; explicit_solver.initialize(ch.mesh,ch.state,opts);
        constexpr double dt=1.e-7; S::Evaluation e; ASSERT_TRUE(S::residual(p,x,dt,e));
        FvStepForcing forcing; ASSERT_EQ(explicit_solver.advance(0,dt,forcing),dt);
        for (int i=0;i<ch.n;++i) {
            EXPECT_NEAR(ch.state.cell_a[i],p.old_area[i]-e.volume[i]/ch.mesh.cell_dx[i],2.e-13);
            EXPECT_NEAR(ch.state.cell_q[i],p.old_discharge[i]-e.momentum[i]/ch.mesh.cell_dx[i],3.e-11);
        }
    }
}

TEST(FvWetSlot, WetFloorRejectsCellsAndReconstructedSidesWithoutClamping) {
    auto ch=makeWet("wet_bed"); auto p=wetProblem(ch,"wet_bed"); auto x=trial(p);
    S::Evaluation e; e.volume={-1};
    x.depth[0]=p.minimum_wet_depth*.5; EXPECT_FALSE(S::residual(p,x,.1,e)); EXPECT_EQ(e.volume,(std::vector<double>{-1}));
    x=trial(p);
    // All cell centres remain wet, but an elevated face bed dries a side.
    ch.mesh.face_zb[0]=ch.mesh.cell_zb[0]+x.depth[0];
    EXPECT_FALSE(S::residual(p,x,.1,e)); EXPECT_EQ(e.volume,(std::vector<double>{-1}));
}

TEST(FvWetSlot, UnsupportedAndInvalidWetCallsLeaveEndpointAndCacheUnchanged) {
    for (int error=0;error<7;++error) {
        auto ch=makeWet("wet_storage"); auto p=wetProblem(ch,"wet_storage",true);
        switch (error) {
        case 0: p.minimum_wet_depth=0; break;
        case 1: p.minimum_wet_depth=nan(); break;
        case 2: ch.mesh.node_sur_depth.clear(); break;
        case 3: ch.mesh.node_sur_depth[0]=-1; break;
        case 4: p.regime=static_cast<S::Regime>(99); break;
        case 5: ch.mesh.geom[0].is_open=1; break;
        case 6: ch.mesh.node_can_pond[1]=1; break;
        }
        S solver; S::Endpoint e; e.depth={-1}; e.node_volume={-2};
        EXPECT_EQ(solver.solve(p,.25,{},e).status,S::Status::InvalidInput);
        EXPECT_EQ(e.depth,(std::vector<double>{-1})); EXPECT_EQ(e.node_volume,(std::vector<double>{-2}));
        EXPECT_EQ(solver.linearWorkspaceBytes(),0u);
    }
}

TEST(FvWetSlot, UnreachableSinkFailsAtomicallyAndPressurizedDefaultRejectsFreeSurface) {
    auto ch=makeWet("wet_fill"); auto p=wetProblem(ch,"wet_fill");
    p.cell_lateral.assign(ch.n,-1.e6); S solver; S::Endpoint e; e.depth={-1}; e.node_volume={-2};
    S::Controls c; c.reuse_linear_workspace=true;
    EXPECT_FALSE(solver.solve(p,1.0,c,e).converged());
    EXPECT_EQ(e.depth,(std::vector<double>{-1})); EXPECT_EQ(e.node_volume,(std::vector<double>{-2}));
    EXPECT_EQ(solver.linearWorkspaceBytes(),0u);
    p=wetProblem(ch,"wet_fill"); p.regime=S::Regime::PressurizedOnly;
    EXPECT_EQ(solver.solve(p,.25,{},e).status,S::Status::InvalidInput);
}

TEST(FvWetSlot, MixedCrownLoopFailureRetainsAcceptedStateAndOriginalBudgets) {
    auto ch=makeWet("wet_loop"); auto p=wetProblem(ch,"wet_loop",true);
    for (int i=0;i<ch.n;++i) {
        p.initial_depth[i]=3.0+.015*std::sin(.31*i+.2);
        p.old_area[i]=kernels::areaOfDepth(ch.mesh.geom[ch.mesh.cell_geom[i]],p.initial_depth[i]);
    }
    for (int j=0;j<ch.mesh.n_nodes();++j) {
        p.initial_head[j]=3.0; if (std::isfinite(p.fixed_head[j])) p.fixed_head[j]=3.0;
    }
    const auto old=p.old_area, head=p.initial_head;
    S solver; S::Controls c; c.reuse_linear_workspace=true; S::Endpoint e;
    e.depth={-1}; e.discharge={-2}; e.head={-3}; e.area={-4}; e.node_volume={-5};
    const auto r=solver.solve(p,.25,c,e);
    // Retain the reproduced nonlinear branch failure as a promotion hold;
    // never turn it into a passed hydraulic case or silently reduce dt.
    ASSERT_EQ(r.status,S::Status::LineSearchFailed);
    EXPECT_EQ(e.depth,(std::vector<double>{-1})); EXPECT_EQ(e.discharge,(std::vector<double>{-2}));
    EXPECT_EQ(e.head,(std::vector<double>{-3})); EXPECT_EQ(e.area,(std::vector<double>{-4}));
    EXPECT_EQ(e.node_volume,(std::vector<double>{-5})); EXPECT_EQ(solver.linearWorkspaceBytes(),0u);
    EXPECT_EQ(p.old_area,old); EXPECT_EQ(p.initial_head,head);
}
