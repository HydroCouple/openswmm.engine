// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include <gtest/gtest.h>
#include "../../verification/fv_implicit_phase1/network_cases.hpp"

namespace {
using namespace fvnetwork;
using Dense=std::vector<std::vector<double>>;
S::Matrix csr(const Dense& a) {
    S::Matrix m; m.row.push_back(0);
    for (std::size_t r=0;r<a.size();++r) {
        for (std::size_t c=0;c<a.size();++c) if (a[r][c]!=0.0 || r==c) {
            m.column.push_back(static_cast<int>(c)); m.value.push_back(a[r][c]);
        }
        m.row.push_back(m.value.size());
    }
    return m;
}
std::vector<double> matvec(const S::Matrix& a,const std::vector<double>& x) {
    std::vector<double> b(x.size()); EXPECT_TRUE(a.apply(x.data(),b.data())); return b;
}
// Independent long-double, globally pivoted oracle. Never called by a solver.
std::vector<double> oracle(const Dense& original,const std::vector<double>& b) {
    const int n=static_cast<int>(b.size());
    std::vector<std::vector<long double>> a(n,std::vector<long double>(n+1));
    for (int r=0;r<n;++r) {
        for (int c=0;c<n;++c) a[r][c]=original[r][c];
        a[r][n]=b[r];
    }
    for (int c=0;c<n;++c) {
        int pivot=c;
        for (int r=c+1;r<n;++r) if (std::fabs(a[r][c])>std::fabs(a[pivot][c])) pivot=r;
        if (a[pivot][c]==0) return {};
        std::swap(a[pivot],a[c]);
        for (int r=c+1;r<n;++r) {
            const auto factor=a[r][c]/a[c][c];
            for (int j=c;j<=n;++j) a[r][j]-=factor*a[c][j];
        }
    }
    std::vector<long double> x(n);
    for (int r=n;r-->0;) {
        auto v=a[r][n];
        for (int c=r+1;c<n;++c) v-=a[r][c]*x[c];
        x[r]=v/a[r][r];
    }
    return {x.begin(),x.end()};
}
S::CondensationControls tree(bool krylov=false) {
    S::CondensationControls controls; controls.tree_interface=true; controls.reduced_gmres_only=krylov; return controls;
}
void check(const S::Matrix& a,const std::vector<double>& b,const std::vector<double>& x,double tolerance) {
    const auto y=matvec(a,x); double error=0;
    for (std::size_t i=0;i<b.size();++i) error=std::hypot(error,y[i]-b[i]);
    EXPECT_LE(error,tolerance);
}
}

TEST(FvInterfaceTree, MixedScalarBlockForestMatchesKnownDenseAndUnpeeledSolutions) {
    NetworkMeshData mesh; mesh.cell_dx.assign(4,1); mesh.conduit_cell_begin={0,2}; mesh.conduit_cell_count={2,2};
    const std::vector<int> start={0,2,4,6,8,9,10}, size={2,2,2,2,1,1,1};
    Dense a(11,std::vector<double>(11));
    for (int k=0;k<7;++k) for (int r=0;r<size[k];++r) for (int c=0;c<size[k];++c)
        a[start[k]+r][start[k]+c]=r==c ? 4.0+r : (r==0 ? .3 : -.2);
    for (const auto [u,v]:{std::pair{0,1},{1,4},{4,2},{2,3},{4,5},{5,6}})
        for (int r=0;r<size[u];++r) for (int c=0;c<size[v];++c) {
            a[start[u]+r][start[v]+c]=.11+.03*r-.02*c;
            a[start[v]+c][start[u]+r]=-.07-.02*c+.01*r;
        }
    const auto m=csr(a); std::vector<double> known(11);
    for (int i=0;i<11;++i) known[i]=std::sin(.3+i);
    const auto b=matvec(m,known), ref=oracle(a,b); ASSERT_EQ(ref.size(),known.size());
    std::vector<double> unpeeled; ASSERT_TRUE(S::solveCondensed(mesh,m,b,{},{},false,unpeeled).converged());
    for (bool krylov:{false,true}) {
        std::vector<double> x; const auto r=S::solveCondensed(mesh,m,b,{},tree(krylov),false,x);
        ASSERT_TRUE(r.converged()); EXPECT_TRUE(r.condensation.used_tree);
        EXPECT_EQ(r.condensation.tree_eliminated,11); EXPECT_EQ(r.condensation.core_scalars,0);
        EXPECT_FALSE(r.condensation.used_dense); EXPECT_FALSE(r.condensation.used_reduced_gmres);
        for (int i=0;i<11;++i) { EXPECT_NEAR(x[i],known[i],1.e-13); EXPECT_NEAR(x[i],ref[i],1.e-13); EXPECT_NEAR(x[i],unpeeled[i],1.e-13); }
        check(m,b,x,r.condensation.full_tolerance);
    }
}

