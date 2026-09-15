/**
 * @file test_object_deletion_ext.cpp
 * @brief Unit tests for the extended delete APIs (Phase 0.2): pollutant,
 *        pattern, aquifer, snowpack, LID control, street, inlet design,
 *        land use, and unit-hydrograph group.
 *
 * @details Models are built from INP text (several stores — inlet usage,
 *          snowpack assignment, STREET cross-sections — have no creation
 *          C API). Fixture files are written to a persistent, reviewable
 *          artifact directory relative to the test cwd
 *          (tests/unit/engine/data/deletion_ext/), never to temp dirs.
 *
 * @ingroup engine_tests
 */

#include <gtest/gtest.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_subcatchments.h>
#include <openswmm/engine/openswmm_gages.h>
#include <openswmm/engine/openswmm_tables.h>
#include <openswmm/engine/openswmm_edit.h>
#include <openswmm/engine/openswmm_inflows.h>
#include <openswmm/engine/openswmm_pollutants.h>
#include <openswmm/engine/openswmm_quality.h>
#include <openswmm/engine/openswmm_infrastructure.h>
#include <openswmm/engine/openswmm_controls.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

// Persistent, reviewable artifact directory (relative to the test cwd, which
// CMake pins to tests/unit/engine/data). NOT deleted after the run.
const fs::path kArtifactDir = "deletion_ext";

