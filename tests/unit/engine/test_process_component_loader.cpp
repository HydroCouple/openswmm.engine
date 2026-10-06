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
 * @file test_process_component_loader.cpp
 * @brief D2, Gate D-a (program plan §C.1) — HydroCouple component discovery.
 *
 * Gate D-a, as the plan states it:
 *   - the smoke component loads from ALL THREE search-path sources;
 *   - a mis-stamped twin is refused, with BOTH stamps in the message;
 *   - an unstamped `CreateComponentInfo` library is reported "unstamped".
 *
 * Every library here is real and reached through dlopen: see
 * tests/components/smoke. The catalogue is process-global, so every test
 * starts from `component_catalog_reset_for_testing()`.
 *
 * Built only with OPENSWMM_WITH_HYDROCOUPLE=ON.
 */

#include <gtest/gtest.h>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_process_components.h>

#include "plugins/PluginFactory.hpp"

// The engine's stamp comes from the same header the engine compiled against,
// so this compares against the real thing, not a copy of its spelling.
#include <hydrocouplecomponentabi.h>

#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#ifndef SMOKE_ENV_DIR
#error "SMOKE_ENV_DIR must be defined by the build"
#endif
#ifndef SMOKE_EXPLICIT_DIR
#error "SMOKE_EXPLICIT_DIR must be defined by the build"
#endif
#ifndef PC_OUTPUT_DIR
#error "PC_OUTPUT_DIR must be defined by the build"
#endif