TEST(FvInterfaceTree, UnsafeScalarAndBlockLeavesRemainForGlobalCorePivoting) {
    for (bool block:{false,true}) {
        NetworkMeshData mesh;
        if (block) { mesh.cell_dx={1,1}; mesh.conduit_cell_begin={0}; mesh.conduit_cell_count={2}; }
        const int width=block ? 2 : 1, n=2*width;
        Dense a(n,std::vector<double>(n));
        for (int i=0;i<width;++i) { a[i][i+width]=2; a[i+width][i]=3; }
        const auto m=csr(a); std::vector<double> known(n);
        for (int i=0;i<n;++i) known[i]=i+1;
        const auto b=matvec(m,known); std::vector<double> x;
        const auto r=S::solveCondensed(mesh,m,b,{},tree(),false,x);
        ASSERT_TRUE(r.converged()); EXPECT_GE(r.condensation.unsafe_leaf_pivots,2);
        EXPECT_EQ(r.condensation.tree_eliminated,0); EXPECT_EQ(r.condensation.core_scalars,n);
        EXPECT_TRUE(r.condensation.used_dense); EXPECT_FALSE(r.fallback);
        for (int i=0;i<n;++i) EXPECT_NEAR(x[i],known[i],1.e-13);
    }
}

TEST(FvInterfaceTree, AnUnsafeLeafCanBecomeAdmissibleAfterItsNeighborIsEliminated) {
    NetworkMeshData mesh; const auto m=csr({{0,2},{3,1}}); const std::vector<double> known={2,-3};
    const auto b=matvec(m,known); std::vector<double> x;
    const auto r=S::solveCondensed(mesh,m,b,{},tree(),false,x);
    ASSERT_TRUE(r.converged()); EXPECT_EQ(r.condensation.unsafe_leaf_pivots,1);
    EXPECT_EQ(r.condensation.tree_eliminated,2); EXPECT_EQ(r.condensation.core_scalars,0);
    EXPECT_FALSE(r.condensation.used_dense);
    EXPECT_NEAR(x[0],2,1.e-13); EXPECT_NEAR(x[1],-3,1.e-13);
}

TEST(FvInterfaceTree, OneWayCycleUsesTheUnionGraphAndRetainsItsCore) {
    NetworkMeshData mesh; const auto m=csr({{3,.2,0},{0,4,-.3},{.1,0,5}});
    const std::vector<double> known={1,-2,3}; const auto b=matvec(m,known);
    for (bool krylov:{false,true}) {
        std::vector<double> x; const auto r=S::solveCondensed(mesh,m,b,{},tree(krylov),false,x);
        ASSERT_TRUE(r.converged()); EXPECT_EQ(r.condensation.tree_eliminated,0);
        EXPECT_EQ(r.condensation.core_scalars,3); EXPECT_EQ(r.condensation.used_reduced_gmres,krylov);
        for (int i=0;i<3;++i) EXPECT_NEAR(x[i],known[i],1.e-10);
    }
}

TEST(FvInterfaceTree, ALongSingleCycleKeepsALargeSparseCoreAfterAttachedLeavesAreRemoved) {
    constexpr int cycle=256,n=cycle+2; NetworkMeshData mesh;
    S::Matrix m; m.row.push_back(0);
    for (int r=0;r<n;++r) {
        std::vector<std::pair<int,double>> entries={{r,4}};
        if (r<cycle) {
            entries.emplace_back((r+1)%cycle,-.3); entries.emplace_back((r+cycle-1)%cycle,-.2);
            if (r==0) entries.emplace_back(cycle,-.1);
        } else {
            entries.emplace_back(r==cycle ? 0 : cycle,-.15);
            if (r==cycle) entries.emplace_back(cycle+1,-.1);
        }
        std::sort(entries.begin(),entries.end());
        for (auto [col,v]:entries) { m.column.push_back(col); m.value.push_back(v); }
        m.row.push_back(m.value.size());
    }
    std::vector<double> known(n);
    for (int i=0;i<n;++i) known[i]=1+.1*std::cos(.2*i);
    const auto b=matvec(m,known); std::vector<double> x;
    const auto r=S::solveCondensed(mesh,m,b,{},tree(),false,x);
    ASSERT_TRUE(r.converged()); EXPECT_EQ(r.condensation.tree_eliminated,2);
    EXPECT_EQ(r.condensation.core_scalars,cycle); EXPECT_FALSE(r.condensation.used_dense);
    EXPECT_TRUE(r.condensation.used_reduced_gmres);
    for (int i=0;i<n;++i) EXPECT_NEAR(x[i],known[i],1.e-9);
    check(m,b,x,r.condensation.full_tolerance);
}

