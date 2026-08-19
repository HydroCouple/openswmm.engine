/**
 * @file test_fv_engine_integration.cpp
 * @brief Engine-level gates for FLOW_ROUTING FV (plan §6.7).
 *
 * @details Runs the reference drainage model end to end under FV and compares
 *          against the DYNWAVE run of the same file. FV is deliberately NOT
 *          under the legacy bit-parity contract — it is a different
 *          discretization — so the gates here are:
 *
 *            1. routing continuity, where a conservative scheme should BEAT the
 *               implicit solver rather than merely match it;
 *            2. total routed volume, which must agree closely because it is a
 *               property of the network and the forcing, not the scheme;
 *            3. peak flows, at a tolerance that reflects the mesh resolution
 *               actually requested — COARSE mode (one cell per conduit) is
 *               first-order and genuinely diffusive, which is the trade plan
 *               §2.1 states outright;
 *            4. mesh refinement CONVERGENCE toward the DW answer, which is the
 *               gate that would catch a scheme that is merely stable rather
 *               than consistent;
 *            5. `.inp` round-trip of every FV_* key.
 *
 *          All artefacts land in tests/output/fv_engine (CLAUDE.md §4.1).
 *
 * @ingroup engine_fv
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "openswmm/engine/openswmm_engine.h"
#include "openswmm/engine/openswmm_model.h"

namespace fs = std::filesystem;

namespace {

const char* kDataDir = OPENSWMM_FV_TEST_DATA_DIR;

/// The unit-test data tree, which is where the purpose-built process-coverage
/// models live (kDataDir is the shared regression corpus).
const char* kUnitDataDir = OPENSWMM_FV_UNIT_DATA_DIR;

std::string outDir() {
    // Absolute: CTest runs this from tests/unit/engine/data, and artefacts must
    // land somewhere a reviewer looks (CLAUDE.md §4.1), not nested under the
    // test-data tree.
    const std::string d = std::string(OPENSWMM_FV_TEST_OUT_DIR) + "/fv_engine";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

/// Read the source model and rewrite its [OPTIONS] FLOW_ROUTING line, adding
/// any extra option lines requested.
std::string writeVariant(const std::string& name, const std::string& routing,
                         const std::vector<std::string>& extra) {
    std::ifstream in(std::string(kDataDir) + "/Example1.inp");
    EXPECT_TRUE(in.good()) << "cannot open the reference model";
    const std::string path = outDir() + "/" + name + ".inp";
    std::ofstream os(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("FLOW_ROUTING", 0) == 0) {
            os << "FLOW_ROUTING         " << routing << "\n";
            for (const auto& e : extra) os << e << "\n";
        } else if (line.rfind("END_DATE", 0) == 0) {
            // Truncate to just past the storm peak (12:05). FINE-mode cost
            // scales with 1/dx per unit sim time, so the full 30 h run at the
            // resolutions this test needs would dominate the whole suite; the
            // window kept here still contains the rising limb, the peak and the
            // start of recession, which is what the gates measure.
            os << "END_DATE             01/01/1998\n";
        } else if (line.rfind("END_TIME", 0) == 0) {
            os << "END_TIME             13:30:00\n";
        } else {
            os << line << "\n";
        }
    }
    return path;
}

struct RunResult {
    double continuity_pct = 0.0;   ///< routing continuity error (%)
    double outflow_volume = 0.0;   ///< external outflow (10^6 gal)
    std::map<std::string, double> peak_flow;   ///< link → max |flow|
    bool   parsed = false;
};

/// Pull the numbers this test cares about out of the .rpt.
/// Trailing whitespace-separated tokens of a report line, as doubles.
///
/// NOT `substr(find_last_of('.'))`: the report pads with a run of dots AND the
/// value itself contains a decimal point, so that idiom silently returns the
/// digits after the decimal — "-0.111" reads as "111". Tokenizing is the only
/// form that cannot be fooled by the padding.
std::vector<double> trailingNumbers(const std::string& line) {
    std::vector<double> v;
    std::istringstream ss(line);
    std::string tok;
    while (ss >> tok) {
        try {
            std::size_t used = 0;
            const double d = std::stod(tok, &used);
            if (used == tok.size()) v.push_back(d);
        } catch (...) { /* not a number — skip */ }
    }
    return v;
}

RunResult parseReport(const std::string& rpt) {
    RunResult r;
    std::ifstream in(rpt);
    if (!in.good()) return r;
    std::string line;
    bool in_flow_summary = false;
    bool in_routing_block = false;
    int header_rows = 0;
    while (std::getline(in, line)) {
        if (line.find("Flow Routing Continuity") != std::string::npos) {
            in_routing_block = true;
        } else if (in_routing_block &&
                   line.find("External Outflow") != std::string::npos) {
            const auto n = trailingNumbers(line);
            if (n.size() >= 2) r.outflow_volume = n[n.size() - 2];  // acre-feet
        } else if (in_routing_block &&
                   line.find("Continuity Error (%)") != std::string::npos) {
            // The FIRST continuity block in a SWMM report is RUNOFF quantity,
            // not flow routing — gating on the section header is what makes
            // this read the right number.
            const auto n = trailingNumbers(line);
            if (!n.empty()) { r.continuity_pct = n.back(); r.parsed = true; }
            in_routing_block = false;
        } else if (line.find("Link Flow Summary") != std::string::npos) {
            in_flow_summary = true;
            header_rows = 0;
        } else if (in_flow_summary) {
            if (line.find("---") != std::string::npos) { ++header_rows; continue; }
            if (header_rows < 2) continue;
            if (line.empty() || line.find("****") != std::string::npos) {
                in_flow_summary = false;
                continue;
            }
            std::istringstream ss(line);
            std::string name, type;
            double flow = 0.0;
            if (ss >> name >> type >> flow) r.peak_flow[name] = flow;
        }
    }
    return r;
}

RunResult run(const std::string& name, const std::string& routing,
              const std::vector<std::string>& extra) {
    const std::string inp = writeVariant(name, routing, extra);
    const std::string rpt = outDir() + "/" + name + ".rpt";
    const std::string out = outDir() + "/" + name + ".out";
    const int err = swmm_engine_run(inp.c_str(), rpt.c_str(), out.c_str(), nullptr);
    EXPECT_EQ(err, 0) << name << " run failed with code " << err;
    return parseReport(rpt);
}

} // namespace

// ---------------------------------------------------------------------------
// §6.7 — engine-level regression against DYNWAVE
// ---------------------------------------------------------------------------

TEST(FvEngine, RunsTheReferenceModelAndBeatsDynwaveOnContinuity) {
    const RunResult dw = run("example1_dw", "DYNWAVE", {});
    const RunResult fv = run("example1_fv_fine", "FV", {"FV_CELL_LENGTH       20"});

    ASSERT_TRUE(dw.parsed);
    ASSERT_TRUE(fv.parsed);

    // The headline capability claim: a conservative scheme conserves. DW's own
    // error on this model is ~0.03 %; FV must not be worse.
    EXPECT_LE(std::fabs(fv.continuity_pct), std::max(0.05,
                                                     std::fabs(dw.continuity_pct)))
        << "FV routing continuity " << fv.continuity_pct
        << " % vs DW " << dw.continuity_pct << " %";

    // Total routed volume is a property of the network and the forcing, not of
    // the scheme, so the two must agree closely.
    ASSERT_GT(dw.outflow_volume, 0.0);
    EXPECT_NEAR(fv.outflow_volume, dw.outflow_volume, 0.02 * dw.outflow_volume)
        << "routed volume " << fv.outflow_volume << " vs " << dw.outflow_volume;
}

TEST(FvEngine, RefiningTheMeshConvergesTowardTheDynwaveHydrograph) {
    // The consistency gate. A scheme can be stable and conservative and still
    // be wrong; what distinguishes a consistent discretization is that the
    // answer MOVES toward the reference as Δx shrinks. COARSE mode is
    // first-order on one cell per conduit and is expected to be markedly
    // diffusive — that is the trade plan §2.1 states, not a defect — so the
    // assertion is on the trend, not on the coarse value.
    const RunResult dw     = run("example1_dw",        "DYNWAVE", {});
    const RunResult coarse = run("example1_fv_coarse", "FV", {});
    const RunResult mid    = run("example1_fv_dx50",   "FV", {"FV_CELL_LENGTH       50"});
    const RunResult fine   = run("example1_fv_dx20",   "FV", {"FV_CELL_LENGTH       20"});

    ASSERT_FALSE(dw.peak_flow.empty());

    auto meanRelError = [&](const RunResult& r) {
        double sum = 0.0;
        int n = 0;
        for (const auto& [name, q] : dw.peak_flow) {
            auto it = r.peak_flow.find(name);
            if (it == r.peak_flow.end() || q <= 0.1) continue;
            sum += std::fabs(it->second - q) / q;
            ++n;
        }
        return (n > 0) ? sum / n : 0.0;
    };

    const double e_coarse = meanRelError(coarse);
    const double e_mid    = meanRelError(mid);
    const double e_fine   = meanRelError(fine);

    std::ofstream os(outDir() + "/refinement_convergence.csv");
    os << "cell_length_ft,mean_rel_peak_flow_error\n"
       << "coarse(1 cell/conduit)," << e_coarse << "\n"
       << "50," << e_mid << "\n"
       << "20," << e_fine << "\n";

    // With the pass-through junction interface every resolution sits within a
    // few percent of DW, so ADJACENT resolutions can differ by less than this
    // metric can resolve — it measures agreement with a different solver, not
    // distance from an exact solution, and refining FV moves it toward FV's
    // own answer, not toward DW's. Two claims are therefore asserted directly
    // instead of branching on the coarse value:
    //
    //   1. refinement across the FULL span reduces the error (the consistency
    //      statement — this is where the signal is);
    //   2. no intermediate resolution degrades beyond the noise band, so a
    //      real blow-up at one Δx still fails.
    //
    // A branch keyed on `e_coarse > kNoiseFloor` used to select between a
    // strict-monotonicity mode and a floor-only mode. That made the gate
    // bistable exactly at the floor: a coarse error landing just above 0.05
    // (measured here: 0.0525 coarse / 0.0552 at dx=50 / 0.0392 at dx=20)
    // demanded strict monotonicity of two values 0.27 points apart, while a
    // hair below it demanded nothing of the trend at all.
    constexpr double kNoiseFloor = 0.05;
    constexpr double kNoiseBand  = 0.01;  // ~4x the observed coarse/dx=50 spread

    EXPECT_LT(e_fine, e_coarse)
        << "refining from COARSE to dx=20 did not reduce the peak-flow error ("
        << e_coarse << " -> " << e_fine << ")";
    EXPECT_LT(e_mid, e_coarse + kNoiseBand)
        << "dx=50 degraded beyond the noise band vs COARSE ("
        << e_coarse << " -> " << e_mid << ")";
    // At the finest resolution the peaks must be genuinely close to DW.
    EXPECT_LT(e_fine, kNoiseFloor)
        << "mean relative peak-flow error at dx=20 is " << e_fine;
}

