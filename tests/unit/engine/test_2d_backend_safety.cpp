// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "platform_test_support.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/MeshData.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/solver/SurfaceSolverFactory.hpp"
#include "2d/solver/PluginSurfaceSolver.hpp"
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_model.h>
#include <atomic>
#include <future>
#include <set>
using namespace openswmm::twoD;
extern "C" int openswmm_test_probe_count();
extern "C" int openswmm_test_factory_count();
namespace {
struct Env {
    std::string key,value; bool existed;
    Env(const char* k,const char* v):key(k),existed(std::getenv(k)!=nullptr) {
        if(existed)value=std::getenv(k);
        if(v)plattest::setEnvVar(k,v);else plattest::unsetEnvVar(k);
    }
    ~Env(){if(existed)plattest::setEnvVar(key.c_str(),value.c_str());else plattest::unsetEnvVar(key.c_str());}
};
}
TEST(BackendSafety, CpuDefaultDoesNotProbeEvenForLargeModels) {
    Env env("OPENSWMM_2D_BACKEND",nullptr);
    SolverOptions2D o;o.auto_backend_cpu=true;
    const auto before=openswmm_test_probe_count();std::string label;
    auto s=makeSurfaceSolver(o,&label,1000000);
    ASSERT_TRUE(s);EXPECT_EQ(label.substr(0,3),"cpu");
    EXPECT_EQ(openswmm_test_probe_count(),before);
    EXPECT_EQ(o.backend,Backend2D::AUTO);
    EXPECT_FALSE(makeGwRechargeBackend(o,1000000).evaluate);
}
TEST(BackendSafety, StalePluginRejectedBeforeProbe) {
    Env env("OPENSWMM_2D_BACKEND","cuda"),stale("OPENSWMM_TEST_STALE_GPU","1");
    SolverOptions2D o;std::string label;const auto before=openswmm_test_probe_count();
    auto s=makeSurfaceSolver(o,&label,1);
    ASSERT_TRUE(s);EXPECT_EQ(label.substr(0,3),"cpu");
    EXPECT_EQ(openswmm_test_probe_count(),before);
}
TEST(BackendSafety, ExplicitRequestsOverrideCpuDefaultAndFactoryFailureIsContained) {
    Env env("OPENSWMM_2D_BACKEND",nullptr),stale("OPENSWMM_TEST_STALE_GPU",nullptr);
    SolverOptions2D o;o.auto_backend_cpu=true;o.backend=Backend2D::CUDA;
    std::string label;auto before=openswmm_test_probe_count();
    auto s=makeSurfaceSolver(o,&label,1);
    ASSERT_TRUE(s);EXPECT_EQ(label.substr(0,3),"cpu");
    EXPECT_GT(openswmm_test_probe_count(),before);
    o.backend=Backend2D::CPU;Env override("OPENSWMM_2D_BACKEND","cuda");
    before=openswmm_test_probe_count();s=makeSurfaceSolver(o,&label,1);
    ASSERT_TRUE(s);EXPECT_GT(openswmm_test_probe_count(),before);
}
TEST(BackendSafety, InvalidEnvironmentFailsBeforeCapabilityFallback) {
    Env env("OPENSWMM_2D_BACKEND","cudaa");SolverOptions2D o;
    EXPECT_THROW(makeSurfaceSolver(o,nullptr,1,false),std::invalid_argument);
}
TEST(BackendSafety, SessionPolicyRequiresMutableLifecycleAndRetainsAuthoredOption) {
    EXPECT_NE(swmm_2d_set_auto_backend_cpu(nullptr,1),SWMM_OK);
    auto e=swmm_engine_create();ASSERT_NE(e,nullptr);
    EXPECT_EQ(swmm_2d_set_auto_backend_cpu(e,1),SWMM_ERR_LIFECYCLE);
    swmm_engine_destroy(e);
    e=swmm_engine_new();ASSERT_NE(e,nullptr);
    EXPECT_EQ(swmm_2d_set_auto_backend_cpu(e,2),SWMM_ERR_BADPARAM);
    for(const char* mode:{"AUTO","CUDA","CPU"}) {
        ASSERT_EQ(swmm_options_set_ext(e,"BACKEND",mode),SWMM_OK);
        ASSERT_EQ(swmm_2d_set_auto_backend_cpu(e,1),SWMM_OK);
        char value[32]={};ASSERT_EQ(swmm_options_get_ext(e,"BACKEND",value,sizeof value),SWMM_OK);
        EXPECT_STREQ(value,mode);
    }
    EXPECT_EQ(swmm_2d_set_auto_backend_cpu(e,0),SWMM_OK);
    swmm_engine_destroy(e);
}
TEST(BackendSafety, ConcurrentCallersHaveOneOwnerAndExceptionsDoNotPoisonQueue) {
    auto owner=GpuExecutionOwner::shared();std::atomic<int> active=0,peak=0;int count=0;
    std::vector<std::future<std::thread::id>> tasks;
    for(int i=0;i<8;++i)tasks.push_back(std::async(std::launch::async,[&] {
        std::thread::id id;
        for(int k=0;k<30;++k)id=owner->invoke([&] {
            peak.store(std::max(peak.load(),++active));++count;
            auto nested=owner->invoke([] {return std::this_thread::get_id();});
            --active;return nested;
        });return id;
    }));
    std::set<std::thread::id> ids;for(auto& task:tasks)ids.insert(task.get());
    EXPECT_EQ(ids.size(),1u);EXPECT_NE(*ids.begin(),std::this_thread::get_id());
    EXPECT_EQ(peak,1);EXPECT_EQ(count,240);
    EXPECT_THROW(owner->invoke([] {throw std::runtime_error("injected");}),std::runtime_error);
    EXPECT_EQ(owner->invoke([] {return 7;}),7);
}
namespace {
struct ThreadBoundSolver final : ISurfaceSolver {
    explicit ThreadBoundSolver(std::thread::id& destroyed) : destroyed(destroyed),thread(std::this_thread::get_id()) {}
    ~ThreadBoundSolver() override {destroyed=std::this_thread::get_id();}
    std::thread::id& destroyed;std::thread::id thread;bool initialized=false;double time=0;
    std::vector<double> exchange{3.0};
    void check(){if(std::this_thread::get_id()!=thread)throw std::runtime_error("wrong owner");}
    void initialize(MeshData&,SurfaceStateData&,SolverOptions2D&) override {check();initialized=true;}
    double advance(double,double end) override {check();time=end;exchange[0]=end;return end;}
    void reinitialize(double t) override {check();time=t;}
    void finalize() override {check();initialized=false;}
    long last_num_steps() const noexcept override{return 2;}
    double last_step_size() const noexcept override{return .5;}
    double completedSourceTime() const noexcept override{return time;}
    const std::vector<double>& last_coupling_exchange() const noexcept override{return exchange;}
    bool is_initialized() const noexcept override{return initialized;}
};
}
TEST(BackendSafety, AdapterLifecycleAndDestructionStayOnAllocatingThread) {
    auto owner=GpuExecutionOwner::shared();std::thread::id destroyed;
    auto* raw=owner->invoke([&] {return new ThreadBoundSolver(destroyed);});
    const auto thread=raw->thread;
    {
        PluginSurfaceSolver solver(raw,[](void* p){delete static_cast<ISurfaceSolver*>(p);},owner);
        MeshData mesh;SurfaceStateData state;SolverOptions2D opts;
        solver.initialize(mesh,state,opts);EXPECT_TRUE(solver.is_initialized());
        EXPECT_DOUBLE_EQ(solver.advance(0,4),4);EXPECT_DOUBLE_EQ(solver.completedSourceTime(),4);
        EXPECT_EQ(solver.last_num_steps(),2);EXPECT_DOUBLE_EQ(solver.last_step_size(),.5);
        EXPECT_EQ(solver.last_coupling_exchange(),std::vector<double>{4});
        solver.reinitialize(1);EXPECT_DOUBLE_EQ(solver.completedSourceTime(),1);
        solver.finalize();EXPECT_FALSE(solver.is_initialized());
    }
    EXPECT_EQ(destroyed,thread);EXPECT_NE(destroyed,std::this_thread::get_id());
}
#ifdef OPENSWMM_TEST_OMP_DIRECTORY
#include "2d/mesh/MeshBuilder.hpp"
TEST(BackendSafety, ConcurrentRealPluginModelsRetainSeparateState) {
    Env backend("OPENSWMM_2D_BACKEND","omp"),path("OPENSWMM_GPU_PLUGIN_PATH",OPENSWMM_TEST_OMP_DIRECTORY);
    Env threads("OPENSWMM_2D_THREADS","2");
    std::vector<std::future<double>> tasks;
    for(int run=0;run<4;++run)tasks.push_back(std::async(std::launch::async,[run] {
        // Each caller owns a different closed basin and exercises creation,
        // stepping and destruction while the other callers remain active.
        for(int repeat=0;repeat<3;++repeat) {
            MeshData mesh;mesh.resize_vertices(4);
            mesh.vx={0,1,0,1};mesh.vy={0,0,1,1};mesh.vz={0,0,0,0};
            mesh.resize_triangles(2);mesh.set_triangle(0,0,1,3);mesh.set_triangle(1,0,3,2);
            mesh.mannings_n={.03,.03};buildMeshTopology(mesh);
            SurfaceStateData state;state.resize(2,4);state.volume={.1*(run+1),0};
            SolverOptions2D options;options.max_timestep=.01;options.cfl_number=.4;
            options.requested_threads=2;options.num_threads=1;
            std::string chosen;auto solver=makeSurfaceSolver(options,&chosen,2);
            if(chosen.rfind("omp",0)!=0)throw std::runtime_error("real OpenMP plugin did not load: "+chosen);
            solver->initialize(mesh,state,options);
            for(int i=0;i<10;++i)solver->advance(i*.01,(i+1)*.01);
            if(state.volume[0]<0||state.volume[1]<=0)throw std::runtime_error("invalid closed-basin redistribution");
            const double error=std::abs(state.volume[0]+state.volume[1]-.1*(run+1));
            if(error>1e-12)throw std::runtime_error("cross-model water corruption");
            solver->finalize();
        }
        return .1*(run+1);
    }));
    for(auto& task:tasks)EXPECT_NO_THROW(task.get());
}
#endif

TEST(BackendSafety, PriorSurfaceAbiIsRejectedBeforeFactoryConstruction) {
    Env backend("OPENSWMM_2D_BACKEND","cuda"),old("OPENSWMM_TEST_OLD_GPU_ABI","1"),stale("OPENSWMM_TEST_STALE_GPU",nullptr);
    SolverOptions2D options;std::string label;const int factories=openswmm_test_factory_count(),probes=openswmm_test_probe_count();
    auto solver=makeSurfaceSolver(options,&label,1);ASSERT_TRUE(solver);EXPECT_EQ(label.substr(0,3),"cpu");
    EXPECT_GT(openswmm_test_probe_count(),probes);EXPECT_EQ(openswmm_test_factory_count(),factories);
}
