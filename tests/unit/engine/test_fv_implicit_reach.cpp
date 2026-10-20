// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include <gtest/gtest.h>

#include <algorithm>
#include "fv_test_support.hpp"
#include "hydraulics/fv/ImplicitReachSolver.hpp"

namespace {
using namespace fvtest;
using Solver = ImplicitReachSolver;
using Pair = Solver::Pair;
constexpr double pi=3.14159265358979323846;

Solver::Problem problem(Channel& ch, bool walls=false) {
    seedLevel(ch,6.0);
    return {&ch.mesh,ch.state.cell_a,ch.state.cell_q,
            walls ? std::numeric_limits<double>::quiet_NaN() : 6.0,
            walls ? std::numeric_limits<double>::quiet_NaN() : 6.0,3.01,100.0};
}
std::vector<Pair> trial(const Solver::Problem& p) {
    std::vector<Pair> x;
    for (std::size_t i=0;i<p.old_area.size();++i)
        x.push_back({kernels::depthOfArea(p.mesh->geom[0],p.old_area[i]),p.old_discharge[i]});
    return x;
}
Pair multiply(const Solver::Block& b,const Pair& v) {
    return {b[0]*v[0]+b[1]*v[1],b[2]*v[0]+b[3]*v[1]};
}
std::vector<Pair> applyBlocks(const Solver::Linearization& l,const std::vector<Pair>& x) {
    std::vector<Pair> y(x.size());
    for (std::size_t i=0;i<x.size();++i) {
        y[i]=multiply(l.diagonal[i],x[i]);
        if (i) { auto v=multiply(l.lower[i],x[i-1]); y[i][0]+=v[0]; y[i][1]+=v[1]; }
        if (i+1<x.size()) { auto v=multiply(l.upper[i],x[i+1]); y[i][0]+=v[0]; y[i][1]+=v[1]; }
    }
    return y;
}
// Independent scalar Gaussian elimination with global partial pivoting.
std::vector<Pair> dense(const Solver::Linearization& l,const std::vector<Pair>& rhs) {
    const int n=static_cast<int>(2*rhs.size());
    std::vector<std::vector<long double>> a(n,std::vector<long double>(n+1));
    for (int i=0;i<n/2;++i) for (int r=0;r<2;++r) {
        a[2*i+r][n]=rhs[i][r];
        for (int c=0;c<2;++c) {
            a[2*i+r][2*i+c]=l.diagonal[i][2*r+c];
            if (i) a[2*i+r][2*i-2+c]=l.lower[i][2*r+c];
            if (i+1<n/2) a[2*i+r][2*i+2+c]=l.upper[i][2*r+c];
        }
    }
    for (int c=0;c<n;++c) {
        int pivot=c;
        for (int r=c+1;r<n;++r) if (std::fabs(a[r][c])>std::fabs(a[pivot][c])) pivot=r;
        std::swap(a[c],a[pivot]);
        for (int r=c+1;r<n;++r) {
            const long double ratio=a[r][c]/a[c][c];
            for (int k=c;k<=n;++k) a[r][k]-=ratio*a[c][k];
        }
    }
    std::vector<long double> x(n);
    for (int r=n;r-->0;) {
        auto v=a[r][n];
        for (int c=r+1;c<n;++c) v-=a[r][c]*x[c];
        x[r]=v/a[r][r];
    }
    std::vector<Pair> result(n/2);
    for (int i=0;i<n;++i) result[i/2][i%2]=static_cast<double>(x[i]);
    return result;
}
void unchanged(const Solver::Endpoint& x) {
    EXPECT_EQ(x.area,(std::vector<double>{-9})); EXPECT_EQ(x.discharge,(std::vector<double>{-8}));
    EXPECT_EQ(x.depth,(std::vector<double>{-7}));
    ASSERT_EQ(x.faces.size(),1u); EXPECT_EQ(x.faces[0].mass,-6);
}
Solver::Endpoint sentinel() {
    Solver::Endpoint x; x.area={-9}; x.discharge={-8}; x.depth={-7}; x.faces.resize(1); x.faces[0].mass=-6; return x;
}
}

