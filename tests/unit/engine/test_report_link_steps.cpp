/**
 * @file      test_report_link_steps.cpp
 * @brief     [REPORT] LINK_STEPS — the optional "Conduit Time Step Summary".
 *
 * @details A five-conduit chain of 400 ft pipes with one 8 ft stub (C3). The
 *          stub is the stiff element: under DW it sets the CFL step, under FV
 *          local time stepping it runs on a finer tier than the long pipes.
 *          Decks and reports are written to `link_steps_out/` in the test's
 *          working directory for review.
 *
 * @see plans/CONDUIT_TIMESTEP_REPORT_PLAN_2026-10-04.md
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>

namespace fs = std::filesystem;

namespace {

const char* kOutDir = "link_steps_out";
constexpr double kRoutingStep = 30.0;

/**
 * @brief Create the fixture output directory and form one artifact filename.
 *
 * Creates missing output-root directories and propagates filesystem errors. Does not
 * create/truncate the artifact, sanitize name or create arbitrary nested parents; no
 * current-directory change occurs.
 *
 * @param[in] name Raw filename/path appended below kOutDir; caller chooses a safe fixture
 *     destination.
 * @return Owned native path string.
 */
std::string outPath(const std::string& name) {
    fs::create_directories(kOutDir);
    return (fs::path(kOutDir) / name).string();
}

/**
 * @brief Build the five-conduit timestep-summary fixture.
 *
 * FV_LTS is enabled in authored options; no parser, file writer or solver is invoked.
 *
 * @param[in] routing Raw FLOW_ROUTING keyword inserted verbatim.
 * @param[in] report_extra Raw REPORT section rows with valid newline separation.
 * @return Owned CFS deck with four 400 ft links, one 8 ft stub, 10 cfs inflow, 30 s maximum routing
 *     step and a 30 minute run.
 */
std::string deck(const std::string& routing, const std::string& report_extra) {
    std::ostringstream s;
    s << "[OPTIONS]\n"
         "FLOW_UNITS           CFS\n"
         "FLOW_ROUTING         "
      << routing
      << "\n"
         "FV_LTS               YES\n"
         "START_DATE           01/01/2026\n"
         "START_TIME           00:00:00\n"
         "END_DATE             01/01/2026\n"
         "END_TIME             00:30:00\n"
         "REPORT_STEP          00:01:00\n"
         "ROUTING_STEP         "
      << kRoutingStep
      << "\n"
         "VARIABLE_STEP        0.75\n"
         "\n[JUNCTIONS]\n"
         "J1 100   10\nJ2 99 10\nJ3 98 10\nJ4 97.98 10\nJ5 97 10\n"
         "\n[OUTFALLS]\nO1 96 FREE\n"
         "\n[CONDUITS]\n"
         "C1 J1 J2 400 0.013 0 0\n"
         "C2 J2 J3 400 0.013 0 0\n"
         "C3 J3 J4 8   0.013 0 0\n"
         "C4 J4 J5 400 0.013 0 0\n"
         "C5 J5 O1 400 0.013 0 0\n"
         "\n[XSECTIONS]\n"
         "C1 CIRCULAR 3 0 0 0 1\nC2 CIRCULAR 3 0 0 0 1\nC3 CIRCULAR 3 0 0 0 1\n"
         "C4 CIRCULAR 3 0 0 0 1\nC5 CIRCULAR 3 0 0 0 1\n"
         "\n[INFLOWS]\n"
         "J1 FLOW \"\" FLOW 1.0 1.0 10\n"
         "\n[REPORT]\n"
      << report_extra;
    return s.str();
}

/**
 * @brief Write/run a timestep-summary deck and return its generated report text.
 *
 * Creates/opens/initializes/starts an engine with nonfatal setup expectations, steps without a
 * guard until error/completion, and then ignores end/report/close statuses before destruction.
 * Existing artifacts are overwritten and retained. A failed setup does not short-circuit later
 * operations.
 *
 * @param[in] name Safe basename under the retained link_steps_out directory.
 * @param[in] text Complete input text, written with unchecked stream status.
 * @return Owned report text, possibly empty/partial after errors; no independent run status.
 */