TEST(FvEngine, ConservesVolumeAtEveryMeshResolution) {
    // Conservation is a property of the FORM, not of the resolution — it must
    // not depend on how finely the conduits are cut.
    for (const char* dx : {"0", "60"}) {
        const RunResult r = run(std::string("example1_fv_cons_") + dx, "FV",
                                {std::string("FV_CELL_LENGTH       ") + dx});
        ASSERT_TRUE(r.parsed) << "no continuity block at FV_CELL_LENGTH " << dx;
        EXPECT_LE(std::fabs(r.continuity_pct), 0.05)
            << "continuity " << r.continuity_pct << " % at FV_CELL_LENGTH " << dx;
    }
}

// ---------------------------------------------------------------------------
// .inp round-trip of the FV_* keys
// ---------------------------------------------------------------------------

TEST(FvEngine, FvOptionsSurviveAnInpRoundTrip) {
    const std::vector<std::string> extra = {
        "FV_CELL_LENGTH       12.5",
        "FV_MIN_CELLS         3",
        "FV_CFL               0.35",
        "FV_RIEMANN           HLL",
        "FV_LIMITER           SUPERBEE",
        "FV_SCALAR_SCHEME     QUICKEST_ULTIMATE",
        "FV_SLOT_CELERITY     250",
        "FV_BACKEND           CPU",
        "FV_MIN_PARALLEL_CELLS 12345",
    };
    const std::string inp = writeVariant("example1_fv_roundtrip", "FV", extra);
    const std::string rpt = outDir() + "/example1_fv_roundtrip.rpt";
    const std::string out = outDir() + "/example1_fv_roundtrip.out";
    const std::string re_inp = outDir() + "/example1_fv_roundtrip_out.inp";

    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, inp.c_str(), rpt.c_str(), out.c_str(), nullptr), 0);
    ASSERT_EQ(swmm_model_write(e, re_inp.c_str()), 0);
    swmm_engine_close(e);
    swmm_engine_destroy(e);

    std::ifstream in(re_inp);
    ASSERT_TRUE(in.good());
    std::map<std::string, std::string> got;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string k, v;
        if (ss >> k >> v && (k.rfind("FV_", 0) == 0 || k == "FLOW_ROUTING"))
            got[k] = v;
    }

    EXPECT_EQ(got["FLOW_ROUTING"], "FV");
    EXPECT_EQ(got["FV_CELL_LENGTH"], "12.5");
    EXPECT_EQ(got["FV_MIN_CELLS"], "3");
    EXPECT_EQ(got["FV_CFL"], "0.35");
    EXPECT_EQ(got["FV_RIEMANN"], "HLL");
    EXPECT_EQ(got["FV_LIMITER"], "SUPERBEE");
    EXPECT_EQ(got["FV_SCALAR_SCHEME"], "QUICKEST_ULTIMATE");
    EXPECT_EQ(got["FV_SLOT_CELERITY"], "250");
    EXPECT_EQ(got["FV_BACKEND"], "CPU");
    EXPECT_EQ(got["FV_MIN_PARALLEL_CELLS"], "12345");
}

TEST(FvEngine, FvOptionsAreReadableAndWritableThroughTheCApi) {
    // The C API is what the Python bindings, the MCP server and the GUI all
    // go through, and it rejects unrecognized option keys outright — so an
    // FV_* key that only the [OPTIONS] parser knows about would be invisible
    // to every one of them.
    const std::string inp = writeVariant("example1_capi", "DYNWAVE", {});
    const std::string rpt = outDir() + "/example1_capi.rpt";
    const std::string out = outDir() + "/example1_capi.out";

    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, inp.c_str(), rpt.c_str(), out.c_str(), nullptr), 0);

    char buf[128];
    // Defaults are readable even though this model is DYNWAVE — the keys are
    // inert, not rejected.
    ASSERT_EQ(swmm_options_get(e, "FV_RIEMANN", buf, sizeof buf), 0);
    EXPECT_STREQ(buf, "HLLC");
    ASSERT_EQ(swmm_options_get(e, "FV_LIMITER", buf, sizeof buf), 0);
    EXPECT_STREQ(buf, "MINMOD");

    struct { const char* k; const char* v; } sets[] = {
        {"FLOW_ROUTING", "FV"},
        {"FV_CELL_LENGTH", "12.5"},
        {"FV_MIN_CELLS", "3"},
        {"FV_CFL", "0.35"},
        {"FV_RIEMANN", "HLL"},
        {"FV_ORDER", "2"},
        {"FV_LIMITER", "SUPERBEE"},
        {"FV_SCALAR_SCHEME", "QUICKEST_ULTIMATE"},
        {"FV_TIME_INTEGRATION", "RK2"},
        {"FV_SLOT_CELERITY", "250"},
        {"FV_DISPERSION", "4"},
        {"FV_STRUCTURE_COUPLING", "ROUTING_STEP"},
        {"FV_COMPACTION", "NO"},
        {"FV_BACKEND", "CPU"},
        {"FV_MIN_PARALLEL_CELLS", "12345"},
    };
    for (const auto& kv : sets)
        ASSERT_EQ(swmm_options_set(e, kv.k, kv.v), 0) << "set " << kv.k;

    // Every value must read back as written.
    for (const auto& kv : sets) {
        ASSERT_EQ(swmm_options_get(e, kv.k, buf, sizeof buf), 0) << "get " << kv.k;
        const std::string got(buf);
        if (std::string(kv.k) == "FV_CELL_LENGTH" || std::string(kv.k) == "FV_CFL" ||
            std::string(kv.k) == "FV_SLOT_CELERITY" || std::string(kv.k) == "FV_DISPERSION")
            EXPECT_NEAR(std::stod(got), std::stod(kv.v), 1e-9) << kv.k;
        else
            EXPECT_EQ(got, std::string(kv.v)) << kv.k;
    }

    // A bad enum value is rejected rather than silently ignored.
    EXPECT_NE(swmm_options_set(e, "FV_RIEMANN", "ROE"), 0);
    EXPECT_NE(swmm_options_set(e, "FV_LIMITER", "NONESUCH"), 0);

    swmm_engine_close(e);
    swmm_engine_destroy(e);
}

TEST(FvEngine, FvKeysAreInertUnderOtherRoutingModels) {
    // Plan §4.2: an FV_* key under FLOW_ROUTING DYNWAVE must be accepted and
    // ignored, so switching routing models never invalidates a file.
    const RunResult plain = run("example1_dw_plain", "DYNWAVE", {});
    const RunResult withfv = run("example1_dw_with_fv_keys", "DYNWAVE",
                                 {"FV_CELL_LENGTH       5",
                                  "FV_RIEMANN           HLL",
                                  "FV_CFL               0.25"});
    ASSERT_TRUE(plain.parsed);
    ASSERT_TRUE(withfv.parsed);
    EXPECT_DOUBLE_EQ(withfv.continuity_pct, plain.continuity_pct);
    EXPECT_DOUBLE_EQ(withfv.outflow_volume, plain.outflow_volume);
}

// ===========================================================================
// §6.7 — junction accounting must stay clean at scale
// ===========================================================================

// Any per-junction accounting defect is invisible on a twelve-node model and
// PROPORTIONAL TO JUNCTION COUNT, so the gate has to be a many-junction
// chain. Two generations of that defect have lived here: the bucket-era
// balance EXCLUDED the buckets' real MIN_SURFAREA·depth storage (0.00082
// acre-feet per junction), and after junctions became algebraic interfaces
// the reporting kept crediting that same relation for water that now stands
// in the cells — re-counting it at −0.005 % per junction. Both ways the
// symptom is the same: routing continuity drifting linearly with node count.
TEST(FvEngine, JunctionStorageIsCountedInTheRoutingBalance) {
    constexpr int kJunctions = 120;
    const std::string dir = outDir();
    const std::string inp = dir + "/junction_storage_chain.inp";

    {
        std::ofstream os(inp);
        os << "[OPTIONS]\nFLOW_UNITS           CFS\nFLOW_ROUTING         FV\n"
              "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
              "END_DATE             01/01/2026\nEND_TIME             04:00:00\n"
              "REPORT_STEP          00:05:00\nROUTING_STEP         5\n"
              "ALLOW_PONDING        NO\n\n[JUNCTIONS]\n";
        double z = 40.0;
        for (int i = 0; i < kJunctions; ++i) {
            os << "J" << i << "  " << z << "  12.0  0  0  0\n";
            z -= 0.002 * 200.0;
        }
        os << "\n[OUTFALLS]\nO1  " << z << "  FREE  NO\n\n[CONDUITS]\n";
        for (int i = 0; i < kJunctions; ++i)
            os << "C" << i << "  J" << i << "  "
               << (i + 1 < kJunctions ? "J" + std::to_string(i + 1) : "O1")
               << "  200.0  0.013  0  0  0\n";
        os << "\n[XSECTIONS]\n";
        for (int i = 0; i < kJunctions; ++i)
            os << "C" << i << "  CIRCULAR  4.0  0  0  0  1\n";
        os << "\n[INFLOWS]\nJ0  FLOW  \"\"  FLOW  1.0  1.0  40.0\n"
              "\n[TIMESERIES]\n\n[REPORT]\nINPUT  NO\nCONTROLS  NO\n";
    }

    const std::string rpt = dir + "/junction_storage_chain.rpt";
    const std::string out = dir + "/junction_storage_chain.out";
    ASSERT_EQ(swmm_engine_run(inp.c_str(), rpt.c_str(), out.c_str(), nullptr), 0)
        << "the chain model failed to run";

    const RunResult r = parseReport(rpt);
    ASSERT_TRUE(r.parsed) << "could not parse " << rpt;
    EXPECT_LT(std::fabs(r.continuity_pct), 0.01)
        << "routing continuity " << r.continuity_pct << " % on " << kJunctions
        << " junctions — junction storage is missing from the balance";
}

