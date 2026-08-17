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
 * @file test_ard_transport_bcs.cpp
 * @brief E5a gates: [TRANSPORT_BOUNDARIES] / [TRANSPORT_SOURCES] + the full
 *        [TRANSPORT_OPTIONS] key set
 *        (plans/transport/EULERIAN_ARD_TRANSPORT_PLAN.md §3.3/§4/§6 E5).
 *
 * @details Observation paths (coverage-geometry discipline):
 *          - BoundaryValueFeedsInflow: inverse of E4's dilution razor — an
 *            inert MSX species seeded at 0 with an inlet BC of 8 must RISE
 *            at the last conduit as the boundary front advects down; a dead
 *            BC leaves the whole chain at 0 forever.
 *          - BoundaryTimeseriesFollows: a step timeseries (8 until t=20 min,
 *            0 after) must produce a rise-then-fall at the last conduit —
 *            falsified by a ts evaluation that returns constants or zeros.
 *          - SourceSteadyStateDeltaIsAnalytic: at steady flow Q, a
 *            distributed source of rate r (species mass/s) raises the
 *            downstream concentration by EXACTLY Δc = r/(kLitersPerFt3·Q) —
 *            the units gate: dropping the L/ft³ conversion misses by 28×.
 *          - PollutantRowsAreRefused: pollutants keep their legacy loading
 *            surface; a TSS boundary or source is a precise error naming
 *            [INFLOWS].
 *          - UnknownNamesAreFatal: node/species/timeseries typos fail the
 *            open with the row identified.
 *          - OrderIndependenceOfComponents: transport.ard listed BEFORE the
 *            reactions component that declares the species — resolution
 *            happens post-apply, so this must open cleanly; resolving at
 *            apply time fails exactly here.
 *          - SchemeKeysConfigureTheEngine: [TRANSPORT_OPTIONS]
 *            SCALAR_SCHEME UPWIND must change the receding-front trajectory
 *            vs the MUSCL default (UPWIND is measurably more diffusive) and
 *            must no longer be a deferral error; TARGET_DX opens since E5b
 *            (its positive coverage lives in the E5b suite).
 *          - LegacyBypassWarnsForBoundaries: boundaries-only model.ard
 *            under QUALITY_SOLVER LEGACY warns (the E3/R4 bypass-warning
 *            surface extended beyond dispersion).
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

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

constexpr double kLperFt3 = 28.316846592;
constexpr double kBc      = 8.0;    ///< boundary concentration
constexpr double kQ       = 5.0;    ///< steady inflow, cfs

/// Flowing five-conduit chain (wet junctions — lesson 9), steady 5 cfs
/// inflow, optional [POLLUTANTS], optional [TIMESERIES] block, and a
/// [PROCESS_COMPONENTS] body given verbatim (ORDER of component lines is a
/// gate axis here).
void write_deck(const char* path, const std::string& pc_lines,
                const std::string& extra_options = "",
                bool pollutants = true, const std::string& timeseries = "") {
    std::ofstream f(path);
    f << "[TITLE]\nE5a transport BC/source gate deck\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\n"
      << "QUALITY_SOLVER EULERIAN_ARD\n"
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME 01:00:00\n"
      << "ROUTING_STEP 5\nREPORT_STEP 00:01:00\n"
      << extra_options << "\n"
      << "[JUNCTIONS]\n"
      << "J0 10.0 10 1.5 0 0\nJ1 9.4  10 1.5 0 0\nJ2 8.8  10 1.5 0 0\n"
      << "J3 8.2  10 1.5 0 0\nJ4 7.6  10 1.5 0 0\n\n"
      << "[OUTFALLS]\nOUT 7.0 FREE  NO\n\n"
      << "[CONDUITS]\n"
      << "C1 J0 J1 500 0.013 0 0 0\nC2 J1 J2 500 0.013 0 0 0\n"
      << "C3 J2 J3 500 0.013 0 0 0\nC4 J3 J4 500 0.013 0 0 0\n"
      << "C5 J4 OUT 500 0.013 0 0 0\n\n"
      << "[XSECTIONS]\n"
      << "C1 CIRCULAR 2.0 0 0 0\nC2 CIRCULAR 2.0 0 0 0\n"
      << "C3 CIRCULAR 2.0 0 0 0\nC4 CIRCULAR 2.0 0 0 0\n"
      << "C5 CIRCULAR 2.0 0 0 0\n\n"
      << "[INFLOWS]\nJ0 FLOW \"\" FLOW 1.0 1.0 " << kQ << "\n\n";
    if (pollutants)
        f << "[POLLUTANTS]\n"
          << ";;Name Units Crain Cgw Crdii Kdecay SnowOnly CoPollut CoFrac "
             "Cdwf Cinit\n"
          << "TSS    MG/L  0     0   0     0      NO       *        0      "
             "0    10\n\n";
    if (!timeseries.empty()) f << "[TIMESERIES]\n" << timeseries << "\n";
    if (!pc_lines.empty())
        f << "[PROCESS_COMPONENTS]\n" << pc_lines << "\n\n";
    f << "[REPORT]\nINPUT NO\n";
}