TEST(FvImplicitReach, SpatialResidualMatchesExistingExplicitFirstOrderOperator) {
    for (double roughness:{0.0,.013}) {
        auto ch=makeNodedChannel(circular(3.0),24,25.0,[](double){return 0.0;},roughness,150.0);
        auto p=problem(ch);
        for (int i=0;i<ch.n;++i) {
            p.old_area[i]=kernels::areaOfDepth(ch.mesh.geom[0],6+.01*std::sin(i*.3));
            p.old_discharge[i]=1+.1*std::cos(i*.4);
        }
        ch.state.cell_a=p.old_area; ch.state.cell_q=p.old_discharge;
        ch.state.node_head={6,6};
        auto opts=defaultOptions(); opts.lts=false; opts.pressurized_implicit=false;
        ExplicitFvSolver explicit_solver; explicit_solver.initialize(ch.mesh,ch.state,opts);
        const double fixed[]={6,6}; FvStepForcing forcing; forcing.node_fixed_head=fixed;
        std::vector<Pair> r;
        constexpr double dt=1.e-7;
        ASSERT_TRUE(Solver::residual(p,trial(p),dt,r));
        ASSERT_EQ(explicit_solver.advance(0,dt,forcing),dt);
        for (int i=0;i<ch.n;++i) {
            EXPECT_NEAR(ch.state.cell_a[i],p.old_area[i]-r[i][0]/ch.dx,2.e-13);
            EXPECT_NEAR(ch.state.cell_q[i],p.old_discharge[i]-r[i][1]/ch.dx,2.e-11);
        }
    }
}

TEST(FvImplicitReach, JacobianMatchesIndependentGlobalDirectionalDifferences) {
    auto ch=makeWalledChannel(circular(3.0),16,12.5,[](double){return 0.0;},.013,660.0);
    auto p=problem(ch,true); auto x=trial(p);
    for (int i=0;i<ch.n;++i) { x[i][0]+=.02*std::sin(.7*i); x[i][1]=.5+.2*std::cos(.8*i); }
    for (double local_step:{1.e-3,1.e-4,1.e-5}) {
        Solver::Linearization l; ASSERT_TRUE(Solver::linearize(p,x,.2,local_step,l));
        std::vector<Pair> direction(ch.n);
        for (int i=0;i<ch.n;++i) direction[i]={std::sin(.37*i+.1),std::cos(.42*i)};
        const auto jv=applyBlocks(l,direction);
        auto plus=x,minus=x;
        const double epsilon=1.e-4;
        for (int i=0;i<ch.n;++i) for (int k=0;k<2;++k) {
            plus[i][k]+=epsilon*l.column_scale[k]*direction[i][k];
            minus[i][k]-=epsilon*l.column_scale[k]*direction[i][k];
        }
        std::vector<Pair> rp,rm; ASSERT_TRUE(Solver::residual(p,plus,.2,rp)); ASSERT_TRUE(Solver::residual(p,minus,.2,rm));
        double error=0,norm=0;
        for (int i=0;i<ch.n;++i) for (int k=0;k<2;++k) {
            const double fd=(rp[i][k]-rm[i][k])/(2*epsilon*l.row_scale[i][k]);
            error=std::hypot(error,fd-jv[i][k]); norm=std::hypot(norm,fd);
        }
        EXPECT_LT(error/norm,1.e-5) << local_step;
    }
}

