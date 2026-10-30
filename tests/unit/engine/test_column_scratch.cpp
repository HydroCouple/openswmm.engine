// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "ColumnScratchFixture.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
using namespace openswmm::twoD;
namespace fixture = openswmm::twoD::column_test;
namespace {
void save(const std::string& name, const openswmm::detail::CompletedSourceArchive& archive) {
    const char* location = std::getenv("OPENSWMM_COLUMN_SCRATCH_TEST_OUT");
    const auto path = std::filesystem::path(location ? location : OPENSWMM_COLUMN_SCRATCH_TEST_DIR);
    std::filesystem::create_directories(path);
    std::ofstream out(path / (name + ".bin"), std::ios::binary);
    out.write(archive.bytes().data(), archive.bytes().size());
    ASSERT_TRUE(out.good());
}
}
TEST(ColumnScratch, ThermalEveryFieldMatchesFreshOwningCallsAcrossFailuresAndReuse) {
    openswmm::detail::CompletedSourceArchive history;
#ifdef OPENSWMM_TEST_COLUMN_SCRATCH
    std::array<ThermalContactScratch, 2> implicit;
    std::array<ThermalMidpointScratch, 2> midpoint;
#endif
    uint64_t index = 0;
    for (int pass : {0, 1}) {
        auto cases = fixture::thermalCases();
        if (pass) std::reverse(cases.begin(), cases.end());
        for (bool rk2 : {false, true})
            for (auto& c : cases) {
                SCOPED_TRACE(c.label + (rk2 ? " midpoint" : " implicit"));
                auto before = fixture::input(c);
                auto expected = rk2 ? solveThermalContactsRk2(c.stores, c.contacts, c.boundaries, c.seconds)
                                    : solveThermalContacts(c.stores, c.contacts, c.boundaries, c.seconds);
                if (c.label.starts_with("recovery-")) ASSERT_TRUE(expected.error.empty());
                if (c.label == "late-receipt-refusal") {
                    ASSERT_FALSE(expected.error.empty());
                    EXPECT_TRUE(expected.energy.empty());
                    EXPECT_TRUE(expected.temperature.empty());
                    EXPECT_EQ(expected.boundary_j.size(), rk2 ? 0u : 1u);
                    if (!rk2) EXPECT_TRUE(std::isnan(expected.boundary_j[0]));
                }
#ifdef OPENSWMM_TEST_COLUMN_SCRATCH
                auto& actual = rk2
                    ? solveThermalContactsRk2(c.stores, c.contacts, c.boundaries, c.seconds,
                                               midpoint[index % 2])
                    : solveThermalContacts(c.stores, c.contacts, c.boundaries, c.seconds,
                                            implicit[index % 2]);
#else
                auto actual = expected;
#endif
                auto bytes = fixture::output(actual);
                EXPECT_EQ(bytes, fixture::output(expected));
                EXPECT_EQ(before, fixture::input(c));
                history(index, rk2, before, bytes);
                // A mutable caller can consume/edit a borrowed result. The next
                // call must reset every field, including flags and empty tails.
                actual.error = "stale"; actual.interval_limit = true;
                actual.energy.assign(3, -1.); actual.temperature.assign(4, -2.);
                actual.contact_j.assign(5, -3.); actual.boundary_j.assign(6, -4.);
                actual.algebraic_residual = 123.; actual.iterations = 456;
                ++index;
            }
    }
    save("thermal", history);
}
TEST(ColumnScratch, PorewaterEveryFieldMatchesFreshOwningCallsAcrossMethodsAndReuse) {
    openswmm::detail::CompletedSourceArchive history;
#ifdef OPENSWMM_TEST_COLUMN_SCRATCH
    std::array<PorewaterTransportScratch, 2> workspaces;
#endif
    uint64_t index = 0;
    for (int pass : {0, 1}) {
        auto cases = fixture::porewaterCases();
        if (pass) std::reverse(cases.begin(), cases.end());
        for (auto& c : cases)
            for (bool euler : {false, true}) {
                SCOPED_TRACE(c.label + (euler ? " Euler" : " implicit"));
                auto before = fixture::input(c);
                auto expected = euler
                    ? advectPorewaterEuler(c.old_water, c.retention, c.mass, c.change,
                                           c.external_water, c.external_mass, c.withdrawal,
                                           c.reserved_water, c.reserved_mass)
                    : advectPorewater(c.old_water, c.new_water, c.retention, c.mass,
                                      c.external_water, c.external_mass, c.withdrawal);
                if (c.label.starts_with("recovery-")) ASSERT_TRUE(expected.error.empty());
                if (c.label == "late-receipt-refusal")
                    EXPECT_EQ(expected.error, euler ? "Euler porewater accepted receipt overflow."
                                                   : "Porewater transport receipt overflow.");
                if (!expected.error.empty()) {
                    EXPECT_TRUE(expected.mass.empty()); EXPECT_TRUE(expected.concentration.empty());
                    EXPECT_TRUE(expected.withdrawn_mass.empty()); EXPECT_TRUE(expected.vertical_water.empty());
                    EXPECT_TRUE(expected.vertical_mass.empty());
                    EXPECT_EQ(expected.external_mass, 0.); EXPECT_EQ(expected.water_residual, 0.);
                }
#ifdef OPENSWMM_TEST_COLUMN_SCRATCH
                // Use the same workspace for adjacent implicit/Euler calls.
                auto& scratch = workspaces[(index / 2) % 2];
                auto& actual = euler
                    ? advectPorewaterEuler(c.old_water, c.retention, c.mass, c.change,
                                           c.external_water, c.external_mass, c.withdrawal,
                                           c.reserved_water, c.reserved_mass, scratch)
                    : advectPorewater(c.old_water, c.new_water, c.retention, c.mass,
                                      c.external_water, c.external_mass, c.withdrawal, scratch);
#else
                auto actual = expected;
#endif
                auto bytes = fixture::output(actual);
                EXPECT_EQ(bytes, fixture::output(expected));
                EXPECT_EQ(before, fixture::input(c));
                history(index, euler, before, bytes);
                actual.error = "stale"; actual.external_mass = 123.; actual.water_residual = 456.;
                actual.mass.assign(3, -1.); actual.concentration.assign(4, -2.);
                actual.withdrawn_mass.assign(5, -3.); actual.vertical_water.assign(6, -4.);
                actual.vertical_mass.assign(7, -5.);
                ++index;
            }
    }
    save("porewater", history);
}
TEST(ColumnScratch, ProducerBuffersAndOwningInputAliasesRetainValueSemantics) {
    openswmm::detail::CompletedSourceArchive history;
    std::vector<ThermalStore> stores;
    std::vector<ThermalContact> contacts;
    for (int n : {1, 8, 2, 0, 16, 1}) {
        PorousHeatColumn c;
        c.area = 2.;
        for (int j = 0; j <= n; ++j) c.depth_edges.push_back(.125 * j);
        for (int j = 0; j < n; ++j) {
            c.capacity.push_back(10. + j); c.energy.push_back(j ? 2. * j : -0.);
            c.conductivity.push_back(.2 + j);
        }
        const auto original_energy = c.energy;
        auto expected_stores = c.stores(); auto expected_contacts = c.contacts();
#ifdef OPENSWMM_TEST_COLUMN_SCRATCH
        c.stores(stores); c.contacts(contacts);
#else
        stores = expected_stores; contacts = expected_contacts;
#endif
        fixture::ThermalCase expected, actual;
        expected.stores = expected_stores; expected.contacts = expected_contacts;
        actual.stores = stores; actual.contacts = contacts;
        auto before = fixture::input(expected), after = fixture::input(actual);
        EXPECT_EQ(before, after); EXPECT_EQ(original_energy, c.energy);
        history(n, after);
        stores.assign(17, {-1., -2.}); contacts.assign(18, {-1, -2, -3.});
    }
    auto c = fixture::porewaterCase(3, 0);
    for (bool euler : {false, true}) {
        auto result = euler
            ? advectPorewaterEuler(c.old_water, c.retention, c.mass, c.change, c.external_water,
                                   c.external_mass, c.withdrawal, c.reserved_water, c.reserved_mass)
            : advectPorewater(c.old_water, c.new_water, c.retention, c.mass, c.external_water,
                              c.external_mass, c.withdrawal);
        ASSERT_TRUE(result.error.empty());
        auto mass = result.mass;
        auto expected = euler
            ? advectPorewaterEuler(c.old_water, c.retention, mass, c.change, c.external_water,
                                   c.external_mass, c.withdrawal, c.reserved_water, c.reserved_mass)
            : advectPorewater(c.old_water, c.new_water, c.retention, mass, c.external_water,
                              c.external_mass, c.withdrawal);
        result = euler
            ? advectPorewaterEuler(c.old_water, c.retention, result.mass, c.change, c.external_water,
                                   c.external_mass, c.withdrawal, c.reserved_water, c.reserved_mass)
            : advectPorewater(c.old_water, c.new_water, c.retention, result.mass, c.external_water,
                              c.external_mass, c.withdrawal);
        auto before = fixture::output(expected), after = fixture::output(result);
        EXPECT_EQ(before, after); history(euler, after);
    }
    save("producers-aliases", history);
}
TEST(ColumnScratch, IndependentBorrowedLifetimesAndDirectSameWorkspaceRecovery) {
    openswmm::detail::CompletedSourceArchive history;
    for (bool rk2 : {false, true}) {
        auto good = fixture::thermalCase(8, 2);
        auto cases = fixture::thermalCases();
        auto bad = *std::find_if(cases.begin(), cases.end(), [](const auto& c) {
            return c.label == "late-receipt-refusal";
        });
        if (rk2) { bad = good; bad.seconds = 1.e20; }
        auto expected = rk2 ? solveThermalContactsRk2(good.stores, good.contacts, good.boundaries, good.seconds)
                            : solveThermalContacts(good.stores, good.contacts, good.boundaries, good.seconds);
        auto refusal = rk2 ? solveThermalContactsRk2(bad.stores, bad.contacts, bad.boundaries, bad.seconds)
                           : solveThermalContacts(bad.stores, bad.contacts, bad.boundaries, bad.seconds);
        ASSERT_TRUE(expected.error.empty()); ASSERT_FALSE(refusal.error.empty());
        auto accepted_bytes = fixture::output(expected), refused_bytes = fixture::output(refusal);
#ifdef OPENSWMM_TEST_COLUMN_SCRATCH
        ThermalContactScratch be_a, be_b;
        ThermalMidpointScratch rk_a, rk_b;
        auto run = [&](const fixture::ThermalCase& c, bool second) -> ThermalSolution& {
            return rk2 ? solveThermalContactsRk2(c.stores, c.contacts, c.boundaries, c.seconds,
                                                  second ? rk_b : rk_a)
                       : solveThermalContacts(c.stores, c.contacts, c.boundaries, c.seconds,
                                               second ? be_b : be_a);
        };
        auto& a = run(good, false);
        EXPECT_EQ(fixture::output(a), accepted_bytes);
        EXPECT_EQ(fixture::output(run(bad, true)), refused_bytes);
        EXPECT_EQ(fixture::output(a), accepted_bytes);
        EXPECT_EQ(fixture::output(run(good, true)), accepted_bytes);
        EXPECT_EQ(fixture::output(a), accepted_bytes);
        EXPECT_EQ(fixture::output(run(bad, false)), refused_bytes);
        EXPECT_EQ(fixture::output(run(good, false)), accepted_bytes);
#endif
        history(rk2, accepted_bytes, refused_bytes, accepted_bytes);
    }
    for (bool euler : {false, true}) {
        auto good = fixture::porewaterCase(8, 0);
        auto cases = fixture::porewaterCases();
        auto bad = *std::find_if(cases.begin(), cases.end(), [](const auto& c) {
            return c.label == "late-receipt-refusal";
        });
        auto owning = [&](const fixture::PorewaterCase& c) {
            return euler ? advectPorewaterEuler(c.old_water, c.retention, c.mass, c.change,
                              c.external_water, c.external_mass, c.withdrawal,
                              c.reserved_water, c.reserved_mass)
                         : advectPorewater(c.old_water, c.new_water, c.retention, c.mass,
                              c.external_water, c.external_mass, c.withdrawal);
        };
        auto expected = owning(good), refusal = owning(bad);
        ASSERT_TRUE(expected.error.empty());
        ASSERT_EQ(refusal.error, euler ? "Euler porewater accepted receipt overflow."
                                      : "Porewater transport receipt overflow.");
        auto accepted_bytes = fixture::output(expected), refused_bytes = fixture::output(refusal);
#ifdef OPENSWMM_TEST_COLUMN_SCRATCH
        PorewaterTransportScratch a_scratch, b_scratch;
        auto run = [&](const fixture::PorewaterCase& c, bool second) -> PorewaterAdvection& {
            auto& scratch = second ? b_scratch : a_scratch;
            return euler ? advectPorewaterEuler(c.old_water, c.retention, c.mass, c.change,
                              c.external_water, c.external_mass, c.withdrawal,
                              c.reserved_water, c.reserved_mass, scratch)
                         : advectPorewater(c.old_water, c.new_water, c.retention, c.mass,
                              c.external_water, c.external_mass, c.withdrawal, scratch);
        };
        auto& a = run(good, false);
        EXPECT_EQ(fixture::output(a), accepted_bytes);
        EXPECT_EQ(fixture::output(run(bad, true)), refused_bytes);
        EXPECT_EQ(fixture::output(a), accepted_bytes);
        EXPECT_EQ(fixture::output(run(good, true)), accepted_bytes);
        EXPECT_EQ(fixture::output(a), accepted_bytes);
        EXPECT_EQ(fixture::output(run(bad, false)), refused_bytes);
        EXPECT_EQ(fixture::output(run(good, false)), accepted_bytes);
#endif
        history(euler, accepted_bytes, refused_bytes, accepted_bytes);
    }
    save("independence-recovery", history);
}