void write_file(const char* path, const std::string& body) {
    std::ofstream c(path);
    c << body;
}

const char* kInertRxn =
    "[REACTION_OPTIONS]\nRATE_UNITS SEC\n"
    "[REACTION_SPECIES]\nBULK X MG\n"
    "[REACTION_PIPES]\nRATE X 0\n"
    "[REACTION_TANKS]\nRATE X 0\n";   // GLOBAL init defaults to 0

std::string pc_two(const char* rxn, const char* ard, bool ard_first) {
    const std::string a =
        std::string("org.hydrocouple.openswmm.transport.ard config=\"") +
        ard + "\"";
    const std::string r =
        std::string("org.hydrocouple.openswmm.reactions config=\"") + rxn +
        "\"";
    return ard_first ? a + "\n" + r : r + "\n" + a;
}

/// Per-step trajectories: first-pollutant link conc + first-MSX link conc.
struct RunRecord {
    std::vector<std::vector<double>> tss_link;  ///< [link][step]
    std::vector<std::vector<double>> msx_link;  ///< [link][step] species 0
    std::vector<std::string> warnings;
    bool ok = false;
};

RunRecord run_recording(const char* inp, const char* rpt, const char* out) {
    RunRecord rec;
    SWMM_Engine e = swmm_engine_create();
    if (e == nullptr) { ADD_FAILURE() << "engine create"; return rec; }
    bool ok = swmm_engine_open(e, inp, rpt, out, nullptr) == SWMM_OK;
    if (!ok) ADD_FAILURE() << "open failed for " << inp;
    if (ok && (swmm_engine_initialize(e) != SWMM_OK ||
               swmm_engine_start(e, 1) != SWMM_OK)) {
        ADD_FAILURE() << "init/start failed for " << inp;
        ok = false;
    }
    if (ok) {
        auto& ctx = as_cpp_engine(e).context();
        const int nl = ctx.n_links();
        const int np = ctx.n_pollutants();
        const int nm = ctx.reactions.n_species();
        rec.tss_link.assign(static_cast<std::size_t>(nl), {});
        rec.msx_link.assign(static_cast<std::size_t>(nl), {});
        double elapsed = 0.0;
        int guard = 0;
        do {
            if (swmm_engine_step(e, &elapsed) != SWMM_OK) {
                ADD_FAILURE() << "step failed for " << inp;
                ok = false;
                break;
            }
            for (int l = 0; l < nl; ++l) {
                if (np > 0)
                    rec.tss_link[static_cast<std::size_t>(l)].push_back(
                        ctx.links.conc[static_cast<std::size_t>(l * np)]);
                if (nm > 0) {
                    const auto idx = static_cast<std::size_t>(l) *
                                     static_cast<std::size_t>(nm);
                    rec.msx_link[static_cast<std::size_t>(l)].push_back(
                        (idx < ctx.reactions.msx_link_conc.size())
                            ? ctx.reactions.msx_link_conc[idx]
                            : -1.0);
                }
            }
        } while (elapsed > 0.0 && ++guard < 20000);
        if (ok) swmm_engine_end(e);
        rec.warnings = ctx.warnings;
    }
    swmm_engine_destroy(e);
    rec.ok = ok;
    return rec;
}

bool has_needle(const std::vector<std::string>& v, const std::string& n) {
    for (const auto& s : v)
        if (s.find(n) != std::string::npos) return true;
    return false;
}

constexpr int kC1 = 0, kC5 = 4;

