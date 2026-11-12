/**
 * @file      test_section_registry.cpp
 * @brief     Unit tests for the SectionRegistry (optional sections support).
 *
 * @see src/engine/input/SectionRegistry.hpp
 * @see src/engine/input/SectionRegistry.cpp
 * @ingroup engine_input
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <algorithm>

#include "../../src/engine/input/SectionRegistry.hpp"
#include "../../src/engine/core/SimulationContext.hpp"

using openswmm::input::SectionRegistry;
using openswmm::SimulationContext;

namespace {

// Helper: build a registry with one built-in "JUNCTIONS" handler
/**
 * @brief Build a registry with a deliberately no-op JUNCTIONS builtin handler.
 *
 * The callback accepts but does not mutate its context/line arguments and captures nothing,
 * remaining valid after return. This fixture tests registration/dispatch bookkeeping rather than
 * actual junction parsing.
 * @return Owned registry with the single named builtin registration.
 */
static SectionRegistry make_registry_with_builtin() {
    SectionRegistry reg;
    reg.register_builtin("JUNCTIONS", [](SimulationContext&, const std::vector<std::string>&) {
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
    EXPECT_TRUE(reg.has("junctions")); // case-insensitive
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
    reg.register_custom("MY_SECTION", [](SimulationContext&, const std::vector<std::string>&) {});
    EXPECT_TRUE(reg.is_custom("MY_SECTION"));
    EXPECT_TRUE(reg.has("MY_SECTION"));
}

// ============================================================================
// dispatch()
// ============================================================================

TEST(SectionRegistryTest, BuiltinSectionDispatched) {
    SectionRegistry reg;
    bool called = false;
    reg.register_builtin("OPTIONS", [&called](SimulationContext&, const std::vector<std::string>&) {
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
    reg.register_custom("MY_SECTION",
                        [&captured](SimulationContext&, const std::vector<std::string>& lines) {
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
    bool custom_called = false;

    reg.register_builtin("JUNCTIONS",
                         [&builtin_called](SimulationContext&, const std::vector<std::string>&) {
                             builtin_called = true;
                         });
    reg.register_custom("JUNCTIONS",
                        [&custom_called](SimulationContext&, const std::vector<std::string>&) {
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
                                               const std::vector<std::string>&) { called = true; });
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
    reg.register_builtin("CONDUITS", [](SimulationContext&, const std::vector<std::string>&) {});
    reg.register_custom("MY_SECTION", [](SimulationContext&, const std::vector<std::string>&) {});

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

// ============================================================================
// View handlers (register_custom_views / dispatch_views)
// ============================================================================

TEST(SectionRegistryTest, ViewHandlerReceivesTheCallersViews) {
    SectionRegistry reg;
    std::vector<std::string_view> seen;
    reg.register_custom_views(
        "2d_vertices",
        [&seen](SimulationContext&, const std::vector<std::string_view>& lines) { seen = lines; });
    EXPECT_TRUE(reg.has("2D_VERTICES"));
    EXPECT_TRUE(reg.is_custom("2D_VERTICES"));

    const std::string buffer = "0 0 1\n10 0 1";
    const std::vector<std::string_view> lines = {std::string_view(buffer).substr(0, 5),
                                                 std::string_view(buffer).substr(6)};
    SimulationContext ctx;
    reg.dispatch_views("2D_VERTICES", ctx, lines);
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0].data(), buffer.data()); // the views themselves, not copies
    EXPECT_EQ(seen[1], "10 0 1");
}

TEST(SectionRegistryTest, EitherDispatchReachesEitherHandlerForm) {
    SectionRegistry reg;
    std::vector<std::string> by_string;
    std::vector<std::string> by_view;
    reg.register_custom("STRINGS",
                        [&by_string](SimulationContext&, const std::vector<std::string>& lines) {
                            by_string = lines;
                        });
    reg.register_custom_views(
        "VIEWS", [&by_view](SimulationContext&, const std::vector<std::string_view>& lines) {
            by_view.assign(lines.begin(), lines.end());
        });

    SimulationContext ctx;
    const std::string a = "a b", b = "c";
    reg.dispatch_views("STRINGS", ctx, {a, b});
    reg.dispatch("VIEWS", ctx, {a, b});
    EXPECT_EQ(by_string, (std::vector<std::string>{"a b", "c"}));
    EXPECT_EQ(by_view, (std::vector<std::string>{"a b", "c"}));
    EXPECT_NO_THROW(reg.dispatch_views("NONEXISTENT", ctx, {a}));
}

TEST(SectionRegistryTest, LaterCustomRegistrationReplacesTheOtherForm) {
    SectionRegistry reg;
    int string_calls = 0, view_calls = 0;
    const auto string_handler =
        [&string_calls](SimulationContext&, const std::vector<std::string>&) { ++string_calls; };
    const auto view_handler = [&view_calls](SimulationContext&,
                                            const std::vector<std::string_view>&) { ++view_calls; };
    SimulationContext ctx;

    reg.register_custom("SEC", string_handler);
    reg.register_custom_views("SEC", view_handler);
    reg.dispatch("SEC", ctx, {});
    reg.dispatch_views("SEC", ctx, {});
    EXPECT_EQ(string_calls, 0);
    EXPECT_EQ(view_calls, 2);

    reg.register_custom("SEC", string_handler);
    reg.dispatch("SEC", ctx, {});
    reg.dispatch_views("SEC", ctx, {});
    EXPECT_EQ(string_calls, 2);
    EXPECT_EQ(view_calls, 2);

    const auto tags = reg.registered_tags();
    EXPECT_EQ(std::count(tags.begin(), tags.end(), "SEC"), 1);
}

TEST(SectionRegistryTest, CustomViewHandlerOverridesBuiltin) {
    SectionRegistry reg;
    bool builtin_called = false, view_called = false;
    reg.register_builtin("JUNCTIONS",
                         [&builtin_called](SimulationContext&, const std::vector<std::string>&) {
                             builtin_called = true;
                         });
    reg.register_custom_views(
        "JUNCTIONS", [&view_called](SimulationContext&, const std::vector<std::string_view>&) {
            view_called = true;
        });
    SimulationContext ctx;
    reg.dispatch("JUNCTIONS", ctx, {});
    EXPECT_FALSE(builtin_called);
    EXPECT_TRUE(view_called);
}

} /* anonymous namespace */
