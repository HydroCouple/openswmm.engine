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
 * @file test_ard_dispersion.cpp
 * @brief E3 gates: dispersion activation in the Eulerian ARD engine
 *        (plans/transport/EULERIAN_ARD_TRANSPORT_PLAN.md §6 E3).
 *
 * @details Two tiers, each with an explicit observation path (the R4
 *          coverage-geometry lesson — every claimed defense has a gate that
 *          can physically see it):
 *
 *          KERNEL tier (synthetic single-chain mesh, no engine):
 *          - UniformArrayBitwiseMatchesScalar gates the E3 kernel edit's
 *            bitwise-preservation argument: a per-cell array of one value
 *            must reproduce the scalar path EXACTLY (0.5*(D+D) == D in
 *            binary FP). This is the executable form of the FV solver's
 *            unchanged-behavior contract.
 *          - VarianceGrowthIsExactlyTwoDDtPerStep: for the interior
 *            implicit scheme on a uniform grid, the discrete second moment
 *            grows by EXACTLY 2·D·dt·mass per solve (summation by parts on
 *            the tridiagonal identity — see the gate body), so the gate is
 *            analytic with a 1e-9 relative band, not a curve-fit.
 *          - HugeDIsStableAndBounded is the plan's "implicit-step
 *            restriction" verify: D = 1e6 with a large dt must stay finite
 *            and inside [min0, max0] (M-matrix max principle).
 *          - HeterogeneousDIsPerCell: a pulse inside a zero-D region of a
 *            chain must stay BITWISE frozen (its matrix rows are identity)
 *            while a pulse in the D > 0 region of the same chain spreads —
 *            falsified by any fallback from the per-cell array to a scalar.
 *
 *          ENGINE tier (decks + model.ard via [PROCESS_COMPONENTS]):
 *          - Receding-front decks (Cinit everywhere, clean steady inflow):
 *            dispersion smears the front, so the last conduit's
 *            concentration trajectory must differ from the no-component run
 *            by an integrated-|diff| margin (falsifies dead plumbing in
 *            either direction), while never exceeding Cinit (max principle
 *            end to end).
 *          - Per-conduit override observation path: with global OFF and an
 *            override on C3 only, links C1/C2 (upstream, separate chains,
 *            no reverse flow) must match the no-component run BITWISE step
 *            by step — dispersion leaking out of C3 fails here — C3 itself
 *            must change (an override that landed on C4 also reshapes C5,
 *            so the downstream check alone is not a mapping razor), and the
 *            downstream trajectory must differ.
 *          - FISCHER mode on a sloped chain (one dead-flat conduit included
 *            to exercise the slope floor) must be active, finite, bounded.
 *          - Config errors: E5/E2b deferral wording, unknown conduit,
 *            negative coefficient — and a failed apply leaves the config
 *            unconfigured (never half-applied).
 *          - Bypass warnings (R4 lesson): LEGACY and IGNORE_QUALITY decks
 *            with dispersion configured warn at open, naming the remedy —
 *            and the REVERSE case, [OPTIONS] FV_DISPERSION under
 *            EULERIAN_ARD, where there is no component to hang a warning on
 *            and the measured effect was zero with no diagnostic at all.
 *
 *          Measured separations (integrated |Δconc| at C5 over the run, as
 *          a fraction of the base signal; tests/output/e3_.../e3_probe.log):
 *          DISPERSION 100 → 0.0447 (gate floor 0.01); C3 override 200 →
 *          0.0178 downstream and 0.0372 in C3 itself (floors 0.005/0.01);
 *          FISCHER → 9.27e-4 (floor 1e-6, deliberately a liveness check);
 *          CMS DISPERSION 10 → 0.0300 (floor 0.01).
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>

#include "core/SWMMEngine.hpp"
#include "hydraulics/fv/NetworkMeshData.hpp"
#include "transport/fvkernels/SpeciesTransportKernels.hpp"

namespace {

namespace fvk = openswmm::transport::fvkernels;

// ===========================================================================
// Kernel tier — synthetic single-chain mesh
// ===========================================================================

/// Single chain of m cells, uniform dx and area. Only the fields
/// dispersionSolve reads are populated (chain arrays, cell_dx; state areas
/// and phi) — the advection-side face records stay empty.
struct ChainFixture {
    openswmm::fv::NetworkMeshData  mesh;
    openswmm::fv::NetworkStateData state;

    ChainFixture(int m, double dx, double area) {
        mesh.cell_dx.assign(static_cast<std::size_t>(m), dx);
        mesh.chain_ptr = {0, m};
        mesh.chain_cells.resize(static_cast<std::size_t>(m));
        for (int i = 0; i < m; ++i)
            mesh.chain_cells[static_cast<std::size_t>(i)] = i;
        state.resize(m, 0, 1);
        for (int i = 0; i < m; ++i)
            state.cell_a[static_cast<std::size_t>(i)] = area;
    }