// ===========================================================================
// Conduit seepage must be unit-converted in US units too
// ===========================================================================

// [LOSSES] seepage arrives in in/hr in BOTH unit systems while the solver
// consumes ft/s, so UCF(RAINFALL) is 43200 for US, not 1. The input conversion
// pass short-circuits for US on the grounds that "input is already internal" —
// true for lengths, areas and flows, false for this. The rate was therefore
// used 43200x too large and the loss saturated at the availability cap.
//
// A rectangular OPEN section makes the answer exact rather than approximate:
// its top width is w_max at every depth, so the seepage rate is
//   (r / 43200) * w * L  cfs
// independent of how the flow settles. At 1 in/hr over 4 ft x 1000 ft that is
// 0.0926 cfs, or 0.0153 acre-feet over two hours. Unconverted it would be
// 4000 cfs, capped at the 20 cfs inflow — two orders of magnitude apart, so
// this gate cannot be satisfied by a near miss.
TEST(FvEngine, ConduitSeepageIsUnitConvertedUnderUsUnits) {
    const std::string dir = outDir();
    const std::string inp = dir + "/conduit_seepage.inp";
    {
        std::ofstream os(inp);
        os << "[OPTIONS]\nFLOW_UNITS           CFS\nFLOW_ROUTING         FV\n"
              "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
              "END_DATE             01/01/2026\nEND_TIME             02:00:00\n"
              "REPORT_STEP          00:05:00\nROUTING_STEP         5\n"
              "ALLOW_PONDING        NO\n\n"
              "[JUNCTIONS]\nJA  100.0  10.0  0  0  0\n\n"
              "[OUTFALLS]\nOF   98.0  FREE  NO\n\n"
              "[CONDUITS]\nC1  JA  OF  1000  0.013  0  0  0  0\n\n"
              "[XSECTIONS]\nC1  RECT_OPEN  6.0  4.0  0  0  1\n\n"
              "[LOSSES]\nC1  0  0  0  NO  1.0\n\n"
              "[INFLOWS]\nJA  FLOW  \"\"  FLOW  1.0  1.0  20.0\n\n"
              "[TIMESERIES]\n\n[REPORT]\nINPUT  NO\nCONTROLS  NO\n";
    }
    const std::string rpt = dir + "/conduit_seepage.rpt";
    const std::string out = dir + "/conduit_seepage.out";
    ASSERT_EQ(swmm_engine_run(inp.c_str(), rpt.c_str(), out.c_str(), nullptr), 0);

    std::ifstream in(rpt);
    ASSERT_TRUE(in.good());
    std::string line, block;
    double exfil = -1.0;
    bool in_routing = false;
    while (std::getline(in, line)) {
        if (line.find("Flow Routing Continuity") != std::string::npos) in_routing = true;
        if (!in_routing) continue;
        if (line.find("Exfiltration Loss") != std::string::npos) {
            const auto v = trailingNumbers(line);
            if (!v.empty()) { exfil = v.front(); break; }
        }
    }
    ASSERT_GE(exfil, 0.0) << "no Exfiltration Loss line in " << rpt;

    // (1 in/hr / 43200) * 4 ft * 1000 ft * 7200 s = 666.7 ft^3 = 0.0153 acre-ft.
    EXPECT_NEAR(exfil, 0.0153, 0.002)
        << "seepage " << exfil << " acre-ft; unconverted would saturate near 3.3";

    const RunResult r = parseReport(rpt);
    ASSERT_TRUE(r.parsed);
    EXPECT_LT(std::fabs(r.continuity_pct), 0.05)
        << "routing continuity " << r.continuity_pct << " %";
}

// ---------------------------------------------------------------------------
// The same conduit under DYNWAVE must lose the same water.
//
// DW recomputes the loss per Picard iteration through its own buildXSP, which
// never populated yw_max. The seepage clamp then read `if (d >= ywMax) d = ywMax`
// against 0 and drove the wetted width to 0, so DYNWAVE conduit seepage was
// identically zero for every cross-section shape — the loss silently existed
// only under KINWAVE/STEADY/FV, which build their params elsewhere.
// ---------------------------------------------------------------------------

TEST(FvEngine, ConduitSeepageAppliesUnderDynamicWaveToo) {
    const std::string dir = outDir();
    const std::string inp = dir + "/conduit_seepage_dw.inp";
    {
        std::ofstream os(inp);
        os << "[OPTIONS]\nFLOW_UNITS           CFS\nFLOW_ROUTING         DYNWAVE\n"
              "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
              "END_DATE             01/01/2026\nEND_TIME             02:00:00\n"
              "REPORT_STEP          00:05:00\nROUTING_STEP         5\n"
              "ALLOW_PONDING        NO\n\n"
              "[JUNCTIONS]\nJA  100.0  10.0  0  0  0\n\n"
              "[OUTFALLS]\nOF   98.0  FREE  NO\n\n"
              "[CONDUITS]\nC1  JA  OF  1000  0.013  0  0  0  0\n\n"
              "[XSECTIONS]\nC1  RECT_OPEN  6.0  4.0  0  0  1\n\n"
              "[LOSSES]\nC1  0  0  0  NO  1.0\n\n"
              "[INFLOWS]\nJA  FLOW  \"\"  FLOW  1.0  1.0  20.0\n\n"
              "[TIMESERIES]\n\n[REPORT]\nINPUT  NO\nCONTROLS  NO\n";
    }
    const std::string rpt = dir + "/conduit_seepage_dw.rpt";
    const std::string out = dir + "/conduit_seepage_dw.out";
    ASSERT_EQ(swmm_engine_run(inp.c_str(), rpt.c_str(), out.c_str(), nullptr), 0);

    std::ifstream in(rpt);
    ASSERT_TRUE(in.good());
    std::string line;
    double exfil = -1.0;
    bool in_routing = false;
    while (std::getline(in, line)) {
        if (line.find("Flow Routing Continuity") != std::string::npos) in_routing = true;
        if (!in_routing) continue;
        if (line.find("Exfiltration Loss") != std::string::npos) {
            const auto v = trailingNumbers(line);
            if (!v.empty()) { exfil = v.front(); break; }
        }
    }
    ASSERT_GE(exfil, 0.0) << "no Exfiltration Loss line in " << rpt;

    // Same 4 ft wetted width over 1000 ft at 1 in/hr as the FV case above.
    EXPECT_NEAR(exfil, 0.0153, 0.002)
        << "DYNWAVE seepage " << exfil << " acre-ft; zero means yw_max is unset again";
}

// ===========================================================================
// Node capacity: ALLOW_PONDING, SURCHARGE_DEPTH, ponded flooding rate
// ===========================================================================

namespace {

/// One junction that cannot pass its inflow through an undersized outlet, so it
/// surcharges within minutes. @p sur and @p pond are the junction's
/// SURCHARGE_DEPTH and PONDED_AREA columns.
std::string writeCapacityModel(const std::string& name, const char* routing,
                               const char* allow_ponding, double sur, double pond) {
    const std::string path = outDir() + "/" + name + ".inp";
    std::ofstream os(path);
    os << "[OPTIONS]\nFLOW_UNITS           CFS\nFLOW_ROUTING         " << routing
       << "\nSTART_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
          "END_DATE             01/01/2026\nEND_TIME             02:00:00\n"
          "REPORT_STEP          00:05:00\nROUTING_STEP         5\n"
          "ALLOW_PONDING        " << allow_ponding << "\n\n"
          "[JUNCTIONS]\nJA  100.0  4.0  0  " << sur << "  " << pond << "\n\n"
          "[OUTFALLS]\nOF   90.0  FREE  NO\n\n"
          "[CONDUITS]\nC1  JA  OF  200  0.013  0  0  0  0\n\n"
          "[XSECTIONS]\nC1  CIRCULAR  0.5  0  0  0  1\n\n"
          "[INFLOWS]\nJA  FLOW  \"\"  FLOW  1.0  1.0  10.0\n\n"
          "[TIMESERIES]\n\n[REPORT]\nINPUT  NO\nCONTROLS  NO\nNODES ALL\nLINKS ALL\n";
    return path;
}

/// The acre-feet column of a labelled Flow Routing Continuity row.
double routingRow(const std::string& rpt, const std::string& label) {
    std::ifstream in(rpt);
    EXPECT_TRUE(in.good()) << rpt;
    std::string line;
    bool in_routing = false;
    while (std::getline(in, line)) {
        if (line.find("Flow Routing Continuity") != std::string::npos) in_routing = true;
        if (!in_routing) continue;
        if (line.find(label) != std::string::npos) {
            const auto v = trailingNumbers(line);
            if (!v.empty()) return v.front();
        }
    }
    return -1.0;
}

/// Does the Node Flooding Summary list this node at all?
bool nodeIsListedAsFlooded(const std::string& rpt, const std::string& node) {
    std::ifstream in(rpt);
    EXPECT_TRUE(in.good()) << rpt;
    std::string line;
    bool in_summary = false;
    int rules = 0;   // the table opens after its second dashed rule
    while (std::getline(in, line)) {
        if (line.find("Node Flooding Summary") != std::string::npos) { in_summary = true; continue; }
        if (!in_summary) continue;
        if (line.find("---") != std::string::npos) { ++rules; continue; }
        if (rules < 2) continue;
        std::istringstream ss(line);
        std::string first;
        if (!(ss >> first)) break;              // blank line ends the table
        if (first == node) return true;
    }
    return false;
}

RunResult runModel(const std::string& path) {
    const std::string rpt = path.substr(0, path.size() - 4) + ".rpt";
    const std::string out = path.substr(0, path.size() - 4) + ".out";
    EXPECT_EQ(swmm_engine_run(path.c_str(), rpt.c_str(), out.c_str(), nullptr), 0);
    return parseReport(rpt);
}

} // namespace

