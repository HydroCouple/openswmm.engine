/**
 * @file test_gage_format_preservation.cpp
 * @brief B1 regression — editing a USER_CSV gage's station id or filename
 *        must not corrupt its file format or column binding — plus the new
 *        file-column C API accessors.
 *
 * @details Before the 2026-08-17 fix, swmm_gage_set_station_id and
 *          swmm_gage_set_filename unconditionally forced
 *          file_format = STAN_PRCP. The GUI calls set_station_id from its
 *          rain-gage property adapter, so editing Station ID silently
 *          destroyed a USER_CSV gage's "path:col" binding.
 *
 *          Fixture: rain_series/rain_us.inp (working dir is
 *          tests/unit/engine/data/) — CSV_EAST is USER_CSV with column
 *          EAST_GAGE; STD_GAGE is STAN_PRCP with station STA_A.
 *
 * @see src/engine/core/openswmm_gages_impl.cpp
 * @see plans/MULTICOLUMN_SERIES_SINGLE_READ_2026-08-17.md (B1)
 */

#include <gtest/gtest.h>

#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_gages.h>

namespace {

constexpr int kStanPrcp = 5;  // RainFileFormat::STAN_PRCP
constexpr int kUserCsv  = 6;  // RainFileFormat::USER_CSV

class GageFormatPreservation : public ::testing::Test {
protected:
    SWMM_Engine engine_ = nullptr;

    void openModel(const char* rpt, const char* out) {
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
        ASSERT_EQ(swmm_engine_open(engine_, "rain_series/rain_us.inp",
                                   rpt, out, nullptr), SWMM_OK);
    }

    void TearDown() override {
        if (engine_) {
            swmm_engine_close(engine_);
            swmm_engine_destroy(engine_);
            engine_ = nullptr;
        }
    }

    int fileFormat(int idx) {
        int fmt = -1;
        EXPECT_EQ(swmm_gage_get_file_format(engine_, idx, &fmt), SWMM_OK);
        return fmt;
    }

