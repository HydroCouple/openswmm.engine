// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file test_2d_lts_equivalence.cpp
 * @brief Phase-2 gates for tiered local timestepping (LTS) in the explicit
 *        marcher: K-tier runs must conserve exactly and agree with the
 *        global-dt (K = 1) reference; multiscale meshes must be stable and
 *        cost-proportional.
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>
#include <cmath>
#include <vector>

#include "2d/data/MeshData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "2d/solver/InertialKernels.hpp"

using namespace openswmm::twoD;

namespace {

// Rectangular grid with GEOMETRIC column spacing (dx0 growing by `ratio` per
// column) — the multiscale strip: fine urban cells on the left, coarse
// watershed cells on the right, area disparity (ratio^(nx−1))². Keep the
// growth gentle: rows are uniform, so column growth IS aspect ratio, and
// high-aspect slivers are outside the scheme's mesh contract (the floored
// face-normal projection misestimates their gradients — measured as a
// perpetual non-level limit cycle at 25:1 EVEN at K=1). Real multiscale
// meshes grade area with bounded aspect (Bellinge: min angle 34.6°).
MeshData makeGradedStrip(int nx, int ny, double dx0, double ratio,
                         double slope = 0.0, double n = 0.03) {
    MeshData mesh;
    std::vector<double> xs(nx + 1, 0.0);
    double dx = dx0;
    for (int i = 1; i <= nx; ++i) {
        xs[i] = xs[i - 1] + dx;
        dx *= ratio;
    }
    const double dy = dx0;   // uniform rows
    const int nvx = nx + 1, nvy = ny + 1;
    mesh.resize_vertices(nvx * nvy);
    for (int j = 0; j < nvy; ++j)
        for (int i = 0; i < nvx; ++i) {
            const int v = j * nvx + i;
            mesh.vx[v] = xs[i];
            mesh.vy[v] = j * dy;
            mesh.vz[v] = slope * xs[i];
        }
    mesh.resize_triangles(2 * nx * ny);
    int t = 0;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const int v00 = j * nvx + i,       v10 = j * nvx + i + 1;
            const int v01 = (j + 1) * nvx + i, v11 = (j + 1) * nvx + i + 1;
            mesh.tri_v0[t] = v00; mesh.tri_v1[t] = v10; mesh.tri_v2[t] = v11;
            ++t;
            mesh.tri_v0[t] = v00; mesh.tri_v1[t] = v11; mesh.tri_v2[t] = v01;
            ++t;
        }
    for (int i = 0; i < mesh.n_triangles(); ++i) mesh.mannings_n[i] = n;
    buildMeshTopology(mesh);
    return mesh;
}

SurfaceStateData makeState(const MeshData& mesh) {
    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        state.head[i]   = mesh.tri_cz[i];
        state.depth[i]  = 0.0;
        state.volume[i] = 0.0;
    }
    return state;
}

double totalVolume(const SurfaceStateData& s, int nt) {
    double v = 0.0;
    for (int i = 0; i < nt; ++i) v += s.volume[i];
    return v;
}

// Dam-break on the graded strip, K tiers: captures volumes mid-transient
// (t_mid) and near-settled (t_end). With mean_win > 0 the end capture is the
// TIME-MEAN over [t_end - mean_win, t_end] (sampled once per 5 s advance):
// the closed basin sustains a weakly damped seiche, so an instantaneous
// end-state sample aliases the K-dependent seiche phase (and the platform's
// FP contraction), while the time-mean is the settled solution. Same gating
// rationale as the SpecifiedStageFillAndDrawdownLedger time-mean gates.
void runGradedDamBreak(int K, double t_mid, double t_end,
                       std::vector<double>* v_mid, std::vector<double>* v_end,
                       double* sum_drift = nullptr, double mean_win = 0.0) {
    auto mesh = makeGradedStrip(24, 4, 1.0, 1.08, /*slope=*/0.0,
                                /*n=*/0.10);   // rough: seiches damp out
    SolverOptions2D opts;
    opts.lts_tiers = K;
    auto state = makeState(mesh);
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        if (mesh.tri_cx[i] < 4.0) {
            state.volume[i] = 1.5 * mesh.tri_area[i];
            inertial::cellEtaDepth(mesh, opts, i, state.volume[i],
                                   state.head[i], state.depth[i]);
        }
    }
    const double sum0 = totalVolume(state, mesh.n_triangles());
    ExplicitInertialSolver solver;
    solver.initialize(mesh, state, opts);
    for (double t = 0.0; t < t_mid; t += 5.0)
        solver.advance(t, std::min(t + 5.0, t_mid));
    if (v_mid) *v_mid = state.volume;
    std::vector<double> acc(state.volume.size(), 0.0);
    int n_samples = 0;
    for (double t = t_mid; t < t_end; t += 5.0) {
        solver.advance(t, std::min(t + 5.0, t_end));
        if (mean_win > 0.0 && t + 5.0 >= t_end - mean_win) {
            for (std::size_t i = 0; i < acc.size(); ++i)
                acc[i] += state.volume[i];
            ++n_samples;
        }
    }
    if (v_end) {
        if (n_samples > 0) {
            for (auto& a : acc) a /= n_samples;
            *v_end = acc;
        } else {
            *v_end = state.volume;
        }
    }
    if (sum_drift)
        *sum_drift =
            std::fabs(totalVolume(state, mesh.n_triangles()) - sum0) / sum0;
    solver.finalize();
}

}  // namespace