// ---------------------------------------------------------------------------
// Gate 1 — a VALUE boundary feeds the external inflow (rising front).
// ---------------------------------------------------------------------------
TEST(ArdTransportBcsTest, BoundaryValueFeedsInflow) {
    write_file("_e5_bc.rxn", kInertRxn);
    write_file("_e5_bc.ard",
               "[TRANSPORT_BOUNDARIES]\nJ0 X VALUE 8\n");
    write_deck("_e5_bc.inp", pc_two("_e5_bc.rxn", "_e5_bc.ard", false));
    const auto rec = run_recording("_e5_bc.inp", "_e5_bc.rpt", "_e5_bc.out");
    ASSERT_TRUE(rec.ok);
    const auto& c5 = rec.msx_link[kC5];
    ASSERT_FALSE(c5.empty());
    EXPECT_LT(c5.front(), 0.05 * kBc)
        << "the chain should start clean (GLOBAL init 0)";
    EXPECT_GT(c5.back(), 0.8 * kBc)
        << "the boundary front never reached the last conduit — the BC "
           "mass injection is dead.";
    // Max principle: the BC is the only source, so nothing exceeds it.
    for (const auto& row : rec.msx_link)
        for (const double v : row) EXPECT_LE(v, kBc * (1.0 + 1.0e-6));
}

// ---------------------------------------------------------------------------
// Gate 2 — a TIMESERIES boundary follows its series (rise then fall).
// ---------------------------------------------------------------------------
TEST(ArdTransportBcsTest, BoundaryTimeseriesFollows) {
    write_file("_e5_ts.rxn", kInertRxn);
    write_file("_e5_ts.ard",
               "[TRANSPORT_BOUNDARIES]\nJ0 X TIMESERIES BCTS\n");
    write_deck("_e5_ts.inp", pc_two("_e5_ts.rxn", "_e5_ts.ard", false), "",
               true,
               "BCTS 0:00 8\nBCTS 0:19 8\nBCTS 0:21 0\nBCTS 2:00 0\n");
    const auto rec = run_recording("_e5_ts.inp", "_e5_ts.rpt", "_e5_ts.out");
    ASSERT_TRUE(rec.ok);
    const auto& c5 = rec.msx_link[kC5];
    ASSERT_FALSE(c5.empty());
    const double peak = *std::max_element(c5.begin(), c5.end());
    EXPECT_GT(peak, 0.5 * kBc)
        << "the 20-minute boundary pulse never showed at the last conduit";
    EXPECT_LT(c5.back(), 0.5 * peak)
        << "the boundary did not fall when its timeseries did — the ts "
           "evaluation is stuck on a constant.";
}

// ---------------------------------------------------------------------------
// Gate 3 — distributed-source steady state is analytic: Δc = r/(L/ft³·Q).
// ---------------------------------------------------------------------------
TEST(ArdTransportBcsTest, SourceSteadyStateDeltaIsAnalytic) {
    // r chosen so Δc = 2 mg/L at Q = 5 cfs: r = 2·5·28.3168 = 283.168 mg/s.
    const double r_mg_s = 2.0 * kQ * kLperFt3;
    write_file("_e5_src.rxn", kInertRxn);
    write_file("_e5_src.ard",
               "[TRANSPORT_SOURCES]\nC3 X VALUE " + std::to_string(r_mg_s) +
                   "\n");
    write_deck("_e5_src.inp", pc_two("_e5_src.rxn", "_e5_src.ard", false));
    const auto rec =
        run_recording("_e5_src.inp", "_e5_src.rpt", "_e5_src.out");
    ASSERT_TRUE(rec.ok);
    const auto& c5 = rec.msx_link[kC5];
    const auto& c1 = rec.msx_link[kC1];
    ASSERT_FALSE(c5.empty());
    // At steady state everything upstream of C3 stays clean and everything
    // downstream carries r/(conv·Q) — the ONLY quantity the units chain
    // (mg/s → conc·ft³/s → mg/L at Q ft³/s) can produce. A dropped
    // conversion misses by 28×; a mis-scaled length by the cell count.
    const double expected = r_mg_s / (kLperFt3 * kQ);   // = 2.0 mg/L
    EXPECT_NEAR(c5.back(), expected, 0.15 * expected)
        << "steady-state downstream concentration is not r/(L/ft³·Q)";
    EXPECT_LT(c1.back(), 0.05 * expected)
        << "a C3 source contaminated C1, upstream of it";
}

