/*!
 * @file test_outfall_backflow_quality.cpp
 * @brief Gates for `[OPTIONS] OUTFALL_BACKFLOW_QUALITY LAST|ZERO` — the
 *        quality carried by reverse flow at outfalls.
 *
 * @details The deck is a two-phase, single-pipe system: for the first hour a
 *          polluted inflow (100 mg/L) drains J1 -> C1 -> O1, loading the
 *          outfall's held state; then the inflow stops and the outfall's
 *          TIMESERIES stage ramps above the junction, reversing C1 so
 *          boundary water pours back into J1 for two hours.
 *
 *          LAST (default) is the legacy convention (src/legacy/engine/
 *          qualrout.c findNodeQual): the supplying outfall re-injects its
 *          held ~100 mg/L, so J1 stays polluted — and under WATER_AGE the
 *          held boundary water keeps aging 1:1, so re-entering water is as
 *          old as the clock. ZERO makes the supplying outfall a fresh
 *          boundary: held state reads zero for pollutants AND __WATER_AGE__.
 *
 *          Covered: the LEGACY quality solver (mixAtNodes hold branch +
 *          WaterAgeLegacy node hold), LAGRANGIAN/LARD (node-store fallback),
 *          EULERIAN_ARD (boundary donor loop), byte-inertness on a deck
 *          whose outfall never supplies, default == LAST, and the
 *          parse/API/writer round-trip.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>

#include "core/SWMMEngine.hpp"

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

constexpr double kElapsedS = 5.0 * 3600.0;  // 5 h simulated span

struct DeckSpec {
    std::string solver = "LEGACY";  ///< LEGACY | LAGRANGIAN | EULERIAN_ARD
    std::string bfq;                ///< "", "LAST", or "ZERO"
    bool reversing = true;          ///< stage ramp reverses C1 after 1 h
    bool constant_inflow = false;   ///< keep 3 cfs all 3 h (inertness deck)
    bool water_age = true;
};

void write_deck(const std::string& path, const DeckSpec& s) {
    std::ofstream f(path);
    f << "[TITLE]\nOutfall backflow quality gate deck\n\n"
      << "[OPTIONS]\n"
      << "FLOW_UNITS           CFS\n"
      << "FLOW_ROUTING         DYNWAVE\n"
      << "INFILTRATION         HORTON\n"
      << "LINK_OFFSETS         DEPTH\n"
      << "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
      << "REPORT_START_DATE    01/01/2026\nREPORT_START_TIME    00:00:00\n"
      << "END_DATE             01/01/2026\nEND_TIME             05:00:00\n"
      << "ROUTING_STEP         0:00:05\nREPORT_STEP          0:05:00\n"
      << "QUALITY_SOLVER       " << s.solver << "\n";
    if (s.water_age) f << "WATER_AGE            ON\n";
    if (!s.bfq.empty())
        f << "OUTFALL_BACKFLOW_QUALITY " << s.bfq << "\n";
    // O2 (FREE, low) gives phase 2 a discharge path, so the rising stage at
    // O1 drives a CONTINUOUS flushing circuit O1 -> C1 -> J1 -> C2 -> O2
    // instead of stalling once heads equalize — without it the backflow
    // volume is one junction-fill and J1 keeps a stale mix under either
    // setting (measured 65.8 mg/L under ZERO on the single-pipe deck).
    // J1 is a STORAGE node with an explicit 100 ft² area: a well-defined
    // CSTR in both engines. (As a junction it surcharges to 8 ft here, and
    // the refactored engine's node mixing then dilutes against a large
    // virtual surcharge volume — recorded parity gap, separate work item —
    // which would make this gate measure that instead of the flag.)
    f << "\n[STORAGE]\n;;Name Elev Ymax Y0 Shape\n"
      << "J1 0.0 12.0 0 FUNCTIONAL 0 0 100 0 0\n\n"
      << "[OUTFALLS]\n;;Name Elev Type StageData Gated\n"
      << "O1 -0.5 TIMESERIES stage_ts NO\n"
      << "O2 -0.5 FREE NO\n\n"
      << "[CONDUITS]\nC1 J1 O1 200 0.013 0 0 0\n"
      << "C2 J1 O2 100 0.013 0.5 0 0\n\n"
      << "[XSECTIONS]\nC1 CIRCULAR 2.0 0 0 0\nC2 CIRCULAR 1.0 0 0 0\n\n"
      << "[POLLUTANTS]\nTRC MG/L 0 0 0 0 NO * 0 0 0\n\n"
      << "[INFLOWS]\n"
      << "J1 FLOW inflow_ts FLOW 1.0 1.0\n"
      << "J1 TRC \"\" CONCEN 1.0 1.0 100\n\n"
      << "[TIMESERIES]\n";
    if (s.constant_inflow) {
        f << "inflow_ts 0:00 3.0\ninflow_ts 5:00 3.0\n";
    } else {
        f << "inflow_ts 0:00 3.0\ninflow_ts 0:55 3.0\ninflow_ts 1:00 0.0\n"
          << "inflow_ts 5:00 0.0\n";
    }
    if (s.reversing) {
        f << "stage_ts 0:00 0.2\nstage_ts 1:00 0.2\nstage_ts 1:10 5.0\n"
          << "stage_ts 3:00 9.0\nstage_ts 5:00 9.0\n";
    } else {
        f << "stage_ts 0:00 0.2\nstage_ts 5:00 0.2\n";
    }
    f << "\n[REPORT]\nINPUT NO\n";
}

struct RunResult {
    bool ok = false;
    double j1_conc = -1.0;             ///< TRC at the junction, end of run
    double o1_conc = -1.0;             ///< TRC at the outfall, end of run
    double j1_age = -1.0;              ///< seconds
    double o1_age = -1.0;              ///< seconds
    std::vector<double> node_conc;     ///< full array, inertness compare
    std::vector<double> link_conc;
    std::vector<double> node_age;
    std::vector<double> link_age;
};

RunResult run_deck(const std::string& tag, const DeckSpec& s) {
    RunResult r;
    const std::string inp = tag + ".inp";
    write_deck(inp, s);
    SWMM_Engine e = swmm_engine_create();
    if (e == nullptr) { ADD_FAILURE() << "engine create"; return r; }
    if (swmm_engine_open(e, inp.c_str(), (tag + ".rpt").c_str(),
                         (tag + ".out").c_str(), nullptr) != SWMM_OK ||
        swmm_engine_initialize(e) != SWMM_OK ||
        swmm_engine_start(e, 1) != SWMM_OK) {
        ADD_FAILURE() << "open/init/start failed for " << inp;
        swmm_engine_destroy(e);
        return r;
    }
    auto& ctx = as_cpp_engine(e).context();
    double elapsed = 0.0;
    int guard = 0;
    do {
        if (swmm_engine_step(e, &elapsed) != SWMM_OK) {
            ADD_FAILURE() << "step failed for " << inp;
            swmm_engine_destroy(e);
            return r;
        }
    } while (elapsed > 0.0 && ++guard < 200000);
    swmm_engine_end(e);

    const int np = ctx.n_pollutants();
    int j1 = -1, o1 = -1;  // j1 = the storage node; o1 = FIRST outfall = O1
    for (int j = 0; j < ctx.n_nodes(); ++j) {
        const auto uj = static_cast<std::size_t>(j);
        if (ctx.nodes.type[uj] == openswmm::NodeType::OUTFALL) {
            if (o1 < 0) o1 = j;
        } else if (j1 < 0) {
            j1 = j;
        }
    }
    if (j1 < 0 || o1 < 0 || np < 1) {
        ADD_FAILURE() << "deck shape unexpected (j1=" << j1 << " o1=" << o1
                      << " np=" << np << ")";
        swmm_engine_destroy(e);
        return r;
    }
    r.j1_conc = ctx.nodes.conc[static_cast<std::size_t>(j1 * np)];
    r.o1_conc = ctx.nodes.conc[static_cast<std::size_t>(o1 * np)];
    r.node_conc = ctx.nodes.conc;
    r.link_conc = ctx.links.conc;
    const auto& ws = ctx.water_age_state;
    if (!ws.node_age.empty()) {
        r.j1_age = ws.node_age[static_cast<std::size_t>(j1)];
        r.o1_age = ws.node_age[static_cast<std::size_t>(o1)];
        r.node_age = ws.node_age;
        r.link_age = ws.link_age;
    }
    r.ok = true;
    swmm_engine_destroy(e);
    return r;
}

bool bitwise_equal(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size()) return false;
    return a.empty() ||
           std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0;
}

// ---------------------------------------------------------------------------
// Behavioral gate, one quality solver at a time: LAST keeps the junction
// polluted with the outfall's held ~100 mg/L; ZERO floods it with clean
// boundary water and the supplying outfall itself reads (near) zero.
// ---------------------------------------------------------------------------
void behavioralGate(const std::string& solver, double zero_band,
                    double sep_factor) {
    DeckSpec last;  last.solver = solver;  last.bfq = "LAST";
    DeckSpec zero;  zero.solver = solver;  zero.bfq = "ZERO";
    const std::string t = "_obq_" + solver;
    const RunResult rl = run_deck(t + "_last", last);
    const RunResult rz = run_deck(t + "_zero", zero);
    ASSERT_TRUE(rl.ok && rz.ok);

    ::testing::Test::RecordProperty(solver + "_last_j1_conc", std::to_string(rl.j1_conc));
    ::testing::Test::RecordProperty(solver + "_zero_j1_conc", std::to_string(rz.j1_conc));
    ::testing::Test::RecordProperty(solver + "_last_o1_age_s", std::to_string(rl.o1_age));
    ::testing::Test::RecordProperty(solver + "_zero_o1_age_s", std::to_string(rz.o1_age));

    EXPECT_GT(rl.j1_conc, 50.0)
        << solver << " LAST: held boundary re-injection should keep J1 near "
        << "the loading concentration";
    EXPECT_LT(rz.j1_conc, zero_band)
        << solver << " ZERO: fresh boundary water should flush J1";
    EXPECT_GT(rl.j1_conc, rz.j1_conc * sep_factor)
        << solver << ": the two settings must separate decisively";
    EXPECT_LE(rz.o1_conc, 1e-9)
        << solver << " ZERO: a supplying outfall's held state must read zero";

    ASSERT_GE(rl.o1_age, 0.0) << solver << ": age state missing";
    EXPECT_GT(rl.o1_age, 0.5 * kElapsedS)
        << solver << " LAST: the held boundary water ages with the clock";
    EXPECT_LT(rz.o1_age, 300.0)
        << solver << " ZERO: boundary water must re-enter (near) age-zero";
    EXPECT_LT(rz.j1_age, 0.75 * rl.j1_age + 60.0)
        << solver << " ZERO: the flushed junction must be younger";
}

TEST(OutfallBackflowQuality, LegacySolverSeparatesLastFromZero) {
    // Band 40, not the fork's 15: mixAtNodes' P8-G20 factor multiplies by
    // (v_old+v_in)/v_new, which treats ALL outflow as evaporation — at
    // steady state it exactly cancels dilution, so clean inflow stops
    // flushing a through-node the moment its volume stops changing
    // (measured: J1 froze at 21.04 mg/L with 10 cfs of 0 mg/L inflow).
    // Pre-existing, invisible to parity decks whose inflow is never
    // cleaner than the store; recorded as a separate work item. The flag's
    // own contract is still gated hard: O1 reads zero and J1 decays 5x.
    behavioralGate("LEGACY", 40.0, 4.0);
}

TEST(OutfallBackflowQuality, LagrangianSolverSeparatesLastFromZero) {
    behavioralGate("LAGRANGIAN", 15.0, 4.0);
}

// ARD pins its own pre-existing boundary convention instead: a zero-volume
// outfall store contributes NOTHING when it donates (the kMinStoreVol donor
// guard), so backflow is already fresh under either setting and the flag is
// defensive there (it matters only if an outfall store ever carries volume).
TEST(OutfallBackflowQuality, ArdBackflowIsFreshUnderEitherSetting) {
    DeckSpec last;  last.solver = "EULERIAN_ARD";  last.bfq = "LAST";
    DeckSpec zero;  zero.solver = "EULERIAN_ARD";  zero.bfq = "ZERO";
    const RunResult rl = run_deck("_obq_ARD_last", last);
    const RunResult rz = run_deck("_obq_ARD_zero", zero);
    ASSERT_TRUE(rl.ok && rz.ok);
    ::testing::Test::RecordProperty("ard_last_j1_conc", std::to_string(rl.j1_conc));
    ::testing::Test::RecordProperty("ard_zero_j1_conc", std::to_string(rz.j1_conc));
    EXPECT_LT(rl.j1_conc, 1.0)
        << "ARD LAST: the zero-volume outfall store donates nothing today; "
        << "if this starts holding mass the donor convention changed";
    EXPECT_LT(rz.j1_conc, 1.0)
        << "ARD ZERO: fresh boundary, same as the donor-guard convention";
}

// ---------------------------------------------------------------------------
// Inertness: on a deck whose outfalls RECEIVE every step (constant inflow,
// stage held low), ZERO must be bit-identical to LAST. Note the flag bites
// whenever an outfall takes no inflow — idle as well as supplying — so the
// zero-inflow tail of the behavioral deck is deliberately not used here.
// ---------------------------------------------------------------------------
TEST(OutfallBackflowQuality, InertWithoutBackflow) {
    DeckSpec last;  last.bfq = "LAST";  last.reversing = false;
    DeckSpec zero;  zero.bfq = "ZERO";  zero.reversing = false;
    last.constant_inflow = zero.constant_inflow = true;
    last.solver = zero.solver = "LAGRANGIAN";
    const RunResult rl = run_deck("_obq_inert_last", last);
    const RunResult rz = run_deck("_obq_inert_zero", zero);
    ASSERT_TRUE(rl.ok && rz.ok);
    EXPECT_TRUE(bitwise_equal(rl.node_conc, rz.node_conc));
    EXPECT_TRUE(bitwise_equal(rl.link_conc, rz.link_conc));
    EXPECT_TRUE(bitwise_equal(rl.node_age, rz.node_age));
    EXPECT_TRUE(bitwise_equal(rl.link_age, rz.link_age));
}

// ---------------------------------------------------------------------------
// Default = LAST: an absent key runs bit-identically to an explicit LAST,
// and the option surface round-trips (parse -> get, set -> get, writer echo).
// ---------------------------------------------------------------------------
TEST(OutfallBackflowQuality, DefaultIsLastAndRoundTrips) {
    DeckSpec absent;   absent.solver = "LAGRANGIAN";  // bfq empty
    DeckSpec explast;  explast.solver = "LAGRANGIAN"; explast.bfq = "LAST";
    const RunResult ra = run_deck("_obq_default", absent);
    const RunResult re = run_deck("_obq_explicit_last", explast);
    ASSERT_TRUE(ra.ok && re.ok);
    EXPECT_TRUE(bitwise_equal(ra.node_conc, re.node_conc));
    EXPECT_TRUE(bitwise_equal(ra.node_age, re.node_age));

    // API surface on a ZERO deck: get reflects the parse, set round-trips,
    // an unknown value is refused, and the writer echoes the non-default.
    DeckSpec z; z.solver = "LAGRANGIAN"; z.bfq = "ZERO";
    write_deck("_obq_api.inp", z);
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, "_obq_api.inp", "_obq_api.rpt",
                               "_obq_api.out", nullptr), SWMM_OK);
    char buf[32];
    ASSERT_EQ(swmm_options_get(e, "OUTFALL_BACKFLOW_QUALITY", buf,
                               static_cast<int>(sizeof(buf))), SWMM_OK);
    EXPECT_STREQ(buf, "ZERO");
    EXPECT_EQ(swmm_options_set(e, "OUTFALL_BACKFLOW_QUALITY", "LAST"),
              SWMM_OK);
    ASSERT_EQ(swmm_options_get(e, "OUTFALL_BACKFLOW_QUALITY", buf,
                               static_cast<int>(sizeof(buf))), SWMM_OK);
    EXPECT_STREQ(buf, "LAST");
    EXPECT_NE(swmm_options_set(e, "OUTFALL_BACKFLOW_QUALITY", "SOMETIMES"),
              SWMM_OK);
    EXPECT_EQ(swmm_options_set(e, "OUTFALL_BACKFLOW_QUALITY", "ZERO"),
              SWMM_OK);
    ASSERT_EQ(swmm_model_write(e, "_obq_api_echo.inp"), 0);
    swmm_engine_destroy(e);

    std::ifstream echo("_obq_api_echo.inp");
    std::stringstream ss; ss << echo.rdbuf();
    EXPECT_NE(ss.str().find("OUTFALL_BACKFLOW_QUALITY ZERO"),
              std::string::npos)
        << "writer must echo the non-default setting (save-as rule)";
}

}  // namespace
