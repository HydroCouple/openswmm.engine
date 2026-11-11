// SPDX-License-Identifier: Apache-2.0
// Exercise the exported component factory, including the private native session boundary.
#include <gtest/gtest.h>
#include "core/FileIO.hpp"
#include <hydrocouplecomponentabi.h>
#include <hydrocoupletemporal.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_passive_exchange.h>
#include <openswmm/engine/openswmm_split_material.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
namespace HC=HydroCouple;
namespace fs=std::filesystem;
using openswmm::io::path_utf8;
using Json=nlohmann::json;
using Status=HC::IModelComponent::ComponentStatus;
namespace {
constexpr char port[]="openswmm.node-surface-material.v10";
std::string bytes(const fs::path& p){std::ifstream in(p,std::ios::binary);if(!in)throw std::runtime_error(p.string());return {std::istreambuf_iterator<char>(in),{}};}
void save(const fs::path& p,const std::string& text){std::ofstream out(p,std::ios::binary);out<<text;if(!out)throw std::runtime_error(p.string());}
void replace(std::string& s,const std::string& from,const std::string& to){auto pos=s.find(from);if(pos==s.npos)throw std::runtime_error("Missing fixture token: "+from);s.replace(pos,from.size(),to);}
class Library {
#ifdef _WIN32
    HMODULE handle_=LoadLibraryA(ENGINE_COMPONENT_LIBRARY);
    void* symbol(const char* name){return reinterpret_cast<void*>(GetProcAddress(handle_,name));}
#else
    void* handle_=dlopen(ENGINE_COMPONENT_LIBRARY,RTLD_NOW|RTLD_LOCAL);
    void* symbol(const char* name){return handle_?dlsym(handle_,name):nullptr;}
#endif
public:
    ~Library(){if(handle_){
#ifdef _WIN32
        FreeLibrary(handle_);
#else
        dlclose(handle_);
#endif
    }}
    std::unique_ptr<HC::IModelComponent> create(){
        auto abi=reinterpret_cast<HydroCoupleComponentAbiFn>(symbol(HYDROCOUPLE_COMPONENT_ABI_SYMBOL));
        auto entry=reinterpret_cast<HydroCoupleComponentInfoFn>(symbol(HYDROCOUPLE_COMPONENT_INFO_SYMBOL));
        if(!abi||std::string(abi())!=HYDROCOUPLE_COMPONENT_ABI_STAMP||!entry)throw std::runtime_error("Component ABI unavailable");
        auto* info=dynamic_cast<HC::IModelComponentInfo*>(entry());
        if(!info)throw std::runtime_error("Missing component factory");return info->createComponentInstance();
    }
};
void argument(HC::IModelComponent& c,const std::string& id,const Json& value){
    for(auto* a:c.arguments())if(a->id()==id){std::string why;if(!a->initialize(value.dump(),HC::IArgument::ArgumentInputType::JSON,why))throw std::runtime_error(why);return;}
    throw std::runtime_error("Missing argument "+id);
}
HC::IOutput* output(HC::IModelComponent& c,const std::string& id){for(auto* item:c.outputs())if(item->id()==id)return item;throw std::runtime_error("Missing output "+id);}
Json read(HC::IModelComponent& c,const std::string& id){std::string text,why;const int64_t zero=0,one=1;
    HC::BufferDescriptor b{&text,HC::DataKind::String,1,&one,nullptr,sizeof(text)};
    if(!output(c,id)->getValuesInto(b,{&zero,1},{&one,1},&why))throw std::runtime_error(why);return Json::parse(text);
}
std::string errors(HC::IModelComponent& c){std::string result;for(const auto& e:c.errors())result+=e.message+"; ";return result;}
double day(HC::IModelComponent& c){return dynamic_cast<HC::Temporal::ITimeModelComponent&>(c).currentDateTime()->julianDay();}
using Event=const std::shared_ptr<HC::IComponentStatusChangeEventArgs>&;
struct Observer:HC::ISlot<Event>{
    std::function<void(Event)> call;
    void operator()(const HC::ISignal<Event>&,Event e)override{call(e);}
};
struct Native {
    SWMM_Engine handle=swmm_engine_create();
    ~Native(){swmm_engine_destroy(handle);}
    void check(int code){if(code)throw std::runtime_error(swmm_get_last_error_msg(handle));}
};
class MaterialComponents:public ::testing::Test {
protected:
    Library library;
    fs::path dir;
    void SetUp()override{dir=fs::path(MATERIAL_COMPONENT_EVIDENCE)/::testing::UnitTest::GetInstance()->current_test_info()->name();fs::remove_all(dir);fs::create_directories(dir);}
    fs::path model(const std::string& name="model",const std::string& units="MG/L",const std::string& sign="positive",bool tail=true){
        auto text=bytes(fs::path(MATERIAL_COMPONENT_DATA)/(std::string(tail?"tail-":"")+sign+".inp"));
        replace(text,"Dye MG/L","Dye "+units);
        if(!tail)replace(text,"REPORT_STEP 00:00:01","REPORT_STEP 00:00:00.00244140625");
        replace(text,"REPORT_2D NO",tail?"REPORT_2D YES\nREPORT_2D_STEP 00:00:01":"REPORT_2D YES\nREPORT_2D_STEP 00:00:00.00244140625");
        auto p=dir/(name+".inp");save(p,text);return p;
    }
    std::unique_ptr<HC::IModelComponent> component(const fs::path& p,const std::string& id,bool surface,int capacity=6){
        auto c=library.create();argument(*c,"hydrocouple.instance_id",{{"values",{id}}});
        Json cfg={{"schema","openswmm.component/1"},{"role",surface?"split_material_surface":"split_material_network"},
            {"model",p.string()},{"output_directory",(dir/id).string()},{"material_capacity",capacity}};
        if(surface)cfg["surface_results"]=true;argument(*c,"configuration",cfg);c->initialize();return c;
    }
    void wire(HC::IModelComponent& n,HC::IModelComponent& s){ASSERT_EQ(n.status(),Status::Initialized)<<errors(n);ASSERT_EQ(s.status(),Status::Initialized)<<errors(s);
        ASSERT_EQ(n.inputs().size(),1u);output(s,port)->addConsumer(n.inputs().front());
        ASSERT_TRUE(n.validate().empty())<<errors(n);ASSERT_TRUE(s.validate().empty())<<errors(s);
    }
    void prepare(HC::IModelComponent& n,HC::IModelComponent& s,bool sourceFirst){
        (sourceFirst?s:n).prepare();(sourceFirst?n:s).prepare();ASSERT_EQ(n.status(),Status::Updated)<<errors(n);ASSERT_EQ(s.status(),Status::Updated)<<errors(s);
    }
};
TEST_F(MaterialComponents, FullDurationAllBasesBothSignsAndOrdersMatchNative){
    for(const std::string units:{"MG/L","UG/L","#/L"})for(const std::string sign:{"positive","negative"}){
        const auto tag=(units=="MG/L"?"mg":units=="UG/L"?"ug":"count")+std::string("-")+sign;SCOPED_TRACE(tag);
        const auto p=model(tag,units,sign);const bool reverse=sign=="negative";
        auto n=component(p,tag+"-network",false),s=component(p,tag+"-surface",true);
        ASSERT_NO_FATAL_FAILURE(wire(*n,*s));
        ASSERT_NO_FATAL_FAILURE(prepare(*n,*s,reverse));
        Native ref;ref.check(swmm_engine_open(ref.handle,path_utf8(p).c_str(),nullptr,path_utf8((dir/(tag+"-native.out"))).c_str(),nullptr));
        ref.check(swmm_2d_set_results_file(ref.handle,path_utf8((dir/(tag+"-native.h5"))).c_str()));
        ref.check(swmm_engine_initialize(ref.handle));ref.check(swmm_engine_start(ref.handle,1));ref.check(swmm_passive_exchange_enable(ref.handle,6,0));
        int updates=0;while(n->status()==Status::Updated&&updates++<100){
            const auto before=day(*s);s->update();ASSERT_EQ(day(*s),before);n->update();ASSERT_NE(n->status(),Status::Failed)<<errors(*n);
            double elapsed=0;ref.check(swmm_engine_step(ref.handle,&elapsed));
            const auto ns=read(*n,"material_session"),ss=read(*s,"material_session");
            ASSERT_EQ(ns.at("accepted_time_s"),ss.at("accepted_time_s"));ASSERT_EQ(day(*n),day(*s));
            double actual=0;ref.check(swmm_engine_get_elapsed_seconds(ref.handle,&actual));ASSERT_EQ(ns.at("accepted_time_s"),actual);
        }
        ASSERT_LT(updates,100);ASSERT_EQ(n->status(),Status::Done);ASSERT_EQ(s->status(),Status::Done);
        ref.check(swmm_engine_end(ref.handle));ref.check(swmm_engine_close(ref.handle));
        (reverse?s:n)->finish();(reverse?n:s)->finish();ASSERT_EQ(n->status(),Status::Finished)<<errors(*n);ASSERT_EQ(s->status(),Status::Finished)<<errors(*s);
        const auto ns=read(*n,"material_session"),ss=read(*s,"material_session");
        EXPECT_EQ(ns.at("accepted_boundaries"),80);EXPECT_EQ(ns.at("accepted_time_s"),10);
        EXPECT_EQ(ss.at("observed_offer"),40);EXPECT_EQ(ss.at("observed_proof"),40);EXPECT_EQ(ss.at("observed_ack"),40);
        EXPECT_EQ(ss.at("applied_legs_observed"),6);EXPECT_GT(ss.at("drain_legs_observed").get<int>(),0);EXPECT_GT(ss.at("spill_legs_observed").get<int>(),0);
        EXPECT_EQ(ss.at("empty_windows_observed"),39);EXPECT_EQ(ss.at("zero_capacity_offers_observed"),39);
        EXPECT_TRUE(ss.at("finished").get<bool>());EXPECT_FALSE(ss.at("failed").get<bool>());
        EXPECT_EQ(bytes(dir/(tag+"-native.out")),bytes(dir/(tag+"-network")/"model.out"));
        save(dir/(tag+"-comparison.json"),Json{{"native_h5",(dir/(tag+"-native.h5")).string()},
            {"surface_h5",(dir/(tag+"-surface")/"model.2d.h5").string()},{"network",ns},{"surface",ss}}.dump(2));
    }
}
TEST_F(MaterialComponents, EarlyFinishInBothOrdersFlushesOnlyPendingNativeWindow){
    for(bool sourceFirst:{false,true}){
        const auto tag=std::string(sourceFirst?"source-first":"network-first");auto p=model(tag);auto n=component(p,tag+"-network",false),s=component(p,tag+"-surface",true);
        ASSERT_NO_FATAL_FAILURE(wire(*n,*s));
        ASSERT_NO_FATAL_FAILURE(prepare(*n,*s,sourceFirst));
        Native ref;ref.check(swmm_engine_open(ref.handle,path_utf8(p).c_str(),nullptr,path_utf8((dir/(tag+"-native.out"))).c_str(),nullptr));
        ref.check(swmm_2d_set_results_file(ref.handle,path_utf8((dir/(tag+"-native.h5"))).c_str()));ref.check(swmm_engine_initialize(ref.handle));ref.check(swmm_engine_start(ref.handle,1));ref.check(swmm_passive_exchange_enable(ref.handle,6,0));
        for(int i=0;i<3;++i){n->update();ASSERT_EQ(n->status(),Status::Updated)<<errors(*n);double elapsed=0;ref.check(swmm_engine_step(ref.handle,&elapsed));}
        (sourceFirst?s:n)->finish();(sourceFirst?n:s)->finish();ref.check(swmm_engine_end(ref.handle));ref.check(swmm_engine_close(ref.handle));
        ASSERT_EQ(n->status(),Status::Finished)<<errors(*n);ASSERT_EQ(s->status(),Status::Finished)<<errors(*s);
        const auto state=read(*s,"material_session");EXPECT_EQ(state.at("accepted_time_s"),.375);EXPECT_EQ(state.at("observed_offer"),2);EXPECT_EQ(state.at("finish_observed"),1);
        EXPECT_EQ(bytes(dir/(tag+"-native.out")),bytes(dir/(tag+"-network")/"model.out"));
        save(dir/(tag+"-comparison.json"),Json{{"native_h5",(dir/(tag+"-native.h5")).string()},{"surface_h5",(dir/(tag+"-surface")/"model.2d.h5").string()}}.dump(2));
    }
}
TEST_F(MaterialComponents, ShortSlicesAndPendingReportWindowsMatchNative){
    for(const std::string units:{"MG/L","UG/L","#/L"})for(const std::string sign:{"positive","negative"})for(int steps:{5,8}){
        const auto tag=(units=="MG/L"?"mg":units=="UG/L"?"ug":"count")+std::string("-")+sign+"-"+std::to_string(steps);SCOPED_TRACE(tag);
        const auto p=model(tag,units,sign,false);
        if(steps==8){auto text=bytes(p);replace(text,"REPORT_STEP 00:00:00.00244140625","REPORT_STEP 00:00:01");replace(text,"REPORT_2D_STEP 00:00:00.00244140625","REPORT_2D_STEP 00:00:01");save(p,text);}
        auto n=component(p,tag+"-network",false,64),s=component(p,tag+"-surface",true,64);
        ASSERT_NO_FATAL_FAILURE(wire(*n,*s));
        ASSERT_NO_FATAL_FAILURE(prepare(*n,*s,steps==5));
        Native ref;ref.check(swmm_engine_open(ref.handle,path_utf8(p).c_str(),nullptr,path_utf8((dir/(tag+"-native.out"))).c_str(),nullptr));
        ref.check(swmm_2d_set_results_file(ref.handle,path_utf8((dir/(tag+"-native.h5"))).c_str()));ref.check(swmm_engine_initialize(ref.handle));ref.check(swmm_engine_start(ref.handle,1));ref.check(swmm_passive_exchange_enable(ref.handle,64,0));
        for(int i=0;i<steps;++i){n->update();ASSERT_EQ(n->status(),Status::Updated)<<errors(*n);double elapsed=0;ref.check(swmm_engine_step(ref.handle,&elapsed));EXPECT_EQ(day(*n),day(*s));}
        (steps==5?s:n)->finish();(steps==5?n:s)->finish();ref.check(swmm_engine_end(ref.handle));ref.check(swmm_engine_close(ref.handle));
        ASSERT_EQ(n->status(),Status::Finished)<<errors(*n);ASSERT_EQ(s->status(),Status::Finished)<<errors(*s);
        const auto state=read(*s,"material_session");if(steps==5)EXPECT_GT(state.at("report_observed").get<int>(),0);EXPECT_EQ(state.at("finish_observed"),1);
        EXPECT_EQ(state.at("observed_offer"),state.at("observed_ack"));EXPECT_EQ(state.at("observed_proof"),state.at("observed_ack"));
        EXPECT_EQ(bytes(dir/(tag+"-native.out")),bytes(dir/(tag+"-network")/"model.out"));
        save(dir/(tag+"-comparison.json"),Json{{"native_h5",(dir/(tag+"-native.h5")).string()},{"surface_h5",(dir/(tag+"-surface")/"model.2d.h5").string()},{"surface",state}}.dump(2));
    }
}
TEST_F(MaterialComponents, NativeArithmeticRefusalRetainsTheLastAcceptedComponentClock){
    auto p=model("report-refusal","MG/L","positive",false);auto n=component(p,"network",false,64),s=component(p,"surface",true,64);
    ASSERT_NO_FATAL_FAILURE(wire(*n,*s));
    ASSERT_NO_FATAL_FAILURE(prepare(*n,*s,false));
    Native ref;ref.check(swmm_engine_open(ref.handle,path_utf8(p).c_str(),nullptr,path_utf8((dir/"native.out")).c_str(),nullptr));ref.check(swmm_2d_set_results_file(ref.handle,path_utf8((dir/"native.h5")).c_str()));ref.check(swmm_engine_initialize(ref.handle));ref.check(swmm_engine_start(ref.handle,1));ref.check(swmm_passive_exchange_enable(ref.handle,64,0));
    for(int i=0;i<6;++i){double elapsed=0;const int rc=swmm_engine_step(ref.handle,&elapsed);n->update();
        if(i<5){ASSERT_EQ(rc,SWMM_OK);ASSERT_EQ(n->status(),Status::Updated)<<errors(*n);}
        else {EXPECT_NE(rc,SWMM_OK);EXPECT_EQ(n->status(),Status::Failed);EXPECT_NE(std::string(swmm_get_last_error_msg(ref.handle)).find("not an exact nonvanishing partition"),std::string::npos);}
    }
    const auto ns=read(*n,"material_session"),ss=read(*s,"material_session");
    EXPECT_EQ(ns.at("accepted_boundaries"),5);EXPECT_EQ(ns.at("accepted_time_s"),5./2048);EXPECT_EQ(ns.at("accepted_time_s"),ss.at("accepted_time_s"));EXPECT_EQ(day(*n),day(*s));
    EXPECT_TRUE(ns.at("failed").get<bool>());EXPECT_TRUE(ss.at("failed").get<bool>());n->finish();s->finish();
    save(dir/"native-refusal.json",Json{{"native_error",swmm_get_last_error_msg(ref.handle)},{"network",ns},{"surface",ss}}.dump(2));
}
TEST_F(MaterialComponents, FrozenConnectionsAndUnsolicitedPullsCannotAdvance){
    auto p=model();auto n=component(p,"network",false),s=component(p,"surface",true),other=component(p,"other",true);
    ASSERT_NO_FATAL_FAILURE(wire(*n,*s));auto* in=n->inputs().front();auto* out=output(*s,port);auto* alternative=output(*other,port);
    EXPECT_FALSE(in->setProvider(alternative));EXPECT_EQ(in->provider(),out);EXPECT_EQ(out->consumers().size(),1u);EXPECT_TRUE(alternative->consumers().empty());
    ASSERT_NO_FATAL_FAILURE(prepare(*n,*s,false));const auto original=read(*s,"material_session");
    EXPECT_FALSE(in->setProvider(nullptr));EXPECT_FALSE(out->removeConsumer(in));EXPECT_THROW(out->updateValues(in),std::exception);
    EXPECT_EQ(read(*s,"material_session"),original);n->update();ASSERT_EQ(n->status(),Status::Updated)<<errors(*n);
    s->finish();n->finish();EXPECT_EQ(n->status(),Status::Finished);EXPECT_EQ(s->status(),Status::Finished);
}
TEST_F(MaterialComponents, StatusObserversCannotReenterLifecycleOrPullPhases){
    auto p=model();auto n=component(p,"network",false),s=component(p,"surface",true);
        ASSERT_NO_FATAL_FAILURE(wire(*n,*s));
    int calls=0,pulls=0;auto observer=std::make_shared<Observer>();observer->call=[&](Event){
        ++calls;const auto ns=n->status(),ss=s->status();n->initialize();n->prepare();n->update();n->finish();(void)n->validate();
        s->initialize();s->prepare();s->update();s->finish();(void)s->validate();
        EXPECT_EQ(n->status(),ns);EXPECT_EQ(s->status(),ss);
        try{output(*s,port)->updateValues(n->inputs().front());}catch(const std::exception&){++pulls;}
    };n->connect(observer);s->connect(observer);
    ASSERT_NO_FATAL_FAILURE(prepare(*n,*s,false));n->update();ASSERT_EQ(n->status(),Status::Updated)<<errors(*n);
    n->finish();s->finish();EXPECT_EQ(n->status(),Status::Finished);EXPECT_EQ(s->status(),Status::Finished);EXPECT_GT(calls,8);EXPECT_EQ(calls,pulls);
}
TEST_F(MaterialComponents, InitializingObserverCannotReplaceComponent){
    auto p=model();auto n=library.create();argument(*n,"configuration",{{"schema","openswmm.component/1"},{"role","split_material_network"},{"model",p.string()},{"output_directory",(dir/"network").string()}});
    int calls=0;auto observer=std::make_shared<Observer>();observer->call=[&](Event){++calls;auto state=n->status();n->initialize();n->finish();n->prepare();n->update();(void)n->validate();EXPECT_EQ(n->status(),state);};n->connect(observer);
    n->initialize();EXPECT_EQ(n->status(),Status::Initialized)<<errors(*n);EXPECT_EQ(calls,2);EXPECT_FALSE(fs::exists(dir/"network"));
}
TEST_F(MaterialComponents, CapacityAndBasisMismatchesFailBeforeAcceptedAdvance){
    for(bool basis:{false,true}){
        auto p=model(basis?"basis":"capacity");auto q=basis?model("wrong-basis","UG/L"):p;
        const auto tag=std::string(basis?"basis":"capacity");auto n=component(p,tag+"-network",false),s=component(q,tag+"-surface",true,basis?6:7);
        ASSERT_NO_FATAL_FAILURE(wire(*n,*s));
        ASSERT_NO_FATAL_FAILURE(prepare(*n,*s,false));n->update();
        EXPECT_EQ(n->status(),Status::Failed)<<errors(*n);const auto ns=read(*n,"material_session"),ss=read(*s,"material_session");
        EXPECT_TRUE(ns.at("failed").get<bool>());EXPECT_TRUE(ss.at("failed").get<bool>());EXPECT_EQ(ns.at("accepted_boundaries"),0);EXPECT_EQ(ss.at("accepted_time_s"),0);
        n->finish();s->finish();
    }
}
TEST_F(MaterialComponents, FailedAndInvalidObserversCannotChangeTheOperation){
    auto p=model();auto n=component(p,"network",false),s=component(p,"surface",true,7);
    ASSERT_NO_FATAL_FAILURE(wire(*n,*s));
    int failed=0;auto observer=std::make_shared<Observer>();observer->call=[&](Event){
        if(n->status()!=Status::Failed)return;++failed;const auto time=day(*n);n->initialize();n->prepare();n->update();n->finish();(void)n->validate();
        s->finish();s->initialize();s->update();EXPECT_EQ(n->status(),Status::Failed);EXPECT_EQ(day(*n),time);
    };n->connect(observer);
    ASSERT_NO_FATAL_FAILURE(prepare(*n,*s,false));n->update();EXPECT_EQ(n->status(),Status::Failed);EXPECT_EQ(failed,1);n->finish();s->finish();
    auto unconnected=component(p,"unconnected",false);int invalid=0;auto onInvalid=std::make_shared<Observer>();onInvalid->call=[&](Event){
        if(unconnected->status()!=Status::Invalid)return;++invalid;unconnected->initialize();unconnected->prepare();unconnected->finish();(void)unconnected->validate();EXPECT_EQ(unconnected->status(),Status::Invalid);
    };unconnected->connect(onInvalid);EXPECT_FALSE(unconnected->validate().empty());EXPECT_EQ(invalid,1);EXPECT_FALSE(fs::exists(dir/"unconnected"));
}
TEST_F(MaterialComponents, ExpiredPeersAreRefusedWithoutDereferencingOldPorts){
    auto p=model();auto n=component(p,"network",false),s=component(p,"surface",true);
        ASSERT_NO_FATAL_FAILURE(wire(*n,*s));
    auto* stale=n->inputs().front();n.reset();EXPECT_FALSE(s->validate().empty());EXPECT_TRUE(output(*s,port)->removeConsumer(stale));
    auto n2=component(p,"network2",false),s2=component(p,"surface2",true);
        ASSERT_NO_FATAL_FAILURE(wire(*n2,*s2));
        ASSERT_NO_FATAL_FAILURE(prepare(*n2,*s2,false));
    s2.reset();n2->update();EXPECT_EQ(n2->status(),Status::Failed);EXPECT_EQ(read(*n2,"material_session").at("accepted_boundaries"),0);n2->finish();
}
TEST_F(MaterialComponents, MetadataValidationIsReadOnlyAndUsesNativeEligibility){
    auto p=model();const auto original=bytes(p);Native e;e.check(swmm_engine_open_metadata(e.handle,path_utf8(p).c_str()));
    EXPECT_EQ(swmm_split_material_validate_input(e.handle),SWMM_OK);EXPECT_EQ(swmm_split_material_validate_input(e.handle),SWMM_OK);EXPECT_EQ(bytes(p),original);
    auto n=component(p,"network",false);EXPECT_EQ(n->status(),Status::Initialized);EXPECT_FALSE(fs::exists(dir/"network"));
    EXPECT_FALSE(n->capabilities().contains(HC::Capability::Checkpointing));
    EXPECT_EQ(dynamic_cast<HC::ICheckpointableModelComponent*>(n.get()),nullptr);
    auto s=component(p,"surface",true);EXPECT_FALSE(s->capabilities().contains(HC::Capability::Checkpointing));
    EXPECT_EQ(dynamic_cast<HC::ICheckpointableModelComponent*>(s.get()),nullptr);
    auto bad=original;replace(bad,"IGNORE_QUALITY NO","IGNORE_QUALITY YES");auto q=dir/"unsupported.inp";save(q,bad);auto refused=component(q,"refused",false);
    EXPECT_EQ(refused->status(),Status::Initialized);EXPECT_FALSE(refused->validate().empty());EXPECT_FALSE(fs::exists(dir/"refused"));
}
} // namespace