namespace {

constexpr const char* kNext       = "org.hydrocouple.openswmm.test.smoke.next";
constexpr const char* kEnv        = "org.hydrocouple.openswmm.test.smoke.env";
constexpr const char* kExplicit   = "org.hydrocouple.openswmm.test.smoke.explicit";
constexpr const char* kMisstamped = "hc_smoke_misstamped";   // refused: matched by file
constexpr const char* kLegacy     = "org.hydrocouple.openswmm.test.smoke.legacy";

void set_env(const char* key, const char* value) {
#if defined(_WIN32)
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}

std::vector<SWMM_ComponentLibraryInfo> all_libraries() {
    std::vector<SWMM_ComponentLibraryInfo> out;
    const int n = swmm_component_library_count();
    for (int i = 0; i < n; ++i) {
        SWMM_ComponentLibraryInfo info{};
        EXPECT_EQ(swmm_component_library_get(i, &info), SWMM_OK);
        out.push_back(info);
    }
    return out;
}

const SWMM_ComponentLibraryInfo* by_id(const std::vector<SWMM_ComponentLibraryInfo>& v,
                                       const std::string& id) {
    for (const auto& r : v) if (id == r.id) return &r;
    return nullptr;
}

const SWMM_ComponentLibraryInfo* by_file(const std::vector<SWMM_ComponentLibraryInfo>& v,
                                         const std::string& stem) {
    for (const auto& r : v)
        if (fs::path(r.path).filename().string().find(stem) != std::string::npos) return &r;
    return nullptr;
}

std::string dump(const std::vector<SWMM_ComponentLibraryInfo>& v) {
    std::string s = "\ncatalogue (" + std::to_string(v.size()) + "):";
    for (const auto& r : v)
        s += "\n  [" + std::string(r.id) + "] " + r.path + "\n      stamp=" + r.stamp +
             (r.load_error[0] ? "\n      error=" + std::string(r.load_error) : "");
    return s;
}

class ComponentLoader : public ::testing::Test {
protected:
    void SetUp() override {
        openswmm::PluginFactory::component_catalog_reset_for_testing();
        set_env("HYDROCOUPLE_COMPONENT_PATH", SMOKE_ENV_DIR);
    }
    void TearDown() override {
        openswmm::PluginFactory::component_catalog_reset_for_testing();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Gate D-a, part 1: all three search-path sources.
// ---------------------------------------------------------------------------
TEST_F(ComponentLoader, SmokeLoadsFromAllThreeSearchPathSources) {
    ASSERT_TRUE(openswmm::PluginFactory::hydrocouple_enabled());
    ASSERT_EQ(swmm_component_search_path_add(SMOKE_EXPLICIT_DIR), SWMM_OK);
    const auto libs = all_libraries();

    struct Source { const char* id; const char* where; };
    const Source sources[] = {
        {kNext,     "next to the host library (components/)"},
        {kEnv,      "HYDROCOUPLE_COMPONENT_PATH"},
        {kExplicit, "swmm_component_search_path_add"},
    };
    for (const auto& s : sources) {
        const auto* r = by_id(libs, s.id);
        ASSERT_NE(r, nullptr) << "not discovered from " << s.where << dump(libs);
        EXPECT_STREQ(r->stamp, HYDROCOUPLE_COMPONENT_ABI_STAMP) << s.where;
        EXPECT_STREQ(r->load_error, "") << s.where;
        EXPECT_STREQ(r->kind, "model") << s.where;
        EXPECT_STREQ(r->version, "0.0.1") << s.where;
    }

    // …and each from where it was put, not merely somewhere.
    const auto* next = by_id(libs, kNext);
    EXPECT_EQ(fs::path(next->path).parent_path().filename(), "components") << dump(libs);
    EXPECT_EQ(fs::weakly_canonical(fs::path(by_id(libs, kEnv)->path).parent_path()),
              fs::weakly_canonical(SMOKE_ENV_DIR));
    EXPECT_EQ(fs::weakly_canonical(fs::path(by_id(libs, kExplicit)->path).parent_path()),
              fs::weakly_canonical(SMOKE_EXPLICIT_DIR));
}

// ---------------------------------------------------------------------------
// Gate D-a, part 2: the mis-stamped twin is refused, naming both stamps.
// ---------------------------------------------------------------------------
TEST_F(ComponentLoader, MisStampedTwinIsRefusedWithBothStamps) {
    ASSERT_EQ(swmm_component_search_path_add(SMOKE_EXPLICIT_DIR), SWMM_OK);
    const auto libs = all_libraries();
    const auto* r = by_file(libs, kMisstamped);
    ASSERT_NE(r, nullptr)
        << "the mis-stamped library was not RECORDED — a refusal must be listed, "
           "not skipped" << dump(libs);

    // Refused: never called beyond its stamp, so it has no id.
    EXPECT_STREQ(r->id, "") << "a mis-stamped library was LOADED" << dump(libs);
    EXPECT_STREQ(r->kind, "");
    // Its own stamp is the forged one: identical to the engine's except iface.
    EXPECT_NE(std::string(r->stamp).find(";iface=2;"), std::string::npos) << r->stamp;
    EXPECT_STRNE(r->stamp, HYDROCOUPLE_COMPONENT_ABI_STAMP);
    // Both stamps in the message, so a user can see what disagreed.
    const std::string err = r->load_error;
    EXPECT_NE(err.find(r->stamp), std::string::npos) << "library stamp missing: " << err;
    EXPECT_NE(err.find(HYDROCOUPLE_COMPONENT_ABI_STAMP), std::string::npos)
        << "engine stamp missing: " << err;
}

// ---------------------------------------------------------------------------
// Gate D-a, part 3: the legacy library loads, and is reported unstamped.
// ---------------------------------------------------------------------------
TEST_F(ComponentLoader, LegacyCreateComponentInfoIsReportedUnstamped) {
    ASSERT_EQ(swmm_component_search_path_add(SMOKE_EXPLICIT_DIR), SWMM_OK);
    const auto libs = all_libraries();
    const auto* r = by_id(libs, kLegacy);
    ASSERT_NE(r, nullptr) << dump(libs);
    EXPECT_STREQ(r->stamp, HYDROCOUPLE_COMPONENT_UNSTAMPED);
    EXPECT_STREQ(r->load_error, "");
    EXPECT_STREQ(r->kind, "model");
}

// ---------------------------------------------------------------------------
// The duplicate-id rule: the same component reached twice is refused once,
// naming where it was already loaded from — never silently shadowed.
// ---------------------------------------------------------------------------
TEST_F(ComponentLoader, SecondCopyOfAnIdIsRefusedNamingTheFirst) {
    ASSERT_EQ(swmm_component_search_path_add(SMOKE_EXPLICIT_DIR), SWMM_OK);

    // A byte-identical copy at a different path. The copy is written where a
    // reviewer can see it (CLAUDE.md §4.1).
    const fs::path dup_dir = fs::path(PC_OUTPUT_DIR) / "duplicate_id";
    fs::create_directories(dup_dir);
    const auto* original = by_id(all_libraries(), kExplicit);
    ASSERT_NE(original, nullptr);
    const fs::path copy = dup_dir / fs::path(original->path).filename();
    fs::copy_file(original->path, copy, fs::copy_options::overwrite_existing);

    ASSERT_EQ(swmm_component_search_path_add(dup_dir.string().c_str()), SWMM_OK);
    const auto libs = all_libraries();

    int loaded = 0;
    const SWMM_ComponentLibraryInfo* refused = nullptr;
    for (const auto& r : libs) {
        if (std::string(r.id) == kExplicit && r.load_error[0] == '\0') ++loaded;
        if (fs::weakly_canonical(r.path) == fs::weakly_canonical(copy)) refused = &r;
    }
    EXPECT_EQ(loaded, 1) << "one id, one loaded library" << dump(libs);
    ASSERT_NE(refused, nullptr) << dump(libs);
    const std::string err = refused->load_error;
    EXPECT_NE(err.find("duplicate component id"), std::string::npos) << err;
    EXPECT_NE(err.find(fs::path(original->path).filename().string()), std::string::npos)
        << "the refusal must name where the id was already loaded from: " << err;
}

// ---------------------------------------------------------------------------
// Re-adding a directory records nothing twice.
// ---------------------------------------------------------------------------
TEST_F(ComponentLoader, AddingTheSameDirectoryTwiceIsIdempotent) {
    ASSERT_EQ(swmm_component_search_path_add(SMOKE_EXPLICIT_DIR), SWMM_OK);
    const int n = swmm_component_library_count();
    ASSERT_EQ(swmm_component_search_path_add(SMOKE_EXPLICIT_DIR), SWMM_OK);
    EXPECT_EQ(swmm_component_library_count(), n);
}

// ---------------------------------------------------------------------------
// The API's own error returns.
// ---------------------------------------------------------------------------
TEST_F(ComponentLoader, ApiRejectsBadArguments) {
    EXPECT_EQ(swmm_component_search_path_add(nullptr), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_component_search_path_add(""), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_component_search_path_add("/no/such/directory/anywhere"),
              SWMM_ERR_BADPARAM);
    SWMM_ComponentLibraryInfo info{};
    EXPECT_EQ(swmm_component_library_get(-1, &info), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_component_library_get(swmm_component_library_count(), &info),
              SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_component_library_get(0, nullptr), SWMM_ERR_BADPARAM);
}
