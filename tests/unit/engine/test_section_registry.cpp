/**
 * @file test_section_registry.cpp
 * @brief Unit tests for the SectionRegistry (optional sections support).
 *
 * @see src/engine/input/SectionRegistry.hpp
 * @see src/engine/input/SectionRegistry.cpp
 * @ingroup engine_input
 */

#include <gtest/gtest.h>

#include "../../src/engine/input/SectionRegistry.hpp"
#include "../../src/engine/core/SimulationContext.hpp"

using openswmm::input::SectionRegistry;
using openswmm::SimulationContext;

namespace {

// Helper: build a registry with one built-in "JUNCTIONS" handler
static SectionRegistry make_registry_with_builtin() {
    SectionRegistry reg;
    reg.register_builtin("JUNCTIONS", [](SimulationContext&,
                                         const std::vector<std::string>&) {
        // no-op for test
    });
    return reg;
}

// ============================================================================
// has() / is_custom()
// ============================================================================

TEST(SectionRegistryTest, HasBuiltinReturnsTrueAfterRegister) {
    auto reg = make_registry_with_builtin();
    EXPECT_TRUE(reg.has("JUNCTIONS"));
    EXPECT_TRUE(reg.has("junctions"));    // case-insensitive
}

TEST(SectionRegistryTest, HasReturnsFalseForUnregistered) {
    SectionRegistry reg;
    EXPECT_FALSE(reg.has("XYZZY"));
}

TEST(SectionRegistryTest, IsCustomFalseForBuiltin) {
    auto reg = make_registry_with_builtin();
    EXPECT_FALSE(reg.is_custom("JUNCTIONS"));
}

TEST(SectionRegistryTest, IsCustomTrueAfterRegisterCustom) {
    SectionRegistry reg;
    reg.register_custom("MY_SECTION", [](SimulationContext&,
                                          const std::vector<std::string>&) {});
    EXPECT_TRUE(reg.is_custom("MY_SECTION"));
    EXPECT_TRUE(reg.has("MY_SECTION"));
}

// ============================================================================
// dispatch()
// ============================================================================

TEST(SectionRegistryTest, BuiltinSectionDispatched) {
    SectionRegistry reg;
    bool called = false;
    reg.register_builtin("OPTIONS", [&called](SimulationContext&,
                                               const std::vector<std::string>&) {
        called = true;
    });

    SimulationContext ctx;
    std::vector<std::string> lines = {"FLOW_UNITS  CFS"};
    reg.dispatch("OPTIONS", ctx, lines);

    EXPECT_TRUE(called);
}

TEST(SectionRegistryTest, UnknownSectionSkipped) {
    SectionRegistry reg;
    SimulationContext ctx;
    // Should not throw or crash
    EXPECT_NO_THROW(reg.dispatch("NONEXISTENT", ctx, {}));
}

TEST(SectionRegistryTest, CustomHandlerCalledWithSectionLines) {
    SectionRegistry reg;
    std::vector<std::string> captured;
    reg.register_custom("MY_SECTION", [&captured](SimulationContext&,
                                                    const std::vector<std::string>& lines) {
        captured = lines;
    });

    SimulationContext ctx;
    std::vector<std::string> input_lines = {"line1", "line2", "line3"};
    reg.dispatch("MY_SECTION", ctx, input_lines);

    EXPECT_EQ(captured, input_lines);
}

TEST(SectionRegistryTest, CustomHandlerOverridesBuiltin) {
    SectionRegistry reg;

    bool builtin_called = false;
    bool custom_called  = false;

    reg.register_builtin("JUNCTIONS", [&builtin_called](SimulationContext&,
                                                         const std::vector<std::string>&) {
        builtin_called = true;
    });
    reg.register_custom("JUNCTIONS", [&custom_called](SimulationContext&,
                                                       const std::vector<std::string>&) {
        custom_called = true;
    });

    SimulationContext ctx;
    reg.dispatch("JUNCTIONS", ctx, {});

    EXPECT_FALSE(builtin_called);
    EXPECT_TRUE(custom_called);
}

TEST(SectionRegistryTest, CaseInsensitiveDispatch) {
    SectionRegistry reg;
    bool called = false;
    reg.register_builtin("CONDUITS", [&called](SimulationContext&,
                                                const std::vector<std::string>&) {
        called = true;
    });
    SimulationContext ctx;
    reg.dispatch("conduits", ctx, {});
    EXPECT_TRUE(called);
}

// ============================================================================
// registered_tags()
// ============================================================================

TEST(SectionRegistryTest, RegisteredTagsListsAll) {
    SectionRegistry reg;
    reg.register_builtin("JUNCTIONS", [](SimulationContext&, const std::vector<std::string>&) {});
    reg.register_builtin("CONDUITS",  [](SimulationContext&, const std::vector<std::string>&) {});
    reg.register_custom("MY_SECTION",[](SimulationContext&, const std::vector<std::string>&) {});

    auto tags = reg.registered_tags();
    ASSERT_EQ(tags.size(), 3u);
    // All three should be present
    auto has_tag = [&](const std::string& t) {
        return std::find(tags.begin(), tags.end(), t) != tags.end();
    };
    EXPECT_TRUE(has_tag("JUNCTIONS"));
    EXPECT_TRUE(has_tag("CONDUITS"));
    EXPECT_TRUE(has_tag("MY_SECTION"));
}

} /* anonymous namespace */
