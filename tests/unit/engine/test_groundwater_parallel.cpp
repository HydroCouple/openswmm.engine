// SPDX-License-Identifier: Apache-2.0
#include "GroundwaterParallelFixture.hpp"
#include <gtest/gtest.h>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>

TEST(GroundwaterParallel, PreservesAquiferHistoriesAndOrderedBalanceBits) {
    const double steps[] = {1., 7., 60., 300., 30., 2., 120., 17., 45.};
    for (bool si : {false, true})
      for (int count : {0, 127, 1027}) {
        std::string reference;
        for (int threads : {1, 4, 8, 0, -1}) {
            openswmm::testing::GroundwaterParallelFixture fixture(count, threads, si);
            openswmm::detail::CompletedSourceArchive history;
            for (int step = 0; step < 9; ++step) {
                fixture.prepare(step);
                fixture.advance(steps[step], step);
                const auto& state = fixture.solver.state();
                for (const auto* values : {&state.theta, &state.lower_depth, &state.old_flow,
                                          &state.gw_flow, &state.upper_evap, &state.lower_evap,
                                          &state.deep_loss, &state.max_infil_vol})
                    for (double value : *values)
                        ASSERT_TRUE(std::isfinite(value));
                const auto& balance = fixture.context.mass_balance;
                for (double value : {balance.gw_infil, balance.gw_upper_evap,
                                     balance.gw_lower_evap, balance.gw_lower_perc,
                                     balance.gw_lateral_flow})
                    ASSERT_TRUE(std::isfinite(value));
                fixture.capture(history);
            }
            const auto& bytes = history.bytes();
            if (threads == 1)
                reference = bytes;
            else
                EXPECT_TRUE(bytes == reference) << count << " aquifers, " << threads << " workers";
            if (const char* output = std::getenv("OPENSWMM_GW_PARALLEL_TEST_OUT")) {
                std::filesystem::create_directories(output);
                const auto path = std::filesystem::path(output) /
                    (std::string(si ? "si_" : "us_") + std::to_string(count) + "_" + std::to_string(threads) + ".bin");
                std::ofstream(path, std::ios::binary).write(bytes.data(), bytes.size());
            }
        }
    }
}
