/**
 * @file      test_si_structure_laws.cpp
 * @brief     Under NODE_CONTINUITY SEMI_IMPLICIT the structure laws have a fixed point and a
 *            usable slope; EXPLICIT keeps the legacy laws.
 *
 * @details Three legacy structure laws keep the dynamic-wave iteration from converging:
 *          - A bottom orifice whose tailwater stands above its crest takes the head difference
 *            h1 - h2 as its head and is then reduced by Villemonte's submergence factor F as
 *            well, in its weir regime only. Its flow jumps by 1/F where the weir and orifice
 *            regimes meet, and an inflow inside the jump has no steady state.
 *          - Villemonte's factor has an infinite slope at equal heads, and the dQ/dH handed to
 *            the node update is taken before the factor.
 *          - An outlet whose rating starts at a nonzero flow switches between full flow and
 *            none on a nearly empty node.
 *          Under SEMI_IMPLICIT (a 6.0 option with no legacy counterpart) the bottom orifice is
 *          corrected once, the submerged laws run linearly to zero below a head difference of
 *          0.05 ft with dQ/dH including the factor's slope, and outlets ramp linearly below a
 *          head of 0.05 ft. Each fixture here has an analytic steady state.
 *
 *          The upstream node is a 1000 ft2 storage unit, so EXPLICIT also settles where the
 *          legacy law has a fixed point; EXPLICIT runs must equal the legacy solver step for
 *          step.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "core/SWMMEngine.hpp"
#include <openswmm/legacy/engine/openswmm_solver.h>

namespace {

const std::filesystem::path out = std::filesystem::path(SI_STRUCTURE_LAWS_OUT_DIR);

constexpr double G = 32.2;        ///< ft/s2, as both engines
constexpr double D_SMOOTH = 0.05; ///< ft, the semi-implicit smoothing head
constexpr double STAGE = 1.0;     ///< ft, fixed outfall stage = tailwater head over the crest

/**
 * @brief Build a one-hour deck: storage J1 (invert 0, 1000 ft2) with a constant inflow drains
 *        through link L1 to outfall O1.
 *
 * @param[in] continuity "EXPLICIT" (line omitted: the legacy solver does not know the option)
 *            or "SEMI_IMPLICIT".
 * @param[in] link The [ORIFICES], [WEIRS] or [OUTLETS] section with L1, plus its cross-section
 *            and curves.
 * @param[in] outfall O1's [OUTFALLS] row.
 * @param[in] y0 J1's initial depth (ft).
 * @param[in] inflow Constant inflow to J1 (cfs).
 * @return Deck text.
 */
std::string deck(const std::string& continuity, const std::string& link, const std::string& outfall,
                 double y0, double inflow) {
    std::ostringstream f;
    f << "[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nLINK_OFFSETS DEPTH\n"
      << "START_DATE 01/01/2026\nEND_DATE 01/01/2026\nEND_TIME 01:00:00\n"
      << "REPORT_STEP 00:05:00\nROUTING_STEP 5\nVARIABLE_STEP 0\n"
      << "MAX_TRIALS 50\nHEAD_TOLERANCE 1e-7\nTHREADS 1\n";
    if (continuity != "EXPLICIT")
        f << "NODE_CONTINUITY " << continuity << "\n";
    f << "[STORAGE]\nJ1 0 10 " << y0 << " FUNCTIONAL 0 0 1000 0 0\n"
      << "[OUTFALLS]\n"
      << outfall << "\n"
      << link << "[INFLOWS]\nJ1 FLOW \"\" FLOW 1.0 1.0 " << inflow << "\n"
      << "[REPORT]\nINPUT NO\n";
    return f.str();
}

/// Bottom orifice, circular, 1 ft, Cd 0.65, crest at J1's invert; tailwater 1 ft above it.
std::string orificeDeck(const std::string& continuity, double inflow) {
    return deck(continuity,
                "[ORIFICES]\nL1 J1 O1 BOTTOM 0 0.65 NO 0\n[XSECTIONS]\nL1 CIRCULAR 1 0 0 0\n",
                "O1 -1 FIXED 1.0 NO", 1.3, inflow);
}

