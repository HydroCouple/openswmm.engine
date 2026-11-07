/**
 * @file      test_fv_slot_volume.cpp
 * @brief     The FV slot-storage diagnostic: links.slot_volume must equal the
 *        closure's own slot content when a conduit is pressurized, read 0
 *        below the crown and under the dynamic-wave router, and surface in
 *        the report as the "Final Slot Storage" continuity line.
 *
 * The pressurized gate leans on an exact identity: with every cell above
 * a_crown, the published link depth is depthOfArea(a_mean) = y_full +
 * (a_mean - a_crown)/t_slot, and the slot volume is (a_mean - a_crown)*L,
 * so slot_volume == t_slot * (depth - y_full) * L to round-off. t_slot is
 * recomputed here from first principles (g*A_full/c^2, 5%-of-Wmax cap),
 * so a wrong crown reference in the accumulation (e.g. a_full instead of
 * a_crown) shifts the magnitude and the gate bites.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifdef _MSC_VER
#define _USE_MATH_DEFINES
#endif
#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>

namespace fs = std::filesystem;

namespace {

const char* kOutDir = "fv_slot_volume_out";

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
 * @brief Write fixture content to a caller-selected file.
 *
 * The parent directory must already exist. Uses ordinary text output mode. Open failure is not
 * reported by this helper. Subsequent write/close failures are not checked; the local stream closes
 * at scope exit and no path/content storage is retained. Allocation exceptions can propagate.
 *
 * @param[in] path Borrowed destination path; an existing file is truncated/overwritten.
 * @param[in] text Borrowed content copied verbatim through the output stream; no newline is
 *     appended.
 */
void writeFile(const std::string& path, const std::string& text) {
    std::ofstream f(path);
    f << text;
}

/**
 * @brief Collect an entire fixture file into owned text.
 *
 * Uses ordinary text mode and its platform newline rules. The local stream closes on return; no
 * model state is touched and no borrowed buffer is returned. Default stream exception settings
 * apply; allocation exceptions may propagate.
 *
 * @param[in] path Borrowed input filename used for this synchronous open/read.
 * @return Owned string collected from the stream; empty can mean an empty file or failure to open.
 *     Partial read failures have no separate status.
 */