TEST(FvImplicitReach, BlockSolveAgreesWithIndependentDenseOracleAndKnownSolution) {
    for (int cells:{4,16,64}) {
        auto ch=makeWalledChannel(circular(3.0),cells,20.0,[](double){return 0.0;},.013,660.0);
        auto p=problem(ch,true); auto x=trial(p);
        for (int i=0;i<cells;++i) { x[i][0]+=.01*std::sin(i); x[i][1]=std::cos(.3*i); }
        Solver::Linearization l; ASSERT_TRUE(Solver::linearize(p,x,10.0,1.e-4,l));
        std::vector<Pair> known(cells);
        for (int i=0;i<cells;++i) known[i]={std::sin(.8*i),std::cos(.3*i)};
        const auto rhs=applyBlocks(l,known), reference=dense(l,rhs);
        std::vector<Pair> solution; double residual;
        ASSERT_TRUE(Solver::solveBlocks(l,rhs,solution,residual)); EXPECT_LT(residual,1.e-12);
        for (int i=0;i<cells;++i) for (int k=0;k<2;++k) {
            EXPECT_NEAR(solution[i][k],known[i][k],2.e-10);
            EXPECT_NEAR(solution[i][k],reference[i][k],2.e-10);
        }
    }
}

TEST(FvImplicitReach, ZeroDiagonalBlockPivotsAndUnsafeEliminationFailsWithoutPublication) {
    Solver::Linearization l; l.lower.resize(2); l.upper.resize(2); l.diagonal={{0,1,2,3},{4,1,1,4}};
    std::vector<Pair> known={{1,2},{3,4}}, solution;
    double residual=0;
    ASSERT_TRUE(Solver::solveBlocks(l,applyBlocks(l,known),solution,residual)); EXPECT_EQ(solution,known);
    // Globally nonsingular but its first diagonal block is singular: this
    // kernel must reject until the reviewed pivot/GMRES fallback is delivered.
    l.diagonal={{0,0,0,0},{0,0,0,0}}; l.upper[0]={1,0,0,1}; l.lower[1]={1,0,0,1};
    solution={{-9,-8}}; const auto saved=solution;
    EXPECT_FALSE(Solver::solveBlocks(l,applyBlocks(l,known),solution,residual)); EXPECT_EQ(solution,saved);
}

TEST(FvImplicitReach, SmoothMomentumChangeVanishesWithTimestep) {
    auto ch=makeWalledChannel(circular(3.0),48,25.0,[](double){return 0.0;},0.0,150.0);
    auto p=problem(ch,true);
    for (int i=0;i<ch.n;++i) p.old_discharge[i]=std::cos(2*pi*i/12.0);
    double previous=1;
    for (double dt:{1.e-3,1.e-5,1.e-7}) {
        Solver solver; Solver::Endpoint out; const auto r=solver.solve(p,dt,{},out);
        ASSERT_TRUE(r.converged()) << static_cast<int>(r.status) << ' ' << r.scaled_residual;
        double change=0,norm=0;
        for (int i=12;i<36;++i) { change+=std::pow(out.discharge[i]-p.old_discharge[i],2); norm+=std::pow(p.old_discharge[i],2); }
        change=std::sqrt(change/norm);
        EXPECT_LT(change,.02*previous); previous=change;
        if (dt==1.e-7) EXPECT_LT(change,1.e-5);
    }
}

TEST(FvImplicitReach, RestIsPreservedForWallAndStageAtLargeAcousticCfl) {
    for (bool walls:{false,true}) for (double speed:{150.0,660.0}) {
        auto ch=makeWalledChannel(circular(3.0),16,25.0,[](double){return 0.0;},.013,speed);
        auto p=problem(ch,walls); Solver solver; Solver::Endpoint out;
        const auto r=solver.solve(p,1.e4,{},out); ASSERT_TRUE(r.converged());
        EXPECT_EQ(out.area,p.old_area);
        // Prescribed depth -> stored area -> inverted depth can differ by one
        // ulp. Require machine-scale rest, not byte identity to decimal 6.0.
        for (double q:out.discharge) EXPECT_NEAR(q,0.0,1.e-12);
    }
}