void write_file(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

int count_entries(const SWMM_ImpactReport& r, int obj_type,
                  const char* field, int cascaded) {
    int n = 0;
    for (int i = 0; i < r.n_entries; ++i) {
        if (r.entries[i].obj_type == obj_type &&
            r.entries[i].cascaded == cascaded &&
            std::strcmp(r.entries[i].field, field) == 0)
            ++n;
    }
    return n;
}

// One model exercising every store the new delete APIs touch.
std::string full_model() {
    return
        "[OPTIONS]\n"
        "FLOW_UNITS           CFS\n"
        "FLOW_ROUTING         KINWAVE\n"
        "INFILTRATION         HORTON\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             02:00:00\n"
        "REPORT_STEP          00:05:00\n"
        "ROUTING_STEP         0:00:30\n"
        "\n[RAINGAGES]\n"
        "RG1  INTENSITY 0:05 1.0 TIMESERIES TS1\n"
        "\n[SUBCATCHMENTS]\n"
        ";;Name RainGage Outlet Area %Imperv Width Slope Curblen SnowPack\n"
        "S1  RG1  J1  10.0  50  500  0.5  0  SP1\n"
        "S2  RG1  J1  5.0   50  400  0.5  0\n"
        "\n[SUBAREAS]\n"
        "S1  0.01  0.1  0.05  0.05  25  OUTLET\n"
        "S2  0.01  0.1  0.05  0.05  25  OUTLET\n"
        "\n[INFILTRATION]\n"
        "S1  3.0  0.5  4.0  7  0\n"
        "S2  3.0  0.5  4.0  7  0\n"
        "\n[TIMESERIES]\n"
        "TS1  01/01/2026 00:00 1.0\n"
        "TS1  01/01/2026 01:00 0.0\n"
        "\n[JUNCTIONS]\n"
        "J1  100.0  10.0  0.0  0.0  0.0\n"
        "\n[OUTFALLS]\n"
        "O1  95.0  FREE\n"
        "\n[CONDUITS]\n"
        "C1  J1  O1  400.0  0.013  0  0\n"
        "C2  J1  O1  400.0  0.016  0  0\n"
        "\n[XSECTIONS]\n"
        "C1  CIRCULAR  1.5  0  0  0  1\n"
        "C2  STREET  ST1\n"
        "\n[STREETS]\n"
        ";;Name Tcrown Hcurb Sx nRoad a W Sides Tback Sback nBack\n"
        "ST1  20  0.5  4  0.016  0  0  1  20  4  0.016\n"
        "\n[INLETS]\n"
        ";;Name Type Length Width Grate\n"
        "IN1  GRATE  2  2  P_BAR-50\n"
        "\n[INLET_USAGE]\n"
        ";;Conduit Inlet Node\n"
        "C2  IN1  J1\n"
        "\n[POLLUTANTS]\n"
        ";;Name Units Crain Cgw Crdii Kdecay SnowOnly CoPollut CoFrac Cdwf Cinit\n"
        "TSS   MG/L  0  0  0  0  NO  *    0.0   0  0\n"
        "Lead  UG/L  0  0  0  0  NO  TSS  0.25  0  0\n"
        "\n[LANDUSES]\n"
        "Res  0  0  0\n"
        "Com  0  0  0\n"
        "\n[COVERAGES]\n"
        "S1  Res  60\n"
        "S1  Com  40\n"
        "\n[LOADINGS]\n"
        "S1  TSS  1.5\n"
        "S1  Lead  2.0\n"
        "\n[BUILDUP]\n"
        "Res  TSS  POW  100  0  1  AREA\n"
        "Com  TSS  POW  50   0  1  AREA\n"
        "\n[WASHOFF]\n"
        "Res  TSS  EXP  0.1  1.0  0  0\n"
        "\n[TREATMENT]\n"
        "J1  TSS  R = 0.5\n"
        "\n[PATTERNS]\n"
        "P1  MONTHLY  1.0 1.0 1.0 1.0 1.0 1.0 1.0 1.0 1.0 1.0 1.0 1.0\n"
        "\n[DWF]\n"
        "J1  FLOW  1.0  P1\n"
        "\n[AQUIFERS]\n"
        ";;Name Por WP FC Ksat Kslope Tslope ETu ETs Seep Ebot Egw Umc ETpat\n"
        "AQ1  0.5  0.15  0.30  5.0  10  15  0.35  14  0.0  0.0  8.0  0.30  P1\n"
        "\n[GROUNDWATER]\n"
        "S1  AQ1  J1  10.0  0.1  1.5  0  0  0  0  *  0  0  *\n"
        "\n[SNOWPACKS]\n"
        "SP1  PLOWABLE    0.001  0.001  32  0.10  0  0  0.5\n"
        "SP1  IMPERVIOUS  0.001  0.001  32  0.10  0  0  0.0\n"
        "SP1  PERVIOUS    0.001  0.001  32  0.10  0  0  0.0\n"
        "SP1  REMOVAL     1.0  0  0  0  0  0  S2\n"
        "\n[LID_CONTROLS]\n"
        "L1  BC\n"
        "L1  SURFACE  6  0.25  0.1  1  5\n"
        "L1  SOIL     12  0.5  0.2  0.1  0.5  10  3.5\n"
        "L1  STORAGE  12  0.75  0.5  0\n"
        "L1  DRAIN    0  0.5  6  6  0  0\n"
        "\n[LID_USAGE]\n"
        ";;Subcatch LID Number Area Width InitSat FromImp ToPerv\n"
        "S1  L1  1  50  10  0  25  0\n"
        "\n[HYDROGRAPHS]\n"
        "UH1  RG1\n"
        "UH1  ALL  SHORT  0.033  1.0  2.0  0  0  0\n"
        "\n[RDII]\n"
        "J1  UH1  12.0\n"
        "\n[CONTROLS]\n"
        "RULE R1\n"
        "IF NODE J1 DEPTH > 2\n"
        "THEN CONDUIT C1 STATUS = CLOSED\n"
        "PRIORITY 1\n"
        "\n"
        "RULE R2\n"
        "IF SIMULATION TIME > 1\n"
        "THEN CONDUIT C2 STATUS = CLOSED\n"
        "PRIORITY 2\n";
}

} // namespace

// ============================================================================
// Fixture — opens the full model from a written INP (OPENED state is
// editable, so the delete APIs work directly on it).
// ============================================================================

class DeletionExtTest : public ::testing::Test {
protected:
    SWMM_Engine engine = nullptr;
    fs::path inp_path, rpt_path, out_path;

    void open_model(const std::string& tag) {
        inp_path = kArtifactDir / (tag + ".inp");
        rpt_path = kArtifactDir / (tag + ".rpt");
        out_path = kArtifactDir / (tag + ".out");
        write_file(inp_path, full_model());

        engine = swmm_engine_create();
        ASSERT_NE(engine, nullptr);
        ASSERT_EQ(swmm_engine_open(engine, inp_path.string().c_str(),
                                   rpt_path.string().c_str(),
                                   out_path.string().c_str(), nullptr), SWMM_OK)
            << swmm_get_last_error_msg(engine);
    }