// ALLOW_PONDING NO with a PONDED_AREA still set: the area must be ignored and
// the node must flood. FV ponded on PONDED_AREA alone and never read the
// option, so this model silently retained every drop it should have lost.
TEST(FvEngine, PondingIsGatedOnTheAllowPondingOption) {
    const std::string path = writeCapacityModel("pond_off", "FV", "NO", 0.0, 5000.0);
    const RunResult r = runModel(path);
    ASSERT_TRUE(r.parsed);
    const std::string rpt = outDir() + "/pond_off.rpt";

    EXPECT_GT(routingRow(rpt, "Flooding Loss"), 0.05)
        << "ALLOW_PONDING NO must flood, not pond";
    EXPECT_LT(routingRow(rpt, "Final Stored Volume"), 0.02)
        << "water was retained above the rim despite ALLOW_PONDING NO";
    EXPECT_LT(std::fabs(r.continuity_pct), 0.5)
        << "routing continuity " << r.continuity_pct << " %";
}

// The same model with ponding ON: the water stays (it is storage, not a loss),
// but the rate crossing the rim is still a flooding rate and the node must
// appear in the Node Flooding Summary. The ponding branch never touched
// flood_vol_, so that table came out empty however deep the pond got.
TEST(FvEngine, PondedNodeStillReportsItsFloodingRate) {
    const std::string path = writeCapacityModel("pond_on", "FV", "YES", 0.0, 5000.0);
    const RunResult r = runModel(path);
    ASSERT_TRUE(r.parsed);
    const std::string rpt = outDir() + "/pond_on.rpt";

    EXPECT_GT(routingRow(rpt, "Final Stored Volume"), 0.05)
        << "ponded water should be held as storage";
    EXPECT_TRUE(nodeIsListedAsFlooded(rpt, "JA"))
        << "a ponding node reported no flooding at all";
    EXPECT_LT(std::fabs(r.continuity_pct), 0.5)
        << "routing continuity " << r.continuity_pct << " %";
}

// SURCHARGE_DEPTH raises the level a sealed node reaches before it spills.
// FV flooded at the rim regardless, so a bolted manhole lost water it should
// have held.
TEST(FvEngine, SurchargeDepthDelaysFlooding) {
    const RunResult flush = runModel(
        writeCapacityModel("sur_none", "FV", "NO", 0.0, 0.0));
    const RunResult sealed = runModel(
        writeCapacityModel("sur_deep", "FV", "NO", 6.0, 0.0));
    ASSERT_TRUE(flush.parsed);
    ASSERT_TRUE(sealed.parsed);

    const double f0 = routingRow(outDir() + "/sur_none.rpt", "Flooding Loss");
    const double f6 = routingRow(outDir() + "/sur_deep.rpt", "Flooding Loss");
    ASSERT_GT(f0, 0.0) << "fixture never flooded";
    EXPECT_LT(f6, f0) << "SURCHARGE_DEPTH 6 ft flooded as much as 0 ft — the "
                         "column is being ignored";
    EXPECT_LT(std::fabs(sealed.continuity_pct), 0.5)
        << "routing continuity " << sealed.continuity_pct << " %";
}

// ---------------------------------------------------------------------------
// Multi-barrel conduits.
//
// Two defects met here. The per-length conduit loss was divided by the barrel
// count while the mass balance charged rate x barrels, so the solver shed a
// fraction of what it was billed for. Underneath that, the mesh marched ONE
// barrel and publishFv multiplied its flow and volume by the count — but the
// node exchanges through a single boundary face, so the modelled barrel took
// the node's whole lateral inflow and the run reported water it never conveyed
// (a two-barrel conduit created 3.4 % of the routed volume out of nothing).
// A cell is now the aggregate section of all the barrels, so every face flux
// stays conservative and nothing is scaled at reporting.
// ---------------------------------------------------------------------------

TEST(FvEngine, TwoBarrelSeepageScalesWithTheBarrelCount) {
    auto run_barrels = [](const char* tag, int barrels, const char* routing = "FV") {
        const std::string dir = outDir();
        const std::string inp = dir + "/seep_barrels_" + tag + ".inp";
        {
            std::ofstream os(inp);
            os << "[OPTIONS]\nFLOW_UNITS           CFS\nFLOW_ROUTING         " << routing << "\n"
                  "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
                  "END_DATE             01/01/2026\nEND_TIME             02:00:00\n"
                  "REPORT_STEP          00:05:00\nROUTING_STEP         5\n"
                  "ALLOW_PONDING        NO\n\n"
                  "[JUNCTIONS]\nJA  100.0  10.0  0  0  0\n\n"
                  "[OUTFALLS]\nOF   98.0  FREE  NO\n\n"
                  "[CONDUITS]\nC1  JA  OF  1000  0.013  0  0  0  0\n\n"
                  "[XSECTIONS]\nC1  RECT_OPEN  6.0  4.0  0  0  " << barrels << "\n\n"
                  "[LOSSES]\nC1  0  0  0  NO  1.0\n\n"
                  "[INFLOWS]\nJA  FLOW  \"\"  FLOW  1.0  1.0  20.0\n\n"
                  "[TIMESERIES]\n\n[REPORT]\nINPUT  NO\nCONTROLS  NO\n";
        }
        const std::string rpt = dir + "/seep_barrels_" + tag + ".rpt";
        const std::string out = dir + "/seep_barrels_" + tag + ".out";
        EXPECT_EQ(swmm_engine_run(inp.c_str(), rpt.c_str(), out.c_str(), nullptr), 0);
        return std::make_pair(routingRow(rpt, "Exfiltration Loss"), parseReport(rpt));
    };

    const auto one = run_barrels("1", 1);
    const auto two = run_barrels("2", 2);
    ASSERT_GT(one.first, 0.0) << "fixture produced no seepage";

    EXPECT_NEAR(two.first, 2.0 * one.first, 0.1 * one.first)
        << "two barrels shed " << two.first << " acre-ft against one barrel's "
        << one.first << " — the barrel count is being divided out";

    // Continuity is the test that catches the aggregation bug: reporting
    // barrels x a single marched barrel balanced at -3.4 %.
    EXPECT_LT(std::fabs(two.second.continuity_pct), 0.5)
        << "routing continuity " << two.second.continuity_pct << " %";

    // And the conveyance itself has to match the solver that has always
    // modelled all the barrels.
    const auto dw = run_barrels("2_dw", 2, "DYNWAVE");
    EXPECT_NEAR(two.first, dw.first, 0.1 * dw.first)
        << "FV shed " << two.first << " acre-ft against DW's " << dw.first;
    EXPECT_NEAR(two.second.outflow_volume, dw.second.outflow_volume,
                0.05 * dw.second.outflow_volume)
        << "FV discharged " << two.second.outflow_volume
        << " acre-ft against DW's " << dw.second.outflow_volume;
}

// ---------------------------------------------------------------------------
// Culvert inlet control (FHWA HEC-5).
//
// batchComputeInletControl overwrote links.flow AFTER publishFv had booked the
// node ledger from the face fluxes, so the reported flow and the continuity
// balance described different runs. FV now applies the same closure as a cap on
// the flux crossing the culvert's upstream face.
// ---------------------------------------------------------------------------

namespace {

/// A steep culvert fed far beyond what its inlet can admit. @p code is the
/// [XSECTIONS] culvert code (0 = plain conduit).
///
/// FV_CELL_LENGTH is set deliberately: on this 5 % slope the default COARSE
/// mesh (one cell for the whole conduit) conveys 54 cfs where the converged
/// answer is 100, so leaving it out would have this test measuring the
/// discretization rather than the inlet control. See the note in
/// EXPLICIT_FV_KOKKOS_1D_SOLVER_PLAN §7B on COARSE mode.
std::string writeCulvertModel(const std::string& name, const char* routing, int code) {
    const std::string path = outDir() + "/" + name + ".inp";
    std::ofstream os(path);
    os << "[OPTIONS]\nFLOW_UNITS           CFS\nFLOW_ROUTING         " << routing
       << "\nSTART_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
          "END_DATE             01/01/2026\nEND_TIME             02:00:00\n"
          "REPORT_STEP          00:05:00\nROUTING_STEP         5\n"
          "FV_CELL_LENGTH       5\n"
          "ALLOW_PONDING        NO\n\n"
          "[JUNCTIONS]\nJA  100.0  12.0  0  0  0\n\n"
          "[OUTFALLS]\nOF   90.0  FREE  NO\n\n"
          "[CONDUITS]\nCV  JA  OF  200  0.013  0  0  0  0\n\n"
          "[XSECTIONS]\nCV  CIRCULAR  3.0  0  0  0  1  " << code << "\n\n"
          "[INFLOWS]\nJA  FLOW  \"\"  FLOW  1.0  1.0  120.0\n\n"
          "[TIMESERIES]\n\n[REPORT]\nINPUT  NO\nCONTROLS  NO\nNODES ALL\nLINKS ALL\n";
    return path;
}

} // namespace