TEST(FvImplicitReach, UniformFrictionMatchesIndependentImplicitQuadratic) {
    auto ch=makeWalledChannel(circular(3.0),12,25.0,[](double){return 0.0;},.013,150.0);
    for (double sign:{-1.0,1.0}) {
        auto p=problem(ch); std::fill(p.old_discharge.begin(),p.old_discharge.end(),sign*3.0);
        const double a=p.old_area[0], r=kernels::hydRadOfDepth(ch.mesh.geom[0],6.0);
        const double gamma=ch.mesh.conduit_rough_factor[0]/(a*r*std::cbrt(r));
        const double dt=20.0, reference=sign*6.0/(1+std::sqrt(1+4*dt*gamma*3));
        Solver solver; Solver::Endpoint out; const auto result=solver.solve(p,dt,{},out);
        ASSERT_TRUE(result.converged()) << static_cast<int>(result.status) << ' ' << result.scaled_residual;
        for (int i=0;i<ch.n;++i) { EXPECT_NEAR(out.discharge[i],reference,2.e-11); EXPECT_NEAR(out.area[i],a,1.e-13); }
    }
}

TEST(FvImplicitReach, StiffStepsDissipateLinearAcousticEnergyAndSatisfyBothEquations) {
    for (double cfl:{.1,1.0,10.0,100.0}) {
        auto ch=makeWalledChannel(circular(3.0),48,25.0,[](double){return 0.0;},0.0,660.0);
        auto p=problem(ch,true); const auto& g=ch.mesh.geom[0];
        for (int i=0;i<ch.n;++i) p.old_area[i]=kernels::areaOfDepth(g,6+.001*std::cos(pi*(i+.5)/ch.n));
        const double c=kernels::celerity(p.old_area[0],g.t_slot), dt=cfl*ch.dx/c;
        Solver solver; Solver::Endpoint out; const auto result=solver.solve(p,dt,{},out);
        ASSERT_TRUE(result.converged()) << cfl << ' ' << static_cast<int>(result.status) << ' ' << result.scaled_residual;
        auto x=trial(p); double before=0,after=0;
        for (int i=0;i<ch.n;++i) {
            before+=ch.dx*g.t_slot*std::pow(x[i][0]-6,2);
            after+=ch.dx*(g.t_slot*std::pow(out.depth[i]-6,2)+std::pow(out.discharge[i],2)/(kernels::kGravity*p.old_area[i]));
            x[i]={out.depth[i],out.discharge[i]};
        }
        EXPECT_LT(after,before*1.00001);
        std::vector<Pair> residual; ASSERT_TRUE(Solver::residual(p,x,dt,residual));
        long double volume_change=0;
        for (int i=0;i<ch.n;++i) {
            EXPECT_LE(std::fabs(residual[i][0]),1.e-11+1.e-12*ch.dx*p.old_area[i]);
            EXPECT_LE(std::fabs(residual[i][1]),1.1e-11);
            volume_change+=static_cast<long double>(ch.dx)*(out.area[i]-p.old_area[i]);
        }
        EXPECT_NEAR(static_cast<double>(volume_change),0,1.e-9);
    }
}

TEST(FvImplicitReach, CoordinateReflectionAndNonuniformCellsRetainOriginalStates) {
    auto ch=makeWalledChannel(circular(3.0),16,25.0,[](double){return 0.0;},.013,150.0);
    auto p=problem(ch); p.left_depth=6.2; p.right_depth=5.8;
    for (int i=0;i<ch.n;++i) {
        ch.mesh.cell_dx[i]=10+i;
        p.old_area[i]=kernels::areaOfDepth(ch.mesh.geom[0],6+.01*std::sin(i)); p.old_discharge[i]=.1*std::cos(i);
    }
    Solver solver; Solver::Endpoint first,second;
    ASSERT_TRUE(solver.solve(p,.5,{},first).converged());
    std::reverse(ch.mesh.cell_dx.begin(),ch.mesh.cell_dx.end());
    std::reverse(p.old_area.begin(),p.old_area.end()); std::reverse(p.old_discharge.begin(),p.old_discharge.end());
    for (auto& q:p.old_discharge) q=-q;
    std::swap(p.left_depth,p.right_depth);
    ASSERT_TRUE(solver.solve(p,.5,{},second).converged());
    ASSERT_EQ(first.area.size(),static_cast<std::size_t>(ch.n));
    for (int i=0;i<ch.n;++i) { EXPECT_NEAR(first.area[i],second.area[ch.n-1-i],1.e-12); EXPECT_NEAR(first.discharge[i],-second.discharge[ch.n-1-i],1.e-10); }
}

