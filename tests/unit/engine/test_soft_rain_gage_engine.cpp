/**
 * @file test_soft_rain_gage_engine.cpp
 * @brief Engine-loop tests for SR-1b gage-level soft rainfall → 1D ROM.
 *
 * Verifies that a configured [SOFT_RAINGAGES] entry activates the 1D network
 * ROM and produces nonzero head bands in the .uncertainty.csv sidecar, and
 * that a zero spread collapses the bands exactly (deviation-form exactness).
 *
 * @ingroup engine_uncertainty
 */

#include <gtest/gtest.h>

#include <openswmm/engine/openswmm_engine.h>
#include "core/SWMMEngine.hpp"
#include "uncertainty/SpectralROM1D.hpp"

#include <cmath>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#if defined(_WIN32)
#  include <process.h>   // _getpid
#  define getpid _getpid
#else
#  include <unistd.h>    // getpid
#endif

namespace {

// Per-process temp prefix to avoid collisions under parallel ctest -j.
const std::string g_pfx = "/tmp/sr1b_" + std::to_string(getpid()) + "_";

// Chain network J1..J5 → O1, one subcatchment per junction, constant rain.
// A [SOFT_RAINGAGES] entry (when requested) attaches gage-level spread.
void writeChainInp(const std::string& path, const char* soft_line) {
    std::ofstream f(path);
    f << "[TITLE]\nSR-1b gage soft rainfall engine test\n\n";
    f << "[OPTIONS]\n";
    f << "FLOW_UNITS CFS\n";
    f << "INFILTRATION HORTON\n";
    f << "FLOW_ROUTING DYNWAVE\n";
    f << "START_DATE 01/01/2025\nSTART_TIME 00:00:00\n";
    f << "REPORT_START_DATE 01/01/2025\nREPORT_START_TIME 00:00:00\n";
    f << "END_DATE 01/01/2025\nEND_TIME 01:00:00\n";
    f << "REPORT_STEP 00:05:00\nWET_STEP 00:01:00\nDRY_STEP 00:05:00\nROUTING_STEP 00:00:30\n";
    f << "MINIMUM_STEP 0.5\nTHREADS 1\n\n";
    f << "[EVAPORATION]\nCONSTANT 0.0\n\n";
    f << "[RAINGAGES]\nRG1 INTENSITY 0:05 1.0 TIMESERIES TS0\n\n";
    f << "[TIMESERIES]\n"
      << "TS0 01/01/2025 00:00 6.0\n"
      << "TS0 01/01/2025 00:30 6.0\n"
      << "TS0 01/01/2025 01:00 6.0\n\n";
    f << "[SUBCATCHMENTS]\n";
    for (int i = 1; i <= 5; ++i)
        f << "S" << i << " RG1 J" << i << " 5.0 100 200 1.0 0\n";
    f << "\n[SUBAREAS]\n";
    for (int i = 1; i <= 5; ++i)
        f << "S" << i << " 0.015 0.20 0.00 0.00 100 OUTLET\n";
    f << "\n[INFILTRATION]\n";
    for (int i = 1; i <= 5; ++i)
        f << "S" << i << " 0.0 0.0 0.0 0.0 0.0\n";
    // Storage nodes with a large constant surface area so heads rise gradually
    // over the whole run (dh/dt stays > 0), keeping the rainfall-rate-driven
    // soft spread active at every report boundary instead of relaxing to zero.
    f << "\n[STORAGE]\n";
    for (int i = 1; i <= 5; ++i)
        f << "J" << i << " " << (100 - i) << " 60 0 FUNCTIONAL 0 0 20000\n";
    f << "\n[OUTFALLS]\nO1 90 FREE NO\n\n";
    f << "[CONDUITS]\n";
    for (int i = 1; i <= 4; ++i)
        f << "C" << i << " J" << i << " J" << (i + 1) << " 200 0.02 0 0 0 0\n";
    f << "C5 J5 O1 200 0.02 0 0 0 0\n\n";
    f << "[XSECTIONS]\n";
    for (int i = 1; i <= 5; ++i)
        f << "C" << i << " CIRCULAR 0.5 0 0 0 1\n";
    // Node coordinates spaced 200 m apart along the chain — required by
    // COHERENCE CORR_LEN so the correlated field has spatial geometry.
    f << "\n[COORDINATES]\n";
    for (int i = 1; i <= 5; ++i)
        f << "J" << i << " " << ((i - 1) * 200.0) << " 0.0\n";
    f << "O1 1000.0 0.0\n";
    f << "\n[REPORT]\nINPUT NO\nCONTINUITY YES\nNODES ALL\nLINKS ALL\n\n";
    if (soft_line && soft_line[0] != '\0')
        f << "[SOFT_RAINGAGES]\n" << soft_line << "\n\n";
}

// Return the maximum band width max|q95 - q05| over all rows in the sidecar.
double maxBandWidth(const std::string& csv_path, bool& found) {
    found = false;
    std::ifstream f(csv_path);
    if (!f.is_open()) return 0.0;
    std::string line;
    std::getline(f, line);  // header
    double worst = 0.0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string tok;
        std::string cols[5];
        int n = 0;
        while (n < 5 && std::getline(ss, tok, ',')) cols[n++] = tok;
        if (n < 5) continue;
        found = true;
        const double q05 = std::stod(cols[2]);
        const double q95 = std::stod(cols[4]);
        worst = std::max(worst, std::abs(q95 - q05));
    }
    return worst;
}

