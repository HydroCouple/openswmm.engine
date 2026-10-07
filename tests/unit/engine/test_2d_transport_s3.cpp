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
 * @file test_2d_transport_s3.cpp
 * @brief Overland transport S3 — the 1D↔2D coupling carries species
 *        (D-2DT4), both directions, through the DECK path.
 *
 * @details Every gate here runs the full engine on a coupled deck, because
 *          the coupling tuple has no meaning without the 1D side: the
 *          marcher's `exch_mass` is only half of it, and the other half is
 *          the node queue, its drain rule, and the quality loader.
 *
 *          The falsifier is the S1 uniformity property carried ACROSS the
 *          domain boundary. Water that crosses at the concentration of the
 *          domain it enters must leave that domain uniform to round-off:
 *          - 2D → 1D: a pan at c0 draining into an initially empty junction
 *            must make the junction read exactly c0 — a mass queue drained
 *            by any rule other than the volume queue's, or a mass booked at
 *            any concentration other than the cell's, reads something else.
 *          - 1D → 2D: a junction fed at c0 spilling onto a pan at c0 must
 *            leave every wet cell at c0 — a spill booked at zero (S1's
 *            placeholder), at a lagged concentration, or against the wrong
 *            volume, reads something else.
 *
 *          The cross-domain mass identity is the second observer: what the
 *          2D ledger says left (× the m³→ft³ factor) must equal what the 1D
 *          quality continuity table received plus whatever is still in the
 *          delivery queue at the final step.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_nodes.h>

#include "2d/SurfaceRouter2D.hpp"
#include "core/SWMMEngine.hpp"
#include "core/SimulationContext.hpp"