/// Transverse weir, 4 ft long, 3 ft high, Cw 3.33, crest at J1's invert; tailwater 1 ft above.
std::string weirDeck(const std::string& continuity, double inflow) {
    return deck(
        continuity,
        "[WEIRS]\nL1 J1 O1 TRANSVERSE 0 3.33 NO 0 0 YES\n[XSECTIONS]\nL1 RECT_OPEN 3 4 0 0\n",
        "O1 -1 FIXED 1.0 NO", 1.0, inflow);
}

/// Flow regulator: tabular outlet rated 1 cfs at every depth, to a free outfall.
std::string outletDeck(const std::string& continuity, double inflow) {
    return deck(continuity,
                "[OUTLETS]\nL1 J1 O1 0 TABULAR/DEPTH reg NO\n"
                "[CURVES]\nreg Rating 0 1.0\nreg 10 1.0\n",
                "O1 -1 FREE NO", 0.0, inflow);
}

struct Trace {
    std::vector<double> y; ///< J1 depth after every routing step (ft)
    std::vector<double> q; ///< L1 flow after every routing step (cfs)
    double dqdh = 0.0;     ///< L1 dQ/dH after the last step (engine only)
    long non_converged = -1;

    /// Largest minus smallest J1 depth over the last @p n steps.
    double tailRange(std::size_t n) const {
        const auto first = y.end() - static_cast<std::ptrdiff_t>(std::min(n, y.size()));
        const auto [lo, hi] = std::minmax_element(first, y.end());
        return *hi - *lo;
    }
};

/**
 * @brief Run a deck to its end and record J1's depth and L1's flow after every step.
 *
 * @param[in] name File stem under the output folder.
 * @param[in] text Deck text.
 * @param[in] legacy true runs the in-tree legacy solver, false the engine.
 * @return The trace.
 */
Trace run(const std::string& name, const std::string& text, bool legacy) {
    std::filesystem::create_directories(out);
    const auto path = (out / (name + (legacy ? "_legacy" : "_engine") + ".inp")).string();
    std::ofstream(path) << text;
    Trace r;
    double elapsed = 0.0;
    if (legacy) {
        EXPECT_EQ(swmm_open(path.c_str(), (path + ".rpt").c_str(), ""), 0);
        EXPECT_EQ(swmm_start(0), 0);
        const int l1 = swmm_getIndex(swmm_LINK, "L1");
        const int j1 = swmm_getIndex(swmm_NODE, "J1");
        do {
            EXPECT_EQ(swmm_step(&elapsed), 0);
            r.y.push_back(swmm_getValue(swmm_NODE_DEPTH, j1));
            r.q.push_back(swmm_getValue(swmm_LINK_FLOW, l1));
        } while (elapsed > 0.0);
        EXPECT_EQ(swmm_end(), 0);
        EXPECT_EQ(swmm_close(), 0);
    } else {
        openswmm::SWMMEngine engine;
        EXPECT_EQ(engine.open(path.c_str(), (path + ".rpt").c_str(), ""), 0);
        EXPECT_EQ(engine.initialize(), 0);
        EXPECT_EQ(engine.start(0), 0);
        const auto& ctx = engine.context();
        const auto l1 = static_cast<std::size_t>(ctx.link_names.find("L1"));
        const auto j1 = static_cast<std::size_t>(ctx.node_names.find("J1"));
        do {
            EXPECT_EQ(engine.step(&elapsed), 0);
            r.y.push_back(ctx.nodes.depth[j1]);
            r.q.push_back(ctx.links.flow[l1]);
        } while (elapsed > 0.0);
        r.dqdh = ctx.links.dqdh[l1];
        r.non_converged = ctx.routing_stats.n_non_converged;
        EXPECT_EQ(engine.end(), 0);
    }
    return r;
}