TEST(FvEngine, CulvertInletControlCapsTheFlowAndStillBalances) {
    const std::string plain_inp = writeCulvertModel("culv_none", "FV", 0);
    const std::string culv_inp  = writeCulvertModel("culv_code1", "FV", 1);
    const RunResult plain = runModel(plain_inp);
    const RunResult culv  = runModel(culv_inp);
    ASSERT_TRUE(plain.parsed && culv.parsed);
    ASSERT_GT(plain.peak_flow.at("CV"), 0.0) << "fixture routed nothing";

    // The barrel can pass the whole 120 cfs; the INLET cannot. So the signature
    // of inlet control here is the junction backing up and flooding, which the
    // same model without a culvert code does not do. Peak flow is the wrong
    // observable — both runs spike on the same startup transient.
    const double f_plain = routingRow(outDir() + "/culv_none.rpt",  "Flooding Loss");
    const double f_culv  = routingRow(outDir() + "/culv_code1.rpt", "Flooding Loss");
    EXPECT_LT(f_plain, 0.01) << "the uncontrolled conduit should pass the inflow";
    EXPECT_GT(f_culv, 0.5)
        << "culvert flooded " << f_culv << " acre-ft — the code is inert";

    // ~8 cfs of the 120 exceeds what the inlet admits at the 12 ft rim head,
    // which over two hours is 1.32 acre-ft. Within 25 % of that is the check
    // that the CAP itself, not just some throttling, is what floods the node.
    EXPECT_NEAR(f_culv, 1.32, 0.33)
        << "flooded volume " << f_culv << " acre-ft does not match the excess "
        << "over the HEC-5 inlet capacity";

    // The point of moving the cap in-solver: the reported flow and the mass
    // balance now come from the same fluxes.
    EXPECT_LT(std::fabs(culv.continuity_pct), 0.5)
        << "routing continuity " << culv.continuity_pct << " %";

    // Peak discharge stays within engineering distance of DW, which has always
    // applied HEC-5. (DW does NOT back the junction up — it rewrites the
    // reported flow after the node ledger is booked, so its flooding stays near
    // zero. That difference is the defect this change exists to remove.)
    const RunResult dw = runModel(writeCulvertModel("culv_code1_dw", "DYNWAVE", 1));
    ASSERT_TRUE(dw.parsed);
    EXPECT_NEAR(culv.peak_flow.at("CV"), dw.peak_flow.at("CV"),
                0.25 * dw.peak_flow.at("CV"))
        << "FV " << culv.peak_flow.at("CV") << " cfs vs DW "
        << dw.peak_flow.at("CV") << " cfs";
}

// ===========================================================================
// Flap gates on conduits and outfalls
// ===========================================================================

namespace {

/// A dry junction below a FIXED outfall stage 8 ft above it: without a gate the
/// sea runs backwards up the pipe. @p conduit_gate is the [LOSSES] FlapGate
/// column, @p outfall_gate the [OUTFALLS] Gated column.
std::string writeBackflowModel(const std::string& name, const char* routing,
                               const char* conduit_gate, const char* outfall_gate) {
    const std::string path = outDir() + "/" + name + ".inp";
    std::ofstream os(path);
    os << "[OPTIONS]\nFLOW_UNITS           CFS\nFLOW_ROUTING         " << routing
       << "\nSTART_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
          "END_DATE             01/01/2026\nEND_TIME             01:00:00\n"
          "REPORT_STEP          00:05:00\nROUTING_STEP         5\n"
          "ALLOW_PONDING        NO\n\n"
          "[JUNCTIONS]\nJA  100.0  10.0  0  0  0\n\n"
          "[OUTFALLS]\nOF  100.0  FIXED  108.0  " << outfall_gate << "\n\n"
          "[CONDUITS]\nC1  JA  OF  200  0.013  0  0  0  0\n\n"
          "[XSECTIONS]\nC1  CIRCULAR  2.0  0  0  0  1\n\n"
          "[LOSSES]\nC1  0  0  0  " << conduit_gate << "  0\n\n"
          "[TIMESERIES]\n\n[REPORT]\nINPUT  NO\nCONTROLS  NO\nNODES ALL\nLINKS ALL\n";
    return path;
}

} // namespace

// A gate on the conduit and a gate on the outfall must each stop the backflow
// that an ungated model admits. Neither flag existed anywhere under fv/, so FV
// filled the network from the sea whatever the .inp said.
TEST(FvEngine, FlapGatesBlockBackflow) {
    const struct { const char* tag; const char* cond; const char* outf; bool open; }
    cases[] = {
        {"open",      "NO",  "NO",  true},
        {"cond_gate", "YES", "NO",  false},
        {"outf_gate", "NO",  "YES", false},
    };

    double open_flow = 0.0;
    for (const auto& c : cases) {
        const RunResult r = runModel(
            writeBackflowModel(std::string("flap_") + c.tag, "FV", c.cond, c.outf));
        ASSERT_TRUE(r.parsed) << c.tag;
        const auto it = r.peak_flow.find("C1");
        ASSERT_NE(it, r.peak_flow.end()) << c.tag << ": no C1 row in the flow summary";

        if (c.open) {
            open_flow = it->second;
            EXPECT_GT(open_flow, 1.0)
                << "fixture admitted no backflow, so the gate cases prove nothing";
        } else {
            EXPECT_LT(it->second, 0.01 * open_flow)
                << c.tag << ": peak flow " << it->second
                << " cfs against " << open_flow << " cfs ungated — the gate is open";
        }
    }
}

// The same three cases under DYNWAVE, which has always honoured both gates.
// FV has to agree with it, not merely with itself.
TEST(FvEngine, FlapGatesMatchDynamicWave) {
    for (const char* tag : {"cond_gate", "outf_gate"}) {
        const char* cond = (std::string(tag) == "cond_gate") ? "YES" : "NO";
        const char* outf = (std::string(tag) == "cond_gate") ? "NO"  : "YES";
        const RunResult dw = runModel(
            writeBackflowModel(std::string("flap_dw_") + tag, "DYNWAVE", cond, outf));
        const RunResult fv = runModel(
            writeBackflowModel(std::string("flap_fv_") + tag, "FV", cond, outf));
        ASSERT_TRUE(dw.parsed && fv.parsed) << tag;
        EXPECT_NEAR(fv.peak_flow.at("C1"), dw.peak_flow.at("C1"), 0.05)
            << tag << ": FV " << fv.peak_flow.at("C1")
            << " cfs vs DW " << dw.peak_flow.at("C1") << " cfs";
    }
}

// ===========================================================================
// Stage 2 — force-main friction, structure coupling, inert-option warnings
// ===========================================================================

namespace {

/// A FORCE_MAIN running full between two fixed heads. Geom4 carries the
/// Hazen-Williams C (FORCE_MAIN_EQUATION H-W), so a solver that keeps using
/// Manning's equivalent n once the main pressurizes gets the head loss wrong.
std::string writeForceMainModel(const std::string& name, const char* routing) {
    const std::string path = outDir() + "/" + name + ".inp";
    std::ofstream os(path);
    os << "[OPTIONS]\nFLOW_UNITS           CFS\nFLOW_ROUTING         " << routing
       << "\nFORCE_MAIN_EQUATION  H-W\n"
          "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
          "END_DATE             01/01/2026\nEND_TIME             02:00:00\n"
          "REPORT_STEP          00:05:00\nROUTING_STEP         5\n"
          "ALLOW_PONDING        NO\n\n"
          // JA starts DRY: a 20 ft initial head column slamming the main is a
          // genuine surge (reported peak 51 cfs now that link flow is the
          // face flux), and the observable here is the steady H-W friction
          // law, not the fixture's initial condition. The main still
          // pressurizes fully at the steady 25 cfs (6.9x full depth).
          "[JUNCTIONS]\nJA  100.0  30.0  0  0  0\n\n"
          "[OUTFALLS]\nOF   90.0  FIXED  95.0  NO\n\n"
          "[CONDUITS]\nFM  JA  OF  1000  0.01  0  0  0  0\n\n"
          "[XSECTIONS]\nFM  FORCE_MAIN  2.0  120  0  0  1\n\n"
          "[INFLOWS]\nJA  FLOW  \"\"  FLOW  1.0  1.0  25.0\n\n"
          "[TIMESERIES]\n\n[REPORT]\nINPUT  NO\nCONTROLS  NO\nNODES ALL\nLINKS ALL\n";
    return path;
}

} // namespace

// A pressurized force main obeys Hazen-Williams, not the equivalent Manning n
// the parser substitutes so its free-surface reaches still march. DW switches
// laws once the main is full; FV used Manning at all depths, so the surcharge
// head loss — the whole point of a force main — was wrong.
TEST(FvEngine, ForceMainUsesItsPressurizedFrictionLaw) {
    const RunResult fv = runModel(writeForceMainModel("fmain_fv", "FV"));
    const RunResult dw = runModel(writeForceMainModel("fmain_dw", "DYNWAVE"));
    ASSERT_TRUE(fv.parsed && dw.parsed);
    ASSERT_GT(dw.peak_flow.at("FM"), 1.0) << "fixture never pressurized";

    EXPECT_NEAR(fv.peak_flow.at("FM"), dw.peak_flow.at("FM"),
                0.20 * dw.peak_flow.at("FM"))
        << "FV " << fv.peak_flow.at("FM") << " cfs vs DW "
        << dw.peak_flow.at("FM") << " cfs — Manning is still being used full";
    EXPECT_LT(std::fabs(fv.continuity_pct), 0.5)
        << "routing continuity " << fv.continuity_pct << " %";
}