namespace {

// A 10×10 m two-triangle pan at bed elevation `z_pan`, initial depth `h0`,
// species Cu seeded at `c0` everywhere, coupled at vertex 0 to `node`.
// `one_d_head` selects the 1D side: a clean network (drain gate) or a junction
// fed at c0 (spill / outfall gates).
std::string deck(const std::string& node, double z_pan, double h0, double c0,
                 const std::string& inflows, double j1_maxdepth) {
    std::ostringstream m;
    m << "[OPTIONS]\n"
         "FLOW_UNITS           CMS\nFLOW_ROUTING         DYNWAVE\n"
         "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
         "END_DATE             01/01/2026\nEND_TIME             00:20:00\n"
         "REPORT_STEP          00:01:00\nROUTING_STEP         5\n"
         "ALLOW_PONDING        NO\n\n"
         "[POLLUTANTS]\n;;Name Units Crain Cgw Crdii Kdecay\n"
         "Cu     MG/L  0     0   0     0\n\n"
         "[JUNCTIONS]\nJ1 0.0 " << j1_maxdepth << " 0 0 0\n\n"
         "[OUTFALLS]\nO1 -0.5 FREE NO\n\n"
         "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n"
         "[XSECTIONS]\nC1 CIRCULAR 0.3 0 0 0 1\n\n"
      << inflows <<
         "[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nLTS_TIERS 1\nMAX_TIMESTEP 5\n"
         "DRY_DEPTH 0.001\nCOUPLING_CD 0.7\nREPORT_2D NO\n\n"
         "[2D_VERTICES]\n"
         " 0.0  0.0 " << z_pan << "\n10.0  0.0 " << z_pan << "\n"
         "10.0 10.0 " << z_pan << "\n 0.0 10.0 " << z_pan << "\n\n"
         "[2D_TRIANGLES]\n;;V1 V2 V3 N INIT_DEPTH\n"
         "0 1 2 0.03 " << h0 << "\n0 2 3 0.03 " << h0 << "\n\n"
         "[2D_VERTEX_NODE_MAP]\n0 " << node << " 0.7 1.0\n\n"
         "[2D_INITIAL_QUALITY]\n* Cu " << c0 << "\n\n"
         "[REPORT]\nINPUT NO\n";
    return m.str();
}

struct CoupledRun {
    SWMM_Engine e = nullptr;
    openswmm::SWMMEngine* eng = nullptr;
    bool ok = false;
};

/// Open/initialize/start and step to the end; the engine is left OPEN (not
/// ended) so the caller can read the live context, then destroyed by `close`.
CoupledRun runDeck(const std::string& tag, const std::string& body) {
    CoupledRun r;
    { std::ofstream f(tag + ".inp"); f << body; }
    r.e = swmm_engine_create();
    if (!r.e) return r;
    if (swmm_engine_open(r.e, (tag + ".inp").c_str(), (tag + ".rpt").c_str(),
                         (tag + ".out").c_str(), nullptr) != SWMM_OK ||
        swmm_engine_initialize(r.e) != SWMM_OK ||
        swmm_engine_start(r.e, 1) != SWMM_OK)
        return r;
    int active = 0;
    swmm_2d_is_active(r.e, &active);
    if (!active) return r;
    double elapsed = 0.0;
    while (swmm_engine_step(r.e, &elapsed) == SWMM_OK && elapsed > 0.0) {}
    r.eng = static_cast<openswmm::SWMMEngine*>(r.e);
    r.ok = true;
    return r;
}

void close(CoupledRun& r) {
    if (!r.e) return;
    swmm_engine_end(r.e);
    swmm_engine_close(r.e);
    swmm_engine_destroy(r.e);
    r.e = nullptr;
}

double m3_to_1d(const openswmm::twoD::SurfaceRouter2D& router) {
    return router.options().flow_2d_to_1d;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 2D → 1D drain. A pan at c0 = 4 drains through J1 (initially empty and
//    clean). J1 receives ONLY drain water, so it must read exactly c0 — the
//    mass queue and the volume queue drained by one rule, the mass booked at
//    the cell's concentration. Cross-domain identity: 2D lost_coupling × f
//    == 1D quality external inflow + mass still queued at the final step.
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS3, DrainDeliversCellConcentrationToTheNode) {
    const double c0 = 4.0;
    CoupledRun r = runDeck("_s3_drain", deck("J1", 1.0, 0.5, c0, "", 1.0));
    ASSERT_TRUE(r.ok) << "coupled deck did not run";
    const auto& ctx    = r.eng->context();
    const auto& router = r.eng->surfaceRouter2D();
    const auto& tr     = router.state().transport;
    ASSERT_EQ(tr.n_species, 1);

    const double drained = ctx.mass_balance_2d.coupling_2d_to_1d_out;
    ASSERT_GT(drained, 0.5) << "fixture produced no 2D→1D drain (m³)";

    // The junction read c0 — mass and water arrived in the cell's ratio.
    const int j1 = swmm_node_index(r.e, "J1");
    ASSERT_GE(j1, 0);
    const double c_j1 = ctx.nodes.conc[static_cast<std::size_t>(j1)];
    EXPECT_NEAR(c_j1, c0, 1.0e-9 * c0)
        << "junction fed only by the drain does not read the pan's "
           "concentration: mass and volume queues drained by different rules, "
           "or mass booked at a concentration other than the cell's";

    // 2D side: the drain is booked to lost_coupling; the S1 identity holds.
    const double surface_total = tr.totalIncludingLedgers(0);
    double init_mass = 0.0;
    for (int c = 0; c < tr.n_cells; ++c)
        init_mass += c0 * 0.5 * router.mesh().tri_area[c];
    EXPECT_NEAR(surface_total, init_mass, 1.0e-9 * init_mass);
    // NET form, matching what the ledger measures: coupling_2d_to_1d_out is
    // the per-window NET per node, and near the rim the exchange flip-flops —
    // small spill-backs return at the junction's own c0 and land in
    // gained_coupling, so gross lost exceeds c0 × net drained (measured
    // 596.66 vs 591.77 on this deck, a 0.8 % flip-flop share). The gross
    // identity lives in the queue comparison below, which counts drain-only
    // exch_mass.
    const double spilled_back = ctx.mass_balance_2d.coupling_1d_to_2d_in;
    EXPECT_NEAR(tr.lost_coupling[0] - tr.gained_coupling[0],
                c0 * (drained - spilled_back),
                1.0e-9 * c0 * drained)
        << "net coupling mass != c0 × net coupling volume";

    // Cross-domain: what 2D says left (1D mass units) is what 1D booked as
    // external quality inflow plus what the delivery queue still holds.
    const double f = m3_to_1d(router);
    double queued = 0.0;
    for (double q : ctx.nodes.coupling_qual_queue) queued += q;
    const double received = ctx.mass_balance.qual_routing_ex_in[0] + queued;
    // What the 1D quality path received must be EXACTLY c0 × the volume the
    // 2D→1D out-ledger booked. The rim exchange flip-flops across whole
    // windows (the check measured 98 m³ spilled back over a 50 m³ pond — the
    // pond recycles), and the two sides split cleanly by window sign:
    // positive-net windows put their volume in the out-ledger and their mass
    // (at the cell's c0) in the queue — this identity; negative-net windows
    // subtract volume from the SIGNED volume queue and their mass leaves the
    // node implicitly through its volume drop at the mixed concentration —
    // which is what keeps J1 reading exactly c0 (the first assert). The
    // within-window mixing residue is the pairing debit and has no ledger of
    // its own. Measured: 20898.1528 == 4 × 147.9424 × f to the digit.
    EXPECT_NEAR(received, c0 * drained * f, 1.0e-9 * c0 * drained * f)
        << "the queue's delivered mass does not match c0 × the out-ledger "
           "volume: mass and volume queues drained by different rules";
    close(r);
}

// ---------------------------------------------------------------------------
// 2. 1D → 2D junction spill. J1 is fed 0.3 m³/s at c0 (well over C1's
//    capacity), surcharges to its 1.0 m rim and spills through the coupling
//    onto a pan at z = 0.5 already holding c0 water. Fed ONLY at c0 from an
//    empty start, J1 reads c0 exactly; the spill must then leave every wet
//    cell at c0. Net 2D coupling mass == c0 × net 2D coupling volume.
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS3, SpillArrivesAtTheNodeConcentration) {
    const double c0 = 4.0;
    const std::string inflows =
        "[INFLOWS]\n"
        ";;Node Constituent TSeries Type Mfactor Sfactor Baseline\n"
        "J1 FLOW \"\" FLOW   1.0 1.0 0.3\n"
        "J1 Cu   \"\" CONCEN 1.0 1.0 4.0\n\n";
    CoupledRun r = runDeck("_s3_spill", deck("J1", 0.5, 0.2, c0, inflows, 1.0));
    ASSERT_TRUE(r.ok);
    const auto& ctx    = r.eng->context();
    const auto& router = r.eng->surfaceRouter2D();
    const auto& st     = router.state();
    const auto& tr     = st.transport;

    const double spilled = ctx.mass_balance_2d.coupling_1d_to_2d_in;
    const double drained = ctx.mass_balance_2d.coupling_2d_to_1d_out;
    ASSERT_GT(spilled, 0.5) << "fixture produced no 1D→2D spill (m³)";

    const int j1 = swmm_node_index(r.e, "J1");
    ASSERT_GE(j1, 0);
    EXPECT_NEAR(ctx.nodes.conc[static_cast<std::size_t>(j1)], c0, 1.0e-9 * c0)
        << "premise: a junction fed only at c0 reads c0";

    double worst = 0.0;
    int wet = 0;
    for (int c = 0; c < tr.n_cells; ++c) {
        const double v_dry = router.options().dry_depth * router.mesh().tri_area[c];
        if (!(st.volume[c] > v_dry)) continue;
        ++wet;
        const double conc = tr.concentration(0, c, st.volume[c], v_dry);
        worst = std::max(worst, std::fabs(conc - c0) / c0);
    }
    ASSERT_GT(wet, 0);
    EXPECT_LT(worst, 1.0e-9)
        << "spill at c0 onto a c0 pan changed a cell: the spill is not booked "
           "at nodes.conc against the volume the exchange actually moved";
    const double net_vol  = spilled - drained;
    const double net_mass = tr.gained_coupling[0] - tr.lost_coupling[0];
    EXPECT_NEAR(net_mass, c0 * net_vol, 1.0e-9 * std::fabs(c0 * net_vol) + 1e-12)
        << "net coupling mass != c0 × net coupling volume";
    close(r);
}

// ---------------------------------------------------------------------------
// 3. 1D → 2D outfall discharge. The same feed reaches O1 through C1 and O1 is
//    the coupled node, so the discharge scatters onto the pan through the
//    coupling_flux/coupling_src pair. C1 starts clean, so the outfall's
//    concentration RISES toward c0 over the run — a uniformity gate would
//    fail for the right reason. What is provable: the pan never exceeds c0
//    (cross-domain max principle), species mass arrived (> 0) and never more
//    than c0 × the discharged volume, and the 2D identity holds.
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS3, OutfallDischargeCarriesTheOutfallConcentration) {
    const double c0 = 4.0;
    const std::string inflows =
        "[INFLOWS]\n"
        "J1 FLOW \"\" FLOW   1.0 1.0 0.05\n"
        "J1 Cu   \"\" CONCEN 1.0 1.0 4.0\n\n";
    CoupledRun r = runDeck("_s3_outfall", deck("O1", -1.0, 0.05, c0, inflows, 5.0));
    ASSERT_TRUE(r.ok);
    const auto& ctx    = r.eng->context();
    const auto& router = r.eng->surfaceRouter2D();
    const auto& st     = router.state();
    const auto& tr     = st.transport;

    const double out_in = ctx.mass_balance_2d.outfall_in;
    ASSERT_GT(out_in, 0.5) << "fixture produced no outfall discharge onto the mesh";
    ASSERT_FALSE(tr.coupling_src.empty()) << "outfall species source not sized";

    EXPECT_GT(tr.gained_coupling[0], 0.0) << "outfall discharge arrived clean";
    EXPECT_LE(tr.gained_coupling[0], c0 * out_in * (1.0 + 1.0e-9))
        << "more mass arrived than c0 × volume: source scattered at a "
           "concentration above the outfall's";
    double cmax = 0.0;
    for (int c = 0; c < tr.n_cells; ++c) {
        const double v_dry = router.options().dry_depth * router.mesh().tri_area[c];
        if (!(st.volume[c] > v_dry)) continue;
        cmax = std::max(cmax, tr.concentration(0, c, st.volume[c], v_dry));
    }
    EXPECT_LE(cmax, c0 * (1.0 + 1.0e-9)) << "cross-domain max principle broken";
    // 2D identity: surface + lost − gained == initial seed.
    double init_mass = 0.0;
    for (int c = 0; c < tr.n_cells; ++c)
        init_mass += c0 * 0.05 * router.mesh().tri_area[c];
    EXPECT_NEAR(tr.totalIncludingLedgers(0), init_mass, 1.0e-9 * init_mass);
    // At the end of a 20-minute constant feed the outfall itself reads c0.
    const int o1 = swmm_node_index(r.e, "O1");
    ASSERT_GE(o1, 0);
    EXPECT_NEAR(ctx.nodes.conc[static_cast<std::size_t>(o1)], c0, 1.0e-6 * c0);
    close(r);
}