/// Root of an increasing function q(x) = target on [lo, hi], by bisection.
double solve(const std::function<double(double)>& q, double target, double lo, double hi) {
    for (int i = 0; i < 200; ++i) {
        const double mid = 0.5 * (lo + hi);
        (q(mid) < target ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}

double villemonte(double r, double p) {
    return std::pow(1.0 - std::pow(r, p), 0.385);
}

/// The fixture's bottom orifice, as legacy orifice_setSetting / orifice_getWeirCoeff build it.
struct BottomOrifice {
    double h_crit, c_weir;
    BottomOrifice() {
        const double f_area = std::acos(-1.0) / 4.0 * std::sqrt(2.0 * G);
        h_crit = 0.65 / 0.414 * 0.25; // (Cd / 0.414) * area / perimeter
        c_weir = 0.65 * std::sqrt(h_crit) * f_area;
    }
    /// Weir-regime flow at head difference dh, without the second factor (continuous at f = 1).
    double once(double dh) const { return c_weir * std::pow(dh / h_crit, 1.5); }
    /// Legacy weir-regime flow: the same, times F at r = H2 / H1.
    double legacy(double dh) const { return once(dh) * villemonte(STAGE / (STAGE + dh), 1.5); }
};

/// The fixture's weir: free flow 3.33 * 4 * H1^1.5 at H1 over the crest.
double weirFree(double H1) {
    return 3.33 * 4.0 * std::pow(H1, 1.5);
}
double weirLegacy(double H1, double H2) {
    return weirFree(H1) * villemonte(H2 / H1, 1.5);
}

TEST(SemiImplicitStructureLaws, BottomOrificeBelowTailwaterIsCorrectedForSubmergenceOnce) {
    const BottomOrifice o;
    const double inflow = 1.25; // about half of c_weir, inside the weir regime
    const double dh_si = solve([&](double x) { return o.once(x); }, inflow, 0.0, o.h_crit);
    const double dh_legacy = solve([&](double x) { return o.legacy(x); }, inflow, 0.0, o.h_crit);
    ASSERT_GT(dh_legacy - dh_si, 0.05); // the fixture tells the two laws apart

    const Trace si = run("orifice_weir_regime_si", orificeDeck("SEMI_IMPLICIT", inflow), false);
    EXPECT_NEAR(si.y.back(), STAGE + dh_si, 1e-4);
    EXPECT_NEAR(si.q.back(), inflow, 1e-4);

    const Trace ex = run("orifice_weir_regime_ex", orificeDeck("EXPLICIT", inflow), false);
    EXPECT_NEAR(ex.y.back(), STAGE + dh_legacy, 1e-4);
    EXPECT_NEAR(ex.q.back(), inflow, 1e-4);
}

TEST(SemiImplicitStructureLaws, BottomOrificeHasSteadyStateInsideLegacyJump) {
    const BottomOrifice o;
    const double inflow = 2.2;
    // Legacy's law jumps from F * c_weir just below f = 1 to c_weir at f = 1; the inflow lies
    // in between, so legacy has no depth at which the orifice passes it.
    ASSERT_LT(o.legacy(o.h_crit * (1.0 - 1e-9)), inflow);
    ASSERT_GT(o.c_weir, inflow);

    const double dh_si = solve([&](double x) { return o.once(x); }, inflow, 0.0, o.h_crit);
    const Trace si = run("orifice_jump_si", orificeDeck("SEMI_IMPLICIT", inflow), false);
    EXPECT_NEAR(si.y.back(), STAGE + dh_si, 1e-4);
    EXPECT_NEAR(si.q.back(), inflow, 1e-4);
    EXPECT_LT(si.tailRange(120), 1e-6); // steady over the last 10 minutes
}

TEST(SemiImplicitStructureLaws, SubmergedWeirRunsLinearlyBelowSmoothingHead) {
    const double inflow = 2.0;
    // Semi-implicit law below d: q = q_legacy(at a head difference of d) * dh / d.
    const auto ramp = [](double dh) {
        const double H1 = STAGE + dh;
        return weirFree(H1) * villemonte((H1 - D_SMOOTH) / H1, 1.5) * dh / D_SMOOTH;
    };
    const double dh_si = solve(ramp, inflow, 0.0, D_SMOOTH);
    const double dh_legacy =
        solve([](double x) { return weirLegacy(STAGE + x, STAGE); }, inflow, 0.0, D_SMOOTH);
    ASSERT_GT(dh_si - dh_legacy, 0.01);

    const Trace si = run("weir_below_d_si", weirDeck("SEMI_IMPLICIT", inflow), false);
    EXPECT_NEAR(si.y.back() - STAGE, dh_si, 1e-4);
    EXPECT_NEAR(si.q.back(), inflow, 1e-4);
    // dQ/dH is the ramp's slope, q at d over d (legacy: 1.5 * q_free / H1, some five times less).
    const double H1 = si.y.back();
    const double slope = weirFree(H1) * villemonte((H1 - D_SMOOTH) / H1, 1.5) / D_SMOOTH;
    EXPECT_NEAR(si.dqdh, slope, 1e-4 * slope);

    const Trace ex = run("weir_below_d_ex", weirDeck("EXPLICIT", inflow), false);
    EXPECT_NEAR(ex.y.back() - STAGE, dh_legacy, 1e-4);
}

TEST(SemiImplicitStructureLaws, SubmergedWeirSlopeIncludesSubmergence) {
    const double inflow = 6.0;
    // Above d the law is legacy's: both continuity options settle at the same depth.
    const double dh =
        solve([](double x) { return weirLegacy(STAGE + x, STAGE); }, inflow, D_SMOOTH, 1.0);
    ASSERT_GT(dh, D_SMOOTH);

    const Trace si = run("weir_above_d_si", weirDeck("SEMI_IMPLICIT", inflow), false);
    const Trace ex = run("weir_above_d_ex", weirDeck("EXPLICIT", inflow), false);
    EXPECT_NEAR(si.y.back() - STAGE, dh, 1e-4);
    EXPECT_NEAR(ex.y.back() - STAGE, dh, 1e-4);

    // How fast the flow falls as the tailwater rises, by central difference of the law.
    const double H1 = si.y.back();
    const double eps = 1e-6;
    const double dq_dh2 = (weirLegacy(H1, STAGE - eps) - weirLegacy(H1, STAGE + eps)) / (2 * eps);
    // Semi-implicit: dQ/dH covers it. Legacy (EXPLICIT): 1.5 * q_free / H1, before the factor,
    // which on this fixture is about two thirds of it.
    EXPECT_GE(si.dqdh, 0.999 * dq_dh2);
    const double H1_ex = ex.y.back();
    EXPECT_NEAR(ex.dqdh, 1.5 * weirFree(H1_ex) / H1_ex, 1e-6 * ex.dqdh);
    EXPECT_LT(ex.dqdh, 0.8 * dq_dh2);
}

TEST(SemiImplicitStructureLaws, ConstantRateOutletRampsBelowSmoothingHead) {
    const double inflow = 0.4; // below the 1 cfs rating: legacy cycles between 1 cfs and none
    const Trace si = run("outlet_si", outletDeck("SEMI_IMPLICIT", inflow), false);
    EXPECT_NEAR(si.y.back(), D_SMOOTH * inflow / 1.0, 1e-5);
    EXPECT_NEAR(si.q.back(), inflow, 1e-4);
    EXPECT_NEAR(si.dqdh, 1.0 / D_SMOOTH, 1e-9);
    EXPECT_LT(si.tailRange(120), 1e-6);
}

// The semi-implicit laws must not reach EXPLICIT runs: the engine keeps matching the legacy
// solver step for step on every fixture.
TEST(SemiImplicitStructureLaws, ExplicitMatchesLegacySolver) {
    const std::vector<std::pair<std::string, std::string>> decks = {
        {"gate_orifice_weir_regime", orificeDeck("EXPLICIT", 1.25)},
        {"gate_orifice_jump", orificeDeck("EXPLICIT", 2.2)},
        {"gate_weir_below_d", weirDeck("EXPLICIT", 2.0)},
        {"gate_weir_above_d", weirDeck("EXPLICIT", 6.0)},
        {"gate_outlet", outletDeck("EXPLICIT", 0.4)},
    };
    for (const auto& [name, text] : decks) {
        SCOPED_TRACE(name);
        const Trace eng = run(name, text, false);
        const Trace leg = run(name, text, true);
        // Legacy's last swmm_step routes and returns elapsed = 0; the engine returns 0 from one
        // more call, so its trace is one entry longer.
        ASSERT_LE(leg.y.size(), eng.y.size());
        ASSERT_LE(eng.y.size(), leg.y.size() + 1);
        for (std::size_t i = 0; i < leg.y.size(); ++i) {
            ASSERT_EQ(eng.y[i], leg.y[i]) << "step " << i;
            ASSERT_EQ(eng.q[i], leg.q[i]) << "step " << i;
        }
    }
}

} // namespace
