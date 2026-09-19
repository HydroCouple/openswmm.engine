/**
 * @file test_2d_surface_quality.cpp
 * @brief S7 (2026-09-19) — cell coverages, buildup, washoff and sweeping on
 *        the 2D overland surface (OVERLAND plan §8; program plan S7,
 *        D-A22–D-A26).
 *
 * Gates (§8.7):
 *   1. BuildupParityWithSubcatchment — a covered cell with no flow accrues
 *      the SAME buildup (user mass per land-use area) as a subcatchment with
 *      the same land use and DRY_DAYS, bit-for-bit; an MSX species with the
 *      pollutant's rows accrues bit-for-bit with the pollutant (BW-MSX).
 *   2. EmcWashoffIsExactOnATriangleAndAQuadPan — a pan draining into a node
 *      under EMC: species mass washed into the rows == C_emc × the volume
 *      that left the pan (initial − final, no rain / evap / infiltration) to
 *      1e-10, on an all-triangle and on a single-quad mesh.
 *   3. LedgerClosesUnderRainWithSweepingAndBmp — rain on the mesh, EXP
 *      washoff, BMP 20 %, sweeping in the dry tail: init + net buildup −
 *      washed − BMP − swept == final store; the S1 conservation identity
 *      (now including gained_washoff) holds to round-off.
 *   4. OwnershipWarnings — RAINFALL_MODE NONE makes the rows inert with one
 *      warning; covered cells inside a covered subcatchment warn once.
 *   5. RoundTripAndGrammar — rows survive write → reopen with their scope
 *      tokens; percent > 100, an unknown land use and a PER_CURB land use
 *      without [2D_CURB_LENGTH] are refused at open.
 *   6. InertRowsLeaveTheHydrographBitIdentical — coverages whose land use has
 *      no buildup / washoff function change nothing at the node.
 *
 * Artefacts land in the working directory: run from
 * tests/output/2d_surface_quality (CLAUDE.md §4.1).
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_sq2d.h>

#include "2d/SurfaceRouter2D.hpp"
#include "2d/quality/SurfaceQuality2D.hpp"
#include "core/SWMMEngine.hpp"
#include "core/SimulationContext.hpp"

using namespace openswmm::twoD;

namespace {

openswmm::SWMMEngine& cpp(SWMM_Engine e) { return *static_cast<openswmm::SWMMEngine*>(e); }
void write(const std::string& path, const std::string& body) { std::ofstream f(path); f << body; }

/// A 10 m × 10 m pan (two triangles, or one quad) at elevation z_pan with
/// initial depth h0, vertex 0 coupled to `node`; optional subcatchment S1
/// with the same land use draining to J1 (for the parity gate); optional
/// rain gage on the mesh.
struct DeckOpts {
    std::string options_extra;      // extra [OPTIONS] lines
    std::string twod_extra;         // extra [2D_OPTIONS] lines
    std::string quality;            // [POLLUTANTS] + [LANDUSES] + [BUILDUP] + [WASHOFF] …
    std::string coverages;          // [2D_COVERAGES] etc. (2D sections)
    std::string process_components; // [PROCESS_COMPONENTS] line(s)
    std::string node = "O1";
    double z_pan = -10.0, h0 = 0.5;
    double exch_area = 1.0;          // [2D_VERTEX_NODE_MAP] exchange area
    bool quad = false;
    // S7 (added by the macOS validating agent 2026-09-19 for handoff §6.5):
    // one quad (left square) + two triangles (right square) sharing edge 1-2,
    // so the flow path crosses a cell boundary before it drains — the case
    // the rejected gross-flux D-A22 double-counted.
    bool mixed = false;
    bool rain = false;              // TS1 1 in/hr for 1 h on RG1
    bool subcatch = false;          // S1 → J1, [COVERAGES] S1 LU1 100
    std::string end_time = "03:00:00";
};

std::string deck(const DeckOpts& o) {
    std::ostringstream m;
    m << "[OPTIONS]\n"
         "FLOW_UNITS           CFS\nFLOW_ROUTING         DYNWAVE\nINFILTRATION         HORTON\n"
         "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
         "END_DATE             01/01/2026\nEND_TIME             " << o.end_time << "\n"
         "REPORT_STEP          00:05:00\nWET_STEP             00:05:00\nDRY_STEP             00:05:00\n"
         "ROUTING_STEP         5\nDRY_DAYS             5\nALLOW_PONDING        NO\n"
      << o.options_extra
      << "\n[RAINGAGES]\nRG1  INTENSITY 1:00 1.0 TIMESERIES TS1\n"   // one record = one hour
         "\n[TIMESERIES]\nTS1  01/01/2026 00:00 " << (o.rain ? "1.0" : "0.0") << "\n"
         "TS1  01/01/2026 01:00 0.0\n";
    if (o.subcatch)
        m << "\n[SUBCATCHMENTS]\nS1  RG1  J1  10.0  50   500  0.5  0\n"
             "\n[SUBAREAS]\nS1  0.01  0.1  0.05  0.05  25  OUTLET\n"
             "\n[INFILTRATION]\nS1  3.0  0.5  4.0  7  0\n";
    m << "\n[JUNCTIONS]\nJ1 0.0 1.0 0 0 0\n\n[OUTFALLS]\nO1 -0.5 FREE NO\n\n"
         "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n[XSECTIONS]\nC1 CIRCULAR 0.3 0 0 0 1\n"
      << o.quality;
    if (o.subcatch) m << "\n[COVERAGES]\nS1  LU1  100\n";
    m << "\n[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nLTS_TIERS 1\nMAX_TIMESTEP 5\n"
         "DRY_DEPTH 0.001\nCOUPLING_CD 0.7\nREPORT_2D NO\nRAINFALL_MODE SYSTEM\n"
      << o.twod_extra
      << "\n[2D_VERTICES]\n 0.0  0.0 " << o.z_pan << "\n10.0  0.0 " << o.z_pan
      << "\n10.0 10.0 " << o.z_pan << "\n 0.0 10.0 " << o.z_pan << "\n";
    if (o.mixed)
        // S7 §6.5 (validating agent): two extra vertices make a second square
        // to the east. The quad carries the drain (vertex 0); the triangles
        // feed it across the shared edge 1-2, so the flow path crosses a cell
        // boundary before it leaves — the case gross-flux D-A22 double-counted.
        // [2D_TRIANGLES] must precede [2D_QUADS] (cells are numbered
        // triangles first, then quads), so the quad is the LAST cell.
        m << "20.0  0.0 " << o.z_pan << "\n20.0 10.0 " << o.z_pan << "\n"
          << "\n[2D_TRIANGLES]\n;;V1 V2 V3 N INIT_DEPTH TAG\n1 4 5 0.03 " << o.h0 << " pan\n"
             "1 5 2 0.03 " << o.h0 << " pan\n"
          << "\n[2D_QUADS]\n;;V1 V2 V3 V4 N INIT_DEPTH TAG\n0 1 2 3 0.03 " << o.h0 << " pan\n";
    else if (o.quad)
        m << "\n[2D_QUADS]\n;;V1 V2 V3 V4 N INIT_DEPTH TAG\n0 1 2 3 0.03 " << o.h0 << " pan\n";
    else
        m << "\n[2D_TRIANGLES]\n;;V1 V2 V3 N INIT_DEPTH TAG\n0 1 2 0.03 " << o.h0 << " pan\n0 2 3 0.03 " << o.h0 << " pan\n";
    m << "\n[2D_VERTEX_NODE_MAP]\n0 " << o.node << " 0.7 " << o.exch_area << "\n"
      << o.coverages
      << (o.process_components.empty() ? "" : "\n[PROCESS_COMPONENTS]\n" + o.process_components + "\n")
      << "\n[REPORT]\nINPUT NO\n";
    return m.str();
}

const char* kQualityPow =
    "\n[POLLUTANTS]\nTSS  MG/L  0  0  0  0  NO  *  0.0  0  0\n"
    "\n[LANDUSES]\nLU1  0  0  0\n"
    "\n[BUILDUP]\nLU1  TSS  POW  100  2  1  AREA\n"
    "\n[WASHOFF]\nLU1  TSS  EXP  0.1  1.0  0  0\n";

struct DeckRun {
    SWMM_Engine e = nullptr; bool ok = false; int rc_open = -1;
    std::vector<double> tss_j1;
    std::vector<std::string> warnings;
    double v_start = 0.0;            // Σ cell volume (m³) right after start
};
DeckRun run(const std::string& tag, const std::string& body, bool step = true) {
    DeckRun r;
    write(tag + ".inp", body);
    r.e = swmm_engine_create();
    if (!r.e) return r;
    r.rc_open = swmm_engine_open(r.e, (tag + ".inp").c_str(), (tag + ".rpt").c_str(), (tag + ".out").c_str(), nullptr);
    if (r.rc_open != SWMM_OK) return r;
    if (swmm_engine_initialize(r.e) != SWMM_OK || swmm_engine_start(r.e, 1) != SWMM_OK) return r;
    auto& ctx = cpp(r.e).context();
    const int j1 = ctx.node_names.find("J1");
    for (double v : cpp(r.e).surfaceRouter2D().state().volume) r.v_start += v;
    if (step) {
        double elapsed = 0.0;
        while (swmm_engine_step(r.e, &elapsed) == SWMM_OK && elapsed > 0.0) {
            if (j1 >= 0 && ctx.n_pollutants() > 0)
                r.tss_j1.push_back(ctx.nodes.conc[static_cast<std::size_t>(j1) * ctx.n_pollutants()]);
        }
    }
    r.warnings = ctx.warnings;
    r.ok = true;
    return r;
}
void close(DeckRun& r) { if (!r.e) return; swmm_engine_end(r.e); swmm_engine_close(r.e); swmm_engine_destroy(r.e); r.e = nullptr; }

double storeUserMass(const SurfaceQuality2D& sq, const MeshData& mesh, int s, double area_to_la, bool si) {
    (void)si;
    double total = 0.0;
    for (int c = 0; c < sq.nCells(); ++c)
        total += sq.buildupPerArea(c, s) * mesh.tri_area[static_cast<std::size_t>(c)] * area_to_la;
    return total;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. Buildup parity with a subcatchment (no flow), pollutant and MSX species.
// ---------------------------------------------------------------------------
TEST(SurfaceQuality2D_S7, BuildupParityWithSubcatchment) {
    write("_s7_par.rxn", "[REACTION_OPTIONS]\nSOLVER RK5\n[REACTION_SPECIES]\nBULK X MG\n"
                         "[REACTION_PIPES]\nRATE X 0\n[REACTION_TANKS]\nRATE X 0\n");
    DeckOpts o;
    o.subcatch = true;
    o.quality = "\n[POLLUTANTS]\nTSS  MG/L  0  0  0  0  NO  *  0.0  0  0\n"
                "\n[LANDUSES]\nLU1  0  0  0\n"
                "\n[BUILDUP]\nLU1  TSS  POW  100  2  1  AREA\nLU1  X  POW  100  2  1  AREA\n"
                "\n[WASHOFF]\nLU1  TSS  EXP  0.1  1.0  0  0\nLU1  X  EXP  0.1  1.0  0  0\n";
    o.coverages = "\n[2D_COVERAGES]\n*  LU1  100\n";
    o.process_components = "org.hydrocouple.openswmm.reactions config=\"_s7_par.rxn\"";
    DeckRun r = run("_s7_par", deck(o));
    ASSERT_TRUE(r.ok) << "open rc " << r.rc_open;
    const auto& eng = cpp(r.e);
    const auto& sq  = eng.surfaceRouter2D().surfaceQuality();
    ASSERT_TRUE(sq.active());
    ASSERT_EQ(sq.nSpecies(), 2);
    ASSERT_EQ(sq.nLandUses(), 1);
    // Subcatchment S1's per-land-use buildup (user mass per acre) after the run…
    const auto& sqs = eng.surfaceQuality();
    const double sub_tss = sqs.buildup[sqs.bu_idx(0, 0, 0)];
    ASSERT_GT(sub_tss, 10.0) << "5 dry days at POW(100,2,1) is 10 lbs/ac, plus 3 h of accrual";
    // …equals every cell's, bit-for-bit, for the pollutant and for X.
    for (int c = 0; c < sq.nCells(); ++c) {
        EXPECT_EQ(sq.buildup(c, 0, 0), sub_tss) << "cell " << c << " TSS";
        EXPECT_EQ(sq.buildup(c, 0, 1), sub_tss) << "cell " << c << " X";
        EXPECT_EQ(sq.buildupPerArea(c, 0), sub_tss);
    }
    // Ledger: initial buildup = 10 lbs/ac × Σ area(ac); net accrual > 0; nothing washed.
    const auto& mesh = eng.surfaceRouter2D().mesh();
    double area_ac = 0.0;
    for (int c = 0; c < mesh.n_triangles(); ++c) area_ac += mesh.tri_area[static_cast<std::size_t>(c)] / 4046.8564224;
    EXPECT_NEAR(sq.ledInitBuildup()[0], 10.0 * area_ac, 1e-12 * 10.0 * area_ac);
    EXPECT_GT(sq.ledBuildup()[0], 0.0);
    EXPECT_EQ(sq.ledWashoff()[0], 0.0) << "no flow, no washoff";
    close(r);
}

// ---------------------------------------------------------------------------
// 2. EMC exactness on a draining pan — triangles and a quad.
//    The pan (0.5 ft of still water; the deck is CFS so the mesh is in feet)
//    drains into J1 through a small exchange area over many runoff steps.
//    D-A22: the runoff a cell produced is its NET outflow, so on the
//    two-triangle pan the water the far triangle hands to the drain
//    triangle counts once, not twice — Σ_c runoff == what reached J1.
// ---------------------------------------------------------------------------
TEST(SurfaceQuality2D_S7, EmcWashoffIsExactOnATriangleAndAQuadPan) {
    for (const bool quad : {false, true}) {
        DeckOpts o;
        o.node = "J1"; o.z_pan = 5.0; o.h0 = 0.5;   // pan above J1's rim (1.0): drains
        o.exch_area = 0.002;                         // ~15 min to empty
        o.quality = "\n[POLLUTANTS]\nTSS  MG/L  0  0  0  0  NO  *  0.0  0  0\n"
                    "\n[LANDUSES]\nLU1  0  0  0\n"
                    "\n[WASHOFF]\nLU1  TSS  EMC  50  0  0  0\n";
        o.coverages = "\n[2D_COVERAGES]\nTAG pan  LU1  100\n";
        o.quad = quad;
        o.end_time = "00:40:00";
        DeckRun r = run(std::string("_s7_emc_") + (quad ? "quad" : "tri"), deck(o));
        ASSERT_TRUE(r.ok) << "open rc " << r.rc_open;
        const auto& router = cpp(r.e).surfaceRouter2D();
        const auto& sq = router.surfaceQuality();
        const auto& st = router.state();
        const auto& tr = st.transport;
        ASSERT_TRUE(sq.active());
        ASSERT_EQ(router.mesh().n_triangles(), quad ? 1 : 2);
        // runoff the quality step has seen, plus what the marcher booked
        // since the last runoff step (the routing loop runs past it)
        double v_end = 0.0, runoff = 0.0, pending = 0.0;
        for (int c = 0; c < tr.n_cells; ++c) {
            v_end   += st.volume[static_cast<std::size_t>(c)];
            runoff  += sq.runoffVolume(c);
            pending += std::max(tr.cell_runoff_vol[static_cast<std::size_t>(c)], 0.0);
        }
        const double v0 = r.v_start;
        ASSERT_NEAR(v0, 0.1524 * 9.290304, 1e-9) << "0.5 ft over 100 ft²";
        const double drained = v0 - v_end;
        ASSERT_GT(drained, 0.5 * v0) << "the pan must actually drain into J1";
        ASSERT_GT(runoff, 0.9 * drained) << "…over several runoff steps";
        // runoff the cells produced == what reached J1 (net, D-A22): the
        // far triangle's water is counted once, not again at the drain.
        EXPECT_NEAR(runoff + pending, drained, 1e-10 * v0)
            << (quad ? "quad" : "tri") << ": Σ net cell outflow must equal the drained volume";
        // washed (row units = mg/L × m³) == C_emc × runoff volume
        EXPECT_NEAR(tr.gained_washoff[0], 50.0 * runoff, 1e-10 * 50.0 * v0)
            << (quad ? "quad" : "tri") << ": EMC washoff must equal C × runoff";
        // …and every gram of it left the pan into J1 or is still on the pan:
        // the S1 identity (surface + lost − gained) is invariant (it was 0 at t=0).
        EXPECT_NEAR(tr.totalIncludingLedgers(0), 0.0, 1e-9 * 50.0 * v0);
        EXPECT_GT(tr.lost_coupling[0], 0.5 * tr.gained_washoff[0])
            << "most of the washed mass rode the later drain steps into J1";
        close(r);
    }
}

// ---------------------------------------------------------------------------
// 2b. S7 handoff §6.5 (added by the macOS validating agent, 2026-09-19):
//     the same EMC identity on a MIXED mesh — one quad sharing an edge with
//     two triangles. This is the shape the rejected gross-flux D-A22 got
//     wrong: the eastern triangles' water crosses into the quad before it
//     drains, so a gross formulation would count it twice and
//     gained_washoff would exceed 50 x the drained volume. Under the net
//     definition the identity must hold exactly as on the uniform pans.
// ---------------------------------------------------------------------------
TEST(SurfaceQuality2D_S7, EmcWashoffIsExactOnAMixedTriQuadPan) {
    DeckOpts o;
    o.node = "J1"; o.z_pan = 5.0; o.h0 = 0.5;
    o.exch_area = 0.002;
    o.quality = "\n[POLLUTANTS]\nTSS  MG/L  0  0  0  0  NO  *  0.0  0  0\n"
                "\n[LANDUSES]\nLU1  0  0  0\n"
                "\n[WASHOFF]\nLU1  TSS  EMC  50  0  0  0\n";
    o.coverages = "\n[2D_COVERAGES]\nTAG pan  LU1  100\n";
    o.mixed = true;
    o.end_time = "00:40:00";
    DeckRun r = run("_s7_emc_mixed", deck(o));
    ASSERT_TRUE(r.ok) << "open rc " << r.rc_open;
    const auto& router = cpp(r.e).surfaceRouter2D();
    const auto& sq = router.surfaceQuality();
    const auto& st = router.state();
    const auto& tr = st.transport;
    ASSERT_TRUE(sq.active());
    // one quad + two triangles (n_triangles() counts ALL cells, as gate 2's
    // single-quad case shows), and the flow path really does cross a face
    ASSERT_EQ(router.mesh().n_quads(), 1);
    ASSERT_EQ(router.mesh().n_triangles(), 3);

    double v_end = 0.0, runoff = 0.0, pending = 0.0;
    for (int c = 0; c < tr.n_cells; ++c) {
        v_end   += st.volume[static_cast<std::size_t>(c)];
        runoff  += sq.runoffVolume(c);
        pending += std::max(tr.cell_runoff_vol[static_cast<std::size_t>(c)], 0.0);
    }
    const double v0 = r.v_start;
    const double drained = v0 - v_end;
    ASSERT_GT(drained, 0.25 * v0) << "the mixed pan must actually drain into J1";
    // The gate is only meaningful if water really crosses the shared edge:
    // cells 0 and 1 are the eastern triangles (triangles are numbered first)
    // and they touch no drain, so any runoff they produced left through the
    // face into the quad — exactly what a gross formulation would recount.
    ASSERT_GT(sq.runoffVolume(0) + sq.runoffVolume(1), 0.05 * drained)
        << "the eastern triangles must feed the quad, else the identity is vacuous";
    // The identity that fails under a gross-flux definition:
    EXPECT_NEAR(runoff + pending, drained, 1e-10 * v0)
        << "mixed: Sigma net cell outflow must equal the drained volume "
           "(gross flux would exceed it — the eastern water counted twice)";
    EXPECT_NEAR(tr.gained_washoff[0], 50.0 * runoff, 1e-10 * 50.0 * v0)
        << "mixed: EMC washoff must equal C x runoff";
    EXPECT_NEAR(tr.totalIncludingLedgers(0), 0.0, 1e-9 * 50.0 * v0);
    close(r);
}

// ---------------------------------------------------------------------------
// 3. Ledger closure under rain with EXP washoff, BMP and sweeping.
// ---------------------------------------------------------------------------
TEST(SurfaceQuality2D_S7, LedgerClosesUnderRainWithSweepingAndBmp) {
    DeckOpts o;
    o.node = "J1"; o.z_pan = 5.0; o.h0 = 0.0;   // dry pan, rain fills and drains it
    o.rain = true;
    o.quality = "\n[POLLUTANTS]\nTSS  MG/L  0  0  0  0  NO  *  0.0  0  0\n"
                "\n[LANDUSES]\nLU1  0.05  60  0\n"                 // sweep every 1.2 h dry
                "\n[BUILDUP]\nLU1  TSS  POW  100  2  1  AREA\n"
                "\n[WASHOFF]\nLU1  TSS  EXP  0.5  1.0  50  20\n";
    o.coverages = "\n[2D_COVERAGES]\n*  LU1  100\n";
    o.end_time = "04:00:00";
    DeckRun r = run("_s7_ledger", deck(o));
    ASSERT_TRUE(r.ok) << "open rc " << r.rc_open;
    const auto& router = cpp(r.e).surfaceRouter2D();
    const auto& sq = router.surfaceQuality();
    const auto& tr = router.state().transport;
    ASSERT_TRUE(sq.active());
    const auto& mesh = router.mesh();
    // final store, user mass
    double final_store = 0.0;
    for (int c = 0; c < sq.nCells(); ++c)
        final_store += sq.buildupPerArea(c, 0) * mesh.tri_area[static_cast<std::size_t>(c)] / 4046.8564224;
    const double lhs = sq.ledInitBuildup()[0] + sq.ledBuildup()[0] - sq.ledWashoff()[0]
                     - sq.ledBmp()[0] - sq.ledSweeping()[0];
    const double scale = sq.ledInitBuildup()[0] + sq.ledBuildup()[0];
    EXPECT_GT(sq.ledWashoff()[0], 0.0)  << "rain must wash something off";
    EXPECT_GT(sq.ledBmp()[0], 0.0);
    EXPECT_GT(sq.ledSweeping()[0], 0.0) << "one sweep in the dry tail";
    EXPECT_NEAR(lhs, final_store, 1e-10 * scale) << "init + net − washed − BMP − swept == store";
    // Species conservation on the water side: what was washed into the rows
    // is on the pan or in J1 (S1 identity with gained_washoff subtracted).
    EXPECT_NEAR(tr.totalIncludingLedgers(0), 0.0, 1e-9 * tr.gained_washoff[0]);
    // Row units ↔ user mass: washed_user = gained_washoff × 1000 × mcf
    EXPECT_NEAR(sq.ledWashoff()[0], tr.gained_washoff[0] * 1000.0 * sq.mcf(0), 1e-9 * sq.ledWashoff()[0]);
    close(r);
}

// ---------------------------------------------------------------------------
// 4. Ownership warnings.
// ---------------------------------------------------------------------------
TEST(SurfaceQuality2D_S7, OwnershipWarnings) {
    {
        DeckOpts o;
        o.quality = kQualityPow;
        o.coverages = "\n[2D_COVERAGES]\n*  LU1  100\n";
        o.twod_extra = "RAINFALL_MODE NONE\n";
        DeckRun r = run("_s7_own_none", deck(o), false);
        ASSERT_TRUE(r.ok);
        int hits = 0;
        for (const auto& w : r.warnings) if (w.find("RAINFALL_MODE NONE") != std::string::npos) ++hits;
        EXPECT_EQ(hits, 1);
        EXPECT_FALSE(cpp(r.e).surfaceRouter2D().surfaceQuality().active()) << "inert";
        close(r);
    }
    {
        // S1 polygon covering the pan → the double-count notice, once.
        DeckOpts o;
        o.subcatch = true;
        o.quality = kQualityPow;
        o.coverages = "\n[2D_COVERAGES]\n*  LU1  100\n"
                      "\n[POLYGONS]\nS1 -1 -1\nS1 11 -1\nS1 11 11\nS1 -1 11\n";
        DeckRun r = run("_s7_own_sub", deck(o), false);
        ASSERT_TRUE(r.ok);
        int hits = 0;
        for (const auto& w : r.warnings)
            if (w.find("covered cells lie inside subcatchment") != std::string::npos && w.find("S1") != std::string::npos) ++hits;
        EXPECT_EQ(hits, 1);
        EXPECT_TRUE(cpp(r.e).surfaceRouter2D().surfaceQuality().active()) << "the run proceeds";
        close(r);
    }
}

// ---------------------------------------------------------------------------
// 5. Round trip and grammar.
// ---------------------------------------------------------------------------
TEST(SurfaceQuality2D_S7, RoundTripAndGrammar) {
    DeckOpts o;
    o.quality = "\n[POLLUTANTS]\nTSS  MG/L  0  0  0  0  NO  *  0.0  0  0\n"
                "\n[LANDUSES]\nLU1  0  0  0\nLU2  0  0  0\n"
                "\n[BUILDUP]\nLU1  TSS  POW  100  2  1  AREA\nLU2  TSS  POW  50  1  1  CURB\n"
                "\n[WASHOFF]\nLU1  TSS  EXP  0.1  1.0  0  0\n";
    o.coverages = "\n[2D_COVERAGES]\n*  LU1  60  LU2  25\nTAG pan  LU1  100\nCELL 2  LU2  100\n"
                  "\n[2D_LOADINGS]\n*  TSS  12.5\nCELL 1  TSS  3\n"
                  "\n[2D_CURB_LENGTH]\n*  30\nCELL 2  45\n";
    write("_s7_rt.inp", deck(o));
    SWMM_Engine e = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e, "_s7_rt.inp", "_s7_rt.rpt", "_s7_rt.out", nullptr), SWMM_OK);
    ASSERT_EQ(swmm_model_write(e, "_s7_rt_saved.inp"), SWMM_OK);
    swmm_engine_close(e); swmm_engine_destroy(e);
    {
        std::ifstream f("_s7_rt_saved.inp"); std::stringstream ss; ss << f.rdbuf();
        const std::string t = ss.str();
        EXPECT_NE(t.find("[2D_COVERAGES]"), std::string::npos);
        EXPECT_NE(t.find("TAG pan"), std::string::npos);
        EXPECT_NE(t.find("CELL 2"), std::string::npos);
        EXPECT_NE(t.find("[2D_LOADINGS]"), std::string::npos);
        EXPECT_NE(t.find("[2D_CURB_LENGTH]"), std::string::npos);
    }
    SWMM_Engine e2 = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e2, "_s7_rt_saved.inp", "_s7_rt2.rpt", "_s7_rt2.out", nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(e2), SWMM_OK);
    const auto& sq = cpp(e2).surfaceRouter2D().surfaceQuality();
    ASSERT_EQ(sq.coverage_rows.size(), 3u);
    ASSERT_EQ(sq.loading_rows.size(), 2u);
    ASSERT_EQ(sq.curb_rows.size(), 2u);
    ASSERT_TRUE(sq.active());
    // GLOBAL < TAG < CELL: cell 1 (index 0) took the TAG row, cell 2 the CELL row.
    EXPECT_DOUBLE_EQ(sq.coverage(0, 0), 1.0);   // LU1 100 %
    EXPECT_DOUBLE_EQ(sq.coverage(0, 1), 0.0);
    EXPECT_DOUBLE_EQ(sq.coverage(1, 0), 0.0);
    EXPECT_DOUBLE_EQ(sq.coverage(1, 1), 1.0);   // LU2 100 %
    EXPECT_DOUBLE_EQ(sq.curbLength(0), 30.0);
    EXPECT_DOUBLE_EQ(sq.curbLength(1), 45.0);
    // loadings: cell 1 → 3 (CELL row wins), cell 2 → 12.5 (GLOBAL); LU2 is
    // PER_CURB on cell 2, so its store is loading × area / curb.
    EXPECT_DOUBLE_EQ(sq.buildup(0, 0, 0), 3.0);
    const double area2_ac = cpp(e2).surfaceRouter2D().mesh().tri_area[1] / 4046.8564224;
    EXPECT_NEAR(sq.buildup(1, 1, 0), 12.5 * area2_ac / 45.0, 1e-12);
    swmm_engine_close(e2); swmm_engine_destroy(e2);

    // Grammar refusals at open.
    auto refused = [&](const char* tag, const std::string& cov, const std::string& quality = kQualityPow) {
        DeckOpts q; q.quality = quality; q.coverages = cov;
        write(std::string(tag) + ".inp", deck(q));
        SWMM_Engine x = swmm_engine_create();
        const int rc = swmm_engine_open(x, (std::string(tag) + ".inp").c_str(),
                                        (std::string(tag) + ".rpt").c_str(), (std::string(tag) + ".out").c_str(), nullptr);
        int rc2 = SWMM_OK;
        if (rc == SWMM_OK) rc2 = swmm_engine_initialize(x);
        swmm_engine_close(x); swmm_engine_destroy(x);
        return rc != SWMM_OK || rc2 != SWMM_OK;
    };
    EXPECT_TRUE(refused("_s7_bad_pct", "\n[2D_COVERAGES]\n*  LU1  70  LU1  40\n")) << "percent > 100";
    EXPECT_TRUE(refused("_s7_bad_lu",  "\n[2D_COVERAGES]\n*  NOPE  100\n"))       << "unknown land use";
    EXPECT_TRUE(refused("_s7_bad_curb", "\n[2D_COVERAGES]\n*  LU1  100\n",
        "\n[POLLUTANTS]\nTSS  MG/L  0  0  0  0  NO  *  0.0  0  0\n\n[LANDUSES]\nLU1  0  0  0\n"
        "\n[BUILDUP]\nLU1  TSS  POW  100  2  1  CURB\n")) << "PER_CURB without [2D_CURB_LENGTH]";
}

// ---------------------------------------------------------------------------
// 6. Inert rows change nothing.
// ---------------------------------------------------------------------------
TEST(SurfaceQuality2D_S7, InertRowsLeaveTheHydrographBitIdentical) {
    DeckOpts a; a.node = "J1"; a.z_pan = 5.0; a.h0 = 0.5; a.end_time = "00:30:00";
    a.quality = "\n[POLLUTANTS]\nTSS  MG/L  0  0  0  0  NO  *  0.0  0  0\n\n[LANDUSES]\nLU1  0  0  0\n";
    a.coverages = "";
    DeckOpts b = a;
    b.coverages = "\n[2D_COVERAGES]\n*  LU1  100\n";   // LU1 has no buildup / washoff function
    DeckRun ra = run("_s7_inert_a", deck(a));
    DeckRun rb = run("_s7_inert_b", deck(b));
    ASSERT_TRUE(ra.ok); ASSERT_TRUE(rb.ok);
    EXPECT_FALSE(cpp(ra.e).surfaceRouter2D().surfaceQuality().active());
    EXPECT_TRUE(cpp(rb.e).surfaceRouter2D().surfaceQuality().active());
    const auto& sa = cpp(ra.e).surfaceRouter2D().state();
    const auto& sb = cpp(rb.e).surfaceRouter2D().state();
    for (std::size_t c = 0; c < sa.volume.size(); ++c)
        EXPECT_EQ(sa.volume[c], sb.volume[c]) << "cell " << c;
    ASSERT_EQ(ra.tss_j1.size(), rb.tss_j1.size());
    for (std::size_t k = 0; k < ra.tss_j1.size(); ++k) ASSERT_EQ(ra.tss_j1[k], rb.tss_j1[k]) << k;
    EXPECT_EQ(sb.transport.gained_washoff[0], 0.0);
    close(ra); close(rb);
}

// ---------------------------------------------------------------------------
// 7. C API: rows edited through the API resolve exactly as the file's rows,
//    share the file's grammar (percent > 100, unknown names refused), and
//    are refused once the run has started.
// ---------------------------------------------------------------------------
TEST(SurfaceQuality2D_S7, CApiRowsShareTheFileGrammar) {
    DeckOpts o;
    o.quality = "\n[POLLUTANTS]\nTSS  MG/L  0  0  0  0  NO  *  0.0  0  0\n"
                "\n[LANDUSES]\nLU1  0  0  0\nLU2  0  0  0\n"
                "\n[BUILDUP]\nLU1  TSS  POW  100  2  1  AREA\nLU2  TSS  POW  50  1  1  CURB\n"
                "\n[WASHOFF]\nLU1  TSS  EXP  0.1  1.0  0  0\n";
    o.coverages = "";
    write("_s7_api.inp", deck(o));
    SWMM_Engine e = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e, "_s7_api.inp", "_s7_api.rpt", "_s7_api.out", nullptr), SWMM_OK);
    int n = -1;
    ASSERT_EQ(swmm_2d_coverage_count(e, &n), SWMM_OK);
    EXPECT_EQ(n, 0);
    const char* lus[2] = {"LU1", "LU2"};
    const double pcts[2] = {60.0, 25.0};
    ASSERT_EQ(swmm_2d_coverage_set(e, SWMM_SQ2D_SCOPE_GLOBAL, nullptr, -1, lus, pcts, 2), SWMM_OK);
    const char* lu1[1] = {"LU1"}; const double p100[1] = {100.0};
    ASSERT_EQ(swmm_2d_coverage_set(e, SWMM_SQ2D_SCOPE_TAG, "pan", -1, lu1, p100, 1), SWMM_OK);
    const char* lu2[1] = {"LU2"};
    ASSERT_EQ(swmm_2d_coverage_set(e, SWMM_SQ2D_SCOPE_CELL, nullptr, 1, lu2, p100, 1), SWMM_OK);
    // grammar: percent > 100, unknown land use, TAG without a tag
    const double bad[2] = {70.0, 40.0};
    EXPECT_EQ(swmm_2d_coverage_set(e, SWMM_SQ2D_SCOPE_GLOBAL, nullptr, -1, lus, bad, 2), SWMM_ERR_BADPARAM);
    const char* nope[1] = {"NOPE"};
    EXPECT_EQ(swmm_2d_coverage_set(e, SWMM_SQ2D_SCOPE_GLOBAL, nullptr, -1, nope, p100, 1), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_2d_coverage_set(e, SWMM_SQ2D_SCOPE_TAG, nullptr, -1, lu1, p100, 1), SWMM_ERR_BADPARAM);
    // a same-scope row replaces: re-set GLOBAL, still 3 rows
    ASSERT_EQ(swmm_2d_coverage_set(e, SWMM_SQ2D_SCOPE_GLOBAL, nullptr, -1, lus, pcts, 2), SWMM_OK);
    ASSERT_EQ(swmm_2d_coverage_count(e, &n), SWMM_OK);
    EXPECT_EQ(n, 3);
    ASSERT_EQ(swmm_2d_loading_set(e, SWMM_SQ2D_SCOPE_GLOBAL, nullptr, -1, "TSS", 12.5), SWMM_OK);
    ASSERT_EQ(swmm_2d_loading_set(e, SWMM_SQ2D_SCOPE_CELL, nullptr, 0, "TSS", 3.0), SWMM_OK);
    EXPECT_EQ(swmm_2d_loading_set(e, SWMM_SQ2D_SCOPE_GLOBAL, nullptr, -1, "__WATER_AGE__", 1.0), SWMM_ERR_BADPARAM);
    ASSERT_EQ(swmm_2d_curb_length_set(e, SWMM_SQ2D_SCOPE_GLOBAL, nullptr, -1, 30.0), SWMM_OK);
    ASSERT_EQ(swmm_2d_curb_length_set(e, SWMM_SQ2D_SCOPE_CELL, nullptr, 1, 45.0), SWMM_OK);
    // read-back of the CELL coverage row (index 2 after the GLOBAL re-set: TAG, CELL, GLOBAL)
    int scope = -1, cell = -1, k = -1; char tag[32] = {0}, lu[32] = {0}; double pct = 0.0;
    ASSERT_EQ(swmm_2d_coverage_row_size(e, 1, &k), SWMM_OK);
    EXPECT_EQ(k, 1);
    ASSERT_EQ(swmm_2d_coverage_get(e, 1, 0, &scope, tag, 32, &cell, lu, 32, &pct), SWMM_OK);
    EXPECT_EQ(scope, SWMM_SQ2D_SCOPE_CELL); EXPECT_EQ(cell, 1); EXPECT_STREQ(lu, "LU2"); EXPECT_DOUBLE_EQ(pct, 100.0);
    double len = 0.0;
    ASSERT_EQ(swmm_2d_curb_length_get(e, 1, &scope, tag, 32, &cell, &len), SWMM_OK);
    EXPECT_EQ(scope, SWMM_SQ2D_SCOPE_CELL); EXPECT_EQ(cell, 1); EXPECT_DOUBLE_EQ(len, 45.0);

    // …and they resolve exactly as the RoundTripAndGrammar deck's rows did.
    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
    const auto& sq = cpp(e).surfaceRouter2D().surfaceQuality();
    ASSERT_TRUE(sq.active());
    EXPECT_DOUBLE_EQ(sq.coverage(0, 0), 1.0);
    EXPECT_DOUBLE_EQ(sq.coverage(1, 1), 1.0);
    EXPECT_DOUBLE_EQ(sq.curbLength(0), 30.0);
    EXPECT_DOUBLE_EQ(sq.curbLength(1), 45.0);
    EXPECT_DOUBLE_EQ(sq.buildup(0, 0, 0), 3.0);
    const double area2_ac = cpp(e).surfaceRouter2D().mesh().tri_area[1] / 4046.8564224;
    EXPECT_NEAR(sq.buildup(1, 1, 0), 12.5 * area2_ac / 45.0, 1e-12);
    // edits are refused once initialized; the store reads back
    EXPECT_EQ(swmm_2d_coverage_remove(e, 0), SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(swmm_2d_curb_length_set(e, SWMM_SQ2D_SCOPE_GLOBAL, nullptr, -1, 1.0), SWMM_ERR_LIFECYCLE);
    double bu[2] = {0.0, 0.0};
    ASSERT_EQ(swmm_2d_get_buildup_bulk(e, "TSS", bu, 2), SWMM_OK);
    EXPECT_DOUBLE_EQ(bu[0], sq.buildupPerArea(0, 0));
    EXPECT_DOUBLE_EQ(bu[1], sq.buildupPerArea(1, 0));
    swmm_engine_close(e); swmm_engine_destroy(e);
}