// ---------------------------------------------------------------------------
// 4. No pollutants ⇒ every S3 branch is dead: the coupled hydraulics of the
//    drain deck are bit-identical with and without a [POLLUTANTS] block.
//    (The species queue is empty, coupling_src is unsized, exch_mass is
//    empty; the 2D bitwise census has no coupled deck with pollutants, so
//    this is the S3 analogue of that net.)
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS3, SpeciesArePassengersOnTheCoupledHydraulics) {
    CoupledRun a = runDeck("_s3_pass_q", deck("J1", 1.0, 0.5, 4.0, "", 1.0));
    ASSERT_TRUE(a.ok);
    std::vector<double> vol_q = a.eng->surfaceRouter2D().state().volume;
    const double drained_q = a.eng->context().mass_balance_2d.coupling_2d_to_1d_out;
    close(a);

    std::string body = deck("J1", 1.0, 0.5, 4.0, "", 1.0);
    // Strip [POLLUTANTS] and [2D_INITIAL_QUALITY].
    auto strip = [&](const std::string& head) {
        const auto p = body.find(head);
        if (p == std::string::npos) return;
        const auto q = body.find("\n\n", p);
        body.erase(p, (q == std::string::npos ? body.size() : q + 2) - p);
    };
    strip("[POLLUTANTS]");
    strip("[2D_INITIAL_QUALITY]");
    CoupledRun b = runDeck("_s3_pass_h", body);
    ASSERT_TRUE(b.ok);
    EXPECT_EQ(b.eng->surfaceRouter2D().state().volume, vol_q);
    EXPECT_EQ(b.eng->context().mass_balance_2d.coupling_2d_to_1d_out, drained_q);
    close(b);
}
