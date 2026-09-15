/**
 * @file test_quality_roundtrip.cpp
 * @brief Iteration-4 quality round-trip coverage: [COVERAGES]/[LOADINGS]
 *        writer emission, initial-loading + bulk-coverage C APIs, land use /
 *        pollutant rename, grow-preserving adds, and the non-mutating
 *        treatment expression validator.
 *
 * @details Fixture models are written to a persistent, reviewable artifact
 *          directory relative to the test cwd
 *          (tests/unit/engine/data/quality_roundtrip/), never to temp dirs.
 *
 * @ingroup engine_tests
 */

#include <gtest/gtest.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_subcatchments.h>
#include <openswmm/engine/openswmm_pollutants.h>
#include <openswmm/engine/openswmm_quality.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_massbalance.h>

#include <cmath>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

const fs::path kArtifactDir = "quality_roundtrip";

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

// Small quality-heavy model: 2 subcatchments, 2 pollutants, 2 land uses,
// coverages, loadings, buildup, washoff, one DWF row keyed by pollutant name.
std::string quality_model() {
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
        "S1  RG1  J1  10.0  50  500  0.5  100\n"
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
        "\n[XSECTIONS]\n"
        "C1  CIRCULAR  1.5  0  0  0  1\n"
        "\n[POLLUTANTS]\n"
        ";;Name Units Crain Cgw Crdii Kdecay SnowOnly CoPollut CoFrac Cdwf Cinit\n"
        "TSS   MG/L  10  1  2  0.1  NO  *    0.0   3  4\n"
        "Lead  UG/L  0   0  0  0    NO  TSS  0.25  0  0\n"
        "\n[LANDUSES]\n"
        "Res  7   0.5  2\n"
        "Com  14  0.3  0\n"
        "\n[COVERAGES]\n"
        "S1  Res  60\n"
        "S1  Com  40\n"
        "S2  Res  25\n"
        "\n[LOADINGS]\n"
        "S1  TSS  1.5\n"
        "S2  Lead  2.25\n"
        "\n[BUILDUP]\n"
        "Res  TSS  POW  100  2  1.5  AREA\n"
        "Com  Lead  SAT  50  0  3  CURB\n"
        "\n[WASHOFF]\n"
        "Res  TSS  EXP  0.1  1.2  30  15\n"
        "\n[DWF]\n"
        "J1  Lead  0.5\n";
}

} // namespace

// ============================================================================
// Fixture
// ============================================================================

class QualityRoundtripTest : public ::testing::Test {
protected:
    SWMM_Engine engine = nullptr;

    void open_model(const std::string& tag,
                    const std::string& text = quality_model()) {
        const fs::path inp = kArtifactDir / (tag + ".inp");
        write_file(inp, text);
        engine = swmm_engine_create();
        ASSERT_NE(engine, nullptr);
        ASSERT_EQ(swmm_engine_open(engine, inp.string().c_str(),
                                   (kArtifactDir / (tag + ".rpt")).string().c_str(),
                                   (kArtifactDir / (tag + ".out")).string().c_str(),
                                   nullptr), SWMM_OK)
            << swmm_get_last_error_msg(engine);
    }

    std::string written_model(const std::string& tag) {
        const fs::path p = kArtifactDir / (tag + "_out.inp");
        EXPECT_EQ(swmm_model_write(engine, p.string().c_str()), SWMM_OK);
        return read_file(p);
    }

    void reopen_from(const std::string& tag) {
        const fs::path p = kArtifactDir / (tag + "_out.inp");
        swmm_engine_destroy(engine);
        engine = swmm_engine_create();
        ASSERT_NE(engine, nullptr);
        ASSERT_EQ(swmm_engine_open(engine, p.string().c_str(),
                                   (kArtifactDir / (tag + "_r.rpt")).string().c_str(),
                                   (kArtifactDir / (tag + "_r.out")).string().c_str(),
                                   nullptr), SWMM_OK)
            << swmm_get_last_error_msg(engine);
    }

    void TearDown() override {
        if (engine) swmm_engine_destroy(engine);
    }
};

// ============================================================================
// [COVERAGES] + [LOADINGS] survive write → reopen
// ============================================================================