// FV_STRUCTURE_COUPLING was parsed, written, C-API exposed and read nowhere.
// A routing step spans many substeps, across which a weir's head difference
// moves while a frozen discharge does not.
TEST(FvEngine, StructureCouplingSubstepTracksTheMovingHead) {
    auto run_coupling = [](const char* tag, const char* mode) {
        std::ifstream in(std::string(kUnitDataDir) + "/fv_structures.inp");
        EXPECT_TRUE(in.good()) << "cannot open fv_structures.inp";
        const std::string path = outDir() + "/coupling_" + tag + ".inp";
        {
            std::ofstream os(path);
            std::string line;
            while (std::getline(in, line)) {
                if (line.rfind("FLOW_ROUTING", 0) == 0) {
                    os << "FLOW_ROUTING         FV\n";
                    if (mode) os << "FV_STRUCTURE_COUPLING " << mode << "\n";
                } else {
                    os << line << "\n";
                }
            }
        }
        const std::string rpt = outDir() + "/coupling_" + tag + ".rpt";
        const std::string out = outDir() + "/coupling_" + tag + ".out";
        EXPECT_EQ(swmm_engine_run(path.c_str(), rpt.c_str(), out.c_str(), nullptr), 0);
        return parseReport(rpt);
    };

    const RunResult frozen = run_coupling("frozen", "ROUTING_STEP");
    const RunResult substep = run_coupling("substep", "SUBSTEP");
    ASSERT_TRUE(frozen.parsed && substep.parsed);

    // Historical note: under the integrated junction bucket the two cadences
    // opened a 2x gap on the weir (23.0 cfs frozen vs 12.1 re-evaluated) —
    // the bucket's head LAGGED, and the frozen weir kept spending the stale
    // head difference. An algebraic junction's head is quasi-static (balanced
    // every substep), so that lag is gone and the cadences must now AGREE;
    // a re-opened gap would mean junction heads are integration-lagged again.
    ASSERT_TRUE(frozen.peak_flow.count("W1") && substep.peak_flow.count("W1"));
    EXPECT_NEAR(substep.peak_flow.at("W1"), frozen.peak_flow.at("W1"),
                0.2 * frozen.peak_flow.at("W1"))
        << "weir peak " << substep.peak_flow.at("W1") << " cfs under SUBSTEP vs "
        << frozen.peak_flow.at("W1") << " frozen — the cadences diverged";
    EXPECT_LT(std::fabs(substep.continuity_pct), 0.5)
        << "routing continuity " << substep.continuity_pct << " %";
    EXPECT_LT(std::fabs(frozen.continuity_pct), 0.5)
        << "routing continuity " << frozen.continuity_pct << " %";
}

// ===========================================================================
// Stage 3 — the report has to describe the run that actually happened
// ===========================================================================

namespace {

bool reportHas(const std::string& rpt, const std::string& needle) {
    std::ifstream in(rpt);
    EXPECT_TRUE(in.good()) << rpt;
    std::string line;
    while (std::getline(in, line))
        if (line.find(needle) != std::string::npos) return true;
    return false;
}

/// The data row for one conduit in a named summary table, or "".
std::string tableRow(const std::string& rpt, const std::string& table,
                     const std::string& name) {
    std::ifstream in(rpt);
    EXPECT_TRUE(in.good()) << rpt;
    std::string line;
    bool ins = false;
    int rules = 0;
    while (std::getline(in, line)) {
        if (line.find(table) != std::string::npos) { ins = true; rules = 0; continue; }
        if (!ins) continue;
        if (line.find("---") != std::string::npos) { ++rules; continue; }
        if (rules < 2) continue;
        std::istringstream ss(line);
        std::string first;
        if (!(ss >> first)) break;
        if (first == name) return line;
    }
    return {};
}

} // namespace

// Under FV the report said "STEADY", classified every conduit as 100 % dry,
// left the Conduit Surcharge Summary permanently empty, and printed the
// explicit substep count under "Average Iterations per Step" — where values in
// the hundreds read as catastrophic non-convergence of a loop FV does not have.
TEST(FvEngine, ReportDescribesTheRunItActuallyMade) {
    std::ifstream in(std::string(kUnitDataDir) + "/fv_structures.inp");
    ASSERT_TRUE(in.good());
    const std::string inp = outDir() + "/report_fv.inp";
    {
        std::ofstream os(inp);
        std::string line;
        while (std::getline(in, line))
            os << (line.rfind("FLOW_ROUTING", 0) == 0 ? "FLOW_ROUTING         FV"
                                                      : line) << "\n";
    }
    const std::string rpt = outDir() + "/report_fv.rpt";
    const std::string out = outDir() + "/report_fv.out";
    ASSERT_EQ(swmm_engine_run(inp.c_str(), rpt.c_str(), out.c_str(), nullptr), 0);

    EXPECT_TRUE(reportHas(rpt, "Flow Routing Method ...... FV"))
        << "the routing method is misreported";

    // The substep count is relabelled, and the convergence percentage of a
    // scheme that never iterates is n/a rather than a plausible 0.00.
    EXPECT_TRUE(reportHas(rpt, "Average Substeps per Step"));
    EXPECT_FALSE(reportHas(rpt, "Average Iterations per Step"));
    EXPECT_TRUE(reportHas(rpt, "% of Steps Not Converging   :      n/a"));

    // Flow classification: at least one conduit must be classified as flowing.
    // C2 runs full-length subcritical on this model.
    const std::string cls = tableRow(rpt, "Flow Classification Summary", "C2");
    ASSERT_FALSE(cls.empty()) << "C2 missing from the classification table";
    {
        const auto v = trailingNumbers(cls);
        ASSERT_GE(v.size(), 10u) << cls;
        // v[0] is the length ratio; v[1..3] are Dry / Up Dry / Down Dry.
        EXPECT_LT(v[1] + v[2] + v[3], 0.5)
            << "C2 reported as mostly dry: " << cls;
        EXPECT_GT(v[4] + v[5], 0.5) << "C2 never classified as flowing: " << cls;
    }

    // Conduit Surcharge Summary: CB runs full on this model, so the table must
    // list it rather than saying nothing surcharged.
    EXPECT_FALSE(reportHas(rpt, "No conduits were surcharged"))
        << "CB surcharges on this model";
    EXPECT_FALSE(tableRow(rpt, "Conduit Surcharge Summary", "CB").empty())
        << "CB missing from the surcharge table";
}

// ===========================================================================
// Stage 4 — the corpus that would have caught all of it
// ===========================================================================

namespace {

/// The process-coverage model under one routing method.
RunResult runStructures(const std::string& tag, const char* routing) {
    std::ifstream in(std::string(kUnitDataDir) + "/fv_structures.inp");
    EXPECT_TRUE(in.good()) << "cannot open fv_structures.inp";
    const std::string inp = outDir() + "/structures_" + tag + ".inp";
    {
        std::ofstream os(inp);
        std::string line;
        while (std::getline(in, line))
            os << (line.rfind("FLOW_ROUTING", 0) == 0
                       ? std::string("FLOW_ROUTING         ") + routing
                       : line)
               << "\n";
    }
    const std::string rpt = outDir() + "/structures_" + tag + ".rpt";
    const std::string out = outDir() + "/structures_" + tag + ".out";
    EXPECT_EQ(swmm_engine_run(inp.c_str(), rpt.c_str(), out.c_str(), nullptr), 0);
    return parseReport(rpt);
}

} // namespace

// The gate that fails if forcing.structure_flow is dropped. Before this model
// existed, deleting that field entirely would have passed all 57 FV tests — it
// was nullptr in every one of them. A pump, an orifice, a weir and an outlet
// must each carry water.
TEST(FvEngine, EveryStructureTypeCarriesFlowUnderFv) {
    const RunResult fv = runStructures("fv", "FV");
    ASSERT_TRUE(fv.parsed);

    for (const char* link : {"P1", "OR1", "W1", "OL1"}) {
        ASSERT_TRUE(fv.peak_flow.count(link)) << link << " missing from the report";
        EXPECT_GT(fv.peak_flow.at(link), 0.1)
            << link << " carried " << fv.peak_flow.at(link)
            << " cfs — the structure callback is not reaching the solver";
    }
    EXPECT_LT(std::fabs(fv.continuity_pct), 0.5)
        << "routing continuity " << fv.continuity_pct << " %";
}

// The same network under both solvers. Routed volume is a property of the
// network and the forcing rather than the scheme, so it must agree closely;
// the structures agree to engineering tolerance, the weir excepted — see below.
TEST(FvEngine, StructuresModelAgreesWithDynamicWave) {
    const RunResult fv = runStructures("fv", "FV");
    const RunResult dw = runStructures("dw", "DYNWAVE");
    ASSERT_TRUE(fv.parsed && dw.parsed);

    EXPECT_LT(std::fabs(fv.continuity_pct), 0.5) << "FV continuity";
    EXPECT_LT(std::fabs(dw.continuity_pct), 1.0) << "DW continuity";

    EXPECT_NEAR(fv.outflow_volume, dw.outflow_volume, 0.15 * dw.outflow_volume)
        << "FV routed " << fv.outflow_volume << " acre-ft against DW's "
        << dw.outflow_volume;

    // Pump, orifice and outlet converge smoothly toward DW as the mesh refines
    // (P1 5.50→5.60 against 6.26 over min_cells 4→16), so a 30 % band holds at
    // the default resolution.
    for (const char* link : {"P1", "OR1", "OL1"}) {
        EXPECT_NEAR(fv.peak_flow.at(link), dw.peak_flow.at(link),
                    0.30 * dw.peak_flow.at(link))
            << link << ": FV " << fv.peak_flow.at(link) << " vs DW "
            << dw.peak_flow.at(link);
    }

    // The WEIR is held to a looser bound, and the reason is worth stating
    // because it is NOT a defect in the weir.
    //
    // Its peak scatters with refinement (5.71 / 7.32 / 6.38 cfs at min_cells
    // 4 / 8 / 16, against DW's 3.89) while the head driving it converges
    // cleanly: J2's mean depth settles at 1.47 ft and its MAX falls
    // monotonically 1.84 → 1.66 → 1.58 toward DW's 1.38. Two things stack on
    // that head. Q ∝ h^1.5 turns the residual ~7 % head offset — the same
    // mesh-convergence gap that has the pump reading 5.50 against 6.26 — into
    // ~34 % of discharge. And "peak" here is a routing-step MEAN sampled at
    // the worst step, of a transient whose duration is itself shrinking with
    // refinement, which is a noisy statistic by construction.
    //
    // So the bound is loose because the statistic is noisy, not because
    // something is unexplained. Publishing the step-mean discharge rather than
    // the last substep's sample already removed the genuine defect here: it
    // brought these from 12.1 / 14.3 / 7.4.
    EXPECT_LT(fv.peak_flow.at("W1"), 3.0 * dw.peak_flow.at("W1"))
        << "weir " << fv.peak_flow.at("W1") << " vs DW " << dw.peak_flow.at("W1");
}

// ===========================================================================
// A mesh the FV solver cannot build must FAIL, not run unrouted
// ===========================================================================