// Max band width for a single node_name (column 1) — isolates a node's band.
double bandAtNode(const std::string& csv_path, const std::string& node,
                  bool& found) {
    found = false;
    std::ifstream f(csv_path);
    if (!f.is_open()) return 0.0;
    std::string line;
    std::getline(f, line);  // header
    double worst = 0.0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string tok;
        std::string cols[5];
        int n = 0;
        while (n < 5 && std::getline(ss, tok, ',')) cols[n++] = tok;
        if (n < 5) continue;
        if (cols[1] != node) continue;
        found = true;
        const double q05 = std::stod(cols[2]);
        const double q95 = std::stod(cols[4]);
        worst = std::max(worst, std::abs(q95 - q05));
    }
    return worst;
}

// Byte-for-byte file comparison (regression lock for the comonotone path).
bool filesIdentical(const std::string& a, const std::string& b) {
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa.is_open() || !fb.is_open()) return false;
    std::stringstream sa, sb;
    sa << fa.rdbuf();
    sb << fb.rdbuf();
    return sa.str() == sb.str();
}

int runToEnd(SWMM_Engine handle, int max_steps = 5000) {
    double elapsed = 1.0;
    int steps = 0;
    while (elapsed > 0.0 && steps < max_steps) {
        if (swmm_engine_step(handle, &elapsed) != SWMM_OK) return -1;
        ++steps;
    }
    return steps;
}

double runCase(const std::string& inp, const std::string& rpt,
               const std::string& csv, bool& found) {
    SWMM_Engine handle = swmm_engine_create();
    EXPECT_NE(handle, nullptr);
    EXPECT_EQ(swmm_engine_open(handle, inp.c_str(), rpt.c_str(), nullptr, nullptr), SWMM_OK);
    EXPECT_EQ(swmm_engine_initialize(handle), SWMM_OK);
    EXPECT_EQ(swmm_engine_start(handle, 0), SWMM_OK);
    EXPECT_GT(runToEnd(handle), 0);
    EXPECT_EQ(swmm_engine_end(handle), SWMM_OK);
    EXPECT_EQ(swmm_engine_close(handle), SWMM_OK);
    swmm_engine_destroy(handle);
    return maxBandWidth(csv, found);
}

TEST(SoftRainGageEngine, NonzeroCvProducesHeadBands) {
    const std::string inp = g_pfx + "cv.inp";
    const std::string rpt = g_pfx + "cv.rpt";
    const std::string csv = g_pfx + "cv.uncertainty.csv";
    writeChainInp(inp, "RG1 NORMAL CV 0.30");

    bool found = false;
    const double band = runCase(inp, rpt, csv, found);
    EXPECT_TRUE(found) << "no ROM quantile rows written to " << csv;
    EXPECT_GT(band, 1.0e-6) << "expected nonzero head band from gage-level spread";
}

