// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "2d/subsurface/LayerTransport.hpp"
#include "core/HotStartManager.hpp"
#include "core/SimulationContext.hpp"
#include <array>
#include <filesystem>
#include <memory>
using namespace openswmm;
using namespace openswmm::twoD;
TEST(PortabilityRegression, TopLayerExternalSinkHasNoReceivingMatrixRow) {
    const auto one=solveLayerTransport({1},{2},{{0,-1,.5}});
    ASSERT_TRUE(one.error.empty())<<one.error;
    ASSERT_EQ(one.mass.size(),1u);ASSERT_EQ(one.transferred.size(),1u);
    EXPECT_NEAR(one.mass[0],4./3.,1e-14);
    EXPECT_NEAR(one.transferred[0],2./3.,1e-14);
    EXPECT_NEAR(one.mass[0]+one.transferred[0],2,1e-14);
    const auto chain=solveLayerTransport({1,1},{4,2},{{0,-1,.5},{0,1,.5},{1,-1,1}});
    ASSERT_TRUE(chain.error.empty())<<chain.error;
    ASSERT_EQ(chain.mass.size(),2u);ASSERT_EQ(chain.transferred.size(),3u);
    EXPECT_DOUBLE_EQ(chain.mass[0],2);EXPECT_DOUBLE_EQ(chain.mass[1],1.5);
    EXPECT_DOUBLE_EQ(chain.mass[0]+chain.mass[1]+chain.transferred[0]+chain.transferred[2],6);
}
TEST(PortabilityRegression, RestartChecksumSupportsEveryByteAlignment) {
    std::filesystem::create_directories(OPENSWMM_PORTABILITY_OUT);
    std::array<bool,4> tested{};
    for(int length=0;length<4;++length) {
        SimulationContext context;context.state=EngineState::RUNNING;
        context.spatial.crs=std::string(length,'x');
        const auto path=(std::filesystem::path(OPENSWMM_PORTABILITY_OUT)/("alignment-"+std::to_string(length)+".hsf")).string();
        std::unique_ptr<HotStartFile> saved(HotStartManager::save(context,path));
        ASSERT_TRUE(saved)<<HotStartManager::last_io_error();saved.reset();
        const auto bytes=std::filesystem::file_size(path);ASSERT_GE(bytes,4u);
        tested[(bytes-4)%4]=true;
        std::unique_ptr<HotStartFile> loaded(HotStartManager::open(path));
        ASSERT_TRUE(loaded)<<HotStartManager::last_io_error();
        EXPECT_EQ(loaded->header.crs,context.spatial.crs);
    }
    for(bool aligned:tested)EXPECT_TRUE(aligned);
}
