// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file test_storage_shape_roundtrip.cpp
 * @brief `.inp` round-trip for the storage shapes — the §0 data-loss path.
 *
 * @details STORAGE_SHAPES_PLAN check 5 (`.inp` side): open a model that carries all
 *          six storage shape forms, read each node's shape + raw dimensions + derived
 *          coefficients through the C API, write it back out with the built-in
 *          `.inp` writer (swmm_model_write — the exact call the GUI's Save makes),
 *          re-open the written file and assert every value survived.
 *
 *          Before this feature, opening a PYRAMIDAL storage and saving silently
 *          rewrote it as FUNCTIONAL with zeroed coefficients (plan §0). This test is
 *          the regression that closes that path: it fails loudly if a shape or its
 *          L/W/Z are dropped or downgraded on write.
 *
 *          The working directory is tests/unit/engine/data/ (see CMakeLists), so the
 *          fixture is referenced as storage_shapes/shapes_all.inp and all artifacts
 *          are written under storage_shapes/ where they can be reviewed.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_model.h>

namespace {

// One storage node's full geometric state, as the C API exposes it.
struct StorageSnap {
    int    shape = -1;
    double p1 = 0, p2 = 0, p3 = 0;   // raw user dimensions (L/W/Z)
    double a  = 0, b  = 0, c  = 0;   // derived / stored area coefficients
};

StorageSnap snapshot(SWMM_Engine e, const char* name) {
    StorageSnap s;
    const int idx = swmm_node_index(e, name);
    EXPECT_GE(idx, 0) << "node not found: " << name;
    if (idx < 0) return s;
    EXPECT_EQ(swmm_node_get_storage_shape(e, idx, &s.shape), SWMM_OK);
    EXPECT_EQ(swmm_node_get_storage_geometry(e, idx, &s.p1, &s.p2, &s.p3), SWMM_OK);
    EXPECT_EQ(swmm_node_get_storage_functional(e, idx, &s.a, &s.b, &s.c), SWMM_OK);
    return s;
}

void expectSame(const StorageSnap& before, const StorageSnap& after, const char* name) {
    EXPECT_EQ(before.shape, after.shape) << name << ": shape changed on round-trip";
    EXPECT_DOUBLE_EQ(before.p1, after.p1) << name << ": p1 changed";
    EXPECT_DOUBLE_EQ(before.p2, after.p2) << name << ": p2 changed";
    EXPECT_DOUBLE_EQ(before.p3, after.p3) << name << ": p3 changed";
    EXPECT_DOUBLE_EQ(before.a,  after.a)  << name << ": coeff a changed";
    EXPECT_DOUBLE_EQ(before.b,  after.b)  << name << ": coeff b changed";
    EXPECT_DOUBLE_EQ(before.c,  after.c)  << name << ": coeff c changed";
}

std::string slurp(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}

constexpr const char* kSrc = "storage_shapes/shapes_all.inp";
constexpr const char* kOut = "storage_shapes/_roundtrip.inp";

// SWMM_StorageShape ordinals — spelled out so a mismatch reads clearly.
constexpr int TABULAR = 0, FUNCTIONAL = 1, CYLINDRICAL = 2,
              CONICAL = 3, PARABOLOID = 4, PYRAMIDAL = 5;

}  // namespace

// ===========================================================================
// The parse itself — the six shapes land with the right shape + coefficients.
// ===========================================================================

TEST(StorageShapeInpRoundTrip, ParsesEveryShapeForm) {
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, kSrc, "storage_shapes/_parse.rpt",
                               "storage_shapes/_parse.out", nullptr), SWMM_OK)
        << swmm_get_last_error_msg(e);

    EXPECT_EQ(snapshot(e, "STAB").shape, TABULAR);
    EXPECT_EQ(snapshot(e, "SFUN").shape, FUNCTIONAL);
    EXPECT_EQ(snapshot(e, "SCYL").shape, CYLINDRICAL);
    EXPECT_EQ(snapshot(e, "SCON").shape, CONICAL);
    EXPECT_EQ(snapshot(e, "SPAR").shape, PARABOLOID);   // keyword PARABOLIC in the file
    EXPECT_EQ(snapshot(e, "SPYR").shape, PYRAMIDAL);

    // Raw dimensions preserved verbatim, and the derived coefficients match the
    // hand-worked legacy formulas (node.c:713-752).
    const StorageSnap pyr = snapshot(e, "SPYR");   // L=30, W=20, Z=2.5
    EXPECT_DOUBLE_EQ(pyr.p1, 30.0);
    EXPECT_DOUBLE_EQ(pyr.p2, 20.0);
    EXPECT_DOUBLE_EQ(pyr.p3, 2.5);
    EXPECT_DOUBLE_EQ(pyr.a, 2.0 * (30.0 + 20.0) * 2.5);   // 250
    EXPECT_DOUBLE_EQ(pyr.b, 4.0 * 2.5 * 2.5);             // 25
    EXPECT_DOUBLE_EQ(pyr.c, 30.0 * 20.0);                 // 600

    const StorageSnap cyl = snapshot(e, "SCYL");   // major 30, minor 20
    EXPECT_DOUBLE_EQ(cyl.a, 0.0);
    EXPECT_DOUBLE_EQ(cyl.b, 0.0);
    EXPECT_DOUBLE_EQ(cyl.c, 3.141592653589793 * 15.0 * 10.0);   // π·A·B

    swmm_engine_close(e);
    swmm_engine_destroy(e);
}