TEST(SoftRainGageEngine, ZeroSpreadCollapsesBandsExactly) {
    const std::string inp = g_pfx + "zero.inp";
    const std::string rpt = g_pfx + "zero.rpt";
    const std::string csv = g_pfx + "zero.uncertainty.csv";
    writeChainInp(inp, "RG1 NORMAL CV 0.0");

    bool found = false;
    const double band = runCase(inp, rpt, csv, found);
    EXPECT_TRUE(found) << "no ROM quantile rows written to " << csv;
    EXPECT_EQ(band, 0.0) << "zero spread must collapse bands exactly (deviation form)";
}

TEST(SoftRainGageEngine, LargerCvWidensBands) {
    const std::string inp_a = g_pfx + "small.inp";
    const std::string inp_b = g_pfx + "large.inp";
    const std::string rpt_a = g_pfx + "small.rpt";
    const std::string rpt_b = g_pfx + "large.rpt";
    const std::string csv_a = g_pfx + "small.uncertainty.csv";
    const std::string csv_b = g_pfx + "large.uncertainty.csv";
    writeChainInp(inp_a, "RG1 NORMAL CV 0.20");
    writeChainInp(inp_b, "RG1 NORMAL CV 0.60");

    bool fa = false, fb = false;
    const double band_a = runCase(inp_a, rpt_a, csv_a, fa);
    const double band_b = runCase(inp_b, rpt_b, csv_b, fb);
    ASSERT_TRUE(fa);
    ASSERT_TRUE(fb);
    EXPECT_GT(band_b, band_a) << "tripling CV should widen the head bands";
}

// ---------------------------------------------------------------------------
// CL-1c: COHERENCE CORR_LEN correlated coherence (1D gage path)
// ---------------------------------------------------------------------------

TEST(SoftRainGageEngine, CorrLenUnsetMatchesComonotone) {
    // Absent COHERENCE must be byte-for-byte identical to explicit COHERENCE
    // FULL — both take the comonotone scalar path (regression lock).
    const std::string inp_a = g_pfx + "unset.inp";
    const std::string inp_b = g_pfx + "full.inp";
    const std::string rpt_a = g_pfx + "unset.rpt";
    const std::string rpt_b = g_pfx + "full.rpt";
    const std::string csv_a = g_pfx + "unset.uncertainty.csv";
    const std::string csv_b = g_pfx + "full.uncertainty.csv";
    writeChainInp(inp_a, "RG1 NORMAL CV 0.30");
    writeChainInp(inp_b, "RG1 NORMAL CV 0.30 COHERENCE FULL");

    bool fa = false, fb = false;
    runCase(inp_a, rpt_a, csv_a, fa);
    runCase(inp_b, rpt_b, csv_b, fb);
    ASSERT_TRUE(fa);
    ASSERT_TRUE(fb);
    EXPECT_TRUE(filesIdentical(csv_a, csv_b))
        << "absent COHERENCE must equal COHERENCE FULL byte-for-byte";
}

TEST(SoftRainGageEngine, LargeCorrLenApproachesComonotone) {
    // corr_len ≫ domain (nodes span 800 m) ⇒ one ranking for all nodes ⇒
    // comonotone up to a member relabelling ⇒ quantile bands ~ identical.
    const std::string inp_a = g_pfx + "cmono.inp";
    const std::string inp_b = g_pfx + "biglen.inp";
    const std::string rpt_a = g_pfx + "cmono.rpt";
    const std::string rpt_b = g_pfx + "biglen.rpt";
    const std::string csv_a = g_pfx + "cmono.uncertainty.csv";
    const std::string csv_b = g_pfx + "biglen.uncertainty.csv";
    writeChainInp(inp_a, "RG1 NORMAL CV 0.30");
    writeChainInp(inp_b, "RG1 NORMAL CV 0.30 COHERENCE CORR_LEN 100000");

    bool fa = false, fb = false;
    const double band_a = runCase(inp_a, rpt_a, csv_a, fa);
    const double band_b = runCase(inp_b, rpt_b, csv_b, fb);
    ASSERT_TRUE(fa);
    ASSERT_TRUE(fb);
    ASSERT_GT(band_a, 1.0e-6);
    EXPECT_NEAR(band_b, band_a, 0.05 * band_a)
        << "very large corr_len should reproduce comonotone bands within ~5%";
}

