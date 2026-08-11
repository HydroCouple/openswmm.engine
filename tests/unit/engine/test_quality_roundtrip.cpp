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