std::string readFile(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// One 300-ft, 3-ft circular conduit from a junction to an outfall. The
// pressurized variant pins the outfall stage 12 ft above the crown so the
// whole barrel sits in the slot at steady state; the free variant trickles
// 1 cfs to a free outfall and never nears the crown.
/**
 * @brief Build the free-surface or pressurized slot-volume fixture.
 *
 * No validation, filesystem operation or solver initialization; raw routing token is unescaped.
 *
 * @param[in] routing Raw FLOW_ROUTING keyword.
 * @param[in] pressurized True fixes outfall head at 115 ft and inflow 20 cfs; false uses free
 *     outfall and 1 cfs.
 * @return Owned one-hour CFS deck with one 300 ft, 3 ft circular pipe and 100 ft/s slot celerity.
 */
std::string deck(const std::string& routing, bool pressurized) {
    std::ostringstream s;
    s << "[OPTIONS]\n"
         "FLOW_UNITS           CFS\n"
         "FLOW_ROUTING         "
      << routing
      << "\n"
         "START_DATE           01/01/2026\n"
         "START_TIME           00:00:00\n"
         "END_DATE             01/01/2026\n"
         "END_TIME             01:00:00\n"
         "REPORT_STEP          00:05:00\n"
         "ROUTING_STEP         0.5\n"
         "FV_SLOT_CELERITY     100\n"
         "LENGTHENING_STEP     0\n"
         "\n"
         "[JUNCTIONS]\n"
         ";;Name Elev MaxDepth InitDepth SurDepth Aponded\n"
         "J0     100.0 40.0 0 0 0\n"
         "\n"
         "[OUTFALLS]\n"
         ";;Name Elev Type StageData Gated\n";
    if (pressurized)
        s << "OF     100.0 FIXED 115.0 NO\n";
    else
        s << "OF     100.0 FREE NO\n";
    s << "\n"
         "[CONDUITS]\n"
         ";;Name From To Length N Zin Zout Q0\n"
         "C1     J0   OF 300 0.013 0 0 0\n"
         "\n"
         "[XSECTIONS]\n"
         ";;Link Shape G1 G2 G3 G4\n"
         "C1     CIRCULAR 3 0 0 0 1\n"
         "\n"
         "[INFLOWS]\n"
         ";;Node Constituent TimeSeries Type Mfactor Sfactor Baseline\n"
         "J0     FLOW \"\" FLOW 1.0 1.0 "
      << (pressurized ? 20.0 : 1.0)
      << "\n"
         "\n"
         "[REPORT]\n"
         "CONTINUITY YES\n";
    return s.str();
}

struct RunResult {
    double depth = -1.0;    // published link depth (CLAMPED to y_full, R0b)
    double j0_depth = -1.0; // upstream node depth (piezometric head - invert)
    double volume = -1.0;
    double slot_volume = -1.0;
    std::string rpt;
};

/**
 * @brief Run the slot-volume fixture and collect final storage/report diagnostics.
 *
 * Owns/closes/destroys engine on normal return. Setup and step errors record nonfatal expectations
 * without short-circuiting; a failure leaving positive elapsed can loop indefinitely.
 * Getter/end/report/close/read errors are not independently returned; no iteration cap or validity
 * flag.
 *
 * @param[in] base Safe retained outPath artifact basename.
 * @param[in] text Complete input passed to writeFile.
 * @return Owned pipe/node depths feet, pipe/slot volume ft3 and report text, possibly
 *     partial/default after nonfatal failure.
 */
RunResult run(const std::string& base, const std::string& text) {
    const std::string inp = outPath(base + ".inp");
    const std::string rpt = outPath(base + ".rpt");
    writeFile(inp, text);
    RunResult r;
    SWMM_Engine e = swmm_engine_create();
    EXPECT_EQ(
        swmm_engine_open(e, inp.c_str(), rpt.c_str(), outPath(base + ".out").c_str(), nullptr), 0)
        << swmm_get_last_error_msg(e);
    EXPECT_EQ(swmm_engine_initialize(e), 0) << swmm_get_last_error_msg(e);
    EXPECT_EQ(swmm_engine_start(e, 0), 0) << swmm_get_last_error_msg(e);
    double elapsed = 0.0;
    do {
        EXPECT_EQ(swmm_engine_step(e, &elapsed), 0) << swmm_get_last_error_msg(e);
    } while (elapsed > 0.0);
    const int c1 = swmm_link_index(e, "C1");
    EXPECT_GE(c1, 0);
    if (c1 >= 0) {
        EXPECT_EQ(swmm_link_get_depth(e, c1, &r.depth), 0);
        EXPECT_EQ(swmm_link_get_volume(e, c1, &r.volume), 0);
        EXPECT_EQ(swmm_link_get_slot_volume(e, c1, &r.slot_volume), 0);
    }
    const int j0 = swmm_node_index(e, "J0");
    if (j0 >= 0)
        EXPECT_EQ(swmm_node_get_depth(e, j0, &r.j0_depth), 0);
    swmm_engine_end(e);
    swmm_engine_report(e); // writes the summary sections (incl. continuity)
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    r.rpt = readFile(rpt);
    return r;
}

} // namespace

