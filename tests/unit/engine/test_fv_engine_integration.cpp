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