// ===========================================================================
// The regression: open → write → re-open preserves shape + L/W/Z + coeffs.
// ===========================================================================

TEST(StorageShapeInpRoundTrip, ShapeAndDimensionsSurviveWrite) {
    const char* nodes[] = {"STAB", "SFUN", "SCYL", "SCON", "SPAR", "SPYR"};

    SWMM_Engine e1 = swmm_engine_create();
    ASSERT_NE(e1, nullptr);
    ASSERT_EQ(swmm_engine_open(e1, kSrc, "storage_shapes/_rt1.rpt",
                               "storage_shapes/_rt1.out", nullptr), SWMM_OK)
        << swmm_get_last_error_msg(e1);

    StorageSnap before[6];
    for (int i = 0; i < 6; ++i) before[i] = snapshot(e1, nodes[i]);

    ASSERT_EQ(swmm_model_write(e1, kOut), SWMM_OK) << swmm_get_last_error_msg(e1);
    swmm_engine_close(e1);
    swmm_engine_destroy(e1);

    SWMM_Engine e2 = swmm_engine_create();
    ASSERT_NE(e2, nullptr);
    ASSERT_EQ(swmm_engine_open(e2, kOut, "storage_shapes/_rt2.rpt",
                               "storage_shapes/_rt2.out", nullptr), SWMM_OK)
        << "written file failed to re-open: " << swmm_get_last_error_msg(e2);

    for (int i = 0; i < 6; ++i) expectSame(before[i], snapshot(e2, nodes[i]), nodes[i]);

    swmm_engine_close(e2);
    swmm_engine_destroy(e2);
}

// ===========================================================================
// The written text uses the shape KEYWORD and the RAW L/W/Z — not the derived
// a/b/c (which would silently downgrade the node to FUNCTIONAL on the next read)
// and the canonical PARABOLIC spelling, not the PARABOLOID enumerator alias.
// ===========================================================================

TEST(StorageShapeInpRoundTrip, WritesKeywordsAndRawDimensions) {
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, kSrc, "storage_shapes/_kw.rpt",
                               "storage_shapes/_kw.out", nullptr), SWMM_OK);
    ASSERT_EQ(swmm_model_write(e, kOut), SWMM_OK);
    swmm_engine_close(e);
    swmm_engine_destroy(e);

    const std::string txt = slurp(kOut);
    for (const char* kw : {"CYLINDRICAL", "CONICAL", "PARABOLIC", "PYRAMIDAL"})
        EXPECT_NE(txt.find(kw), std::string::npos) << "shape keyword missing: " << kw;
    EXPECT_EQ(txt.find("PARABOLOID"), std::string::npos)
        << "must emit the canonical PARABOLIC keyword, not the PARABOLOID alias";

    // The pyramid's own line must carry the raw 30 / 20 / 2.5 (L/W/Z), never the
    // derived 250 / 25 / 600 coefficients.
    // The [STORAGE] row for SPYR is the one that starts with SPYR AND names the
    // shape — "SPYR" also appears in [COORDINATES], which we must not match here.
    std::istringstream is(txt);
    std::string line;
    bool checked = false;
    while (std::getline(is, line)) {
        if (line.rfind("SPYR", 0) == 0 && line.find("PYRAMIDAL") != std::string::npos) {
            EXPECT_NE(line.find("30"),  std::string::npos) << line;
            EXPECT_NE(line.find("2.5"), std::string::npos) << line;
            EXPECT_EQ(line.find("250"), std::string::npos)
                << "wrote derived coefficient instead of raw L/W/Z: " << line;
            checked = true;
        }
    }
    EXPECT_TRUE(checked) << "no [STORAGE] line for SPYR in the written file";
}