TEST(FvSlotVolume, pressurizedSlotVolumeMatchesTheClosure) {
    const RunResult r = run("pressurized_fv", deck("FV", true));

    // R0b: the PUBLISHED link depth is truncated to the pipe's true max
    // height even though the barrel is pressurized well past it.
    ASSERT_GT(r.slot_volume, 0.0);  // pressurized for certain...
    EXPECT_DOUBLE_EQ(r.depth, 3.0); // ...and reported at the crown

    // The piezometric identity now reads through the NODE heads: at steady
    // state on a flat pipe the HGL is linear, A(h) is affine above the
    // crown, so the length-mean piezometric depth is the mean of the end
    // heads — upstream from J0, downstream pinned by the 115 ft outfall
    // stage over the 100 ft invert. End-face vs node-head offsets leave a
    // small residual, hence 2% (the pre-R0b gate read the exact mean from
    // the published depth, which no longer carries it).
    ASSERT_GT(r.j0_depth, 3.0);
    const double mean_piezo = 0.5 * (r.j0_depth + 15.0);
    const double a_full = M_PI * 1.5 * 1.5;
    const double t_req = 32.2 * a_full / (100.0 * 100.0); // g*A_full/c^2
    const double t_slot = std::min(t_req, 0.05 * 3.0);    // 5%-of-Wmax cap
    const double expected = t_slot * (mean_piezo - 3.0) * 300.0;
    EXPECT_NEAR(r.slot_volume, expected, 0.02 * expected);

    // Always a subset of the link's total stored volume.
    EXPECT_LT(r.slot_volume, r.volume);

    // And the continuity block surfaces it.
    EXPECT_NE(r.rpt.find("Final Slot Storage"), std::string::npos);
}

TEST(FvSlotVolume, belowCrownReadsZeroAndReportStaysSilent) {
    const RunResult r = run("free_fv", deck("FV", false));
    ASSERT_GE(r.depth, 0.0);
    EXPECT_LT(r.depth, 2.9); // never near the crown band
    EXPECT_EQ(r.slot_volume, 0.0);
    EXPECT_EQ(r.rpt.find("Final Slot Storage"), std::string::npos);
}

TEST(FvSlotVolume, dynamicWaveNeverPopulatesTheChannel) {
    const RunResult r = run("pressurized_dw", deck("DYNWAVE", true));
    EXPECT_EQ(r.slot_volume, 0.0);
    EXPECT_EQ(r.rpt.find("Final Slot Storage"), std::string::npos);
    EXPECT_EQ(r.rpt.find("Slot Storage Summary"), std::string::npos);
}

// Slot program R0 — the share statistics ride the same identity as the
// volume: on a run that is steady and fully pressurized for most of its
// span, the run-level share (∫slot dt / ∫stored dt) approaches the final
// instantaneous ratio, the peak share bounds it, and the summary block +
// dt-argmin attribution both surface in the report.
TEST(FvSlotVolume, shareStatisticsAndReportBlock) {
    const std::string base = "pressurized_share";
    const std::string inp = outPath(base + ".inp");
    const std::string rpt = outPath(base + ".rpt");
    writeFile(inp, deck("FV", true));
    SWMM_Engine e = swmm_engine_create();
    ASSERT_EQ(
        swmm_engine_open(e, inp.c_str(), rpt.c_str(), outPath(base + ".out").c_str(), nullptr), 0);
    ASSERT_EQ(swmm_engine_initialize(e), 0);
    ASSERT_EQ(swmm_engine_start(e, 0), 0);
    double elapsed = 0.0;
    do {
        ASSERT_EQ(swmm_engine_step(e, &elapsed), 0);
    } while (elapsed > 0.0);
    const int c1 = swmm_link_index(e, "C1");
    ASSERT_GE(c1, 0);
    double vol = 0.0, slot = 0.0, run_share = 0.0, peak_share = 0.0;
    EXPECT_EQ(swmm_link_get_volume(e, c1, &vol), 0);
    EXPECT_EQ(swmm_link_get_slot_volume(e, c1, &slot), 0);
    EXPECT_EQ(swmm_link_get_stat_slot_share(e, c1, &run_share), 0);
    EXPECT_EQ(swmm_link_get_stat_peak_slot_share(e, c1, &peak_share), 0);
    swmm_engine_end(e);
    swmm_engine_report(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);

    const double inst_share = slot / vol; // final instantaneous
    ASSERT_GT(inst_share, 0.0);
    EXPECT_GT(run_share, 0.0);
    EXPECT_LE(run_share, peak_share + 1.0e-12);
    EXPECT_LT(peak_share, 1.0);
    // Steady for most of the hour → the integrals converge on the final
    // ratio; 5% covers the fill transient's dilution of the average.
    EXPECT_NEAR(run_share, inst_share, 0.05 * inst_share);

    const std::string text = readFile(rpt);
    EXPECT_NE(text.find("Slot Storage Summary"), std::string::npos);
    EXPECT_NE(text.find("Run Slot Share (%)"), std::string::npos);
    // On a deck pressurized for nearly the whole run, the pressurized
    // regime must OWN the dt argmin — the classification, not just the row.
    const auto pos = text.find("dt Argmin Pressurized (%)");
    ASSERT_NE(pos, std::string::npos);
    double pct = -1.0;
    std::sscanf(text.c_str() + pos, "dt Argmin Pressurized (%%)%lf", &pct);
    EXPECT_GT(pct, 50.0);
}