TEST(SoftRainGageEngine, SmallCorrLenNarrowsDownstreamBands) {
    // Skipped in SP1 (CORR_LEN was parsed but run as FULL); live since SP3.
    // corr_len ≪ node spacing (200 m) ⇒ each node ranks independently ⇒ the
    // most-downstream node's band narrows vs comonotone (spatial cancellation).
    const std::string inp_a = g_pfx + "cmono2.inp";
    const std::string inp_b = g_pfx + "smalllen.inp";
    const std::string rpt_a = g_pfx + "cmono2.rpt";
    const std::string rpt_b = g_pfx + "smalllen.rpt";
    const std::string csv_a = g_pfx + "cmono2.uncertainty.csv";
    const std::string csv_b = g_pfx + "smalllen.uncertainty.csv";
    writeChainInp(inp_a, "RG1 NORMAL CV 0.40");
    writeChainInp(inp_b, "RG1 NORMAL CV 0.40 COHERENCE CORR_LEN 20");

    bool fa = false, fb = false;
    runCase(inp_a, rpt_a, csv_a, fa);
    runCase(inp_b, rpt_b, csv_b, fb);
    bool na = false, nb = false;
    const double dn_a = bandAtNode(csv_a, "J5", na);
    const double dn_b = bandAtNode(csv_b, "J5", nb);
    ASSERT_TRUE(na);
    ASSERT_TRUE(nb);
    ASSERT_GT(dn_a, 1.0e-6);
    EXPECT_LT(dn_b, dn_a)
        << "small corr_len should narrow the downstream (J5) band vs comonotone";
}

// ---------------------------------------------------------------------------
// SP1 / H7: two gages with DIFFERENT families feed the two coefficient planes
// ---------------------------------------------------------------------------

// Same chain, but S1-S2 use RG1 and S3-S5 use RG2 (both on TS0).
void writeTwoGageInp(const std::string& path, const char* soft_lines) {
    writeChainInp(path, nullptr);
    std::ifstream in(path);
    std::stringstream buf;
    buf << in.rdbuf();
    std::string txt = buf.str();
    in.close();
    auto replaceAll = [&](const std::string& from, const std::string& to) {
        for (std::size_t pos = 0; (pos = txt.find(from, pos)) != std::string::npos;
             pos += to.size())
            txt.replace(pos, from.size(), to);
    };
    replaceAll("[RAINGAGES]\nRG1 INTENSITY 0:05 1.0 TIMESERIES TS0\n",
               "[RAINGAGES]\nRG1 INTENSITY 0:05 1.0 TIMESERIES TS0\n"
               "RG2 INTENSITY 0:05 1.0 TIMESERIES TS0\n");
    for (int i = 3; i <= 5; ++i)
        replaceAll("S" + std::to_string(i) + " RG1 ", "S" + std::to_string(i) + " RG2 ");
    std::ofstream out(path);
    out << txt;
    if (soft_lines && soft_lines[0] != '\0')
        out << "[SOFT_RAINGAGES]\n" << soft_lines << "\n\n";
}

struct PlaneProbe {
    bool   ran = false;
    int    planes = 0;              // additive-channel planes (one per family)
    int    mixed_family_warnings = 0;
    int    corr_len_warnings = 0;
    double band = 0.0;
};