// ---------------------------------------------------------------------------
// Gate 4 — pollutant rows are refused with the legacy-pathway message.
// ---------------------------------------------------------------------------
TEST(ArdTransportBcsTest, PollutantRowsAreRefused) {
    for (const bool src : {false, true}) {
        const char* tag = src ? "_e5_prf_s" : "_e5_prf_b";
        write_file((std::string(tag) + ".rxn").c_str(), kInertRxn);
        write_file((std::string(tag) + ".ard").c_str(),
                   src ? "[TRANSPORT_SOURCES]\nC3 TSS VALUE 1\n"
                       : "[TRANSPORT_BOUNDARIES]\nJ0 TSS VALUE 1\n");
        write_deck((std::string(tag) + ".inp").c_str(),
                   pc_two((std::string(tag) + ".rxn").c_str(),
                          (std::string(tag) + ".ard").c_str(), false));
        SWMM_Engine e = swmm_engine_create();
        ASSERT_NE(e, nullptr);
        EXPECT_NE(swmm_engine_open(e, (std::string(tag) + ".inp").c_str(),
                                   (std::string(tag) + ".rpt").c_str(),
                                   (std::string(tag) + ".out").c_str(),
                                   nullptr),
                  SWMM_OK)
            << tag;
        auto& ctx = as_cpp_engine(e).context();
        EXPECT_TRUE(has_needle(ctx.errors, "is a pollutant")) << tag;
        EXPECT_TRUE(has_needle(ctx.errors, "[INFLOWS]")) << tag;
        swmm_engine_destroy(e);
    }
}

// ---------------------------------------------------------------------------
// Gate 5 — unknown node / species / timeseries are fatal and precise.
// ---------------------------------------------------------------------------
TEST(ArdTransportBcsTest, UnknownNamesAreFatal) {
    struct Case { const char* tag; const char* ard; const char* needle; };
    const Case cases[] = {
        {"_e5_un_n", "[TRANSPORT_BOUNDARIES]\nNOPE X VALUE 8\n",
         "unknown node 'NOPE'"},
        {"_e5_un_s", "[TRANSPORT_BOUNDARIES]\nJ0 NOPE VALUE 8\n",
         "unknown species 'NOPE'"},
        {"_e5_un_t", "[TRANSPORT_BOUNDARIES]\nJ0 X TIMESERIES NOPE\n",
         "unknown timeseries 'NOPE'"},
    };
    for (const auto& c : cases) {
        write_file((std::string(c.tag) + ".rxn").c_str(), kInertRxn);
        write_file((std::string(c.tag) + ".ard").c_str(), c.ard);
        write_deck((std::string(c.tag) + ".inp").c_str(),
                   pc_two((std::string(c.tag) + ".rxn").c_str(),
                          (std::string(c.tag) + ".ard").c_str(), false));
        SWMM_Engine e = swmm_engine_create();
        ASSERT_NE(e, nullptr);
        EXPECT_NE(swmm_engine_open(e, (std::string(c.tag) + ".inp").c_str(),
                                   (std::string(c.tag) + ".rpt").c_str(),
                                   (std::string(c.tag) + ".out").c_str(),
                                   nullptr),
                  SWMM_OK)
            << c.tag;
        auto& ctx = as_cpp_engine(e).context();
        EXPECT_TRUE(has_needle(ctx.errors, c.needle)) << c.tag;
        swmm_engine_destroy(e);
    }
}

// ---------------------------------------------------------------------------
// Gate 6 — component ORDER does not matter (post-apply resolution).
// ---------------------------------------------------------------------------
TEST(ArdTransportBcsTest, OrderIndependenceOfComponents) {
    // transport.ard FIRST: its boundary row names species X BEFORE the
    // reactions component that declares it has applied. Resolution happens
    // after all components, so this must open and run identically to the
    // reactions-first ordering.
    write_file("_e5_ord.rxn", kInertRxn);
    write_file("_e5_ord.ard", "[TRANSPORT_BOUNDARIES]\nJ0 X VALUE 8\n");
    write_deck("_e5_ord.inp", pc_two("_e5_ord.rxn", "_e5_ord.ard",
                                     /*ard_first=*/true));
    const auto rec =
        run_recording("_e5_ord.inp", "_e5_ord.rpt", "_e5_ord.out");
    ASSERT_TRUE(rec.ok)
        << "listing transport.ard before the reactions component must not "
           "fail — species resolution is post-apply by design.";
    ASSERT_FALSE(rec.msx_link[kC5].empty());
    EXPECT_GT(rec.msx_link[kC5].back(), 0.8 * kBc);
}