// ---------------------------------------------------------------------------
// The default slot celerity is one physical speed, not one number
// ---------------------------------------------------------------------------

namespace {

// The pressurized fixture in SI: 100 m of 1 m circular pipe from a junction to
// an outfall whose stage sits 4 m above the crown, fed 0.5 m3/s. `celerity`
// is the whole FV_SLOT_CELERITY option line, or empty to leave the key out.
std::string siDeck(const std::string& celerity) {
    return "[OPTIONS]\n"
           "FLOW_UNITS           CMS\n"
           "FLOW_ROUTING         FV\n"
           "START_DATE           01/01/2026\n"
           "START_TIME           00:00:00\n"
           "END_DATE             01/01/2026\n"
           "END_TIME             01:00:00\n"
           "REPORT_STEP          00:05:00\n"
           "ROUTING_STEP         0.5\n"
           "LENGTHENING_STEP     0\n" +
           celerity +
           "\n"
           "[JUNCTIONS]\n"
           ";;Name Elev MaxDepth InitDepth SurDepth Aponded\n"
           "J0     100.0 12.0 0 0 0\n"
           "\n"
           "[OUTFALLS]\n"
           ";;Name Elev Type StageData Gated\n"
           "OF     100.0 FIXED 105.0 NO\n"
           "\n"
           "[CONDUITS]\n"
           ";;Name From To Length N Zin Zout Q0\n"
           "C1     J0   OF 100 0.013 0 0 0\n"
           "\n"
           "[XSECTIONS]\n"
           ";;Link Shape G1 G2 G3 G4\n"
           "C1     CIRCULAR 1 0 0 0 1\n"
           "\n"
           "[INFLOWS]\n"
           ";;Node Constituent TimeSeries Type Mfactor Sfactor Baseline\n"
           "J0     FLOW \"\" FLOW 1.0 1.0 0.5\n"
           "\n"
           "[REPORT]\n"
           "CONTINUITY YES\n";
}

/**
 * @brief Open a deck and read one option back through the C API.
 * @param[in] base Safe retained outPath artifact basename.
 * @param[in] text Complete input passed to writeFile.
 * @param[in] key [OPTIONS] key to read.
 * @param[in] rewrite When set, swmm_model_write() the opened model to outPath(rewrite).
 * @return The option's string as the API returns it; empty on failure.
 */
std::string openAndGetOption(const std::string& base, const std::string& text, const char* key,
                             const std::string& rewrite = std::string()) {
    const std::string inp = outPath(base + ".inp");
    writeFile(inp, text);
    SWMM_Engine e = swmm_engine_create();
    EXPECT_EQ(swmm_engine_open(e, inp.c_str(), outPath(base + ".rpt").c_str(),
                               outPath(base + ".out").c_str(), nullptr),
              0)
        << swmm_get_last_error_msg(e);
    char buf[64] = {0};
    EXPECT_EQ(swmm_options_get(e, key, buf, sizeof buf), 0) << key;
    if (!rewrite.empty())
        EXPECT_EQ(swmm_model_write(e, outPath(rewrite).c_str()), 0) << swmm_get_last_error_msg(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    return buf;
}

} // namespace

// FV_SLOT_CELERITY is read in project length units per second, and its design
// default is 100 ft/s (FvOptions.hpp). A default stored as the NUMBER 100 made
// an SI deck run at 100 m/s: a slot 3.3x stiffer, a pressurized step 3.3x
// smaller, and junctions 500 m above their rims on the Bellinge example. Left
// out, the key must mean 100 ft/s = 30.48 m/s in either unit system.
TEST(FvSlotCelerityDefault, anSiDeckWithoutTheKeyRunsAt100FeetPerSecond) {
    const RunResult absent = run("si_celerity_absent", siDeck(""));
    const RunResult stated = run("si_celerity_30_48", siDeck("FV_SLOT_CELERITY     30.48\n"));
    const RunResult hundred = run("si_celerity_100", siDeck("FV_SLOT_CELERITY     100\n"));
    ASSERT_GT(absent.slot_volume, 0.0); // pressurized for certain
    ASSERT_GT(hundred.slot_volume, 0.0);

    // 30.48 m/s is exactly 100 ft/s (30.48 / 0.3048 == 100.0 in IEEE double),
    // so leaving the key out must reproduce the stated value bit for bit.
    EXPECT_EQ(absent.slot_volume, stated.slot_volume);
    EXPECT_EQ(absent.j0_depth, stated.j0_depth);

    // And 100 m/s is a different slot: T_slot = g A_full / c^2, so the same
    // head above the crown holds (30.48/100)^2 = 0.0929 of the volume.
    const double ratio = hundred.slot_volume / absent.slot_volume;
    const double expected = (30.48 / 100.0) * (30.48 / 100.0);
    EXPECT_NEAR(ratio, expected, 0.05 * expected)
        << "slot volume at 100 m/s / at the default: " << ratio;
}

// The options API and the .inp writer state the default in the project's own
// units, so a GUI or a round trip sees 30.48 for an SI model and 100 for a US
// one, never the bare number the deck happened to omit.
TEST(FvSlotCelerityDefault, apiAndWriterStateTheDefaultInProjectUnits) {
    const std::string si = openAndGetOption("si_celerity_api", siDeck(""), "FV_SLOT_CELERITY",
                                            "si_celerity_api_rewritten.inp");
    EXPECT_DOUBLE_EQ(std::stod(si), 30.48) << "API returned '" << si << "'";

    std::string us = deck("FV", true);
    const auto key = us.find("FV_SLOT_CELERITY");
    ASSERT_NE(key, std::string::npos);
    us.erase(key, us.find('\n', key) + 1 - key); // leave the key out of the CFS deck
    const std::string cfs = openAndGetOption("us_celerity_api", us, "FV_SLOT_CELERITY");
    EXPECT_DOUBLE_EQ(std::stod(cfs), 100.0) << "API returned '" << cfs << "'";

    // An explicit value is reported as given, in either system.
    const std::string given = openAndGetOption("si_celerity_api_given",
                                               siDeck("FV_SLOT_CELERITY     100\n"), "FV_SLOT_CELERITY");
    EXPECT_DOUBLE_EQ(std::stod(given), 100.0) << "API returned '" << given << "'";

    // The rewritten SI deck carries the default spelled out in m/s.
    const std::string text = readFile(outPath("si_celerity_api_rewritten.inp"));
    const auto pos = text.find("FV_SLOT_CELERITY");
    ASSERT_NE(pos, std::string::npos) << text;
    double written = 0.0;
    ASSERT_EQ(std::sscanf(text.c_str() + pos, "FV_SLOT_CELERITY %lf", &written), 1);
    EXPECT_DOUBLE_EQ(written, 30.48);
}