PlaneProbe runPlaneProbe(const std::string& inp, const std::string& rpt,
                         const std::string& csv) {
    PlaneProbe pr;
    SWMM_Engine handle = swmm_engine_create();
    if (!handle) return pr;
    if (swmm_engine_open(handle, inp.c_str(), rpt.c_str(), nullptr, nullptr) != SWMM_OK ||
        swmm_engine_initialize(handle) != SWMM_OK ||
        swmm_engine_start(handle, 0) != SWMM_OK) {
        swmm_engine_destroy(handle);
        return pr;
    }
    runToEnd(handle);
    auto* eng = static_cast<openswmm::SWMMEngine*>(handle);
    if (eng->rom1d()) pr.planes = eng->rom1dAdditivePlanes();
    for (const auto& w : eng->context().warnings) {
        if (w.find("mixed distribution families") != std::string::npos)
            ++pr.mixed_family_warnings;
        if (w.find("CORR_LEN") != std::string::npos) ++pr.corr_len_warnings;
    }
    swmm_engine_end(handle);
    swmm_engine_close(handle);
    swmm_engine_destroy(handle);
    bool found = false;
    pr.band = maxBandWidth(csv, found);
    pr.ran = found;
    return pr;
}

TEST(SoftRainGageEngine, MixedFamiliesArmBothPlanesWithNoFallback) {
    // NORMAL gage + UNIFORM gage on one network. The pre-port engine used the
    // first gage's family for both and warned; the two-plane API gives each
    // gage its own family's coefficient. HALFRANGE 1.8 on a 6.0 in/hr rain is
    // a relative spread of 0.30, matching the NORMAL gage's CV.
    const std::string inp = g_pfx + "mixed.inp";
    const std::string rpt = g_pfx + "mixed.rpt";
    const std::string csv = g_pfx + "mixed.uncertainty.csv";
    writeTwoGageInp(inp, "RG1 NORMAL CV 0.30\nRG2 UNIFORM HALFRANGE 1.8");
    const PlaneProbe pr = runPlaneProbe(inp, rpt, csv);
    ASSERT_TRUE(pr.ran);
    // O-16b: each family plane is carried as a + and a - half, so 2 families = 4.
    EXPECT_EQ(pr.planes, 4) << "each family must carry its own plane (+/- halves)";
    EXPECT_EQ(pr.mixed_family_warnings, 0)
        << "the first-family fallback and its warning must not exist";
    EXPECT_GT(pr.band, 1.0e-6);
}

TEST(SoftRainGageEngine, SingleFamilyLeavesSecondPlaneUnarmed) {
    // One family only => null second plane => the bit-identical single-family
    // call (H7's regression lock), for each of the two plane classes.
    const std::string inp_n = g_pfx + "one_normal.inp";
    const std::string inp_u = g_pfx + "one_uniform.inp";
    writeTwoGageInp(inp_n, "RG1 NORMAL CV 0.30\nRG2 LOGNORMAL CV 0.30");
    writeTwoGageInp(inp_u, "RG1 UNIFORM HALFRANGE 1.8\nRG2 UNIFORM HALFRANGE 1.8");
    const PlaneProbe pn = runPlaneProbe(inp_n, g_pfx + "one_normal.rpt",
                                        g_pfx + "one_normal.uncertainty.csv");
    const PlaneProbe pu = runPlaneProbe(inp_u, g_pfx + "one_uniform.rpt",
                                        g_pfx + "one_uniform.uncertainty.csv");
    ASSERT_TRUE(pn.ran);
    ASSERT_TRUE(pu.ran);
    // NORMAL + LOGNORMAL share the probit coefficient: one plane, no warning.
    EXPECT_EQ(pn.planes, 2);  // one family = one plane = +/- halves (O-16b)
    EXPECT_EQ(pn.mixed_family_warnings, 0);
    EXPECT_GT(pn.band, 1.0e-6);
    // All-UNIFORM: the UNIFORM plane is the primary and the only plane.
    EXPECT_EQ(pu.planes, 2);  // one family = one plane = +/- halves (O-16b)
    EXPECT_GT(pu.band, 1.0e-6);
}