// ---------------------------------------------------------------------------
// Gate 7 — SCALAR_SCHEME/LIMITER keys configure the engine; TARGET_DX defers.
// ---------------------------------------------------------------------------
TEST(ArdTransportBcsTest, SchemeKeysConfigureTheEngine) {
    // UPWIND vs the MUSCL default on the receding TSS front: first-order
    // upwinding diffuses the front measurably, so the C5 trajectory must
    // differ. LIMITER SUPERBEE must parse (no deferral error).
    // ONE key per comparison. The delivered gate set SCALAR_SCHEME UPWIND
    // and LIMITER SUPERBEE on the SAME deck, so either write alone produced
    // the separation and neither had an observer of its own — removing the
    // SCALAR_SCHEME write left the gate green (falsifier vii) because
    // SUPERBEE was still reaching the reconstruction.
    write_deck("_e5_mu.inp", "");
    const auto mu = run_recording("_e5_mu.inp", "_e5_mu.rpt", "_e5_mu.out");
    ASSERT_TRUE(mu.ok);

    const auto separation = [&](const RunRecord& r) {
        double sep = 0.0, norm = 0.0;
        const std::size_t n =
            std::min(r.tss_link[kC5].size(), mu.tss_link[kC5].size());
        for (std::size_t t = 0; t < n; ++t) {
            sep  += std::fabs(r.tss_link[kC5][t] - mu.tss_link[kC5][t]);
            norm += mu.tss_link[kC5][t];
        }
        return (norm > 0.0) ? sep / norm : 0.0;
    };

    write_file("_e5_up.ard",
               "[TRANSPORT_OPTIONS]\nSCALAR_SCHEME UPWIND\nDISPERSION OFF\n");
    write_deck("_e5_up.inp",
               "org.hydrocouple.openswmm.transport.ard config=\"_e5_up.ard\"");
    const auto up = run_recording("_e5_up.inp", "_e5_up.rpt", "_e5_up.out");
    ASSERT_TRUE(up.ok);
    EXPECT_GT(separation(up), 0.005)
        << "SCALAR_SCHEME UPWIND left the trajectory identical to MUSCL — "
           "the model.ard key does not reach the engine.";

    write_file("_e5_lim.ard",
               "[TRANSPORT_OPTIONS]\nLIMITER SUPERBEE\nDISPERSION OFF\n");
    write_deck("_e5_lim.inp",
               "org.hydrocouple.openswmm.transport.ard config=\"_e5_lim.ard\"");
    const auto lim = run_recording("_e5_lim.inp", "_e5_lim.rpt", "_e5_lim.out");
    ASSERT_TRUE(lim.ok);
    EXPECT_GT(separation(lim), 1.0e-6)
        << "LIMITER SUPERBEE left the trajectory identical to the default — "
           "the model.ard key does not reach the engine.";

    // E5b FLIP (lesson 21, applied in the retiring changeset this time):
    // TARGET_DX is no longer a deferral — it sets the transport-mesh cell
    // length under non-FV hydraulics. Positive coverage (the mesh actually
    // coarsens/refines) lives in the E5b suite; here the former error case
    // must simply OPEN.
    write_file("_e5_dx.ard", "[TRANSPORT_OPTIONS]\nTARGET_DX 25\n");
    write_deck("_e5_dx.inp",
               "org.hydrocouple.openswmm.transport.ard config=\"_e5_dx.ard\"");
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(swmm_engine_open(e, "_e5_dx.inp", "_e5_dx.rpt", "_e5_dx.out",
                               nullptr),
              SWMM_OK)
        << "the retired TARGET_DX deferral error still fires";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 8 — boundaries-only config under LEGACY warns (bypass surface).
// ---------------------------------------------------------------------------
TEST(ArdTransportBcsTest, LegacyBypassWarnsForBoundaries) {
    write_file("_e5_leg.rxn", kInertRxn);
    write_file("_e5_leg.ard", "[TRANSPORT_BOUNDARIES]\nJ0 X VALUE 8\n");
    write_deck("_e5_leg.inp", pc_two("_e5_leg.rxn", "_e5_leg.ard", false),
               "QUALITY_SOLVER LEGACY\n");
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, "_e5_leg.inp", "_e5_leg.rpt", "_e5_leg.out",
                               nullptr),
              SWMM_OK);
    EXPECT_TRUE(has_needle(as_cpp_engine(e).context().warnings,
                           "QUALITY_SOLVER is not EULERIAN_ARD"))
        << "boundaries-only model.ard under LEGACY ran without a word";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 9 — a boundary on an MSX-ONLY model (no [POLLUTANTS]). Every other
