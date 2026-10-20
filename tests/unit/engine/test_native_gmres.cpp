// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include "hydraulics/fv/NativeGmres.hpp"

namespace {
using Solver=openswmm::fv::NativeGmres;
Solver::Apply identity(std::size_t n) {
    return [n](const double* x,double* y) { std::copy_n(x,n,y); return true; };
}
}

TEST(NativeGmres, IdentityAndZeroRhsRequireTrueResidual) {
    Solver solver; std::vector<double> out={-9};
    auto r=solver.solve({1,2,3},identity(3),{}, {},out);
    ASSERT_TRUE(r.converged()); EXPECT_TRUE(r.happy_breakdown); EXPECT_EQ(r.iterations,1);
    EXPECT_GE(r.matvecs,3); EXPECT_LE(r.true_residual,r.tolerance);
    for (int i=0;i<3;++i) EXPECT_NEAR(out[i],i+1,1.e-13);
    r=solver.solve({0,0,0},identity(3),{}, {},out);
    ASSERT_TRUE(r.converged()); EXPECT_EQ(r.iterations,0); EXPECT_EQ(r.matvecs,1);
    EXPECT_EQ(out,(std::vector<double>{0,0,0}));
}

TEST(NativeGmres, RestartedNonsymmetricSolveRetainsKnownSolution) {
    constexpr int n=32;
    auto matrix=[](const double* x,double* y) {
        for (int i=0;i<n;++i) y[i]=4*x[i]-(i ? x[i-1] : .2*x[n-1])-.3*(i+1<n ? x[i+1] : x[0]);
        return true;
    };
    std::vector<double> known(n),rhs(n),out;
    for (int i=0;i<n;++i) known[i]=std::sin(.3*i)+.2;
    matrix(known.data(),rhs.data());
    Solver solver; Solver::Controls c; c.restart=2;
    const auto r=solver.solve(rhs,matrix,{},c,out);
    ASSERT_TRUE(r.converged()) << static_cast<int>(r.status) << ' ' << r.true_residual;
    EXPECT_GT(r.restarts,0); EXPECT_GT(r.reorthogonalizations,0);
    EXPECT_LE(r.true_residual,r.tolerance);
    for (int i=0;i<n;++i) EXPECT_NEAR(out[i],known[i],1.e-9);
}

TEST(NativeGmres, SingleCycleOperatorAtRestart20_40_80) {
    constexpr int n=256;
    auto matrix=[](const double* x,double* y) {
        for (int i=0;i<n;++i) y[i]=3*x[i]-.7*x[(i+n-1)%n]-.2*x[(i+1)%n];
        return true;
    };
    std::vector<double> known(n),rhs(n),out;
    for (int i=0;i<n;++i) known[i]=std::cos(.13*i)+std::sin(.7*i);
    matrix(known.data(),rhs.data()); Solver solver;
    for (int restart:{20,40,80}) {
        Solver::Controls c; c.restart=restart;
        auto r=solver.solve(rhs,matrix,{},c,out);
        ASSERT_TRUE(r.converged()); EXPECT_LE(r.workspace_bytes,c.workspace_limit_bytes);
        for (int i=0;i<n;++i) EXPECT_NEAR(out[i],known[i],1.e-9);
    }
}

TEST(NativeGmres, FixedRightPreconditionerAcceleratesWithoutChangingMatrix) {
    constexpr int n=20;
    auto matrix=[](const double* x,double* y) { for (int i=0;i<n;++i) y[i]=(i+1)*x[i]; return true; };
    auto inverse=[](const double* x,double* y) { for (int i=0;i<n;++i) y[i]=x[i]/(i+1); return true; };
    std::vector<double> rhs(n),out;
    for (int i=0;i<n;++i) rhs[i]=i+1;
    Solver solver; Solver::Controls c; c.restart=20;
    const auto plain=solver.solve(rhs,matrix,{},c,out);
    ASSERT_TRUE(plain.converged());
    const auto preconditioned=solver.solve(rhs,matrix,inverse,c,out);
    ASSERT_TRUE(preconditioned.converged()); EXPECT_EQ(preconditioned.iterations,1);
    EXPECT_GT(plain.iterations,preconditioned.iterations);
    for (double value:out) EXPECT_NEAR(value,1,1.e-12);
}