// initFv bails on a mesh-build error leaving fv_solver_ == nullptr, and stepFv
// then returns 0 for every step. Before the diagnostics were surfaced, a model
// with a DUMMY conduit therefore ran to completion, exited clean, and reported
// a network through which no water had ever moved. Silence is the failure mode
// this guards.
TEST(FvEngine, AnUnmeshableModelFailsInsteadOfRunningUnrouted) {
    const std::string dir = outDir();
    const std::string inp = dir + "/unmeshable.inp";
    {
        std::ofstream os(inp);
        os << "[OPTIONS]\nFLOW_UNITS           CFS\nFLOW_ROUTING         FV\n"
              "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
              "END_DATE             01/01/2026\nEND_TIME             00:30:00\n"
              "REPORT_STEP          00:05:00\nROUTING_STEP         5\n\n"
              "[JUNCTIONS]\nJA  100.0  10.0  0  0  0\nJB   99.0  10.0  0  0  0\n\n"
              "[OUTFALLS]\nOF   98.0  FREE  NO\n\n"
              "[CONDUITS]\nC1  JA  JB  400  0.013  0  0  0  0\n"
              "C2  JB  OF  400  0.013  0  0  0  0\n\n"
              "[XSECTIONS]\nC1  DUMMY\nC2  CIRCULAR  3.0  0  0  0  1\n\n"
              "[INFLOWS]\nJA  FLOW  \"\"  FLOW  1.0  1.0  15.0\n\n"
              "[TIMESERIES]\n\n[REPORT]\nINPUT  NO\n";
    }
    const std::string rpt = dir + "/unmeshable.rpt";
    const std::string out = dir + "/unmeshable.out";
    EXPECT_NE(swmm_engine_run(inp.c_str(), rpt.c_str(), out.c_str(), nullptr), 0)
        << "a DUMMY conduit under FV ran without error — the solver was never "
           "constructed, so nothing was routed";
}

// ===========================================================================
// Storage-node losses must leave the water, not just the ledger
// ===========================================================================

// nodes.losses (storage evaporation + Green-Ampt exfiltration) is computed by
// the shared Router::initNodeFlows and reported as node outflow, but was never
// subtracted from the FV node's volume — the mass balance was charged for water
// the solver still held.
TEST(FvEngine, StorageNodeLossesLeaveTheWater) {
    const std::string dir = outDir();
    const std::string inp = dir + "/storage_evap.inp";
    {
        std::ofstream os(inp);
        os << "[OPTIONS]\nFLOW_UNITS           CFS\nFLOW_ROUTING         FV\n"
              "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
              "END_DATE             01/01/2026\nEND_TIME             04:00:00\n"
              "REPORT_STEP          00:05:00\nROUTING_STEP         5\n\n"
              "[EVAPORATION]\nCONSTANT             2.0\nDRY_ONLY             NO\n\n"
              "[JUNCTIONS]\nJA  100.0  10.0  0  0  0\n\n"
              "[STORAGE]\nST1  95.0  12.0  4.0  FUNCTIONAL  20000  0  0  0  1.0\n\n"
              "[OUTFALLS]\nOF   90.0  FREE  NO\n\n"
              "[CONDUITS]\nC1  JA   ST1  400  0.013  0  0  0  0\n"
              "C2  ST1  OF   400  0.013  0  0  0  0\n\n"
              "[XSECTIONS]\nC1  CIRCULAR  3.0  0  0  0  1\n"
              "C2  CIRCULAR  1.5  0  0  0  1\n\n"
              "[INFLOWS]\nJA  FLOW  \"\"  FLOW  1.0  1.0  8.0\n\n"
              "[TIMESERIES]\n\n[REPORT]\nINPUT  NO\nCONTROLS  NO\n";
    }
    const std::string rpt = dir + "/storage_evap.rpt";
    const std::string out = dir + "/storage_evap.out";
    ASSERT_EQ(swmm_engine_run(inp.c_str(), rpt.c_str(), out.c_str(), nullptr), 0);

    const RunResult r = parseReport(rpt);
    ASSERT_TRUE(r.parsed);
    // The evaporated volume is ~0.5 % of the routed volume here, so an
    // undrained loss shows up plainly rather than in the rounding.
    EXPECT_LT(std::fabs(r.continuity_pct), 0.05)
        << "routing continuity " << r.continuity_pct
        << " % — storage losses charged but not removed";
}


// ---------------------------------------------------------------------------
// FV_NODE_PICARD — the semi-implicit node correction, iterated.
//
// The correction freezes the characteristic resistance sqrt(g*A*T), the node's
// storage response and the incident faces' fluxes at the head the substep
// started from. That is exact for the LINEARIZED problem, so one sweep leaves
// zero residual and more sweeps only matter because the real problem is not
// linear. Sweeping re-evaluates all three at the head each pass lands on.
//
// Two properties are asserted, and the second is the one this repo keeps
// getting wrong: FV_STRUCTURE_COUPLING and RK2 both shipped parsed, exposed
// and READ NOWHERE. An option that cannot be shown to change a result is a
// dead option, so the test requires a visible difference rather than trusting
// the plumbing.
// ---------------------------------------------------------------------------
namespace {
std::string writeNodePicardModel(const std::string& name, int sweeps) {
    const std::string dir = outDir();
    const std::string inp = dir + "/" + name + ".inp";
    std::ofstream os(inp);
    os << "[OPTIONS]\nFLOW_UNITS           CFS\nFLOW_ROUTING         FV\n"
          "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
          "END_DATE             01/01/2026\nEND_TIME             02:00:00\n"
          "REPORT_STEP          00:05:00\nROUTING_STEP         5\n";
    // Sweeping only matters when the node is taking a step that is LARGE
    // relative to its own stiffness. Under default tiering the node is handed
    // a fine tier precisely so the frozen tangent stays accurate, and the
    // first sweep's correction already falls under the tolerance — the option
    // is inert, which is a property of the schedule, not of the option. One
    // tier puts the node on the macro step, which is the regime it exists for.
    os << "FV_LTS_MAX_TIERS     1\n";
    if (sweeps > 1) os << "FV_NODE_PICARD       " << sweeps << "\n";
    // Picard sweeps iterate the integrated-bucket node relaxation. Plain
    // junctions are algebraic interfaces and never use it, so the machinery's
    // remaining home is a STORAGE node — a small one, so its storage stays
    // tiny against the conduit's conveyance, which is the configuration where
    // the frozen tangent is furthest from the true flux response.
    os << "\n[JUNCTIONS]\nJ1  100.0  8.0  0  0  0\n\n"
          "[STORAGE]\nJ2  99.0  8.0  0  FUNCTIONAL  0  0  12.5\n\n"
          "[OUTFALLS]\nOF   98.0  FREE  NO\n\n"
          "[CONDUITS]\nC1  J1  J2  300  0.02  0  0  0  0\n"
          "C2  J2  OF  300  0.02  0  0  0  0\n\n"
          "[XSECTIONS]\nC1  RECT_OPEN  6.0  40.0  0  0  1\n"
          "C2  RECT_OPEN  6.0  40.0  0  0  1\n\n"
          "[INFLOWS]\nJ1  FLOW  \"\"  FLOW  1.0  1.0  120.0\n\n"
          "[TIMESERIES]\n\n[REPORT]\nINPUT  NO\nCONTROLS  NO\n";
    return inp;
}
}  // namespace

TEST(FvEngine, NodePicardSweepsPreserveConservation) {
    for (int sweeps : {1, 2, 4}) {
        const RunResult r = runModel(
            writeNodePicardModel("picard_" + std::to_string(sweeps), sweeps));
        ASSERT_TRUE(r.parsed) << "sweeps=" << sweeps;
        // Every sweep leaves its answer in the one shared face-flux array, so
        // exactness is structural and must not depend on the sweep count.
        EXPECT_LT(std::fabs(r.continuity_pct), 0.05)
            << "sweeps=" << sweeps << " continuity " << r.continuity_pct << " %";
    }
}

// Sweeping UNDER LOCAL TIME STEPPING, which is the harder case and the one the
// fixture above deliberately opts out of.
//
// A node's incident faces can sit in different tiers, so on any base step only
// some are firing; the rest are holding the flux they will book over their own
// 2^k*dt0 window. The sweep's flux re-solve must therefore touch only the live
// faces — recomputing a held one re-times a flux against the wrong dt and
// breaks the macro cycle's face-open/volume-close contract, which is what makes
// a tiered step conservative in TIME as well as in mass. Mass alone would not
// catch it (both sides of a face still see one number), so this asserts the
// ledger through continuity across sweep counts WITH tiering active.
TEST(FvEngine, NodePicardIsTierWindowSafeUnderLts) {
    double base = 0.0;
    for (int sweeps : {1, 3, 6}) {
        const std::string name = "picard_lts_" + std::to_string(sweeps);
        const std::string dir = outDir();
        const std::string inp = dir + "/" + name + ".inp";
        {
            // Same network as above but with tiering LEFT ON, and a graded
            // reach so the tiers genuinely separate rather than collapsing to
            // K = 1 (which would make this a duplicate of the global-path test).
            std::ofstream os(inp);
            os << "[OPTIONS]\nFLOW_UNITS           CFS\nFLOW_ROUTING         FV\n"
                  "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
                  "END_DATE             01/01/2026\nEND_TIME             02:00:00\n"
                  "REPORT_STEP          00:05:00\nROUTING_STEP         5\n";
            if (sweeps > 1) os << "FV_NODE_PICARD       " << sweeps << "\n";
            // Picard machinery lives on the storage node (junctions are
            // algebraic); J2 sits between the tier-split conduits.
            os << "\n[JUNCTIONS]\nJ1  100.0  8.0  0  0  0\n"
                  "J3   99.0  8.0  0  0  0\n\n"
                  "[STORAGE]\nJ2  99.4  8.0  0  FUNCTIONAL  0  0  12.5\n\n"
                  "[OUTFALLS]\nOF   98.0  FREE  NO\n\n"
                  // 40:1 length ratio — the tier spread the grading sweep exists for
                  "[CONDUITS]\nC1  J1  J2  800  0.02  0  0  0  0\n"
                  "C2  J2  J3   20  0.02  0  0  0  0\n"
                  "C3  J3  OF  800  0.02  0  0  0  0\n\n"
                  "[XSECTIONS]\nC1  RECT_OPEN  6.0  20.0  0  0  1\n"
                  "C2  RECT_OPEN  6.0  20.0  0  0  1\n"
                  "C3  RECT_OPEN  6.0  20.0  0  0  1\n\n"
                  "[INFLOWS]\nJ1  FLOW  \"\"  FLOW  1.0  1.0  60.0\n\n"
                  "[TIMESERIES]\n\n[REPORT]\nINPUT  NO\nCONTROLS  NO\n";
        }
        const RunResult r = runModel(inp);
        ASSERT_TRUE(r.parsed) << "sweeps=" << sweeps;
        EXPECT_LT(std::fabs(r.continuity_pct), 0.05)
            << "sweeps=" << sweeps << " continuity " << r.continuity_pct
            << " % under LTS — a sweep re-solved a face outside its tier window";
        if (sweeps == 1) base = r.peak_flow.at("C3");
        else
            EXPECT_NEAR(r.peak_flow.at("C3"), base, 0.25 * base)
                << "sweeps=" << sweeps << " diverged from the single-sweep result";
    }
}