TEST(SoftRainGageEngine, CorrLenGageIsWiredAndDoesNotWarn) {
    // SP1 emitted a "not yet wired" warning for CORR_LEN gages; SP3 wires them,
    // so the warning must be gone and the fixture ([COORDINATES] present) must
    // produce a band without any fallback warning either.
    const std::string inp = g_pfx + "cl_live.inp";
    writeChainInp(inp, "RG1 NORMAL CV 0.30 COHERENCE CORR_LEN 200");
    const PlaneProbe pr = runPlaneProbe(inp, g_pfx + "cl_live.rpt",
                                        g_pfx + "cl_live.uncertainty.csv");
    ASSERT_TRUE(pr.ran);
    EXPECT_EQ(pr.corr_len_warnings, 0) << "no 'not wired' / fallback warning expected";
    EXPECT_GT(pr.band, 1.0e-6);
}

TEST(SoftRainGageEngine, CorrLenWithMixedFamiliesIsRefusedAtInitialize) {
    // H7 scope guard: the correlated field carries one coefficient family per
    // source, so CORR_LEN x mixed families is a hard error, not an
    // approximation. Per-line parsing cannot see it; initialize() can.
    const std::string inp = g_pfx + "cl_mixed.inp";
    writeTwoGageInp(inp, "RG1 NORMAL CV 0.30 COHERENCE CORR_LEN 200\nRG2 UNIFORM HALFRANGE 1.8");
    SWMM_Engine handle = swmm_engine_create();
    ASSERT_NE(handle, nullptr);
    ASSERT_EQ(swmm_engine_open(handle, inp.c_str(), (g_pfx + "cl_mixed.rpt").c_str(),
                               nullptr, nullptr), SWMM_OK);
    // Raised inside initialize() via set_error -> ERROR_STATE. Before SP3,
    // initialize() overwrote that state with INITIALIZED and returned SWMM_OK,
    // so init-time hard errors were swallowed; SP3 made initialize() honour it.
    EXPECT_NE(swmm_engine_initialize(handle), SWMM_OK)
        << "CORR_LEN x mixed families must be refused";
    const std::string msg = swmm_get_last_error_msg(handle);   // read before start() overwrites it
    EXPECT_NE(msg.find("CORR_LEN"), std::string::npos) << msg;
    EXPECT_NE(swmm_engine_start(handle, 0), SWMM_OK)
        << "an engine in ERROR_STATE must not start";
    swmm_engine_close(handle);
    swmm_engine_destroy(handle);
}

TEST(SoftRainGageEngine, CorrLenWithoutCoordinatesWarnsAndFallsBackToFull) {
    // Same chain, [COORDINATES] stripped: no geometry => comonotone fallback,
    // said once, and the band equals the FULL band byte-for-byte.
    const std::string inp_a = g_pfx + "cl_nocoord_full.inp";
    const std::string inp_b = g_pfx + "cl_nocoord_cl.inp";
    const std::string csv_a = g_pfx + "cl_nocoord_full.uncertainty.csv";
    const std::string csv_b = g_pfx + "cl_nocoord_cl.uncertainty.csv";
    auto strip = [](const std::string& path) {
        std::ifstream in(path); std::stringstream buf; buf << in.rdbuf(); in.close();
        std::string t = buf.str();
        const auto a = t.find("[COORDINATES]");
        const auto b = t.find("[REPORT]");
        if (a != std::string::npos && b != std::string::npos && b > a) t.erase(a, b - a);
        std::ofstream out(path); out << t;
    };
    writeChainInp(inp_a, "RG1 NORMAL CV 0.30"); strip(inp_a);
    writeChainInp(inp_b, "RG1 NORMAL CV 0.30 COHERENCE CORR_LEN 50"); strip(inp_b);
    bool fa = false;
    runCase(inp_a, g_pfx + "cl_nocoord_full.rpt", csv_a, fa);
    const PlaneProbe pb = runPlaneProbe(inp_b, g_pfx + "cl_nocoord_cl.rpt", csv_b);
    ASSERT_TRUE(fa);
    ASSERT_TRUE(pb.ran);
    EXPECT_EQ(pb.corr_len_warnings, 1);
    EXPECT_TRUE(filesIdentical(csv_a, csv_b))
        << "fallback must reproduce the FULL path exactly";
}

// ---------------------------------------------------------------------------
// SR-6: runoff-elasticity probe must leave the deterministic path bit-identical
// ---------------------------------------------------------------------------

