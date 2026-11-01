#include <gtest/gtest.h>
#include <cstdlib>
#include <new>
#include "quality/ConservativeMixing.hpp"
static bool measuring=false;
static std::size_t allocations=0;
void* operator new(std::size_t n) {if(measuring)++allocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n) {return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
TEST(ConservativeMixingWorkspace, WarmPrepareAndSolveAllocateNothing) {
    using namespace openswmm::quality;
    MixingWater w{{0,10,0},{0,10,0},{1,0,0},{0,0,1},{0,0,0},{0,0,0},{{0,1,1},{1,2,1}}};
    MixingScalar s{{0,10,0},{20,0,0},{},true};ConservativeMixing solver;MixedScalar out;
    for(int i=0;i<4;++i) {ASSERT_TRUE(solver.prepare(w));ASSERT_TRUE(solver.solve(s,out).converged);}
    bool passed=true;allocations=0;measuring=true;
    for(int i=0;i<50;++i) {passed &=solver.prepare(w);passed &=solver.solve(s,out).converged;}
    measuring=false;EXPECT_TRUE(passed);EXPECT_EQ(allocations,0);
}