    fvk::SpeciesKernelView view() {
        fvk::SpeciesKernelView v;
        v.mesh  = &mesh;
        v.state = &state;
        return v;
    }
};

TEST(ArdDispersionKernel, UniformArrayBitwiseMatchesScalar) {
    constexpr int    kM  = 32;
    constexpr double kD  = 0.7;
    constexpr double kDt = 12.5;

    ChainFixture fa(kM, 10.0, 2.0);
    ChainFixture fb(kM, 10.0, 2.0);
    for (int i = 0; i < kM; ++i) {
        // Irregular profile so every face carries a distinct flux.
        const double phi = 3.0 + 2.0 * std::sin(0.7 * i) + 0.01 * i * i;
        fa.state.cell_phi[static_cast<std::size_t>(i)] = phi;
        fb.state.cell_phi[static_cast<std::size_t>(i)] = phi;
    }

    auto va = fa.view();
    va.dispersion = kD;                      // scalar path (pre-E3)
    fvk::dispersionSolve(va, kDt);

    const std::vector<double> uniform(static_cast<std::size_t>(kM), kD);
    auto vb = fb.view();
    vb.dispersion      = 0.0;                // must be ignored
    vb.cell_dispersion = &uniform;           // per-cell path (E3)
    fvk::dispersionSolve(vb, kDt);

    for (int i = 0; i < kM; ++i)
        EXPECT_EQ(fa.state.cell_phi[static_cast<std::size_t>(i)],
                  fb.state.cell_phi[static_cast<std::size_t>(i)])
            << "cell " << i
            << ": per-cell uniform array diverged from the scalar path — "
               "the E3 bitwise-preservation argument (0.5*(D+D) == D) is "
               "violated, so the FV solver's behavior changed too.";
}

TEST(ArdDispersionKernel, VarianceGrowthIsExactlyTwoDDtPerStep) {
    // Derivation the band rests on: interior row of the implicit solve is
    //   phi^n_i = phi^{n+1}_i − λ (phi^{n+1}_{i+1} − 2 phi^{n+1}_i +
    //             phi^{n+1}_{i−1}),  λ = D·dt/dx².
    // Multiply by a·dx·x_i² and sum; summation by parts turns
    // Σ x²Δphi into Σ phi·Δ(x²) = 2dx²·Σ phi (compact support), giving
    //   M2^{n+1} = M2^n + 2·D·dt·mass   EXACTLY,
    // as long as the profile's support stays far from the chain ends (the
    // implicit tails decay geometrically; 100 cells of clearance puts them
    // far below machine epsilon).
    constexpr int    kM    = 201;
    constexpr int    kMid  = 100;
    constexpr double kDx   = 1.0;
    constexpr double kA    = 1.0;
    constexpr double kD    = 0.25;
    constexpr double kDt   = 0.5;
    constexpr int    kSteps = 20;

    ChainFixture f(kM, kDx, kA);
    f.state.cell_phi[kMid] = 1.0;

    const auto moments = [&](double& mass, double& m2) {
        mass = 0.0;
        m2   = 0.0;
        for (int i = 0; i < kM; ++i) {
            const double w =
                kA * kDx * f.state.cell_phi[static_cast<std::size_t>(i)];
            const double x = (i - kMid) * kDx;
            mass += w;
            m2   += w * x * x;
        }
    };

    double mass0 = 0.0, m2_0 = 0.0;
    moments(mass0, m2_0);

    auto v = f.view();
    v.dispersion = kD;
    for (int n = 0; n < kSteps; ++n) fvk::dispersionSolve(v, kDt);

    double mass1 = 0.0, m2_1 = 0.0;
    moments(mass1, m2_1);

    EXPECT_NEAR(mass1, mass0, 1.0e-12 * mass0)
        << "implicit dispersion lost mass on a uniform chain";
    const double expected = kSteps * 2.0 * kD * kDt * mass0;
    EXPECT_NEAR(m2_1 - m2_0, expected, 1.0e-9 * expected)
        << "second-moment growth is not 2·D·dt·mass per step — the "
           "discrete diffusion coefficient does not equal D.";
}

TEST(ArdDispersionKernel, HugeDIsStableAndBounded) {
    // The plan's implicit-step restriction verify: no Δx²/(2D) constraint.
    constexpr int    kM  = 64;
    constexpr double kD  = 1.0e6;
    constexpr double kDt = 1.0e3;

    ChainFixture f(kM, 5.0, 1.5);
    double lo0 = 1.0e300, hi0 = -1.0e300;
    for (int i = 0; i < kM; ++i) {
        const double phi = (i % 7 == 0) ? 9.0 : 1.0;
        f.state.cell_phi[static_cast<std::size_t>(i)] = phi;
        lo0 = std::min(lo0, phi);
        hi0 = std::max(hi0, phi);
    }

    auto v = f.view();
    v.dispersion = kD;
    for (int n = 0; n < 5; ++n) fvk::dispersionSolve(v, kDt);

    for (int i = 0; i < kM; ++i) {
        const double phi = f.state.cell_phi[static_cast<std::size_t>(i)];
        EXPECT_TRUE(std::isfinite(phi)) << "cell " << i;
        EXPECT_GE(phi, lo0 - 1.0e-12) << "cell " << i;
        EXPECT_LE(phi, hi0 + 1.0e-12) << "cell " << i;
    }
}

TEST(ArdDispersionKernel, HeterogeneousDIsPerCell) {
    constexpr int    kM  = 60;
    constexpr double kDt = 4.0;

    ChainFixture f(kM, 8.0, 1.0);
    std::vector<double> d(static_cast<std::size_t>(kM), 0.0);
    for (int i = 30; i < kM; ++i) d[static_cast<std::size_t>(i)] = 2.0;

    // Pulse A deep inside the zero-D region; pulse B in the D > 0 region.
    f.state.cell_phi[10] = 5.0;
    f.state.cell_phi[45] = 5.0;
    const std::vector<double> before(f.state.cell_phi.begin(),
                                     f.state.cell_phi.end());

    auto v = f.view();
    v.dispersion      = 0.0;
    v.cell_dispersion = &d;
    fvk::dispersionSolve(v, kDt);

    // Zero-D rows are identity rows of the tridiagonal (both face
    // coefficients vanish), so the left half must be BITWISE unchanged —
    // any fallback to a scalar D breaks this in one direction or the other.
    for (int i = 0; i < 29; ++i)
        EXPECT_EQ(f.state.cell_phi[static_cast<std::size_t>(i)],
                  before[static_cast<std::size_t>(i)])
            << "cell " << i << " inside the D = 0 region changed";

    // The D > 0 pulse must have spread to its neighbours.
    EXPECT_LT(f.state.cell_phi[45], 5.0);
    EXPECT_GT(f.state.cell_phi[44], 0.0);
    EXPECT_GT(f.state.cell_phi[46], 0.0);
}

// ===========================================================================
// Engine tier — decks + model.ard
// ===========================================================================

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

/// Five conduits in series, declining inverts, steady clean inflow at J0,
/// TSS Cinit everywhere: a receding front sweeps the chain. `flat_c3`
/// zeroes C3's slope (both ends at the same invert) to exercise the FISCHER
/// slope floor. `extra_options` appends to [OPTIONS].
///
/// The junction INITIAL DEPTH is load-bearing, not decoration. initQuality()
/// seeds Cinit only into elements that are already WET (legacy
/// qualrout_init's 1 mm test); with the depth column at 0 the whole network
/// starts dry, Cinit is discarded, and the clean inflow carries no
/// pollutant — so the deck holds zero TSS for the entire run and every
/// front-based metric divides by zero. Measured: initial stored mass 0.000
/// lb at depth 0 versus 3.491 lb at 1.5 ft.
void write_chain_deck(const char* path, const std::string& pc_lines,
                      const std::string& extra_options = "",
                      const char* flow_units = "CFS", bool flat_c3 = false) {
    // C3 spans J2 → J3; a flat C3 puts both of its ends at J3's invert.
    const double j2_inv = flat_c3 ? 8.2 : 8.8;
    std::ofstream f(path);
    f << "[TITLE]\nE3 dispersion gate deck\n\n[OPTIONS]\n"
      << "FLOW_UNITS " << flow_units << "\nFLOW_ROUTING DYNWAVE\n"
      << "QUALITY_SOLVER EULERIAN_ARD\n"
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME 01:00:00\n"
      << "ROUTING_STEP 5\nREPORT_STEP 00:01:00\n"
      << extra_options << "\n"
      << "[JUNCTIONS]\n"                   // Name Elev MaxDepth InitDepth …
      << "J0 10.0 10 1.5 0 0\n"
      << "J1 9.4  10 1.5 0 0\n"
      << "J2 " << j2_inv << " 10 1.5 0 0\n"
      << "J3 8.2  10 1.5 0 0\n"
      << "J4 7.6  10 1.5 0 0\n\n"
      << "[OUTFALLS]\nOUT 7.0 FREE  NO\n\n"
      << "[CONDUITS]\n"
      << "C1 J0 J1 500 0.013 0 0 0\n"
      << "C2 J1 J2 500 0.013 0 0 0\n"
      << "C3 J2 J3 500 0.013 0 0 0\n"
      << "C4 J3 J4 500 0.013 0 0 0\n"
      << "C5 J4 OUT 500 0.013 0 0 0\n\n"
      << "[XSECTIONS]\n"
      << "C1 CIRCULAR 2.0 0 0 0\nC2 CIRCULAR 2.0 0 0 0\n"
      << "C3 CIRCULAR 2.0 0 0 0\nC4 CIRCULAR 2.0 0 0 0\n"
      << "C5 CIRCULAR 2.0 0 0 0\n\n"
      << "[INFLOWS]\n"
      << "J0 FLOW \"\" FLOW 1.0 1.0 5\n\n"
      << "[POLLUTANTS]\n"
      << ";;Name Units Crain Cgw Crdii Kdecay SnowOnly CoPollut CoFrac Cdwf Cinit\n"
      << "TSS    MG/L  0     0   0     0      NO       *        0      0    10\n\n";
    if (!pc_lines.empty())
        f << "[PROCESS_COMPONENTS]\n" << pc_lines << "\n\n";
    f << "[REPORT]\nINPUT NO\n";
}

/// The EXACT metric transcription of write_chain_deck's CFS geometry: the
/// same physical system described in SI. Every factor below is a finite
/// decimal (1 ft = 0.3048 m exactly, 1 ft3 = 0.028316846592 m3 exactly), so
/// the two decks are the same model to the last digit the parser reads —
/// which is what lets a gate assert they must produce the SAME answer
/// instead of asserting a magnitude.
///
/// NOTE this is NOT write_chain_deck(..., "CMS"): that flips FLOW_UNITS and
/// leaves the numbers alone, so it describes a 500 m chain carrying 5 m3/s
/// — a different, much larger system. Useful for exercising the SI code
/// path, useless for comparing against anything.
void write_chain_deck_si(const char* path, const std::string& pc_lines) {
    std::ofstream f(path);
    f << "[TITLE]\nE3 dispersion gate deck (SI transcription)\n\n[OPTIONS]\n"
      << "FLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\n"
      << "QUALITY_SOLVER EULERIAN_ARD\n"
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME 01:00:00\n"
      << "ROUTING_STEP 5\nREPORT_STEP 00:01:00\n\n"
      << "[JUNCTIONS]\n"                  // ft x 0.3048
      << "J0 3.048   3.048 0.4572 0 0\n"   // 10.0 / 10 / 1.5
      << "J1 2.86512 3.048 0.4572 0 0\n"   //  9.4
      << "J2 2.68224 3.048 0.4572 0 0\n"   //  8.8
      << "J3 2.49936 3.048 0.4572 0 0\n"   //  8.2
      << "J4 2.31648 3.048 0.4572 0 0\n\n" //  7.6
      << "[OUTFALLS]\nOUT 2.1336 FREE  NO\n\n"   // 7.0
      << "[CONDUITS]\n"                   // 500 ft = 152.4 m
      << "C1 J0 J1 152.4 0.013 0 0 0\n"
      << "C2 J1 J2 152.4 0.013 0 0 0\n"
      << "C3 J2 J3 152.4 0.013 0 0 0\n"
      << "C4 J3 J4 152.4 0.013 0 0 0\n"
      << "C5 J4 OUT 152.4 0.013 0 0 0\n\n"
      << "[XSECTIONS]\n"                  // 2.0 ft = 0.6096 m
      << "C1 CIRCULAR 0.6096 0 0 0\nC2 CIRCULAR 0.6096 0 0 0\n"
      << "C3 CIRCULAR 0.6096 0 0 0\nC4 CIRCULAR 0.6096 0 0 0\n"
      << "C5 CIRCULAR 0.6096 0 0 0\n\n"
      << "[INFLOWS]\n"                    // 5 cfs = 0.14158423296 m3/s
      << "J0 FLOW \"\" FLOW 1.0 1.0 0.14158423296\n\n"
      << "[POLLUTANTS]\n"                 // mg/L is unit-system independent
      << "TSS    MG/L  0     0   0     0      NO       *        0      0    10\n\n";
    if (!pc_lines.empty())
        f << "[PROCESS_COMPONENTS]\n" << pc_lines << "\n\n";
    f << "[REPORT]\nINPUT NO\n";
}

void write_ard(const char* path, const std::string& body) {
    std::ofstream c(path);
    c << body;
}

/// Per-step trajectory of one link's first-pollutant concentration over a
/// full run. ASSERT on lifecycle (a failed open leaves conc arrays empty —
/// the R4 EXPECT-on-open lesson).
class ArdDispersionEngineTest : public ::testing::Test {
protected:
    /// One full run on a fresh engine; records every link's conc per step.
    /// Returns false (with ADD_FAILURE already raised) on lifecycle failure.
    bool run_recording(const char* inp, const char* rpt, const char* out,
                       std::vector<std::vector<double>>& traj,
                       std::vector<std::string>* warnings = nullptr) {
        traj.clear();
        SWMM_Engine e = swmm_engine_create();
        if (e == nullptr) { ADD_FAILURE() << "engine create"; return false; }
        bool ok = true;
        if (swmm_engine_open(e, inp, rpt, out, nullptr) != SWMM_OK) {
            ADD_FAILURE() << "open failed for " << inp;
            ok = false;
        }
        if (ok && (swmm_engine_initialize(e) != SWMM_OK ||
                   swmm_engine_start(e, 1) != SWMM_OK)) {
            ADD_FAILURE() << "init/start failed for " << inp;
            ok = false;
        }
        if (ok) {
            auto& ctx = as_cpp_engine(e).context();
            const int nl = ctx.n_links();
            traj.assign(static_cast<std::size_t>(nl), {});
            double elapsed = 0.0;
            int guard = 0;
            do {
                if (swmm_engine_step(e, &elapsed) != SWMM_OK) {
                    ADD_FAILURE() << "step failed for " << inp;
                    ok = false;
                    break;
                }
                for (int l = 0; l < nl; ++l)
                    traj[static_cast<std::size_t>(l)].push_back(
                        ctx.links.conc[static_cast<std::size_t>(l)]);
            } while (elapsed > 0.0 && ++guard < 20000);
            if (ok) swmm_engine_end(e);
            if (warnings != nullptr) *warnings = ctx.warnings;
        }
        swmm_engine_destroy(e);
        return ok;
    }