TEST(FvInterfaceTree, DisconnectedAndIsolatedCompatibleRowsReceiveNoEquationPins) {
    NetworkMeshData mesh; const auto m=csr({{2,.1,0,0},{.2,3,0,0},{0,0,4,0},{0,0,0,0}});
    const std::vector<double> b={1,2,3,0}; std::vector<double> x;
    const auto r=S::solveCondensed(mesh,m,b,{},tree(),false,x);
    ASSERT_TRUE(r.converged()); EXPECT_EQ(r.condensation.tree_eliminated,3);
    EXPECT_EQ(r.condensation.core_scalars,1); EXPECT_TRUE(r.condensation.used_reduced_gmres);
    EXPECT_EQ(x[3],0); check(m,b,x,r.condensation.full_tolerance);
    for (bool fallback:{false,true}) {
        x={-9}; const auto failed=S::solveCondensed(mesh,m,{1,2,3,1},{},tree(),fallback,x);
        EXPECT_FALSE(failed.converged()); EXPECT_EQ(failed.fallback,fallback);
        EXPECT_EQ(x,(std::vector<double>{-9}));
    }
}

TEST(FvInterfaceTree, ResourceLimitsAndExhaustedCoreCannotPublishAnIncompleteCorrection) {
    NetworkMeshData mesh; const auto m=csr({{3,.2,0},{0,4,-.3},{.1,0,5}});
    for (bool memory:{false,true}) {
        auto controls=tree();
        if (memory) controls.workspace_limit_bytes=1; else controls.max_schur_entries=1;
        std::vector<double> x={-9}; const auto failed=S::solveCondensed(mesh,m,{1,2,3},{},controls,false,x);
        EXPECT_FALSE(failed.converged()); EXPECT_EQ(x,(std::vector<double>{-9}));
        EXPECT_EQ(failed.condensation.status,memory ? S::CondensationStatus::MemoryLimit : S::CondensationStatus::FillLimit);
        const auto fallback=S::solveCondensed(mesh,m,{1,2,3},{},controls,true,x);
        EXPECT_TRUE(fallback.converged()); EXPECT_TRUE(fallback.fallback);
    }
    auto linear=NativeGmres::Controls{}; linear.max_iterations=0;
    std::vector<double> x={-9}; const auto failed=S::solveCondensed(mesh,m,{1,2,3},linear,tree(true),false,x);
    EXPECT_FALSE(failed.converged()); EXPECT_TRUE(failed.condensation.used_tree);
    EXPECT_TRUE(failed.condensation.used_reduced_gmres); EXPECT_EQ(x,(std::vector<double>{-9}));
    // Budget sufficient for unpeeled small dense solve, insufficient for the
    // extra graph buffers: the tree must account for its OWN storage too.
    auto unpeeled=S::solveCondensed(mesh,m,{1,2,3},{},{},false,x); ASSERT_TRUE(unpeeled.converged());
    auto controls=tree(); controls.workspace_limit_bytes=unpeeled.condensation.peak_workspace_bytes;
    x={-9}; const auto limited=S::solveCondensed(mesh,m,{1,2,3},{},controls,false,x);
    EXPECT_FALSE(limited.converged()); EXPECT_EQ(limited.condensation.status,S::CondensationStatus::MemoryLimit);
    EXPECT_EQ(x,(std::vector<double>{-9}));
    // A late core failure, AFTER leaves were eliminated, must also preserve
    // the nonlinear endpoint rather than publish a partially reconstructed step.
    auto ch=make("loop",8,660); auto p=problem(ch,"loop",true);
    S::Controls nonlinear; nonlinear.method=S::LinearMethod::CondensedOnly;
    nonlinear.condensation=tree(true); nonlinear.linear.max_iterations=0;
    S solver; S::Endpoint endpoint; endpoint.depth={-1}; endpoint.area={-2};
    endpoint.discharge={-3}; endpoint.head={-4};
    const auto result=solver.solve(p,.25,nonlinear,endpoint);
    EXPECT_EQ(result.status,S::Status::LinearFailure); ASSERT_EQ(result.linear_history.size(),1u);
    EXPECT_GT(result.last_linear.condensation.tree_eliminated,0);
    EXPECT_GT(result.last_linear.condensation.core_scalars,0);
    EXPECT_EQ(endpoint.depth,(std::vector<double>{-1})); EXPECT_EQ(endpoint.area,(std::vector<double>{-2}));
    EXPECT_EQ(endpoint.discharge,(std::vector<double>{-3})); EXPECT_EQ(endpoint.head,(std::vector<double>{-4}));
}