    // Write the edited model back out and return its text (round-trip oracle).
    std::string written_model(const std::string& tag) {
        const fs::path p = kArtifactDir / (tag + "_objdel_out.inp");
        EXPECT_EQ(swmm_model_write(engine, p.string().c_str()), SWMM_OK);
        return read_file(p);
    }

    void TearDown() override {
        if (engine) swmm_engine_destroy(engine);
    }
};

// ============================================================================
// Pollutant
// ============================================================================

TEST_F(DeletionExtTest, PollutantDeleteRepacksAndNullifies) {
    open_model("pollut_delete");
    ASSERT_EQ(swmm_pollutant_count(engine), 2);
    const int tss = swmm_pollutant_index(engine, "TSS");
    ASSERT_EQ(tss, 0);

    SWMM_ImpactReport report{};
    ASSERT_EQ(swmm_pollutant_delete(engine, tss, &report), SWMM_OK);
    EXPECT_GE(count_entries(report, SWMM_REF_LANDUSE, "buildup", 1), 2);   // Res + Com
    EXPECT_GE(count_entries(report, SWMM_REF_LANDUSE, "washoff", 1), 1);   // Res
    EXPECT_EQ(count_entries(report, SWMM_REF_TREATMENT, "expression", 1), 1);
    EXPECT_EQ(count_entries(report, SWMM_REF_POLLUTANT, "co_pollut", 0), 1);
    swmm_impact_report_free(&report);

    ASSERT_EQ(swmm_pollutant_count(engine), 1);
    EXPECT_STREQ(swmm_pollutant_id(engine, 0), "Lead");

    // Lead's co-pollutant reference (was TSS) must be nullified.
    int co = -99; double frac = -1.0;
    ASSERT_EQ(swmm_pollutant_get_co_pollutant(engine, 0, &co, &frac), SWMM_OK);
    EXPECT_EQ(co, -1);

    // Buildup/washoff matrices re-packed: Lead (now idx 0) has no functions.
    int ft = -1, norm = 0; double c1, c2, c3;
    ASSERT_EQ(swmm_buildup_get(engine, 0, 0, &ft, &c1, &c2, &c3, &norm), SWMM_OK);
    EXPECT_EQ(ft, 0);

    // Treatment for TSS is gone; the matrix must not misalign (no crash, no
    // stale expression on Lead).
    char buf[64] = {0};
    ASSERT_EQ(swmm_treatment_get(engine, 0, 0, buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "");

    // Lead's [LOADINGS] initial buildup must survive the column re-pack.
    const std::string out = written_model("pollut_delete");
    EXPECT_NE(out.find("Lead"), std::string::npos);
    EXPECT_EQ(out.find("TSS"), std::string::npos);
}

// ============================================================================
// Pattern
// ============================================================================

TEST_F(DeletionExtTest, PatternDeleteClearsNameRefs) {
    open_model("pattern_delete");
    const int p1 = swmm_pattern_index(engine, "P1");
    ASSERT_GE(p1, 0);

    SWMM_ImpactReport report{};
    ASSERT_EQ(swmm_pattern_delete(engine, p1, &report), SWMM_OK);
    EXPECT_EQ(count_entries(report, SWMM_REF_DWF_INFLOW, "pat1", 0), 1);
    EXPECT_EQ(count_entries(report, SWMM_REF_AQUIFER, "upper_evap_pat", 0), 1);
    swmm_impact_report_free(&report);

    EXPECT_EQ(swmm_pattern_count(engine), 0);

    // DWF row survives with its pattern cleared.
    ASSERT_EQ(swmm_dwf_count(engine), 1);
    int node = -1; double avg = 0.0;
    char cons[16], pa[32], pb[32], pc[32], pd[32];
    ASSERT_EQ(swmm_dwf_get(engine, 0, &node, cons, sizeof cons, &avg,
                           pa, sizeof pa, pb, sizeof pb,
                           pc, sizeof pc, pd, sizeof pd), SWMM_OK);
    EXPECT_STREQ(pa, "");

    char pat[32] = {0};
    ASSERT_EQ(swmm_aquifer_get_evap_pattern(engine, 0, pat, sizeof pat), SWMM_OK);
    EXPECT_STREQ(pat, "");
}

// ============================================================================
// Aquifer
// ============================================================================

TEST_F(DeletionExtTest, AquiferDeleteDropsSubcatchGroundwater) {
    open_model("aquifer_delete");
    ASSERT_EQ(swmm_aquifer_count(engine), 1);

    SWMM_ImpactReport report{};
    ASSERT_EQ(swmm_aquifer_delete(engine, 0, &report), SWMM_OK);
    EXPECT_EQ(count_entries(report, SWMM_REF_SUBCATCH, "gw_aquifer", 0), 1);
    swmm_impact_report_free(&report);

    EXPECT_EQ(swmm_aquifer_count(engine), 0);
    int aq = -99;
    ASSERT_EQ(swmm_subcatch_get_aquifer(engine, 0, &aq), SWMM_OK);
    EXPECT_EQ(aq, -1);
}

// ============================================================================
// Snowpack
// ============================================================================

TEST_F(DeletionExtTest, SnowpackDeleteClearsSubcatchRef) {
    open_model("snowpack_delete");
    ASSERT_EQ(swmm_snowpack_count(engine), 1);

    SWMM_ImpactReport report{};
    ASSERT_EQ(swmm_snowpack_delete(engine, 0, &report), SWMM_OK);
    EXPECT_EQ(count_entries(report, SWMM_REF_SUBCATCH, "snowpack", 0), 1);
    swmm_impact_report_free(&report);

    EXPECT_EQ(swmm_snowpack_count(engine), 0);
    // The written model must not reference SP1 anywhere (subcatchment column
    // cleared, [SNOWPACKS] section gone).
    const std::string out = written_model("snowpack_delete");
    EXPECT_EQ(out.find("SP1"), std::string::npos);
}

// ============================================================================
// LID control
// ============================================================================

TEST_F(DeletionExtTest, LidDeleteCascadesUsage) {
    open_model("lid_delete");
    ASSERT_EQ(swmm_lid_count(engine), 1);
    ASSERT_EQ(swmm_lid_usage_count(engine), 1);

    SWMM_ImpactReport report{};
    ASSERT_EQ(swmm_lid_delete(engine, 0, &report), SWMM_OK);
    EXPECT_EQ(count_entries(report, SWMM_REF_LID_USAGE, "lid_index", 1), 1);
    swmm_impact_report_free(&report);

    EXPECT_EQ(swmm_lid_count(engine), 0);
    EXPECT_EQ(swmm_lid_usage_count(engine), 0);
}

// ============================================================================
// Street
// ============================================================================

TEST_F(DeletionExtTest, StreetDeleteResetsStreetXsections) {
    open_model("street_delete");
    ASSERT_EQ(swmm_street_count(engine), 1);

    SWMM_ImpactReport report{};
    ASSERT_EQ(swmm_street_delete(engine, 0, &report), SWMM_OK);
    EXPECT_EQ(count_entries(report, SWMM_REF_LINK, "xsect_street", 0), 1);
    swmm_impact_report_free(&report);

    EXPECT_EQ(swmm_street_count(engine), 0);
    // C2's STREET cross-section must fall back (no dangling ST1 name).
    const std::string out = written_model("street_delete");
    EXPECT_EQ(out.find("ST1"), std::string::npos);
}

// ============================================================================
// Inlet design
// ============================================================================

TEST_F(DeletionExtTest, InletDeleteCascadesUsage) {
    open_model("inlet_delete");
    ASSERT_EQ(swmm_inlet_count(engine), 1);

    SWMM_ImpactReport predicted{};
    ASSERT_EQ(swmm_inlet_analyze_impact(engine, 0, &predicted), SWMM_OK);
    EXPECT_EQ(count_entries(predicted, SWMM_REF_INLET_USAGE, "design_index", 1), 1);
    swmm_impact_report_free(&predicted);

    SWMM_ImpactReport report{};
    ASSERT_EQ(swmm_inlet_delete(engine, 0, &report), SWMM_OK);
    EXPECT_EQ(count_entries(report, SWMM_REF_INLET_USAGE, "design_index", 1), 1);
    swmm_impact_report_free(&report);

    EXPECT_EQ(swmm_inlet_count(engine), 0);
    const std::string out = written_model("inlet_delete");
    EXPECT_EQ(out.find("IN1"), std::string::npos);
}

// ============================================================================
// Land use
// ============================================================================

TEST_F(DeletionExtTest, LanduseDeleteRepacksCoverage) {
    open_model("landuse_delete");
    ASSERT_EQ(swmm_landuse_count(engine), 2);
    const int res = swmm_landuse_index(engine, "Res");
    ASSERT_EQ(res, 0);

    SWMM_ImpactReport report{};
    ASSERT_EQ(swmm_landuse_delete(engine, res, &report), SWMM_OK);
    EXPECT_GE(count_entries(report, SWMM_REF_POLLUTANT, "buildup", 1), 1);
    EXPECT_GE(count_entries(report, SWMM_REF_POLLUTANT, "washoff", 1), 1);
    EXPECT_EQ(count_entries(report, SWMM_REF_SUBCATCH, "coverage", 0), 1);
    swmm_impact_report_free(&report);

    ASSERT_EQ(swmm_landuse_count(engine), 1);
    EXPECT_STREQ(swmm_landuse_id(engine, 0), "Com");

    // S1's Com coverage (40%, stored as percent) must move with the re-pack.
    double pct = -1.0;
    ASSERT_EQ(swmm_subcatch_get_coverage(engine, 0, 0, &pct), SWMM_OK);
    EXPECT_NEAR(pct, 40.0, 1e-9);

    // Com's buildup (was at row 1) must move to row 0 intact.
    int ft = -1, norm = 0; double c1 = 0, c2 = 0, c3 = 0;
    const int tss = swmm_pollutant_index(engine, "TSS");
    ASSERT_GE(tss, 0);
    ASSERT_EQ(swmm_buildup_get(engine, 0, tss, &ft, &c1, &c2, &c3, &norm), SWMM_OK);
    EXPECT_EQ(ft, 1);              // POW
    EXPECT_NEAR(c1, 50.0, 1e-9);   // Com's coefficient, not Res's 100
}

// ============================================================================
// Unit-hydrograph group
// ============================================================================

TEST_F(DeletionExtTest, HydrographDeleteCascadesRdii) {
    open_model("hydrograph_delete");
    ASSERT_EQ(swmm_hydrograph_group_count(engine), 1);
    ASSERT_EQ(swmm_rdii_count(engine), 1);

    SWMM_ImpactReport report{};
    ASSERT_EQ(swmm_hydrograph_delete(engine, "UH1", &report), SWMM_OK);
    EXPECT_EQ(count_entries(report, SWMM_REF_RDII_ASSIGN, "uh_name", 1), 1);
    swmm_impact_report_free(&report);

    EXPECT_EQ(swmm_hydrograph_group_count(engine), 0);
    EXPECT_EQ(swmm_rdii_count(engine), 0);
    EXPECT_EQ(swmm_hydrograph_gage_count(engine), 0);

    // Unknown group name is rejected.
    EXPECT_EQ(swmm_hydrograph_delete(engine, "NOPE", nullptr), SWMM_ERR_BADPARAM);
}

// ============================================================================
// analyze_impact parity for the new types (predict == report, as multisets
// of (type, field, cascaded) — the shared scan guarantees it)
// ============================================================================

TEST_F(DeletionExtTest, AnalyzeMatchesDeleteForPollutant) {
    open_model("pollut_parity");
    SWMM_ImpactReport predicted{};
    ASSERT_EQ(swmm_pollutant_analyze_impact(engine, 0, &predicted), SWMM_OK);
    SWMM_ImpactReport actual{};
    ASSERT_EQ(swmm_pollutant_delete(engine, 0, &actual), SWMM_OK);
    EXPECT_EQ(predicted.n_entries, actual.n_entries);
    swmm_impact_report_free(&predicted);
    swmm_impact_report_free(&actual);
}

// ============================================================================
// Control-rule reference scan (Phase 0.3)
// ============================================================================

TEST_F(DeletionExtTest, ControlFindReferencesMatchesClauses) {
    open_model("control_refs");
    ASSERT_EQ(swmm_control_count(engine), 2);

    // J1 appears in R1's premise only.
    int idx[4] = {-1, -1, -1, -1};
    int n = 4;
    ASSERT_EQ(swmm_control_find_references(engine, "J1", idx, &n), SWMM_OK);
    ASSERT_EQ(n, 1);
    EXPECT_EQ(idx[0], 0);

    // C2 appears in R2's action only; case-insensitive match.
    n = 4;
    ASSERT_EQ(swmm_control_find_references(engine, "c2", idx, &n), SWMM_OK);
    ASSERT_EQ(n, 1);
    EXPECT_EQ(idx[0], 1);

    // O1 appears in no rule.
    n = 4;
    ASSERT_EQ(swmm_control_find_references(engine, "O1", idx, &n), SWMM_OK);
    EXPECT_EQ(n, 0);

    // Count-only query (NULL out array).
    n = 0;
    ASSERT_EQ(swmm_control_find_references(engine, "C1", nullptr, &n), SWMM_OK);
    EXPECT_EQ(n, 1);
}

TEST_F(DeletionExtTest, NodeDeleteReportsAffectedRulesWithoutEditingThem) {
    open_model("control_refs_delete");
    const int j1 = swmm_node_index(engine, "J1");
    ASSERT_GE(j1, 0);

    SWMM_ImpactReport predicted{};
    ASSERT_EQ(swmm_node_analyze_impact(engine, j1, &predicted), SWMM_OK);
    // J1 is in R1 directly; C1 and C2 (cascaded links) are in R1/R2 — the
    // analyze report carries at least J1's own rule hit.
    EXPECT_GE(count_entries(predicted, SWMM_REF_CONTROL_RULE, "rule_text", 0), 1);
    swmm_impact_report_free(&predicted);

    SWMM_ImpactReport report{};
    ASSERT_EQ(swmm_node_delete(engine, j1, &report), SWMM_OK);
    // Delete reports J1's rule plus the cascaded links' rules (C1→R1, C2→R2).
    EXPECT_GE(count_entries(report, SWMM_REF_CONTROL_RULE, "rule_text", 0), 3);
    swmm_impact_report_free(&report);

    // Rule text is NEVER edited by a delete.
    EXPECT_EQ(swmm_control_count(engine), 2);
    char buf[256] = {0};
    ASSERT_EQ(swmm_control_get_rule(engine, 0, buf, sizeof buf), SWMM_OK);
    EXPECT_NE(std::string(buf).find("J1"), std::string::npos);
}

TEST_F(DeletionExtTest, ControlRemoveRuleShiftsIndices) {
    open_model("control_remove");
    ASSERT_EQ(swmm_control_count(engine), 2);
    ASSERT_EQ(swmm_control_remove_rule(engine, 0), SWMM_OK);
    ASSERT_EQ(swmm_control_count(engine), 1);

    char buf[256] = {0};
    ASSERT_EQ(swmm_control_get_rule(engine, 0, buf, sizeof buf), SWMM_OK);
    EXPECT_NE(std::string(buf).find("R2"), std::string::npos);

    EXPECT_EQ(swmm_control_remove_rule(engine, 5), SWMM_ERR_BADINDEX);
}

TEST_F(DeletionExtTest, AnalyzeImpactDoesNotMutateNewTypes) {
    open_model("analyze_no_mutate");
    SWMM_ImpactReport r{};
    ASSERT_EQ(swmm_pattern_analyze_impact(engine, 0, &r), SWMM_OK);
    swmm_impact_report_free(&r);
    ASSERT_EQ(swmm_aquifer_analyze_impact(engine, 0, &r), SWMM_OK);
    swmm_impact_report_free(&r);
    ASSERT_EQ(swmm_snowpack_analyze_impact(engine, 0, &r), SWMM_OK);
    swmm_impact_report_free(&r);
    ASSERT_EQ(swmm_lid_analyze_impact(engine, 0, &r), SWMM_OK);
    swmm_impact_report_free(&r);
    ASSERT_EQ(swmm_street_analyze_impact(engine, 0, &r), SWMM_OK);
    swmm_impact_report_free(&r);
    ASSERT_EQ(swmm_landuse_analyze_impact(engine, 0, &r), SWMM_OK);
    swmm_impact_report_free(&r);
    ASSERT_EQ(swmm_hydrograph_analyze_impact(engine, "UH1", &r), SWMM_OK);
    swmm_impact_report_free(&r);

    EXPECT_EQ(swmm_pattern_count(engine), 1);
    EXPECT_EQ(swmm_aquifer_count(engine), 1);
    EXPECT_EQ(swmm_snowpack_count(engine), 1);
    EXPECT_EQ(swmm_lid_count(engine), 1);
    EXPECT_EQ(swmm_street_count(engine), 1);
    EXPECT_EQ(swmm_landuse_count(engine), 2);
    EXPECT_EQ(swmm_hydrograph_group_count(engine), 1);
    EXPECT_EQ(swmm_pollutant_count(engine), 2);
}