    static double integratedAbsDiff(const std::vector<double>& a,
                                    const std::vector<double>& b) {
        double s = 0.0;
        const std::size_t n = std::min(a.size(), b.size());
        for (std::size_t i = 0; i < n; ++i) s += std::fabs(a[i] - b[i]);
        return s;
    }
    /// Worst per-link, per-step disagreement between two recordings.
    static double worstAbsDiff(const std::vector<std::vector<double>>& a,
                               const std::vector<std::vector<double>>& b) {
        double worst = 0.0;
        for (std::size_t l = 0; l < std::min(a.size(), b.size()); ++l)
            for (std::size_t t = 0; t < std::min(a[l].size(), b[l].size()); ++t)
                worst = std::max(worst, std::fabs(a[l][t] - b[l][t]));
        return worst;
    }
    static double integrated(const std::vector<double>& a) {
        double s = 0.0;
        for (const double x : a) s += x;
        return s;
    }
};

// Link order matches [CONDUITS] declaration order: C1..C5 = 0..4.
constexpr int kC1 = 0, kC2 = 1, kC3 = 2, kC5 = 4;

TEST_F(ArdDispersionEngineTest, GlobalValueDispersionIsObservableAndBounded) {
    write_chain_deck("_e3_base.inp", "");
    write_chain_deck(
        "_e3_disp.inp",
        "org.hydrocouple.openswmm.transport.ard config=\"_e3_disp.ard\"");
    write_ard("_e3_disp.ard", "[TRANSPORT_OPTIONS]\nDISPERSION 100\n");

    std::vector<std::vector<double>> base, disp;
    ASSERT_TRUE(run_recording("_e3_base.inp", "_e3_base.rpt", "_e3_base.out",
                              base));
    ASSERT_TRUE(run_recording("_e3_disp.inp", "_e3_disp.rpt", "_e3_disp.out",
                              disp));
    ASSERT_EQ(base.size(), disp.size());

    // Dispersion must observably reshape the receding front at the LAST
    // conduit (integrated-|diff| ≥ 1% of the base signal — with D = 100
    // ft²/s over a 2500-ft chain the true separation is O(1); the loose
    // floor exists to be tightened from the validator's measured value).
    const double sep  = integratedAbsDiff(base[kC5], disp[kC5]);
    const double norm = integrated(base[kC5]);
    ASSERT_GT(norm, 0.0) << "base run never carried TSS to C5 — deck defect";
    EXPECT_GT(sep, 0.01 * norm)
        << "dispersion left the last conduit's trajectory unchanged — the "
           "engine plumbing (updateDispersion / substep dispersionSolve) is "
           "dead.";

    // Max principle end to end: dispersion mixes, never manufactures.
    for (const auto& row : disp)
        for (const double c : row)
            EXPECT_LE(c, 10.0 * (1.0 + 1.0e-6));
}

TEST_F(ArdDispersionEngineTest, SiDeckMatchesItsUsEquivalent) {
    // What this gate claims is that DISPERSION in m²/s means the same
    // physics as the equivalent number in ft²/s. So it asserts exactly
    // that, as an INVARIANCE between two descriptions of one system,
    // rather than as "the trajectory moved by at least X".
    //
    // The magnitude form is what this gate used to be, and it rotted: the
    // 1%-of-base-signal floor had been calibrated while the ARD node store
    // was diverging on the CMS deck (base signal 856626; 2224.8 once the
    // store was fixed). The bar was 1% of a blown-up number, so repairing
    // an unrelated defect "broke" the gate and invited someone to re-pick
    // the constant from whatever the code happened to produce next.
    //
    // Every bound below is instead derived from THIS run: the acceptance
    // bar is the deck pair's own unit-transcription round-off, measured
    // with dispersion off, and the discrimination bar is a multiple of the
    // same quantity. There is no number here taken from observed output.
    const char* kUsArd = "org.hydrocouple.openswmm.transport.ard "
                         "config=\"_e3_us_eq.ard\"";
    const char* kSiArd = "org.hydrocouple.openswmm.transport.ard "
                         "config=\"_e3_si_eq.ard\"";

    // 1. The floor: the same model in two unit systems, dispersion OFF.
    //    Whatever these two disagree by is the cost of transcribing the
    //    deck, and no dispersion assertion can be sharper than it.
    write_chain_deck("_e3_us_nd.inp", "");
    write_chain_deck_si("_e3_si_nd.inp", "");
    std::vector<std::vector<double>> us_nd, si_nd;
    ASSERT_TRUE(run_recording("_e3_us_nd.inp", "_e3_us_nd.rpt",
                              "_e3_us_nd.out", us_nd));
    ASSERT_TRUE(run_recording("_e3_si_nd.inp", "_e3_si_nd.rpt",
                              "_e3_si_nd.out", si_nd));
    const double floor_diff = worstAbsDiff(us_nd, si_nd);
    ASSERT_GT(floor_diff, 0.0)
        << "the two unit systems agreed EXACTLY with dispersion off — the "
           "SI deck is probably not being read as SI at all, which would "
           "make every comparison below vacuous";

    // 2. 100 ft²/s and 9.290304 m²/s are the same coefficient
    //    (1 ft² = 0.09290304 m² exactly) on decks that are the same model,
    //    so turning dispersion on must not widen the gap.
    write_chain_deck("_e3_us_eq.inp", kUsArd);
    write_ard("_e3_us_eq.ard", "[TRANSPORT_OPTIONS]\nDISPERSION 100\n");
    write_chain_deck_si("_e3_si_eq.inp", kSiArd);
    write_ard("_e3_si_eq.ard", "[TRANSPORT_OPTIONS]\nDISPERSION 9.290304\n");
    std::vector<std::vector<double>> us_d, si_d;
    ASSERT_TRUE(run_recording("_e3_us_eq.inp", "_e3_us_eq.rpt",
                              "_e3_us_eq.out", us_d));
    ASSERT_TRUE(run_recording("_e3_si_eq.inp", "_e3_si_eq.rpt",
                              "_e3_si_eq.out", si_d));
    const double disp_diff = worstAbsDiff(us_d, si_d);
    EXPECT_LT(disp_diff, 1.5 * floor_diff)
        << "dispersion added " << disp_diff / floor_diff
        << "x the deck pair's transcription round-off (" << floor_diff
        << " mg/L) — DISPERSION is not converting as ft²/s per m²/s";

    // 3. And the pair must be ABLE to see a bad conversion: feed the SI
    //    side the unconverted number — what the engine would use if the
    //    ucf² division were dropped — and require it to miss by far more
    //    than the floor. Without this, a deck on which dispersion happened
    //    to do nothing would satisfy step 2 for the wrong reason.
    write_ard("_e3_si_eq.ard", "[TRANSPORT_OPTIONS]\nDISPERSION 100\n");
    std::vector<std::vector<double>> si_wrong;
    ASSERT_TRUE(run_recording("_e3_si_eq.inp", "_e3_si_eq.rpt",
                              "_e3_si_eq.out", si_wrong));
    const double wrong_diff = worstAbsDiff(us_d, si_wrong);
    EXPECT_GT(wrong_diff, 10.0 * floor_diff)
        << "a deliberately mis-converted DISPERSION landed within "
        << wrong_diff / floor_diff << "x the transcription floor of the "
           "correct run — this deck pair cannot observe the conversion, so "
           "the assertion above proves nothing";
}

TEST_F(ArdDispersionEngineTest, PerConduitOverrideStaysInItsConduit) {
    write_chain_deck("_e3_ov_base.inp", "");
    write_chain_deck(
        "_e3_ov.inp",
        "org.hydrocouple.openswmm.transport.ard config=\"_e3_ov.ard\"");
    write_ard("_e3_ov.ard",
              "[TRANSPORT_OPTIONS]\nDISPERSION OFF\n"
              "[CONDUIT_DISPERSION]\nC3 200\n");

    std::vector<std::vector<double>> base, ov;
    ASSERT_TRUE(run_recording("_e3_ov_base.inp", "_e3_ov_base.rpt",
                              "_e3_ov_base.out", base));
    ASSERT_TRUE(
        run_recording("_e3_ov.inp", "_e3_ov.rpt", "_e3_ov.out", ov));
    ASSERT_EQ(base[kC1].size(), ov[kC1].size());

    // Upstream of the override nothing may change — C1/C2 sit in their own
    // chains, no reverse flow reaches them, and zero-D rows are identity
    // rows, so the claim is BITWISE. Any leak of the override out of C3's
    // cells (index arithmetic, chain mapping) fails here first.
    for (std::size_t t = 0; t < base[kC1].size(); ++t) {
        EXPECT_EQ(base[kC1][t], ov[kC1][t]) << "C1 diverged at step " << t;
        EXPECT_EQ(base[kC2][t], ov[kC2][t]) << "C2 diverged at step " << t;
    }

    // C3 ITSELF must change: the downstream check alone cannot tell an
    // override that landed on C3 from one that landed on C4, since both
    // reshape C5. Measured 3.7% of C3's own signal.
    const double sep3  = integratedAbsDiff(base[kC3], ov[kC3]);
    const double norm3 = integrated(base[kC3]);
    ASSERT_GT(norm3, 0.0);
    EXPECT_GT(sep3, 0.01 * norm3)
        << "the overridden conduit itself did not disperse — the override "
           "landed somewhere other than C3.";

    // Downstream of C3 the smeared front must be visible.
    const double sep  = integratedAbsDiff(base[kC5], ov[kC5]);
    const double norm = integrated(base[kC5]);
    ASSERT_GT(norm, 0.0);
    EXPECT_GT(sep, 0.005 * norm)
        << "an override of 200 ft²/s in C3 produced no downstream effect — "
           "the link → mesh-conduit override mapping is broken.";
}

TEST_F(ArdDispersionEngineTest, FischerModeIsActiveFiniteAndBounded) {
    // Sloped chain with C3 dead-flat: the slope floor must keep U* > 0
    // (finite D) rather than dividing by zero.
    write_chain_deck("_e3_fi_base.inp", "", "", "CFS", /*flat_c3=*/true);
    write_chain_deck(
        "_e3_fi.inp",
        "org.hydrocouple.openswmm.transport.ard config=\"_e3_fi.ard\"", "",
        "CFS", /*flat_c3=*/true);
    write_ard("_e3_fi.ard", "[TRANSPORT_OPTIONS]\nDISPERSION FISCHER\n");

    std::vector<std::vector<double>> base, fi;
    ASSERT_TRUE(run_recording("_e3_fi_base.inp", "_e3_fi_base.rpt",
                              "_e3_fi_base.out", base));
    ASSERT_TRUE(
        run_recording("_e3_fi.inp", "_e3_fi.rpt", "_e3_fi.out", fi));

    for (const auto& row : fi)
        for (const double c : row) {
            ASSERT_TRUE(std::isfinite(c))
                << "FISCHER produced a non-finite concentration (slope "
                   "floor / depth guard failure)";
            EXPECT_LE(c, 10.0 * (1.0 + 1.0e-6));
            EXPECT_GE(c, -1.0e-9);
        }

    // Activation: the flowing, sloped chain must produce a measurable —
    // if modest — Fischer coefficient (v ≈ several ft/s here gives
    // D = O(1) ft²/s). Threshold deliberately tiny: it detects a dead
    // FISCHER branch, not a magnitude claim.
    const double sep  = integratedAbsDiff(base[kC5], fi[kC5]);
    const double norm = integrated(base[kC5]);
    ASSERT_GT(norm, 0.0);
    EXPECT_GT(sep, 1.0e-6 * norm)
        << "FISCHER mode produced zero effect on a flowing sloped chain — "
           "the auto-computation branch never ran.";
}

TEST_F(ArdDispersionEngineTest, ConfigErrorsArePreciseAndNeverHalfApply) {
    struct Case {
        const char* name;
        const char* body;
        const char* needle;
    };
    const Case cases[] = {
        // E5a implements SCALAR_SCHEME/LIMITER and [TRANSPORT_BOUNDARIES],
        // so the two E5-deferral cases that used to live here are gone.
        // Retargeted rather than deleted: both still need to be ERROR cases
        // with a good row parsed BEFORE them, which is what gives the
        // never-half-apply assertions below their teeth. Semantic coverage
        // of the new sections lives in test_ard_transport_bcs.cpp.
        {"_e3_err_scheme",
         "[TRANSPORT_OPTIONS]\nDISPERSION 5\nSCALAR_SCHEME BOGUS\n",
         "is not UPWIND, MUSCL, or QUICKEST_ULTIMATE"},
        {"_e3_err_bc",
         "[TRANSPORT_OPTIONS]\nDISPERSION 5\n[TRANSPORT_BOUNDARIES]\nOUT "
         "TSS VALUE\n",
         "[TRANSPORT_BOUNDARIES] expects"},
        {"_e3_err_mix",
         "[STORAGE_MIXING]\nS1 FIFO\n",
         "plan phase E2b"},
        {"_e3_err_name",
         "[CONDUIT_DISPERSION]\nNOSUCH 5\n",
         "unknown conduit 'NOSUCH'"},
        {"_e3_err_neg",
         "[CONDUIT_DISPERSION]\nC3 -1\n",
         "non-negative"},
        {"_e3_err_dup",
         "[CONDUIT_DISPERSION]\nC3 1\nC3 2\n",
         "duplicate row for conduit 'C3'"},
    };
    for (const auto& c : cases) {
        const std::string inp = std::string(c.name) + ".inp";
        const std::string ard = std::string(c.name) + ".ard";
        write_chain_deck(inp.c_str(),
                         "org.hydrocouple.openswmm.transport.ard config=\"" +
                             ard + "\"");
        write_ard(ard.c_str(), c.body);

        SWMM_Engine e = swmm_engine_create();
        ASSERT_NE(e, nullptr);
        const int rc = swmm_engine_open(
            e, inp.c_str(), (std::string(c.name) + ".rpt").c_str(),
            (std::string(c.name) + ".out").c_str(), nullptr);
        EXPECT_NE(rc, SWMM_OK) << c.name << ": open should fail";
        auto& ctx = as_cpp_engine(e).context();
        bool found = false;
        for (const auto& err : ctx.errors)
            if (err.find(c.needle) != std::string::npos) found = true;
        EXPECT_TRUE(found) << c.name << ": no error contains '" << c.needle
                           << "'";
        // Never half-applied. `configured` alone cannot observe this:
        // applyArdSections resets wholesale on ENTRY too, and the error
        // path returns before setting it, so the flag is false either way
        // (falsifier vii left every gate green until these two lines were
        // added). The reset-on-error's actual job is discarding rows that
        // parsed BEFORE the bad one — _e3_err_scheme's `DISPERSION 5` and
        // _e3_err_dup's first C3 row. Under a lenient (editor) open the
        // engine survives with the errors recorded, so a half-parsed
        // ard_config would be readable by the caller.
        EXPECT_FALSE(ctx.ard_config.configured) << c.name;
        EXPECT_EQ(ctx.ard_config.dispersion_mode,
                  openswmm::ArdDispersionMode::OFF)
            << c.name << ": a row parsed before the error survived";
        EXPECT_TRUE(ctx.ard_config.conduit_disp_link.empty())
            << c.name << ": a conduit row parsed before the error survived";
        swmm_engine_destroy(e);
    }
}

TEST_F(ArdDispersionEngineTest, BypassConfigurationsWarnLoudly) {
    // R4 lesson 5: enumerate the configurations in which the parsed
    // dispersion reaches nothing and gate the warning for each.
    {   // LEGACY solver
        write_chain_deck("_e3_leg.inp",
                         "org.hydrocouple.openswmm.transport.ard "
                         "config=\"_e3_leg.ard\"",
                         "QUALITY_SOLVER LEGACY\n");
        write_ard("_e3_leg.ard", "[TRANSPORT_OPTIONS]\nDISPERSION 5\n");
        SWMM_Engine e = swmm_engine_create();
        ASSERT_NE(e, nullptr);
        ASSERT_EQ(swmm_engine_open(e, "_e3_leg.inp", "_e3_leg.rpt",
                                   "_e3_leg.out", nullptr),
                  SWMM_OK);
        auto& ctx = as_cpp_engine(e).context();
        bool found = false;
        for (const auto& w : ctx.warnings)
            if (w.find("QUALITY_SOLVER is not EULERIAN_ARD") !=
                std::string::npos)
                found = true;
        EXPECT_TRUE(found)
            << "LEGACY + transport.ard dispersion ran without a warning";
        swmm_engine_destroy(e);
    }
    {   // IGNORE_QUALITY
        write_chain_deck("_e3_iq.inp",
                         "org.hydrocouple.openswmm.transport.ard "
                         "config=\"_e3_iq.ard\"",
                         "IGNORE_QUALITY YES\n");
        write_ard("_e3_iq.ard", "[TRANSPORT_OPTIONS]\nDISPERSION 5\n");
        SWMM_Engine e = swmm_engine_create();
        ASSERT_NE(e, nullptr);
        ASSERT_EQ(swmm_engine_open(e, "_e3_iq.inp", "_e3_iq.rpt",
                                   "_e3_iq.out", nullptr),
                  SWMM_OK);
        auto& ctx = as_cpp_engine(e).context();
        bool found = false;
        for (const auto& w : ctx.warnings)
            if (w.find("IGNORE_QUALITY") != std::string::npos &&
                w.find("transport.ard") != std::string::npos)
                found = true;
        EXPECT_TRUE(found)
            << "IGNORE_QUALITY + transport.ard dispersion ran without a "
               "warning";
        swmm_engine_destroy(e);
    }
}

// ---------------------------------------------------------------------------
// Gate 11 — the reverse bypass: dispersion spelled the FV way. The component
// warnings above only fire for models that HAVE a model.ard; a user who sets
// [OPTIONS] FV_DISPERSION and selects EULERIAN_ARD has no component at all,
// and measured zero effect with zero warnings before this gate existed.
// ---------------------------------------------------------------------------
TEST_F(ArdDispersionEngineTest, FvDispersionKeyUnderArdIsAnnounced) {
    write_chain_deck("_e3_fvd.inp", "", "FV_DISPERSION 100\n");
    std::vector<std::vector<double>> traj;
    std::vector<std::string> warnings;
    ASSERT_TRUE(run_recording("_e3_fvd.inp", "_e3_fvd.rpt", "_e3_fvd.out",
                              traj, &warnings));
    bool found = false;
    for (const auto& w : warnings)
        if (w.find("FV_DISPERSION") != std::string::npos &&
            w.find("transport.ard") != std::string::npos)
            found = true;
    EXPECT_TRUE(found)
        << "FV_DISPERSION under EULERIAN_ARD applied nothing and said nothing";
}

}  // namespace
