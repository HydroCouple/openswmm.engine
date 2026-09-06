/**
 * @file test_fv_node_head_consistency.cpp
 * @brief FV published node heads must be face-consistent across estimator
 *        classes (HGL_STEP_ATTRIBUTION.md, mechanism B), and the Preissmann
 *        slot-width cap must be surfaced when it overrides FV_SLOT_CELERITY.
 *
 * The pass-through head estimator votes min(cell_zb, face_zb) + h per wet
 * neighbour and takes the max — the same reconstruction publishFv uses for
 * virtual junctions. The retired arithmetic mean of cell-centre etas sat
 * ~S0·dx/2 above the face stage on steep COARSE chains (asymmetric slopes),
 * stepping against the VJ rule at every pass-through/VJ pair.
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_nodes.h>

namespace fs = std::filesystem;

namespace {

const char* kOutDir = "fv_node_head_out";

std::string outPath(const std::string& name) {
    fs::create_directories(kOutDir);
    return (fs::path(kOutDir) / name).string();
}

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream f(path);
    f << text;
}

std::string readFile(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Steep asymmetric chain: J0(150) -20%-> MID(130) -25%-> OF(105), 100 ft
// 1-ft circular conduits, steady 0.6 cfs, COARSE mesh (1 cell/conduit).
// Supercritical normal depth ~0.2 ft; the mid node's face-consistent stage is
// invert + ~y_n. The retired cell-centre mean read (140+y + 125+y)/2 =
// 132.5+y — a manufactured 2.5 ft of depth from the bed asymmetry alone.
std::string steepChain(bool virtual_mid, const std::string& fv_extra = "") {
    std::string junctions =
        "[JUNCTIONS]\n"
        ";;Name  Elev   MaxDepth InitDepth SurDepth Aponded\n"
        "J0      150.0  40.0  0  0  0\n";
    if (!virtual_mid) junctions += "MID     130.0  40.0  0  0  0\n";
    const std::string mid_section = virtual_mid
        ? "[VIRTUAL_JUNCTIONS]\n;;Name  Elev\nMID     130.0\n\n" : "";
    return
        "[OPTIONS]\n"
        "FLOW_UNITS           CFS\n"
        "FLOW_ROUTING         FV\n"
        "LINK_OFFSETS         DEPTH\n"
        "ALLOW_PONDING        NO\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             01:00:00\n"
        "REPORT_STEP          0:01:00\n"
        "ROUTING_STEP         1\n"
        "MIN_SURFAREA         0\n" + fv_extra +
        "\n" + junctions + "\n" + mid_section +
        "[OUTFALLS]\n"
        ";;Name  Elev   Type  Gated\n"
        "OF      105.0  FREE  NO\n"
        "\n"
        "[CONDUITS]\n"
        ";;Name  From  To   Length  N      Z1  Z2\n"
        "C1      J0    MID  100.0   0.013  0   0\n"
        "C2      MID   OF   100.0   0.013  0   0\n"
        "\n"
        "[XSECTIONS]\n"
        ";;Link  Shape     G1   G2  G3  G4  Barrels\n"
        "C1      CIRCULAR  1.0  0   0   0   1\n"
        "C2      CIRCULAR  1.0  0   0   0   1\n"
        "\n"
        "[DWF]\n"
        ";;Node  Param  Value\n"
        "J0      FLOW   0.6\n"
        "\n"
        "[COORDINATES]\n"
        ";;Node  X      Y\n"
        "J0      0.0    0.0\n"
        "MID     100.0  0.0\n"
        "OF      200.0  0.0\n";
}

// Open, run to completion, and return MID's final depth (ft).
double runMidDepth(const std::string& base, const std::string& text) {
    const std::string inp = outPath(base + ".inp");
    writeFile(inp, text);
    SWMM_Engine e = swmm_engine_create();
    EXPECT_EQ(swmm_engine_open(e, inp.c_str(),
                               outPath(base + ".rpt").c_str(),
                               outPath(base + ".out").c_str(), nullptr), 0)
        << swmm_get_last_error_msg(e);
    EXPECT_EQ(swmm_engine_initialize(e), 0) << swmm_get_last_error_msg(e);
    EXPECT_EQ(swmm_engine_start(e, 0), 0) << swmm_get_last_error_msg(e);
    double elapsed = 0.0;
    do {
        EXPECT_EQ(swmm_engine_step(e, &elapsed), 0)
            << swmm_get_last_error_msg(e);
    } while (elapsed > 0.0);
    const int mid = swmm_node_index(e, "MID");
    EXPECT_GE(mid, 0);
    double d = -1.0;
    if (mid >= 0) swmm_node_get_depth(e, mid, &d);
    swmm_engine_end(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    return d;
}

}  // namespace

TEST(FvNodeHeadConsistency, SteepPassThroughHeadIsFaceConsistent) {
    // Face-consistent stage at MID is invert + ~normal depth (< 1 ft for
    // 0.6 cfs in a 1-ft pipe at 20-25% slope). The retired cell-centre mean
    // manufactured ~2.5 ft from bed asymmetry alone.
    const double d = runMidDepth("steep_pass", steepChain(false));
    EXPECT_GT(d, 0.01);   // wet
    EXPECT_LT(d, 1.0);
}

TEST(FvNodeHeadConsistency, PassThroughAgreesWithVirtualJunction) {
    // The same physical chain, mid node declared as a plain junction vs a
    // virtual junction, must publish the same stage — the pass-through
    // estimator and the VJ publish rule are now the same reconstruction.
    const double d_pass = runMidDepth("agree_pass", steepChain(false));
    const double d_vj = runMidDepth("agree_vj", steepChain(true));
    EXPECT_NEAR(d_pass, d_vj, 0.05);
}

TEST(FvNodeHeadConsistency, SlotCapOverrideIsSurfaced) {
    // 8-ft circular pipe: cap-implied celerity ~ sqrt(505*8) ~ 64 ft/s, so
    // FV_SLOT_CELERITY 50 is overridden by the 5%-of-width cap -> WARNING 108.
    std::string big = steepChain(false, "FV_SLOT_CELERITY     50\n");
    // swap both conduits to 8 ft
    for (std::string::size_type p = big.find("CIRCULAR  1.0");
         p != std::string::npos; p = big.find("CIRCULAR  1.0"))
        big.replace(p, 13, "CIRCULAR  8.0");
    runMidDepth("slot_capped", big);
    EXPECT_NE(readFile(outPath("slot_capped.rpt")).find("WARNING 108"),
              std::string::npos);

    // Control: 1-ft pipe (cap-implied ~22 ft/s < 50) must NOT warn.
    runMidDepth("slot_uncapped",
                steepChain(false, "FV_SLOT_CELERITY     50\n"));
    EXPECT_EQ(readFile(outPath("slot_uncapped.rpt")).find("WARNING 108"),
              std::string::npos);
}