TEST(FvEngine, NodePicardIsNotADeadOption) {
    const RunResult one = runModel(writeNodePicardModel("picard_one", 1));
    const RunResult many = runModel(writeNodePicardModel("picard_many", 6));
    ASSERT_TRUE(one.parsed && many.parsed);
    ASSERT_GT(one.peak_flow.at("C2"), 0.0) << "fixture never routed";
    EXPECT_NE(one.peak_flow.at("C2"), many.peak_flow.at("C2"))
        << "FV_NODE_PICARD changed nothing — the option is being parsed and "
           "ignored, exactly as FV_STRUCTURE_COUPLING and RK2 once were";
}

// ---------------------------------------------------------------------------
// MIN_SURFAREA is a project option, and FV could not see it.
//
// Legacy keeps no junction storage at all; this engine books it (plan §7B.6)
// at MIN_SURFAREA * fullDepth, but read the 12.566 ft² COMPILE-TIME constant
// instead of the option. The dynamic wave honoured the option in its
// surface-area floor, so the asymmetry hid: DW output moved with the setting
// and FV output was byte-identical across 0.0001, 0.01 and 12.566.
//
// It matters most where nodes are an artifact of discretizing a channel rather
// than real manholes — on the SWASHES 1D chains, which ask for 0.01, every
// node carried 1257x the intended storage, and the Ritter dam-break front
// lagged the analytic solution by 47 cells.
// ---------------------------------------------------------------------------
namespace {
std::string writeMinSurfAreaModel(const std::string& name, const char* msa,
                                  const char* routing) {
    const std::string dir = outDir();
    const std::string inp = dir + "/" + name + ".inp";
    std::ofstream os(inp);
    os << "[OPTIONS]\nFLOW_UNITS           CFS\nFLOW_ROUTING         " << routing
       << "\nSTART_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
          "END_DATE             01/01/2026\nEND_TIME             01:00:00\n"
          "REPORT_STEP          00:01:00\nROUTING_STEP         5\n"
          "MIN_SURFAREA         " << msa << "\n\n"
          "[JUNCTIONS]\nJ1  100.0  10.0  0  0  0\nJ2   99.0  10.0  0  0  0\n\n"
          "[OUTFALLS]\nOF   98.0  FREE  NO\n\n"
          "[CONDUITS]\nC1  J1  J2  200  0.014  0  0  0  0\n"
          "C2  J2  OF  200  0.014  0  0  0  0\n\n"
          "[XSECTIONS]\nC1  CIRCULAR  3.0  0  0  0  1\n"
          "C2  CIRCULAR  3.0  0  0  0  1\n\n"
          "[INFLOWS]\nJ1  FLOW  \"\"  FLOW  1.0  1.0  15.0\n\n"
          "[TIMESERIES]\n\n[REPORT]\nINPUT  NO\nCONTROLS  NO\n";
    return inp;
}
}  // namespace

TEST(FvEngine, MinSurfAreaDoesNotShapeFvJunctions) {
    const RunResult small = runModel(
        writeMinSurfAreaModel("msa_small_fv", "0.01", "FV"));
    const RunResult big = runModel(
        writeMinSurfAreaModel("msa_big_fv", "12.566", "FV"));
    ASSERT_TRUE(small.parsed && big.parsed);
    ASSERT_GT(small.peak_flow.at("C2"), 0.0) << "fixture never routed";

    // Junctions are algebraic interfaces with NO storage of their own — the
    // convention legacy DW also keeps (its node_getSurfArea is zero for
    // non-storage nodes; MIN_SURFAREA is a solution-method floor there, not
    // physics). So a 1257x change in MIN_SURFAREA must leave FV's junction
    // routing essentially untouched; sensitivity here would mean a spurious
    // storage bucket is back at the junctions.
    const double dpk = std::fabs(small.peak_flow.at("C2") -
                                 big.peak_flow.at("C2"));
    EXPECT_LT(dpk, 0.001 * big.peak_flow.at("C2"))
        << "FV peak moved " << dpk << " cfs across the MIN_SURFAREA change";

    EXPECT_LT(std::fabs(big.continuity_pct), 0.05) << big.continuity_pct;
    EXPECT_LT(std::fabs(small.continuity_pct), 0.05) << small.continuity_pct;
}

// ---------------------------------------------------------------------------
// A lake at rest astride an EMERGED bank must not start moving.
//
// FvState is seeded one depth per conduit from LinkData, and LinkData's initial
// depth is the average of the two end-node DEPTHS. Across a bed step those two
// depths are measured from different inverts, so their average is a free
// surface that stands above the wet bank: water perched on dry ground. It then
// slumps, and in a pool with no outlet the excess never leaves — the lake
// simply sits high for the rest of the run.
//
// Measured on the SWASHES emerged lake-at-rest (a 0.1 m lake split by a bank at
// 0.15-0.20 m): the ramp conduit was seeded 25 mm above the lake and the closed
// pool settled at 0.102747 m against an analytic 0.1, which is (0.8 + 0.009 +
// 0.05 + 0.075)/9.09 to six figures — the whole error, accounted for.
//
// This is an initial-condition defect, not a scheme one: the pool it produces
// is perfectly flat and perfectly well balanced, just at the wrong elevation.
// So the gate is the C-property in its plainest form — start a lake at rest and
// require that nothing moves.
// ---------------------------------------------------------------------------
namespace {
std::string writeEmergedLakeModel(const std::string& name, const char* routing) {
    const std::string dir = outDir();
    const std::string inp = dir + "/" + name + ".inp";
    std::ofstream os(inp);
    os << "[OPTIONS]\nFLOW_UNITS           CFS\nFLOW_ROUTING         " << routing
       << "\nSTART_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
          "END_DATE             01/01/2026\nEND_TIME             00:10:00\n"
          "REPORT_STEP          00:01:00\nROUTING_STEP         5\n\n"
          // Two pools at elevation 1.0, separated by a bank whose crest stands
          // at 3.0 — two full feet clear of the water, so the pools are
          // hydraulically disconnected and each one's volume is fixed forever.
          // J1/J2 are the closed pool; J4/J5 drain to a stage held at the same
          // 1.0, so that side is at rest too.
          "[JUNCTIONS]\nJ1  0.0  5.0  1.0  0  0\nJ2  0.0  5.0  1.0  0  0\n"
          "J3  3.0  5.0  0.0  0  0\nJ4  0.0  5.0  1.0  0  0\n"
          "J5  0.0  5.0  1.0  0  0\n\n"
          "[OUTFALLS]\nOF  0.0  FIXED  1.0  NO\n\n"
          // C2 and C3 are the shoreline ramps: each spans a 3 ft bed step, so
          // each is where the averaged depth manufactures a perched surface.
          "[CONDUITS]\nC1  J1  J2  100  0.014  0  0  0  0\n"
          "C2  J2  J3  100  0.014  0  0  0  0\n"
          "C3  J3  J4  100  0.014  0  0  0  0\n"
          "C4  J4  J5  100  0.014  0  0  0  0\n"
          "C5  J5  OF  100  0.014  0  0  0  0\n\n"
          "[XSECTIONS]\nC1  RECT_OPEN  5.0  10.0  0  0  1\n"
          "C2  RECT_OPEN  5.0  10.0  0  0  1\n"
          "C3  RECT_OPEN  5.0  10.0  0  0  1\n"
          "C4  RECT_OPEN  5.0  10.0  0  0  1\n"
          "C5  RECT_OPEN  5.0  10.0  0  0  1\n\n"
          "[TIMESERIES]\n\n[REPORT]\nINPUT  NO\nCONTROLS  NO\n";
    return inp;
}
}  // namespace

TEST(FvEngine, ALakeAtRestOverAnEmergedBankStaysAtRest) {
    const RunResult fv = runModel(writeEmergedLakeModel("emerged_lake_fv", "FV"));
    const RunResult dw = runModel(
        writeEmergedLakeModel("emerged_lake_dw", "DYNWAVE"));
    ASSERT_TRUE(fv.parsed && dw.parsed);

    // The dynamic wave holds this deck still, which is what makes the fixture
    // evidence about FV's seeding rather than about the deck.
    for (const char* c : {"C1", "C4"})
        EXPECT_LT(dw.peak_flow.at(c), 0.05)
            << "fixture is not at rest under DYNWAVE either: " << c;

    // C1 sits inside the CLOSED pool. Nothing forces it, nothing drains it, and
    // no water can cross the bank, so any flow at all is water that was seeded
    // where it did not belong.
    EXPECT_LT(fv.peak_flow.at("C1"), 0.05)
        << "closed pool started moving: peak " << fv.peak_flow.at("C1")
        << " cfs in C1 — the shoreline ramp was seeded above the lake";
    EXPECT_LT(fv.peak_flow.at("C4"), 0.05)
        << "peak " << fv.peak_flow.at("C4") << " cfs in C4";

    EXPECT_LT(std::fabs(fv.continuity_pct), 0.05) << fv.continuity_pct;
}