std::string run(const std::string& name, const std::string& text) {
    const std::string inp = outPath(name + ".inp");
    const std::string rpt = outPath(name + ".rpt");
    {
        std::ofstream f(inp);
        f << text;
    }
    SWMM_Engine e = swmm_engine_create();
    EXPECT_EQ(
        swmm_engine_open(e, inp.c_str(), rpt.c_str(), outPath(name + ".out").c_str(), nullptr), 0);
    EXPECT_EQ(swmm_engine_initialize(e), 0);
    EXPECT_EQ(swmm_engine_start(e, 0), 0);
    double elapsed = 0.0;
    do {
        if (swmm_engine_step(e, &elapsed) != 0)
            break;
    } while (elapsed > 0.0);
    swmm_engine_end(e);
    swmm_engine_report(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    std::ifstream f(rpt);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct Row {
    double min = 0, avg = 0, max = 0;
    double bins[5] = {0, 0, 0, 0, 0};
    std::string converged; // number, or "-"
};

/**
 * @brief Parse the report-format-specific Conduit Time Step Summary rows.
 *
 * Reads min/avg/max seconds, five percent bins and convergence token without checking stream
 * success. Duplicate names overwrite and malformed lines can create default/partial rows; no
 * general report syntax validation.
 *
 * @param[in] rpt Report text searched for the exact LF-delimited title.
 * @return Owned name-keyed rows between the second and third dashed rules, empty when title absent.
 */
std::map<std::string, Row> parseTable(const std::string& rpt) {
    std::map<std::string, Row> rows;
    const auto at = rpt.find("\n  Conduit Time Step Summary\n");
    if (at == std::string::npos)
        return rows;
    std::istringstream in(rpt.substr(at + 1));
    std::string line;
    int rules = 0;
    while (std::getline(in, line)) {
        if (line.rfind("  ---", 0) == 0) {
            if (++rules == 3)
                break;
            continue;
        }
        if (rules != 2)
            continue; // rows sit between the 2nd and 3rd rule
        std::istringstream ls(line);
        std::string name;
        Row r;
        ls >> name >> r.min >> r.avg >> r.max;
        for (double& b : r.bins)
            ls >> b;
        ls >> r.converged;
        rows[name] = r;
    }
    return rows;
}

/**
 * @brief Assert timestep-summary ordering and histogram closure.
 *
 * For every row, checks bin sum within 0.05 of 100, positive minimum, ordered min/avg/max within
 * 1e-4 s and maximum<=kRoutingStep+1e-4 s. Empty input passes vacuously; does not validate
 * convergence or expected row count.
 *
 * @param[in] rows Parsed rows with min/avg/max seconds and five percentage bins; unchanged.
 */
void expectConsistent(const std::map<std::string, Row>& rows) {
    for (const auto& [name, r] : rows) {
        SCOPED_TRACE(name);
        double sum = 0.0;
        for (double b : r.bins)
            sum += b;
        EXPECT_NEAR(sum, 100.0, 0.05);
        EXPECT_GT(r.min, 0.0);
        EXPECT_LE(r.min, r.avg + 1e-4);
        EXPECT_LE(r.avg, r.max + 1e-4);
        EXPECT_LE(r.max, kRoutingStep + 1e-4);
    }
}

} // namespace

TEST(ReportLinkSteps, absentByDefault) {
    const std::string rpt = run("dw_default", deck("DYNWAVE", "LINKS ALL\n"));
    EXPECT_EQ(rpt.find("Conduit Time Step Summary\n"), std::string::npos);
}

TEST(ReportLinkSteps, dynamicWaveReportsCflStepAndConvergence) {
    const std::string rpt = run("dw_on", deck("DYNWAVE", "LINK_STEPS YES\nLINKS ALL\n"));
    EXPECT_NE(rpt.find("Dynamic wave: the CFL time step"), std::string::npos);
    const auto rows = parseTable(rpt);
    ASSERT_EQ(rows.size(), 5u);
    expectConsistent(rows);
    // The 8 ft stub is the CFL limiter.
    EXPECT_LT(rows.at("C3").avg, 0.5 * rows.at("C1").avg);
    // A converging chain: every conduit converged on every step.
    for (const auto& [name, r] : rows)
        EXPECT_EQ(r.converged, "100.00") << name;
}

TEST(ReportLinkSteps, finiteVolumeReportsLocalStepsTaken) {
    const std::string rpt = run("fv_on", deck("FV", "LINK_STEPS YES\nLINKS ALL\n"));
    EXPECT_NE(rpt.find("Finite volume: the local time step"), std::string::npos);
    // The tiers must actually have engaged for this test to mean anything.
    const auto fired = rpt.find("LTS Macro Cycles Fired ...");
    ASSERT_NE(fired, std::string::npos);
    EXPECT_GT(std::stol(rpt.substr(fired + 26, 16)), 0L);

    const auto rows = parseTable(rpt);
    ASSERT_EQ(rows.size(), 5u);
    expectConsistent(rows);
    // The stub runs on a finer tier than the long end pipes.
    EXPECT_LT(rows.at("C3").avg, 0.5 * rows.at("C1").avg);
    EXPECT_LT(rows.at("C3").avg, 0.5 * rows.at("C5").avg);
    // No Picard loop under FV.
    for (const auto& [name, r] : rows)
        EXPECT_EQ(r.converged, "-") << name;
}

TEST(ReportLinkSteps, honoursLinkSelection) {
    const std::string rpt = run("dw_some", deck("DYNWAVE", "LINK_STEPS YES\nLINKS C3\n"));
    const auto rows = parseTable(rpt);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.count("C3"), 1u);
}

TEST(ReportLinkSteps, kinematicWaveSaysNotAvailable) {
    const std::string rpt = run("kw_on", deck("KINWAVE", "LINK_STEPS YES\nLINKS ALL\n"));
    EXPECT_NE(rpt.find("Conduit Time Step Summary\n"), std::string::npos);
    EXPECT_NE(rpt.find("Not available for this routing method."), std::string::npos);
}