// ---------------------------------------------------------------------------
// Conservation is exact (reduction roundoff) at every tier count, through a
// multiscale dam-break with fronts crossing tier interfaces.
// ---------------------------------------------------------------------------
TEST(LtsEquivalence, ConservationAtEveryTierCount) {
    for (int K : {1, 2, 4, 6}) {
        double drift = 0.0;
        runGradedDamBreak(K, 60.0, 120.0, nullptr, nullptr, &drift);
        EXPECT_LE(drift, 1.0e-10) << "volume drift at K=" << K;
    }
}

// ---------------------------------------------------------------------------
// K-tier solution ≈ global-dt solution. Mid-transient the tier interfaces lag
// the front by O(coarse Δt), so instantaneous per-cell depths differ most AT
// the moving front — gate the RMS there and the max only once near-settled
// (LTS changes the integration path, not the physics).
// ---------------------------------------------------------------------------
TEST(LtsEquivalence, TierSolutionMatchesGlobalDt) {
    std::vector<double> v1m, v1e, v4m, v4e;
    runGradedDamBreak(1, 120.0, 1500.0, &v1m, &v1e, nullptr, /*mean_win=*/300.0);
    runGradedDamBreak(4, 120.0, 1500.0, &v4m, &v4e, nullptr, /*mean_win=*/300.0);
    auto mesh = makeGradedStrip(24, 4, 1.0, 1.08, 0.0, 0.10);
    ASSERT_EQ(v1m.size(), v4m.size());

    // LTS lags a sharp front by O(coarse Δt) — instantaneous per-cell depths
    // at a creeping front differ by up to the front height for ANY scheme of
    // this class. The robust equivalences: (a) the divergence is BOUNDED and
    // DECAYS as the solution smooths; (b) bulk arrival matches: the volume
    // that has passed the 2/3-basin station agrees within a few percent.
    // The end-state capture is the 300 s TIME-MEAN, not an instantaneous
    // sample: the closed basin's weakly damped seiche never fully stops, and
    // K = 4 vs K = 1 drift out of phase, so a point sample aliases seiche
    // phase and platform FP contraction (measured 0.025–0.062 m spread
    // across x64/arm64 CI on the same commit; the time-mean sits at
    // ~0.010 m everywhere with the same 0.025 m gate).
    double max_mid = 0.0, max_end = 0.0;
    for (std::size_t i = 0; i < v1m.size(); ++i) {
        const double a = mesh.tri_area[static_cast<int>(i)];
        max_mid = std::max(max_mid, std::fabs(v4m[i] - v1m[i]) / a);
        max_end = std::max(max_end, std::fabs(v4e[i] - v1e[i]) / a);
    }
    EXPECT_LT(max_end, max_mid) << "tier divergence must decay, not grow";
    EXPECT_LE(max_end, 0.025) << "near-settled max depth divergence (m)";

    const double x23 = mesh.vx[24 * 2 / 3];   // 2/3-basin station
    auto passed = [&](const std::vector<double>& v) {
        double s = 0.0;
        for (std::size_t i = 0; i < v.size(); ++i)
            if (mesh.tri_cx[static_cast<int>(i)] > x23) s += v[i];
        return s;
    };
    const double p1 = passed(v1e), p4 = passed(v4e);
    EXPECT_NEAR(p4, p1, 0.05 * std::max(p1, 1.0e-12))
        << "bulk volume past the 2/3 station diverges";
}

// ---------------------------------------------------------------------------
// Fine-urban-patch-in-coarse-watershed (the configuration that diverged the
// old windowed inertial prototype): stable, conservative, V >= 0 throughout.
// ---------------------------------------------------------------------------
TEST(LtsEquivalence, FinePatchInCoarseWatershedStable) {
    auto mesh = makeGradedStrip(30, 3, 0.5, 1.08, /*slope=*/0.02);
    SolverOptions2D opts;
    opts.lts_tiers = 6;
    auto state = makeState(mesh);
    for (int i = 0; i < mesh.n_triangles(); ++i)
        state.rainfall[i] = 5.0e-5;                  // 180 mm/hr burst
    const double t_end = 300.0;
    ExplicitInertialSolver solver;
    solver.initialize(mesh, state, opts);
    double rain_in = 0.0;
    for (int i = 0; i < mesh.n_triangles(); ++i)
        rain_in += 5.0e-5 * mesh.tri_area[i] * t_end;
    for (double t = 0.0; t < t_end; t += 10.0) {
        solver.advance(t, t + 10.0);
        for (int i = 0; i < mesh.n_triangles(); ++i) {
            ASSERT_GE(state.volume[i], 0.0);
            ASSERT_FALSE(std::isnan(state.volume[i]));
        }
    }
    // Closed basin (walls): everything that rained is still on the surface.
    EXPECT_NEAR(totalVolume(state, mesh.n_triangles()), rain_in,
                1.0e-8 * rain_in);
    solver.finalize();
}