    std::string fileColumn(int idx) {
        char buf[128] = {};
        EXPECT_EQ(swmm_gage_get_file_column(engine_, idx, buf, sizeof(buf)),
                  SWMM_OK);
        return buf;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// New getters read what the parser stored
// ---------------------------------------------------------------------------

TEST_F(GageFormatPreservation, GettersReflectParsedState) {
    openModel("_gfmt_get.rpt", "_gfmt_get.out");
    const int csv = swmm_gage_index(engine_, "CSV_EAST");
    const int std_g = swmm_gage_index(engine_, "STD_GAGE");
    ASSERT_GE(csv, 0);
    ASSERT_GE(std_g, 0);

    EXPECT_EQ(fileFormat(csv), kUserCsv);
    EXPECT_EQ(fileColumn(csv), "EAST_GAGE");
    EXPECT_EQ(fileFormat(std_g), kStanPrcp);
    EXPECT_EQ(fileColumn(std_g), "");
}

// ---------------------------------------------------------------------------
// B1 — set_station_id must not touch the file format
// ---------------------------------------------------------------------------

TEST_F(GageFormatPreservation, SetStationIdPreservesUserCsv) {
    openModel("_gfmt_sta.rpt", "_gfmt_sta.out");
    const int csv = swmm_gage_index(engine_, "CSV_EAST");
    ASSERT_GE(csv, 0);

    ASSERT_EQ(swmm_gage_set_station_id(engine_, csv, "GHCND:US1"), SWMM_OK);

    EXPECT_EQ(fileFormat(csv), kUserCsv)
        << "editing Station ID silently corrupted USER_CSV gages (B1)";
    EXPECT_EQ(fileColumn(csv), "EAST_GAGE");
}

// ---------------------------------------------------------------------------
// B1 — set_filename preserves USER_CSV; a plain gage still gets STAN_PRCP
// ---------------------------------------------------------------------------

TEST_F(GageFormatPreservation, SetFilenamePreservesUserCsv) {
    openModel("_gfmt_fname.rpt", "_gfmt_fname.out");
    const int csv = swmm_gage_index(engine_, "CSV_EAST");
    ASSERT_GE(csv, 0);

    ASSERT_EQ(swmm_gage_set_filename(engine_, csv, "other.csv", ""), SWMM_OK);

    EXPECT_EQ(fileFormat(csv), kUserCsv);
    EXPECT_EQ(fileColumn(csv), "EAST_GAGE");
}

TEST_F(GageFormatPreservation, SetFilenameOnStanGageStaysStan) {
    openModel("_gfmt_stan.rpt", "_gfmt_stan.out");
    const int std_g = swmm_gage_index(engine_, "STD_GAGE");
    ASSERT_GE(std_g, 0);

    ASSERT_EQ(swmm_gage_set_filename(engine_, std_g, "rain2.dat", "STA_B"),
              SWMM_OK);
    EXPECT_EQ(fileFormat(std_g), kStanPrcp);

    char buf[64] = {};
    ASSERT_EQ(swmm_gage_get_station_id(engine_, std_g, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "STA_B");
}

// ---------------------------------------------------------------------------
// set_file_column — non-empty implies USER_CSV; clearing keeps the format
// ---------------------------------------------------------------------------

TEST_F(GageFormatPreservation, SetFileColumnImpliesUserCsv) {
    openModel("_gfmt_col.rpt", "_gfmt_col.out");
    const int std_g = swmm_gage_index(engine_, "STD_GAGE");
    ASSERT_GE(std_g, 0);
    ASSERT_EQ(fileFormat(std_g), kStanPrcp);

    ASSERT_EQ(swmm_gage_set_file_column(engine_, std_g, "P1"), SWMM_OK);
    EXPECT_EQ(fileFormat(std_g), kUserCsv);
    EXPECT_EQ(fileColumn(std_g), "P1");

    // Clearing the column keeps USER_CSV — an empty column on a USER_CSV
    // gage means "first data column" (B2), not "revert to STAN_PRCP".
    ASSERT_EQ(swmm_gage_set_file_column(engine_, std_g, ""), SWMM_OK);
    EXPECT_EQ(fileFormat(std_g), kUserCsv);
    EXPECT_EQ(fileColumn(std_g), "");
}

// ---------------------------------------------------------------------------
// set_file_format — the way back OUT of USER_CSV (no more one-way door)
// ---------------------------------------------------------------------------

TEST_F(GageFormatPreservation, FileFormatRoundTripsOutOfAndBackIntoUserCsv) {
    openModel("_gfmt_fmt.rpt", "_gfmt_fmt.out");
    const int csv = swmm_gage_index(engine_, "CSV_EAST");
    ASSERT_GE(csv, 0);
    ASSERT_EQ(fileFormat(csv), kUserCsv);
    ASSERT_EQ(fileColumn(csv), "EAST_GAGE");

    // USER_CSV → STAN_PRCP. Every other setter preserves USER_CSV by design
    // (that is the B1 fix), so without this accessor a host that once set a
    // column could never return the gage to a standard rain file.
    ASSERT_EQ(swmm_gage_set_file_format(engine_, csv, kStanPrcp), SWMM_OK);
    EXPECT_EQ(fileFormat(csv), kStanPrcp);
    EXPECT_EQ(fileColumn(csv), "")
        << "a station-based format has no column selector";

    // The station id is now settable and sticks.
    ASSERT_EQ(swmm_gage_set_station_id(engine_, csv, "STA_A"), SWMM_OK);
    char sta[64] = {};
    ASSERT_EQ(swmm_gage_get_station_id(engine_, csv, sta, sizeof(sta)), SWMM_OK);
    EXPECT_STREQ(sta, "STA_A");

    // STAN_PRCP → USER_CSV clears the station id (a multi-column file has no
    // station column) and the column can be re-bound.
    ASSERT_EQ(swmm_gage_set_file_format(engine_, csv, kUserCsv), SWMM_OK);
    EXPECT_EQ(fileFormat(csv), kUserCsv);
    ASSERT_EQ(swmm_gage_get_station_id(engine_, csv, sta, sizeof(sta)), SWMM_OK);
    EXPECT_STREQ(sta, "");
    ASSERT_EQ(swmm_gage_set_file_column(engine_, csv, "WEST_GAGE"), SWMM_OK);
    EXPECT_EQ(fileColumn(csv), "WEST_GAGE");
    EXPECT_EQ(fileFormat(csv), kUserCsv);
}

TEST_F(GageFormatPreservation, SetFileFormatValidatesTheCode) {
    openModel("_gfmt_fmtbad.rpt", "_gfmt_fmtbad.out");
    const int csv = swmm_gage_index(engine_, "CSV_EAST");
    ASSERT_GE(csv, 0);

    EXPECT_NE(swmm_gage_set_file_format(engine_, csv, 7), SWMM_OK)
        << "7 is not a RainFileFormat";
    EXPECT_NE(swmm_gage_set_file_format(engine_, csv, -2), SWMM_OK);
    EXPECT_NE(swmm_gage_set_file_format(engine_, 99, kStanPrcp), SWMM_OK);
    // Rejected calls must not have changed anything.
    EXPECT_EQ(fileFormat(csv), kUserCsv);
    EXPECT_EQ(fileColumn(csv), "EAST_GAGE");

    // The full documented range is accepted (-1 = UNKNOWN .. 6 = USER_CSV).
    for (int f = -1; f <= 6; ++f)
        EXPECT_EQ(swmm_gage_set_file_format(engine_, csv, f), SWMM_OK) << f;
}

// ---------------------------------------------------------------------------
// Bad-index / bad-param guards
// ---------------------------------------------------------------------------

TEST_F(GageFormatPreservation, AccessorsRejectBadIndex) {
    openModel("_gfmt_bad.rpt", "_gfmt_bad.out");
    int fmt = 0;
    char buf[8] = {};
    EXPECT_NE(swmm_gage_get_file_format(engine_, 99, &fmt), SWMM_OK);
    EXPECT_NE(swmm_gage_get_file_column(engine_, 99, buf, sizeof(buf)), SWMM_OK);
    EXPECT_NE(swmm_gage_set_file_column(engine_, 99, "X"), SWMM_OK);
    EXPECT_NE(swmm_gage_get_file_column(engine_, 0, nullptr, 0), SWMM_OK);
}