// deck in this suite carries a pollutant row, and that is what made the
// difference invisible: the BC injects `qual_vol_in * concentration`, and
// qual_vol_in is accumulated by the QualitySolver external-load loaders,
// each of which used to bail at `np <= 0`. Measured before the fix: this
// deck delivered exactly 0.0 while the pollutant-bearing control delivered
// 8.0 — and an MSX network with an inlet boundary and no legacy pollutant
// is precisely the nh2cl shape E5a exists to unblock.
// ---------------------------------------------------------------------------
TEST(ArdTransportBcsTest, BoundaryFeedsAnMsxOnlyModel) {
    write_file("_e5_mo.rxn", kInertRxn);
    write_file("_e5_mo.ard", "[TRANSPORT_BOUNDARIES]\nJ0 X VALUE 8\n");
    write_deck("_e5_mo.inp", pc_two("_e5_mo.rxn", "_e5_mo.ard", false), "",
               /*pollutants=*/false);
    const auto rec = run_recording("_e5_mo.inp", "_e5_mo.rpt", "_e5_mo.out");
    ASSERT_TRUE(rec.ok);
    const auto& c5 = rec.msx_link[kC5];
    ASSERT_FALSE(c5.empty());
    EXPECT_LT(c5.front(), 0.05 * kBc) << "the chain should start clean";
    EXPECT_GT(c5.back(), 0.8 * kBc)
        << "the boundary delivered nothing on a model with no [POLLUTANTS] — "
           "the external-inflow VOLUME the BC rides on was never assembled.";
    for (const auto& row : rec.msx_link)
        for (const double v : row) EXPECT_LE(v, kBc * (1.0 + 1.0e-6));
}

// ---------------------------------------------------------------------------
// Gate 10 — a TIMESERIES source equals the equivalent VALUE source. The
// species-mass/s -> internal conc*ft3/s conversion lives in TWO places: at
// resolution for VALUE rows and at evaluation for TIMESERIES rows. Gate 3 is
// the analytic units gate but only exercises VALUE, so the second division
// had no observer — dropping it there would have passed the whole suite.
// ---------------------------------------------------------------------------
TEST(ArdTransportBcsTest, TimeseriesSourceMatchesValueSource) {
    write_file("_e5_sv.rxn", kInertRxn);
    write_file("_e5_sv.ard", "[TRANSPORT_SOURCES]\nC3 X VALUE 50\n");
    write_deck("_e5_sv.inp", pc_two("_e5_sv.rxn", "_e5_sv.ard", false));
    const auto val = run_recording("_e5_sv.inp", "_e5_sv.rpt", "_e5_sv.out");
    ASSERT_TRUE(val.ok);

    write_file("_e5_st.rxn", kInertRxn);
    write_file("_e5_st.ard", "[TRANSPORT_SOURCES]\nC3 X TIMESERIES SRCTS\n");
    write_deck("_e5_st.inp", pc_two("_e5_st.rxn", "_e5_st.ard", false), "",
               /*pollutants=*/true, "SRCTS 0:00 50\nSRCTS 24:00 50\n");
    const auto ts = run_recording("_e5_st.inp", "_e5_st.rpt", "_e5_st.out");
    ASSERT_TRUE(ts.ok);

    const auto& a = val.msx_link[kC5];
    const auto& b = ts.msx_link[kC5];
    ASSERT_EQ(a.size(), b.size());
    ASSERT_FALSE(a.empty());
    ASSERT_GT(a.back(), 0.0) << "the VALUE source delivered nothing — this "
                                "gate cannot compare two zeroes";
    for (std::size_t k = 0; k < a.size(); ++k)
        EXPECT_NEAR(b[k], a[k], 1.0e-9 * std::max(1.0, a[k]))
            << "step " << k
            << ": a constant TIMESERIES source disagrees with the identical "
               "VALUE source — the two mass/s -> conc*ft3/s conversions have "
               "diverged.";
}

}  // namespace