TEST_F(QualityRoundtripTest, CoveragesAndLoadingsSurviveInpRoundtrip) {
    open_model("cov_load");

    const std::string text = written_model("cov_load");
    EXPECT_NE(text.find("[COVERAGES]"), std::string::npos);
    EXPECT_NE(text.find("[LOADINGS]"), std::string::npos);

    reopen_from("cov_load");

    const int s1 = swmm_subcatch_index(engine, "S1");
    const int s2 = swmm_subcatch_index(engine, "S2");
    const int res = swmm_landuse_index(engine, "Res");
    const int com = swmm_landuse_index(engine, "Com");
    ASSERT_GE(s1, 0); ASSERT_GE(s2, 0); ASSERT_GE(res, 0); ASSERT_GE(com, 0);

    double v = -1.0;
    ASSERT_EQ(swmm_subcatch_get_coverage(engine, s1, res, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 60.0);
    ASSERT_EQ(swmm_subcatch_get_coverage(engine, s1, com, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 40.0);
    ASSERT_EQ(swmm_subcatch_get_coverage(engine, s2, res, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 25.0);
    ASSERT_EQ(swmm_subcatch_get_coverage(engine, s2, com, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 0.0);

    const int tss = swmm_pollutant_index(engine, "TSS");
    const int lead = swmm_pollutant_index(engine, "Lead");
    ASSERT_GE(tss, 0); ASSERT_GE(lead, 0);
    ASSERT_EQ(swmm_subcatch_get_initial_loading(engine, s1, tss, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 1.5);
    ASSERT_EQ(swmm_subcatch_get_initial_loading(engine, s2, lead, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 2.25);
    ASSERT_EQ(swmm_subcatch_get_initial_loading(engine, s1, lead, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 0.0);

    // Buildup/washoff came along too (pre-existing writer sections).
    int ft = -1, norm = -1;
    double c1 = 0, c2 = 0, c3 = 0;
    ASSERT_EQ(swmm_buildup_get(engine, res, tss, &ft, &c1, &c2, &c3, &norm),
              SWMM_OK);
    EXPECT_EQ(ft, 1);                 // POW
    EXPECT_DOUBLE_EQ(c1, 100.0);
    EXPECT_DOUBLE_EQ(c3, 1.5);
    EXPECT_EQ(norm, 0);               // AREA
}

// ============================================================================
// Loadings setter + bulk coverage getter
// ============================================================================

TEST_F(QualityRoundtripTest, InitialLoadingSetGetAndBulkCoverages) {
    open_model("loading_api");

    const int s2 = swmm_subcatch_index(engine, "S2");
    const int tss = swmm_pollutant_index(engine, "TSS");
    ASSERT_GE(s2, 0); ASSERT_GE(tss, 0);

    ASSERT_EQ(swmm_subcatch_set_initial_loading(engine, s2, tss, 7.5), SWMM_OK);
    double v = 0;
    ASSERT_EQ(swmm_subcatch_get_initial_loading(engine, s2, tss, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 7.5);

    // Bulk coverages for S1: [Res, Com] = [60, 40].
    const int s1 = swmm_subcatch_index(engine, "S1");
    double cov[2] = {-1, -1};
    ASSERT_EQ(swmm_subcatch_get_coverages(engine, s1, cov, 2), SWMM_OK);
    EXPECT_DOUBLE_EQ(cov[swmm_landuse_index(engine, "Res")], 60.0);
    EXPECT_DOUBLE_EQ(cov[swmm_landuse_index(engine, "Com")], 40.0);

    EXPECT_EQ(swmm_subcatch_get_coverages(engine, s1, cov, 5),
              SWMM_ERR_BADINDEX);   // n exceeds land use count
    EXPECT_EQ(swmm_subcatch_get_coverages(engine, s1, nullptr, 2),
              SWMM_ERR_BADPARAM);
}

// ============================================================================
// Renames
// ============================================================================

TEST_F(QualityRoundtripTest, LanduseRenamePreservesMatricesAndCoverage) {
    open_model("lu_rename");

    const int res = swmm_landuse_index(engine, "Res");
    ASSERT_GE(res, 0);
    ASSERT_EQ(swmm_landuse_rename(engine, res, "Residential"), SWMM_OK);
    EXPECT_EQ(swmm_landuse_index(engine, "Res"), -1);
    EXPECT_EQ(swmm_landuse_index(engine, "Residential"), res);

    // Duplicate + empty rejected.
    EXPECT_EQ(swmm_landuse_rename(engine, res, "Com"), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_landuse_rename(engine, res, ""), SWMM_ERR_BADPARAM);

    // Positional data survives.
    double v = 0;
    ASSERT_EQ(swmm_landuse_get_sweep_interval(engine, res, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 7.0);
    const int s1 = swmm_subcatch_index(engine, "S1");
    ASSERT_EQ(swmm_subcatch_get_coverage(engine, s1, res, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 60.0);
    int ft = -1, norm = -1;
    double c1 = 0, c2 = 0, c3 = 0;
    const int tss = swmm_pollutant_index(engine, "TSS");
    ASSERT_EQ(swmm_buildup_get(engine, res, tss, &ft, &c1, &c2, &c3, &norm),
              SWMM_OK);
    EXPECT_EQ(ft, 1);

    // The written model uses the new name.
    const std::string text = written_model("lu_rename");
    EXPECT_NE(text.find("Residential"), std::string::npos);
    EXPECT_NE(text.find("[COVERAGES]"), std::string::npos);
}

TEST_F(QualityRoundtripTest, PollutantRenameFollowsNameStoredRefs) {
    open_model("pol_rename");

    const int lead = swmm_pollutant_index(engine, "Lead");
    ASSERT_GE(lead, 0);
    ASSERT_EQ(swmm_pollutant_rename(engine, lead, "Pb"), SWMM_OK);
    EXPECT_EQ(swmm_pollutant_index(engine, "Lead"), -1);
    EXPECT_EQ(swmm_pollutant_index(engine, "Pb"), lead);

    // The DWF row keyed "Lead" follows the rename; the co-pollutant ref
    // (index-stored on Lead's row pointing at TSS) is untouched.
    const std::string text = written_model("pol_rename");
    EXPECT_EQ(text.find(" Lead "), std::string::npos);
    EXPECT_NE(text.find("Pb"), std::string::npos);

    reopen_from("pol_rename");
    const int pb = swmm_pollutant_index(engine, "Pb");
    ASSERT_GE(pb, 0);
    int co_idx = -1;
    double frac = 0.0;
    ASSERT_EQ(swmm_pollutant_get_co_pollutant(engine, pb, &co_idx, &frac),
              SWMM_OK);
    EXPECT_EQ(co_idx, swmm_pollutant_index(engine, "TSS"));
    EXPECT_DOUBLE_EQ(frac, 0.25);
}

// ============================================================================
// Grow-preserving adds
// ============================================================================

TEST_F(QualityRoundtripTest, LanduseAddPreservesExistingQualityData) {
    open_model("lu_add");

    ASSERT_EQ(swmm_landuse_add(engine, "Ind"), SWMM_OK);
    ASSERT_EQ(swmm_landuse_count(engine), 3);

    // Pre-existing buildup, sweeping, and coverages are intact.
    const int res = swmm_landuse_index(engine, "Res");
    const int tss = swmm_pollutant_index(engine, "TSS");
    int ft = -1, norm = -1;
    double c1 = 0, c2 = 0, c3 = 0, v = 0;
    ASSERT_EQ(swmm_buildup_get(engine, res, tss, &ft, &c1, &c2, &c3, &norm),
              SWMM_OK);
    EXPECT_EQ(ft, 1);
    EXPECT_DOUBLE_EQ(c1, 100.0);
    ASSERT_EQ(swmm_landuse_get_sweep_removal(engine, res, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 0.5);
    const int s1 = swmm_subcatch_index(engine, "S1");
    ASSERT_EQ(swmm_subcatch_get_coverage(engine, s1, res, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 60.0);
    ASSERT_EQ(swmm_subcatch_get_coverage(
                  engine, s1, swmm_landuse_index(engine, "Com"), &v),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 40.0);

    // The new land use starts empty.
    const int ind = swmm_landuse_index(engine, "Ind");
    ASSERT_EQ(swmm_subcatch_get_coverage(engine, s1, ind, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 0.0);
    ASSERT_EQ(swmm_buildup_get(engine, ind, tss, &ft, &c1, &c2, &c3, &norm),
              SWMM_OK);
    EXPECT_EQ(ft, 0);
}

TEST_F(QualityRoundtripTest, PollutantAddOnOpenedModelPreservesMatrices) {
    open_model("pol_add");

    // Previously BUILDING-only; must now work on an OPENED model.
    ASSERT_EQ(swmm_pollutant_add(engine, "BOD", /*MG/L*/0), SWMM_OK);
    ASSERT_EQ(swmm_pollutant_count(engine), 3);

    // Existing pollutant params survive (grow-preserving defs).
    const int tss = swmm_pollutant_index(engine, "TSS");
    double v = 0;
    ASSERT_EQ(swmm_pollutant_get_rain_conc(engine, tss, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 10.0);

    // Buildup/washoff/loadings survive the stride change.
    const int res = swmm_landuse_index(engine, "Res");
    int ft = -1, norm = -1;
    double c1 = 0, c2 = 0, c3 = 0;
    ASSERT_EQ(swmm_buildup_get(engine, res, tss, &ft, &c1, &c2, &c3, &norm),
              SWMM_OK);
    EXPECT_EQ(ft, 1);
    EXPECT_DOUBLE_EQ(c1, 100.0);
    double coeff = 0, expon = 0, se = 0, be = 0;
    ASSERT_EQ(swmm_washoff_get(engine, res, tss, &ft, &coeff, &expon, &se, &be),
              SWMM_OK);
    EXPECT_EQ(ft, 1);                 // EXP
    EXPECT_DOUBLE_EQ(coeff, 0.1);
    EXPECT_DOUBLE_EQ(se, 30.0);
    const int s1 = swmm_subcatch_index(engine, "S1");
    ASSERT_EQ(swmm_subcatch_get_initial_loading(engine, s1, tss, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 1.5);

    // The new pollutant's column starts empty and is addressable.
    const int bod = swmm_pollutant_index(engine, "BOD");
    ASSERT_EQ(swmm_buildup_get(engine, res, bod, &ft, &c1, &c2, &c3, &norm),
              SWMM_OK);
    EXPECT_EQ(ft, 0);
    ASSERT_EQ(swmm_buildup_set(engine, res, bod, 2, 5.0, 0.5, 0.0, 0), SWMM_OK);
    ASSERT_EQ(swmm_buildup_get(engine, res, bod, &ft, &c1, &c2, &c3, &norm),
              SWMM_OK);
    EXPECT_EQ(ft, 2);
    // ...and TSS's row still reads back unchanged afterwards.
    ASSERT_EQ(swmm_buildup_get(engine, res, tss, &ft, &c1, &c2, &c3, &norm),
              SWMM_OK);
    EXPECT_EQ(ft, 1);
    EXPECT_DOUBLE_EQ(c1, 100.0);
}

// ============================================================================
// Treatment expression validator
// ============================================================================

TEST_F(QualityRoundtripTest, TreatmentValidateExpression) {
    open_model("treat_validate");

    char err[256] = {};
    int col = -2;

    // Valid expressions.
    EXPECT_EQ(swmm_treatment_validate_expression(
                  engine, "R = 1.0 - exp(-0.5 * HRT)", err, sizeof(err), &col),
              SWMM_OK);
    EXPECT_STREQ(err, "");
    EXPECT_EQ(swmm_treatment_validate_expression(
                  engine, "C = min(BOD_LIMIT, 10)", err, sizeof(err), &col),
              SWMM_ERR_BADPARAM);   // unknown identifier
    EXPECT_EQ(swmm_treatment_validate_expression(
                  engine, "c = min(C, 10) ^ 2", err, sizeof(err), &col),
              SWMM_OK);

    // Bad LHS.
    EXPECT_EQ(swmm_treatment_validate_expression(
                  engine, "X = 1", err, sizeof(err), &col),
              SWMM_ERR_BADPARAM);
    EXPECT_NE(std::strlen(err), 0u);

    // Unknown variable with position.
    EXPECT_EQ(swmm_treatment_validate_expression(
                  engine, "C = FLOW * 2", err, sizeof(err), &col),
              SWMM_ERR_BADPARAM);
    EXPECT_NE(std::string(err).find("FLOW"), std::string::npos);
    EXPECT_EQ(col, 4);

    // Unknown character (stricter than the lenient tokenizer).
    EXPECT_EQ(swmm_treatment_validate_expression(
                  engine, "C = 1 @ 2", err, sizeof(err), &col),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(col, 6);

    // Mismatched parens / incomplete expressions.
    EXPECT_EQ(swmm_treatment_validate_expression(
                  engine, "C = (1 + 2", err, sizeof(err), &col),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_treatment_validate_expression(
                  engine, "C = 1 +", err, sizeof(err), &col),
              SWMM_ERR_BADPARAM);

    // Co-pollutant refs are reported unsupported.
    EXPECT_EQ(swmm_treatment_validate_expression(
                  engine, "R = R_TSS * 0.5", err, sizeof(err), &col),
              SWMM_ERR_BADPARAM);
    EXPECT_NE(std::string(err).find("co-pollutant"), std::string::npos);

    // Non-mutating: no treatment rows appeared.
    char buf[256] = {};
    ASSERT_EQ(swmm_treatment_get(engine, 0, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "");
}

// ============================================================================
// KD1 — the [POLLUTANTS] Kdecay column is 1/day (KDECAY_UNITS_TRIAGE
// 2026-08-31). Legacy divides by SECperDAY at parse (landuse.c:173); until
// KD1 the new engine applied the raw column value against dt in SECONDS —
// 86,400x too fast, and the linearized node factor 1 - k*dt went NEGATIVE,
// annihilating the pollutant with a 100% continuity error booked nowhere.
// ============================================================================

namespace {

// The triage probe as a deck function: constant 1 cfs at 100 mg/L TSS into
// one short conduit under KINWAVE. Residence is minutes, so k = 1/day decays
// ~0.1% (legacy: Mass Reacted 0.159 of 134.7 lbs); an engine reading the
// column as 1/sec destroys everything.
std::string kdecay_routed_model(double kdecay_col) {
    std::ostringstream m;
    m <<
        "[OPTIONS]\n"
        "FLOW_UNITS           CFS\n"
        "FLOW_ROUTING         KINWAVE\n"
        "INFILTRATION         HORTON\n"
        "START_DATE           01/01/2024\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2024\n"
        "END_TIME             02:00:00\n"
        "REPORT_STEP          00:05:00\n"
        "ROUTING_STEP         0:00:30\n"
        "\n"
        "[JUNCTIONS]\n"
        "J1               100        10         0          0          0\n"
        "\n"
        "[OUTFALLS]\n"
        "O1               95         FREE                        NO\n"
        "\n"
        "[CONDUITS]\n"
        "C1               J1               O1               400        0.013      0          0          0          0\n"
        "\n"
        "[XSECTIONS]\n"
        "C1               CIRCULAR     2                0          0          0          1\n"
        "\n"
        "[POLLUTANTS]\n"
        ";;Name Units Crain Cgw Crdii Kdecay SnowOnly CoPollut CoFrac Cdwf Cinit\n"
        "TSS              MG/L   0.0        0.0        0.0        " << kdecay_col <<
        "        NO         *                0.0        0.0        0.0\n"
        "\n"
        "[INFLOWS]\n"
        "J1               FLOW             \"\"               FLOW     1.0      1.0      1.0\n"
        "J1               TSS              \"\"               CONCEN   1.0      1.0      100.0\n"
        "\n"
        "[REPORT]\n"
        "INPUT NO\n";
    return m.str();
}

}  // namespace

// Gate i (KD1 §5): a deck Kdecay of 1.0 is one per DAY. Over a residence of
// minutes it must leave the concentration essentially intact — not read it
// as 1/sec (86,400/day) and zero the network.
TEST_F(QualityRoundtripTest, KdecayDeckColumnIsPerDayNotPerSecond) {
    open_model("kd_perday", kdecay_routed_model(1.0));

    ASSERT_EQ(swmm_engine_initialize(engine), SWMM_OK)
        << swmm_get_last_error_msg(engine);
    ASSERT_EQ(swmm_engine_start(engine, 0), SWMM_OK)
        << swmm_get_last_error_msg(engine);
    double elapsed = 1.0;
    while (swmm_engine_step(engine, &elapsed) == SWMM_OK && elapsed > 0.0) {}
    ASSERT_EQ(swmm_engine_end(engine), SWMM_OK);

    const int o1 = swmm_node_index(engine, "O1");
    ASSERT_GE(o1, 0);
    double conc = -1.0;
    ASSERT_EQ(swmm_node_get_quality(engine, o1, 0, &conc), SWMM_OK);
    EXPECT_GT(conc, 90.0)
        << "k = 1/day annihilated a 100 mg/L stream over minutes of "
           "residence — the column is being applied as 1/sec";

    double err = -1.0;
    ASSERT_EQ(swmm_get_quality_continuity_error(engine, 0, &err), SWMM_OK);
    EXPECT_LT(std::fabs(err), 0.02)
        << "quality continuity error " << err * 100.0 << "%";
}

// Gate iv (KD1 §5): decayed mass is BOOKED (qual_routing_reacted), not left
// as continuity error. k = 200/day decays a real fraction over the run;
// only a booked ledger keeps the continuity error small. At base this deck
// reads 100% error and an untouched Mass Reacted of zero.
TEST_F(QualityRoundtripTest, KdecayLegacyPathBooksReactedMass) {
    open_model("kd_booked", kdecay_routed_model(200.0));

    ASSERT_EQ(swmm_engine_initialize(engine), SWMM_OK)
        << swmm_get_last_error_msg(engine);
    ASSERT_EQ(swmm_engine_start(engine, 0), SWMM_OK)
        << swmm_get_last_error_msg(engine);
    double elapsed = 1.0;
    while (swmm_engine_step(engine, &elapsed) == SWMM_OK && elapsed > 0.0) {}
    ASSERT_EQ(swmm_engine_end(engine), SWMM_OK);

    // The observable is the OUTFALL: junctions never decay (legacy
    // findNodeQual applies no decay — KD1 restored that), so the decayed
    // signal arrives through the link.
    const int o1 = swmm_node_index(engine, "O1");
    ASSERT_GE(o1, 0);
    double conc = -1.0;
    ASSERT_EQ(swmm_node_get_quality(engine, o1, 0, &conc), SWMM_OK);
    EXPECT_GT(conc, 5.0)  << "annihilated — units still wrong";
    EXPECT_LT(conc, 99.0) << "no decay at all — the stage went dead";

    double err = -1.0;
    ASSERT_EQ(swmm_get_quality_continuity_error(engine, 0, &err), SWMM_OK);
    EXPECT_LT(std::fabs(err), 0.02)
        << "decayed mass is escaping the ledger (continuity error "
        << err * 100.0 << "%) — qual_routing_reacted is not being booked";
}

// Gate ii (KD1 §5): the Kdecay COLUMN round-trips through the writer at file
// units. Parse-only conversion would write the internal 1/sec value —
// 1.157e-5, which %10.4f prints as 0.0000, silently erasing the decay.
TEST_F(QualityRoundtripTest, KdecayColumnRoundTripsThroughWriteAndReopen) {
    open_model("kd_rt", kdecay_routed_model(1.0));

    double k = -1.0;
    ASSERT_EQ(swmm_pollutant_get_kdecay(engine, 0, &k), SWMM_OK);
    EXPECT_NEAR(k, 1.0, 1e-9) << "API contract is deck units (1/day)";

    const std::string text = written_model("kd_rt");
    const auto pos = text.find("[POLLUTANTS]");
    ASSERT_NE(pos, std::string::npos);
    const auto row = text.find("TSS", pos);
    ASSERT_NE(row, std::string::npos);
    std::istringstream ls(text.substr(row, text.find('\n', row) - row));
    std::string name, units, crain, cgw, crdii, kcol;
    ls >> name >> units >> crain >> cgw >> crdii >> kcol;
    EXPECT_NEAR(std::stod(kcol), 1.0, 1e-4)
        << "written Kdecay column reads " << kcol
        << " — the writer is not emitting file units";

    reopen_from("kd_rt");
    k = -1.0;
    ASSERT_EQ(swmm_pollutant_get_kdecay(engine, 0, &k), SWMM_OK);
    EXPECT_NEAR(k, 1.0, 1e-4) << "Kdecay drifted across write -> reopen";
}

// Gate iii (KD1 §5): the C API speaks deck units (1/day) in both directions
// — the header has documented that contract all along. A set of 2.0 must
// read back 2.0 and write as 2.0000.
TEST_F(QualityRoundtripTest, KdecayApiContractIsPerDayBothWays) {
    open_model("kd_api", kdecay_routed_model(0.0));

    ASSERT_EQ(swmm_pollutant_set_kdecay(engine, 0, 2.0), SWMM_OK);
    double k = -1.0;
    ASSERT_EQ(swmm_pollutant_get_kdecay(engine, 0, &k), SWMM_OK);
    EXPECT_NEAR(k, 2.0, 1e-9);

    const std::string text = written_model("kd_api");
    const auto pos = text.find("[POLLUTANTS]");
    ASSERT_NE(pos, std::string::npos);
    const auto row = text.find("TSS", pos);
    ASSERT_NE(row, std::string::npos);
    std::istringstream ls(text.substr(row, text.find('\n', row) - row));
    std::string name, units, crain, cgw, crdii, kcol;
    ls >> name >> units >> crain >> cgw >> crdii >> kcol;
    EXPECT_NEAR(std::stod(kcol), 2.0, 1e-4)
        << "API-set Kdecay wrote as " << kcol;
}

// Gate v (KD1, added by the check pass): the NODE-side decay + booking has
// its own observer. Junctions never decay (legacy findNodeQual), but a
// STORAGE node does (findStorageQual via getReactedQual) — this pins that
// the storage path still decays, and that its removal is BOOKED from the
// node-volume basis (the k=200 routed deck above only exercises the LINK
// booking).
TEST_F(QualityRoundtripTest, KdecayStorageNodeStillDecaysAndBooks) {
    std::string deck = kdecay_routed_model(200.0);
    // DYNWAVE: KINWAVE's storage handling leaves a ~9% FLOW continuity
    // error on this shape, which no quality ledger can close over.
    auto rt = deck.find("FLOW_ROUTING         KINWAVE");
    ASSERT_NE(rt, std::string::npos);
    deck.replace(rt, std::string("FLOW_ROUTING         KINWAVE").size(),
                 "FLOW_ROUTING         DYNWAVE");
    // Splice a storage unit between J1 and O1: J1 -C1-> ST1 -C2-> O1.
    const std::string storage =
        "[STORAGE]\n"
        ";;Name Elev MaxDepth InitDepth Shape      Coeff Expon Const\n"
        "ST1    96   10       2.0       FUNCTIONAL 0     0     1000\n"
        "\n"
        "[OUTFALLS]\n";
    auto pos = deck.find("[OUTFALLS]");
    ASSERT_NE(pos, std::string::npos);
    deck.replace(pos, std::string("[OUTFALLS]").size(), storage);
    const std::string conduits =
        "[CONDUITS]\n"
        "C1               J1               ST1              400        0.013      0          0          0          0\n"
        "C2               ST1              O1               400        0.013      0          0          0          0\n";
    pos = deck.find("[CONDUITS]");
    ASSERT_NE(pos, std::string::npos);
    auto endpos = deck.find("\n\n", pos);
    deck.replace(pos, endpos - pos + 1, conduits);
    const std::string xsects =
        "[XSECTIONS]\n"
        "C1               CIRCULAR     2                0          0          0          1\n"
        "C2               CIRCULAR     2                0          0          0          1\n";
    pos = deck.find("[XSECTIONS]");
    ASSERT_NE(pos, std::string::npos);
    endpos = deck.find("\n\n", pos);
    deck.replace(pos, endpos - pos + 1, xsects);

    open_model("kd_storage", deck);

    ASSERT_EQ(swmm_engine_initialize(engine), SWMM_OK)
        << swmm_get_last_error_msg(engine);
    ASSERT_EQ(swmm_engine_start(engine, 0), SWMM_OK)
        << swmm_get_last_error_msg(engine);
    double elapsed = 1.0;
    while (swmm_engine_step(engine, &elapsed) == SWMM_OK && elapsed > 0.0) {}
    ASSERT_EQ(swmm_engine_end(engine), SWMM_OK);

    const int st1 = swmm_node_index(engine, "ST1");
    ASSERT_GE(st1, 0);
    double conc = -1.0;
    ASSERT_EQ(swmm_node_get_quality(engine, st1, 0, &conc), SWMM_OK);
    EXPECT_GT(conc, 1.0)  << "storage annihilated — the clamp regressed";
    EXPECT_LT(conc, 95.0) << "the storage node no longer decays — the "
                             "junction gate over-reached";

    double err = -1.0;
    ASSERT_EQ(swmm_get_quality_continuity_error(engine, 0, &err), SWMM_OK);
    // Threshold: this deck's storage volume-balance accounting leaks even
    // with NO decay (this engine k=0: +3.9%; LEGACY on the same deck:
    // +6.5% at k=200/day, +7.0% at k=0), and per-step transit mass takes
    // the decay factor outside any volume-basis booking (the P2.4 storage
    // mixing class, present in both engines). 15% cleanly separates
    // "booked, with the pre-existing slop" from unbooked decay (~+50%
    // here) and from the pre-KD1 creation (-47%).
    EXPECT_LT(std::fabs(err), 0.15)
        << "storage decay is escaping the ledger (continuity error "
        << err * 100.0 << "%)";
}