TEST(NativeGmres, TinyAndLargeOperatorScalesUseRelativeResidual) {
    for (double scale:{1.e-150,1.e150}) {
        auto matrix=[=](const double* x,double* y) { for (int i=0;i<3;++i) y[i]=scale*x[i]; return true; };
        Solver solver; Solver::Controls c; c.absolute_tolerance=0;
        std::vector<double> out;
        const auto r=solver.solve({scale,2*scale,3*scale},matrix,{},c,out);
        ASSERT_TRUE(r.converged()); EXPECT_LE(r.true_residual,r.tolerance);
        for (int i=0;i<3;++i) EXPECT_NEAR(out[i],i+1,1.e-12);
    }
}

TEST(NativeGmres, SingularConsistentHappyBreakdownAndInconsistentFailure) {
    auto matrix=[](const double* x,double* y) { y[0]=x[0]; y[1]=0; return true; };
    Solver solver; std::vector<double> out={-9};
    auto r=solver.solve({1,0},matrix,{}, {},out);
    ASSERT_TRUE(r.converged()); EXPECT_TRUE(r.happy_breakdown);
    out={-9}; r=solver.solve({0,1},matrix,{}, {},out);
    EXPECT_EQ(r.status,Solver::Status::Breakdown); EXPECT_EQ(out,(std::vector<double>{-9}));
}

TEST(NativeGmres, StagnationAndIterationLimitsDoNotPublishPartialIterates) {
    auto rotation=[](const double* x,double* y) { y[0]=-x[1]; y[1]=x[0]; return true; };
    Solver solver; Solver::Controls c; c.restart=1;
    std::vector<double> out={-9};
    auto r=solver.solve({1,0},rotation,{},c,out);
    EXPECT_EQ(r.status,Solver::Status::Stagnated); EXPECT_EQ(out,(std::vector<double>{-9}));
    c.max_iterations=0; r=solver.solve({1,0},rotation,{},c,out);
    EXPECT_EQ(r.status,Solver::Status::IterationLimit); EXPECT_EQ(out,(std::vector<double>{-9}));
    c.max_iterations=1; r=solver.solve({1,0},rotation,{},c,out);
    EXPECT_EQ(r.status,Solver::Status::IterationLimit); EXPECT_EQ(out,(std::vector<double>{-9}));
}

TEST(NativeGmres, MemoryBudgetIncludesRetainedCapacityAndAllowsFreshReuseLayout) {
    Solver solver; Solver::Controls c; c.restart=5;
    std::vector<double> out;
    ASSERT_TRUE(solver.solve(std::vector<double>(200,1),identity(200),{},c,out).converged());
    c.restart=40; c.workspace_limit_bytes=sizeof(double)*(41*40+41*40+6*40+4*40+1);
    auto r=solver.solve(std::vector<double>(40,1),identity(40),{},c,out);
    ASSERT_TRUE(r.converged()); EXPECT_LE(r.workspace_bytes,c.workspace_limit_bytes);
    out={-9}; c.workspace_limit_bytes=1;
    r=solver.solve({1,2},identity(2),{},c,out);
    EXPECT_EQ(r.status,Solver::Status::MemoryLimit); EXPECT_EQ(out,(std::vector<double>{-9}));
}

TEST(NativeGmres, OperatorAndPreconditionerFailuresAreExplicit) {
    Solver solver; std::vector<double> out={-9};
    Solver::Apply failure=[](const double*,double*) { return false; };
    auto r=solver.solve({1,2},failure,{}, {},out);
    EXPECT_EQ(r.status,Solver::Status::OperatorFailure); EXPECT_EQ(out,(std::vector<double>{-9}));
    r=solver.solve({1,2},identity(2),failure,{},out);
    EXPECT_EQ(r.status,Solver::Status::PreconditionerFailure); EXPECT_EQ(out,(std::vector<double>{-9}));
    Solver::Apply nonfinite=[](const double*,double* y) { y[0]=std::numeric_limits<double>::quiet_NaN(); y[1]=0; return true; };
    r=solver.solve({1,2},nonfinite,{}, {},out);
    EXPECT_EQ(r.status,Solver::Status::NonFinite); EXPECT_EQ(out,(std::vector<double>{-9}));
}

TEST(NativeGmres, InvalidControlsAndNonfiniteRhsFailBeforeIteration) {
    Solver solver; Solver::Controls c; std::vector<double> out={-9};
    c.restart=0; EXPECT_EQ(solver.solve({1},identity(1),{},c,out).status,Solver::Status::InvalidInput);
    c={}; c.relative_tolerance=-1; EXPECT_EQ(solver.solve({1},identity(1),{},c,out).status,Solver::Status::InvalidInput);
    c={}; EXPECT_EQ(solver.solve({std::numeric_limits<double>::infinity()},identity(1),{},c,out).status,Solver::Status::InvalidInput);
    EXPECT_EQ(out,(std::vector<double>{-9}));
}