struct DetProbe {
    std::vector<double> heads;        // end-of-run node heads
    std::vector<double> stat_imperv;  // runoff statistics accumulators
    std::vector<double> stat_evap;
    std::vector<double> runoff;
    int probe_steps = 0;
    double band = 0.0;
};

DetProbe runDetProbe(const std::string& inp, const std::string& tag, bool elasticity_on) {
    DetProbe d;
    const std::string rpt = g_pfx + tag + ".rpt";
    const std::string csv = g_pfx + tag + ".uncertainty.csv";
    SWMM_Engine handle = swmm_engine_create();
    if (swmm_engine_open(handle, inp.c_str(), rpt.c_str(), nullptr, nullptr) != SWMM_OK) return d;
    auto* eng = static_cast<openswmm::SWMMEngine*>(handle);
    eng->rom1dRunoffElasticityEnabled() = elasticity_on;
    if (swmm_engine_initialize(handle) != SWMM_OK || swmm_engine_start(handle, 0) != SWMM_OK) return d;
    runToEnd(handle);
    d.heads       = eng->context().nodes.head;
    d.stat_imperv = eng->context().subcatches.stat_imperv_vol;
    d.stat_evap   = eng->context().subcatches.stat_evap_vol;
    d.runoff      = eng->context().subcatches.runoff;
    d.probe_steps = eng->rom1dRunoffProbe().steps();
    swmm_engine_end(handle); swmm_engine_close(handle); swmm_engine_destroy(handle);
    bool found = false;
    d.band = maxBandWidth(csv, found);
    return d;
}

TEST(SoftRainGageEngine, RunoffElasticityProbeLeavesDeterministicPathBitIdentical) {
    // Same model, probe off vs on. The deterministic heads, the runoff
    // statistics accumulators and the final runoff rates must be EQUAL (not
    // near): the perturbed members run on a swapped-in copy of the solver
    // state and every ctx array execute() writes is restored. The band itself
    // must differ (the probe is doing work) and the probe must have stepped.
    const std::string inp = g_pfx + "sr6_bitid.inp";
    writeChainInp(inp, "RG1 NORMAL CV 0.30");
    const DetProbe off = runDetProbe(inp, "sr6_off", false);
    const DetProbe on  = runDetProbe(inp, "sr6_on",  true);
    ASSERT_FALSE(off.heads.empty());
    ASSERT_EQ(on.heads.size(), off.heads.size());
    for (std::size_t i = 0; i < off.heads.size(); ++i)
        EXPECT_EQ(on.heads[i], off.heads[i]) << "node " << i;
    for (std::size_t i = 0; i < off.stat_imperv.size(); ++i) {
        EXPECT_EQ(on.stat_imperv[i], off.stat_imperv[i]) << "stat_imperv " << i;
        EXPECT_EQ(on.stat_evap[i],   off.stat_evap[i])   << "stat_evap " << i;
        EXPECT_EQ(on.runoff[i],      off.runoff[i])      << "runoff " << i;
    }
    EXPECT_EQ(off.probe_steps, 0);
    EXPECT_GT(on.probe_steps, 0) << "probe never advanced";
    EXPECT_GT(on.band, off.band) << "elasticity > 1 on the rising limb must widen the band";
}

TEST(SoftRainGageEngine, NoSoftRainMeansNoProbeAtAll) {
    // Without a [SOFT_RAINGAGES] section the probe must not even seed: every
    // non-soft-rain run is untouched by SR-6.
    const std::string inp = g_pfx + "sr6_nosoft.inp";
    writeChainInp(inp, nullptr);
    SWMM_Engine handle = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(handle, inp.c_str(), (g_pfx + "sr6_nosoft.rpt").c_str(), nullptr, nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(handle), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(handle, 0), SWMM_OK);
    runToEnd(handle);
    auto* eng = static_cast<openswmm::SWMMEngine*>(handle);
    EXPECT_FALSE(eng->rom1dRunoffProbe().seeded());
    EXPECT_EQ(eng->rom1dRunoffProbe().steps(), 0);
    swmm_engine_end(handle); swmm_engine_close(handle); swmm_engine_destroy(handle);
}

} // anonymous namespace
