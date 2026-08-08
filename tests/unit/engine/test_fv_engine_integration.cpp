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

    EXPECT_LT(e_mid, e_coarse)
        << "refining from COARSE to dx=50 did not reduce the peak-flow error ("
        << e_coarse << " -> " << e_mid << ")";
    EXPECT_LT(e_fine, e_coarse)
        << "refining from COARSE to dx=20 did not reduce the peak-flow error ("
        << e_coarse << " -> " << e_fine << ")";
    // At the finest resolution the peaks must be genuinely close to DW.
    EXPECT_LT(e_fine, 0.20)
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
// §6.7 — junction storage must appear in the routing balance
// ===========================================================================

// The reference model has twelve nodes, and that is why this defect shipped:
// the routing mass balance excludes plain-junction storage by legacy
// convention (report_full_volume_ is zero for a junction), which the dynamic
// wave solver can afford because it never has to hold water in a junction to
// stay stable. The finite-volume solver's node IS an explicit control volume,
// so the water standing in it is real — and excluding it turns genuine storage
// into an apparent continuity error PROPORTIONAL TO JUNCTION COUNT.
//
// Measured before the fix: 0.00082 acre-feet per junction. On twelve nodes
// that rounds to 0.000 %; on five hundred it was 0.887 %. The gate therefore
// has to be a many-junction model, not a bigger tolerance on a small one.
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