TEST(FvImplicitReach, FailuresPreserveOutputAndRejectUnsupportedGeometry) {
    auto ch=makeWalledChannel(circular(3.0),12,25.0,[](double){return 0.0;},.013,150.0);
    auto p=problem(ch); p.left_depth=6.2;
    Solver solver; auto out=sentinel(); Solver::Controls controls; controls.max_newton=0;
    EXPECT_EQ(solver.solve(p,.1,controls,out).status,Solver::Status::IterationLimit); unchanged(out);
    ch.mesh.cell_zb[1]=.1;
    EXPECT_EQ(solver.solve(p,.1,{},out).status,Solver::Status::InvalidInput); unchanged(out);
    ch.mesh.cell_zb[1]=0; p.old_area[2]=kernels::areaOfDepth(ch.mesh.geom[0],2.9);
    EXPECT_EQ(solver.solve(p,.1,{},out).status,Solver::Status::InvalidInput); unchanged(out);
    p=problem(ch); p.old_discharge[1]=std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(solver.solve(p,.1,{},out).status,Solver::Status::InvalidInput); unchanged(out);
}

TEST(FvImplicitReach, GmresFallbackSolvesNonsingularUnsafeBlockAndMatchesDense) {
    Solver::Linearization l; l.lower.resize(2); l.upper.resize(2); l.diagonal.resize(2);
    l.upper[0]={1,0,0,1}; l.lower[1]={1,0,0,1};
    const std::vector<Pair> known={{1,2},{3,4}}, rhs=applyBlocks(l,known), reference=dense(l,rhs);
    std::vector<Pair> output={{-9,-8}};
    Solver::LinearControls controls; controls.method=Solver::LinearMethod::StructuredOnly;
    auto result=Solver::solveLinear(l,rhs,controls,output);
    EXPECT_FALSE(result.converged); EXPECT_EQ(result.direct.failed_block,0);
    EXPECT_EQ(output,(std::vector<Pair>{{-9,-8}}));
    controls.method=Solver::LinearMethod::StructuredThenGmres;
    result=Solver::solveLinear(l,rhs,controls,output);
    ASSERT_TRUE(result.converged); EXPECT_TRUE(result.fallback); EXPECT_TRUE(result.used_gmres);
    EXPECT_EQ(result.direct.failed_block,0); EXPECT_EQ(result.shifted_preconditioner_blocks,1);
    EXPECT_GT(result.maximum_preconditioner_shift,0);
    EXPECT_EQ(result.direct.minimum_pivot_ratio,0);
    EXPECT_LE(result.wrapper_bytes+result.gmres.workspace_bytes,controls.gmres.workspace_limit_bytes);
    for (int i=0;i<2;++i) for (int k=0;k<2;++k) EXPECT_NEAR(output[i][k],reference[i][k],1.e-12);
    output={{-9,-8}}; controls.gmres.max_iterations=0;
    result=Solver::solveLinear(l,rhs,controls,output);
    EXPECT_FALSE(result.converged); EXPECT_EQ(result.gmres.status,NativeGmres::Status::IterationLimit);
    EXPECT_EQ(output,(std::vector<Pair>{{-9,-8}}));
    controls.gmres.max_iterations=200; controls.preconditioner=Solver::Preconditioner::BlockJacobi;
    result=Solver::solveLinear(l,rhs,controls,output);
    ASSERT_TRUE(result.converged); EXPECT_EQ(result.identity_preconditioner_blocks,2);
    for (int i=0;i<2;++i) for (int k=0;k<2;++k) EXPECT_NEAR(output[i][k],known[i][k],1.e-12);
    controls.preconditioner=Solver::Preconditioner::BlockLine;
    controls.gmres.workspace_limit_bytes=1; output={{-9,-8}};
    result=Solver::solveLinear(l,rhs,controls,output);
    EXPECT_FALSE(result.converged); EXPECT_EQ(result.gmres.status,NativeGmres::Status::MemoryLimit);
    EXPECT_EQ(output,(std::vector<Pair>{{-9,-8}}));
    // A shifted, invertible preconditioner must never make the original
    // incompatible zero matrix appear solved.
    controls.gmres.workspace_limit_bytes=64*1024*1024;
    l.upper[0]={}; l.lower[1]={};
    result=Solver::solveLinear(l,rhs,controls,output);
    EXPECT_FALSE(result.converged); EXPECT_EQ(result.gmres.status,NativeGmres::Status::Breakdown);
    EXPECT_EQ(output,(std::vector<Pair>{{-9,-8}}));
}