TEST(FvInterfaceTree, LeafAndInteriorReconstructionsMustPassOriginalResiduals) {
    auto linear=NativeGmres::Controls{}; linear.absolute_tolerance=1.e-30; linear.relative_tolerance=1.e-30;
    NetworkMeshData mesh; auto m=csr({{.1,.2},{.3,.4}}); std::vector<double> x={-9};
    auto failed=S::solveCondensed(mesh,m,{.7,.8},linear,tree(),false,x);
    EXPECT_FALSE(failed.converged()); EXPECT_EQ(failed.condensation.status,S::CondensationStatus::ReconstructionFailure);
    EXPECT_EQ(failed.condensation.core_scalars,0); EXPECT_EQ(x,(std::vector<double>{-9}));
    mesh.cell_dx={1,1,1}; mesh.conduit_cell_begin={0}; mesh.conduit_cell_count={3};
    Dense a(6,std::vector<double>(6));
    for (int i:{0,1,4,5}) a[i][i]=1;
    a[2][2]=.1; a[2][3]=.2; a[3][2]=.3; a[3][3]=.4; m=csr(a);
    failed=S::solveCondensed(mesh,m,{1,0,.7,.8,0,0},linear,tree(),false,x);
    EXPECT_FALSE(failed.converged()); EXPECT_EQ(failed.condensation.status,S::CondensationStatus::ReconstructionFailure);
    EXPECT_EQ(failed.condensation.reduced_true_residual,0);
    EXPECT_GT(failed.condensation.full_true_residual,failed.condensation.full_tolerance);
    EXPECT_EQ(x,(std::vector<double>{-9}));
}

TEST(FvInterfaceTree, OriginalHydraulicCorrectionsAndNonlinearBudgetsMatchBothExistingPaths) {
    for (const std::string name:{"bed","geometry","virtual","tee_source","loop","reversed"}) {
        SCOPED_TRACE(name); auto ch=make(name,8,660); auto p=problem(ch,name,true);
        S::Linearization l; ASSERT_TRUE(S::linearize(p,trial(p),.25,1.e-4,l));
        std::vector<double> known(l.residual.size());
        for (std::size_t i=0;i<known.size();++i) known[i]=std::sin(.7*i+.2);
        const auto b=matvec(l.matrix,known); std::vector<double> full,unpeeled;
        ASSERT_TRUE(S::solveLinear(l.matrix,b,{},full).converged());
        ASSERT_TRUE(S::solveCondensed(ch.mesh,l.matrix,b,{},{},false,unpeeled).converged());
        S solver; S::Endpoint reference; ASSERT_TRUE(solver.solve(p,.25,{},reference).converged());
        for (bool krylov:{false,true}) {
            std::vector<double> x;
            const auto r=S::solveCondensed(ch.mesh,l.matrix,b,{},tree(krylov),false,x);
            ASSERT_TRUE(r.converged()) << static_cast<int>(r.condensation.status);
            const auto& d=r.condensation;
            EXPECT_EQ(d.tree_eliminated+d.core_scalars,d.retained);
            EXPECT_EQ(d.eliminated+d.retained,known.size());
            EXPECT_LE(d.peak_workspace_bytes,tree().workspace_limit_bytes);
            if (name=="loop") EXPECT_GT(d.core_scalars,0); else EXPECT_EQ(d.core_scalars,0);
            for (std::size_t i=0;i<known.size();++i) {
                EXPECT_NEAR(x[i],known[i],2.e-9); EXPECT_NEAR(x[i],full[i],2.e-9); EXPECT_NEAR(x[i],unpeeled[i],2.e-9);
            }
            check(l.matrix,b,x,d.full_tolerance);
            S::Controls controls; controls.method=S::LinearMethod::CondensedOnly; controls.condensation=tree(krylov);
            S::Endpoint endpoint; const auto result=solver.solve(p,.25,controls,endpoint);
            ASSERT_TRUE(result.converged()); EXPECT_LE(result.scaled_residual,1.0);
            for (int i=0;i<ch.n;++i) {
                EXPECT_NEAR(endpoint.depth[i],reference.depth[i],1.e-8);
                EXPECT_NEAR(endpoint.area[i],reference.area[i],1.e-10);
                EXPECT_NEAR(endpoint.discharge[i],reference.discharge[i],1.e-8);
            }
            for (const auto& line:result.linear_history) EXPECT_FALSE(line.fallback);
        }
    }
}
