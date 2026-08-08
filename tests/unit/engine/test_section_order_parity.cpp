/**
 * @file test_section_order_parity.cpp
 * @brief .inp section-order independence for sections that reference objects
 *        declared elsewhere — legacy two-pass parsing parity.
 *
 * @details The legacy engine parses the .inp in two passes, so a section
 *          may reference an object defined in a later section. The refactored
 *          single-pass parser used to resolve names at parse time and
 *          silently drop rows naming not-yet-parsed objects (zero RDII/DWF/
 *          external inflow with no warning). Node-referencing handlers now
 *          store the raw node name and PostParseResolver re-resolves;
 *          link-referencing handlers ([XSECTIONS], [LOSSES]) stash the
 *          unresolved row and InputReader replays it after the last section.
 *          A name that never resolves is a fatal ERR_NAME, matching legacy
 *          error_setInpError(ERR_NAME, ...) in inflow.c / rdii.c / link.c.
 *
 *          Each parity test runs the same model twice — sections in
 *          conventional (network-first) order and reversed — and requires
 *          identical inflow volumes.
 *
 *          Working directory is tests/unit/engine/data/ at runtime; all
 *          scratch files use unique names and are removed on teardown.
 *
 * @see Legacy: src/legacy/engine/project.c (two-pass), inflow.c, rdii.c
 * @ingroup engine_tests
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_massbalance.h>

namespace fs = std::filesystem;

namespace {

const char* kOptions = R"([OPTIONS]
FLOW_UNITS           CFS
FLOW_ROUTING         DYNWAVE
START_DATE           01/01/2026
START_TIME           00:00:00
REPORT_START_DATE    01/01/2026
REPORT_START_TIME    00:00:00
END_DATE             01/01/2026
END_TIME             02:00:00
REPORT_STEP          00:05:00
WET_STEP             00:05:00
DRY_STEP             00:05:00
ROUTING_STEP         0:00:30
)";

const char* kNetwork = R"(
[JUNCTIONS]
J1  100.0  10.0  0.0  0.0  0.0

[OUTFALLS]
O1  95.0  FREE

[CONDUITS]
C1  J1  O1  400.0  0.013  0  0

[XSECTIONS]
C1  CIRCULAR  1.5  0  0  0  1
)";

// [RDII] + its rain forcing, referencing node J1.
const char* kRdii = R"(
[RAINGAGES]
RG1  INTENSITY  0:05  1.0  TIMESERIES  TS1

[HYDROGRAPHS]
UH1  RG1
UH1  ALL  SHORT   0.30  1.0  2.0  0  0  0
UH1  ALL  MEDIUM  0.20  2.0  4.0  0  0  0
UH1  ALL  LONG    0.10  4.0  8.0  0  0  0

[RDII]
J1  UH1  5.0

[TIMESERIES]
TS1  0:00  1.0
TS1  1:00  0.0
)";

// [DWF] referencing node J1.
const char* kDwf = R"(
[DWF]
J1  FLOW  1.5
)";

// [INFLOWS] referencing node J1, with its timeseries.
const char* kInflows = R"(
[INFLOWS]
J1  FLOW  TS_IN  FLOW  1.0  1.0

[TIMESERIES]
TS_IN  0:00  2.0
TS_IN  2:00  2.0
)";

// ---------------------------------------------------------------------------
// Link-referencing property sections. [XSECTIONS] and [LOSSES] carry no link of
// their own — every row names a link declared elsewhere — so both are subject
// to the same ordering hazard as the node sections above.
// ---------------------------------------------------------------------------

// The nodes and the steady forcing, shared by the link-order fixtures.
const char* kLinkNodes = R"(
[JUNCTIONS]
J1  100.0  10.0  0.0  0.0  0.0

[OUTFALLS]
O1  95.0  FREE

[INFLOWS]
J1  FLOW  ""  FLOW  1.0  1.0  2.0
)";

const char* kConduitOnly = R"(
[CONDUITS]
C1  J1  O1  400.0  0.013  0  0
)";

const char* kConduitXsect = R"(
[XSECTIONS]
C1  CIRCULAR  1.5  0  0  0  1
)";

const char* kConduitLosses = R"(
[LOSSES]
C1  0  0  0  NO  0.50
)";

// The reported case: [XSECTIONS] sits in its conventional slot right after
// [CONDUITS], so geometry for an orifice declared in the later [ORIFICES]
// section could not resolve. Zero area meant zero flow under 5 ft of head.
const char* kOrificeOnly = R"(
[ORIFICES]
OR1  J1  O1  SIDE  0  0.65  NO  0
)";

const char* kOrificeXsect = R"(
[XSECTIONS]
OR1  CIRCULAR  1.0  0  0  0  1
)";

class SectionOrderParityTest : public ::testing::Test {
protected:
    SWMM_Engine engine_ = nullptr;
    std::vector<std::string> scratch_;

    void TearDown() override {
        destroy_engine();
        for (const auto& p : scratch_) {
            std::error_code ec;
            fs::remove(p, ec);
        }
    }

    void destroy_engine() {
        if (engine_) {
            swmm_engine_close(engine_);
            swmm_engine_destroy(engine_);
            engine_ = nullptr;
        }
    }

    std::string scratch(const std::string& name) {
        auto p = (fs::current_path() / name).string();
        scratch_.push_back(p);
        return p;
    }

    /// Open + run to completion; returns the requested routing total (ft3).
    double run_total(const std::string& tag, const std::string& inp_text,
                     int component) {
        const auto inp = scratch("secorder_" + tag + ".inp");
        const auto rpt = scratch("secorder_" + tag + ".rpt");
        const auto out = scratch("secorder_" + tag + ".out");
        {
            std::ofstream f(inp);
            EXPECT_TRUE(f.is_open());
            f << inp_text;
        }

        engine_ = swmm_engine_create();
        EXPECT_NE(engine_, nullptr);
        EXPECT_EQ(swmm_engine_open(engine_, inp.c_str(), rpt.c_str(),
                                   out.c_str(), nullptr), SWMM_OK)
            << swmm_get_last_error_msg(engine_);
        EXPECT_EQ(swmm_engine_initialize(engine_), SWMM_OK);
        EXPECT_EQ(swmm_engine_start(engine_, 0), SWMM_OK)
            << swmm_get_last_error_msg(engine_);

        double elapsed = 0.0;
        for (;;) {
            const int rc = swmm_engine_step(engine_, &elapsed);
            EXPECT_EQ(rc, SWMM_OK) << swmm_get_last_error_msg(engine_);
            if (rc != SWMM_OK || elapsed <= 0.0) break;
        }
        EXPECT_EQ(swmm_engine_end(engine_), SWMM_OK);

        double v = 0.0;
        EXPECT_EQ(swmm_get_routing_total(engine_, component, &v), SWMM_OK);
        destroy_engine();
        return v;
    }
};

// ---------------------------------------------------------------------------
// [RDII] before the network sections must match network-first exactly.
// ---------------------------------------------------------------------------

TEST_F(SectionOrderParityTest, RdiiBeforeNetwork_MatchesNetworkFirst) {
    const double conventional = run_total(
        "rdii_conv", std::string(kOptions) + kNetwork + kRdii,
        SWMM_ROUTING_RDII);
    ASSERT_GT(conventional, 0.0) << "fixture produced no RDII";

    const double reversed = run_total(
        "rdii_rev", std::string(kOptions) + kRdii + kNetwork,
        SWMM_ROUTING_RDII);

    EXPECT_DOUBLE_EQ(reversed, conventional)
        << "[RDII] before [JUNCTIONS] must parse identically (legacy parity)";
}

// ---------------------------------------------------------------------------
// [DWF] before the network sections must match network-first exactly.
// ---------------------------------------------------------------------------

TEST_F(SectionOrderParityTest, DwfBeforeNetwork_MatchesNetworkFirst) {
    const double conventional = run_total(
        "dwf_conv", std::string(kOptions) + kNetwork + kDwf,
        SWMM_ROUTING_DRY_WEATHER);
    ASSERT_GT(conventional, 0.0) << "fixture produced no DWF";

    const double reversed = run_total(
        "dwf_rev", std::string(kOptions) + kDwf + kNetwork,
        SWMM_ROUTING_DRY_WEATHER);

    EXPECT_DOUBLE_EQ(reversed, conventional)
        << "[DWF] before [JUNCTIONS] must parse identically (legacy parity)";
}

// ---------------------------------------------------------------------------
// [INFLOWS] before the network sections must match network-first exactly.
// ---------------------------------------------------------------------------

TEST_F(SectionOrderParityTest, InflowsBeforeNetwork_MatchesNetworkFirst) {
    const double conventional = run_total(
        "inf_conv", std::string(kOptions) + kNetwork + kInflows,
        SWMM_ROUTING_EXTERNAL);
    ASSERT_GT(conventional, 0.0) << "fixture produced no external inflow";

    const double reversed = run_total(
        "inf_rev", std::string(kOptions) + kInflows + kNetwork,
        SWMM_ROUTING_EXTERNAL);

    EXPECT_DOUBLE_EQ(reversed, conventional)
        << "[INFLOWS] before [JUNCTIONS] must parse identically (legacy parity)";
}

// ---------------------------------------------------------------------------
// [XSECTIONS] before [CONDUITS] must match conduits-first.
// ---------------------------------------------------------------------------

TEST_F(SectionOrderParityTest, XsectionsBeforeConduits_MatchesConduitsFirst) {
    const double conventional = run_total(
        "xsect_conv", std::string(kOptions) + kLinkNodes + kConduitOnly + kConduitXsect,
        SWMM_ROUTING_OUTFLOW);
    ASSERT_GT(conventional, 0.0) << "fixture routed nothing";

    const double reversed = run_total(
        "xsect_rev", std::string(kOptions) + kLinkNodes + kConduitXsect + kConduitOnly,
        SWMM_ROUTING_OUTFLOW);

    EXPECT_DOUBLE_EQ(reversed, conventional)
        << "[XSECTIONS] before [CONDUITS] must parse identically (legacy parity)";
}

// ---------------------------------------------------------------------------
// [XSECTIONS] in its conventional slot, ahead of [ORIFICES]. This is the case
// that shipped broken: the row resolved against no link and was dropped, so the
// orifice ran at zero area and passed no flow at all.
// ---------------------------------------------------------------------------

TEST_F(SectionOrderParityTest, XsectionsBeforeOrifices_MatchesOrificesFirst) {
    const double conventional = run_total(
        "orif_conv", std::string(kOptions) + kLinkNodes + kOrificeOnly + kOrificeXsect,
        SWMM_ROUTING_OUTFLOW);
    ASSERT_GT(conventional, 0.0) << "fixture routed nothing through the orifice";

    const double reversed = run_total(
        "orif_rev", std::string(kOptions) + kLinkNodes + kOrificeXsect + kOrificeOnly,
        SWMM_ROUTING_OUTFLOW);

    EXPECT_DOUBLE_EQ(reversed, conventional)
        << "[XSECTIONS] before [ORIFICES] must parse identically (legacy parity)";
}

// ---------------------------------------------------------------------------
// [LOSSES] before [CONDUITS] must match conduits-first. Gated on seepage, the
// one loss term a dropped row would silence outright.
// ---------------------------------------------------------------------------

TEST_F(SectionOrderParityTest, LossesBeforeConduits_MatchesConduitsFirst) {
    const double conventional = run_total(
        "loss_conv",
        std::string(kOptions) + kLinkNodes + kConduitOnly + kConduitXsect + kConduitLosses,
        SWMM_ROUTING_SEEP_LOSS);
    ASSERT_GT(conventional, 0.0) << "fixture produced no seepage";

    const double reversed = run_total(
        "loss_rev",
        std::string(kOptions) + kLinkNodes + kConduitLosses + kConduitOnly + kConduitXsect,
        SWMM_ROUTING_SEEP_LOSS);

    EXPECT_DOUBLE_EQ(reversed, conventional)
        << "[LOSSES] before [CONDUITS] must parse identically (legacy parity)";
}

// ---------------------------------------------------------------------------
// A genuinely unknown link is fatal too — a row that survives the deferred
// replay names an object that does not exist.
// ---------------------------------------------------------------------------

TEST_F(SectionOrderParityTest, UnknownLinkFailsOpen) {
    const struct { const char* tag; const char* section; } cases[] = {
        {"bad_xsect",  "\n[XSECTIONS]\nNO_SUCH_LINK  CIRCULAR  1.5  0  0  0  1\n"},
        {"bad_losses", "\n[LOSSES]\nNO_SUCH_LINK  0  0  0  NO  0.50\n"},
    };

    for (const auto& c : cases) {
        const auto inp = scratch(std::string("secorder_") + c.tag + ".inp");
        const auto rpt = scratch(std::string("secorder_") + c.tag + ".rpt");
        const auto out = scratch(std::string("secorder_") + c.tag + ".out");
        {
            std::ofstream f(inp);
            ASSERT_TRUE(f.is_open());
            f << kOptions << kLinkNodes << kConduitOnly << kConduitXsect << c.section;
        }
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
        EXPECT_NE(swmm_engine_open(engine_, inp.c_str(), rpt.c_str(),
                                   out.c_str(), nullptr), SWMM_OK)
            << c.tag << ": unknown link must fail open (legacy ERR_NAME)";
        destroy_engine();
    }
}

// ---------------------------------------------------------------------------
// A genuinely unknown node is a fatal input error (legacy ERR_NAME), not a
// silent drop — in any of the three sections.
// ---------------------------------------------------------------------------

TEST_F(SectionOrderParityTest, UnknownNodeFailsOpen) {
    const struct { const char* tag; const char* section; } cases[] = {
        {"bad_rdii", "\n[RAINGAGES]\nRG1  INTENSITY  0:05  1.0  TIMESERIES  TS1\n"
                     "\n[HYDROGRAPHS]\nUH1  RG1\nUH1  ALL  SHORT  0.30  1.0  2.0  0  0  0\n"
                     "\n[RDII]\nNO_SUCH_NODE  UH1  5.0\n"
                     "\n[TIMESERIES]\nTS1  0:00  1.0\nTS1  1:00  0.0\n"},
        {"bad_dwf", "\n[DWF]\nNO_SUCH_NODE  FLOW  1.5\n"},
        {"bad_inf", "\n[INFLOWS]\nNO_SUCH_NODE  FLOW  TS_IN\n"
                    "\n[TIMESERIES]\nTS_IN  0:00  2.0\nTS_IN  2:00  2.0\n"},
    };

    for (const auto& c : cases) {
        const auto inp = scratch(std::string("secorder_") + c.tag + ".inp");
        const auto rpt = scratch(std::string("secorder_") + c.tag + ".rpt");
        const auto out = scratch(std::string("secorder_") + c.tag + ".out");
        {
            std::ofstream f(inp);
            ASSERT_TRUE(f.is_open());
            f << kOptions << kNetwork << c.section;
        }
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
        EXPECT_NE(swmm_engine_open(engine_, inp.c_str(), rpt.c_str(),
                                   out.c_str(), nullptr), SWMM_OK)
            << c.tag << ": unknown node must fail open (legacy ERR_NAME)";
        destroy_engine();
    }
}

} // namespace