TEST(FvImplicitReach, GmresComparatorAgreesWithDirectAndDenseHydraulicCorrections) {
    for (int cells:{4,16,64}) {
        auto ch=makeWalledChannel(circular(3.0),cells,20.0,[](double){return 0.0;},.013,660.0);
        auto p=problem(ch,true); auto x=trial(p);
        for (int i=0;i<cells;++i) { x[i][0]+=.01*std::sin(i); x[i][1]=std::cos(.3*i); }
        Solver::Linearization l; ASSERT_TRUE(Solver::linearize(p,x,.1,1.e-4,l));
        auto rhs=l.residual; for (auto& v:rhs) { v[0]=-v[0]; v[1]=-v[1]; }
        const auto reference=dense(l,rhs);
        for (int restart:{20,40,80}) {
            Solver::LinearControls controls; controls.method=Solver::LinearMethod::GmresOnly; controls.gmres.restart=restart;
            std::vector<Pair> result;
            const auto solve=Solver::solveLinear(l,rhs,controls,result);
            ASSERT_TRUE(solve.converged) << cells << ' ' << restart << ' ' << static_cast<int>(solve.gmres.status);
            EXPECT_FALSE(solve.fallback); EXPECT_TRUE(solve.used_gmres);
            EXPECT_LE(solve.gmres.true_residual,solve.gmres.tolerance);
            for (int i=0;i<cells;++i) for (int k=0;k<2;++k)
                EXPECT_NEAR(result[i][k],reference[i][k],2.e-9*std::max(1.0,std::fabs(reference[i][k])));
        }
    }
}

TEST(FvImplicitReach, ForcedGmresNewtonMatchesDirectAndFailurePreservesEndpoint) {
    auto ch=makeWalledChannel(circular(3.0),48,25.0,[](double){return 0.0;},0.0,150.0);
    auto p=problem(ch,true);
    for (int i=0;i<ch.n;++i) p.old_area[i]=kernels::areaOfDepth(ch.mesh.geom[0],6+.001*std::cos(pi*(i+.5)/ch.n));
    Solver solver; Solver::Endpoint direct,iterative;
    ASSERT_TRUE(solver.solve(p,.05,{},direct).converged());
    Solver::Controls controls; controls.linear.method=Solver::LinearMethod::GmresOnly;
    const auto result=solver.solve(p,.05,controls,iterative);
    ASSERT_TRUE(result.converged()) << static_cast<int>(result.status) << ' ' << static_cast<int>(result.last_linear.gmres.status);
    EXPECT_GT(result.gmres_solves,0); EXPECT_GT(result.gmres_iterations,0); EXPECT_EQ(result.linear_fallbacks,0);
    for (int i=0;i<ch.n;++i) { EXPECT_NEAR(iterative.area[i],direct.area[i],1.e-12); EXPECT_NEAR(iterative.discharge[i],direct.discharge[i],1.e-10); }
    auto output=sentinel(); controls.linear.gmres.max_iterations=0;
    const auto failed=solver.solve(p,.05,controls,output);
    EXPECT_EQ(failed.status,Solver::Status::LinearFailure); unchanged(output);
}
