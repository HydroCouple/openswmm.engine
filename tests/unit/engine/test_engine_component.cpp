// SPDX-License-Identifier: Apache-2.0
// Native/direct-component parity and real receiver acceptance through the ABI.
#include <gtest/gtest.h>
#include <hydrocouplecomponentabi.h>
#include <hydrocoupletemporal.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_output.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cmath>
#include <map>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
namespace HC=HydroCouple;
namespace fs=std::filesystem;
using Json=nlohmann::json;
using Status=HC::IModelComponent::ComponentStatus;
namespace {
class Library {
public:
    explicit Library(const char* path) {
#ifdef _WIN32
        handle_=LoadLibraryA(path);
#else
        handle_=dlopen(path,RTLD_NOW|RTLD_LOCAL);
#endif
    }
    ~Library() {
        if(!handle_)return;
#ifdef _WIN32
        FreeLibrary(handle_);
#else
        dlclose(handle_);
#endif
    }
    void* symbol(const char* name) const {
#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(handle_,name));
#else
        return handle_?dlsym(handle_,name):nullptr;
#endif
    }
    std::unique_ptr<HC::IModelComponent> create() const {
        auto abi=reinterpret_cast<HydroCoupleComponentAbiFn>(symbol(HYDROCOUPLE_COMPONENT_ABI_SYMBOL));
        if(!abi || std::string(abi())!=HYDROCOUPLE_COMPONENT_ABI_STAMP)return {};
        auto entry=reinterpret_cast<HydroCoupleComponentInfoFn>(symbol(HYDROCOUPLE_COMPONENT_INFO_SYMBOL));
        auto* info=entry?dynamic_cast<HC::IModelComponentInfo*>(entry()):nullptr;
        return info?info->createComponentInstance():nullptr;
    }
private:
#ifdef _WIN32
    HMODULE handle_=nullptr;
#else
    void* handle_=nullptr;
#endif
};
struct NativeEngine {
    SWMM_Engine value=swmm_engine_create();
    ~NativeEngine(){swmm_engine_destroy(value);}
};
std::string bytes(const fs::path& path) {
    std::ifstream file(path,std::ios::binary);EXPECT_TRUE(file.good())<<path;
    return {std::istreambuf_iterator<char>(file),{}};
}
HC::IOutput* output(HC::IModelComponent& component,const std::string& id) {
    for(auto* item:component.outputs())if(item->id()==id)return item;
    ADD_FAILURE()<<"Missing output "<<id;return nullptr;
}
template<class T> T read(HC::IOutput* item) {
    T value{};const int64_t one=1,zero=0;std::string message;
    HC::BufferDescriptor buffer{&value,std::is_same_v<T,double>?HC::DataKind::Float64:HC::DataKind::String,1,&one,nullptr,sizeof(T)};
    EXPECT_TRUE(item->getValuesInto(buffer,{&zero,1},{&one,1},&message))<<message;return value;
}
void argument(HC::IModelComponent& component,const std::string& id,const Json& value) {
    for(auto* item:component.arguments())if(item->id()==id) {
        std::string message;ASSERT_TRUE(item->initialize(value.dump(),HC::IArgument::ArgumentInputType::JSON,message))<<message;return;
    }
    FAIL()<<"Missing argument "<<id;
}
std::string diagnostics(HC::IModelComponent& component) {
    std::string message;for(const auto& error:component.errors())message+=error.message+"; ";return message;
}
class EngineComponent:public ::testing::Test {
protected:
    Library library{ENGINE_COMPONENT_LIBRARY};
    std::unique_ptr<HC::IModelComponent> component;
    fs::path directory,model;
    Json configuration;
    void SetUp() override {
        directory=fs::absolute(fs::path("component_engine")/::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::create_directories(directory);fs::remove_all(directory/"wrapped");
        model=directory/"model.inp";fs::copy_file("component_metadata/reference.inp",model,fs::copy_options::overwrite_existing);
        component=library.create();ASSERT_NE(component,nullptr);
        EXPECT_EQ(component->componentInfo()->id(),"org.hydrocouple.openswmm.engine");
        component->setReferenceDirectory(directory.string());
        argument(*component,"hydrocouple.instance_id",{{"values",{"network"}}});
        configuration={{"schema","openswmm.component/1"},{"model","model.inp"},{"role","complete_model"},{"output_directory","wrapped"}};
    }
    void initialize(bool exports=false) {
        if(exports) { configuration["outfalls"]={"O"};configuration["exchange_times_s"]={60,120,600}; }
        argument(*component,"configuration",configuration);component->initialize();
        ASSERT_EQ(component->status(),Status::Initialized)<<diagnostics(*component);
    }
    void ready(bool exports=false) {
        initialize(exports);ASSERT_TRUE(component->validate().empty())<<diagnostics(*component);
        component->prepare();ASSERT_EQ(component->status(),Status::Updated)<<diagnostics(*component);
    }
    void nativeStart(NativeEngine& native) {
        fs::create_directories(directory/"native");
        ASSERT_EQ(swmm_engine_open(native.value,model.string().c_str(),(directory/"native/model.rpt").string().c_str(),(directory/"native/model.out").string().c_str(),nullptr),0);
        ASSERT_EQ(swmm_engine_initialize(native.value),0);ASSERT_EQ(swmm_engine_start(native.value,1),0);
    }
    void reaction() {
        // Wet link level pool: native TSS remains at Cinit=100 and X=2*TSS
        // is observable in the pipe. A zero-volume junction need not react.
        auto text=bytes(model);
        auto replace=[&](const std::string& from,const std::string& to){const auto at=text.find(from);ASSERT_NE(at,std::string::npos);text.replace(at,from.size(),to);};
        replace("FLOW_ROUTING KINWAVE","FLOW_ROUTING DYNWAVE");
        replace("J 10 10 0 0 0","J 10 10 1.5 0 0");
        replace("O 9 FREE NO","O 10 FIXED 11.5 NO");
        replace("[DWF]\nJ FLOW 1\nJ A 100\nJ B 100000\n","");
        replace("[POLLUTANTS]\nA ","[POLLUTANTS]\nTSS ");
        std::ofstream(model)<<text;
        std::ofstream(directory/"model.rxn")<<"[REACTION_SPECIES]\nBULK X MG\nWALL W MG\n[REACTION_PIPES]\nFORMULA X 2*TSS\n[REACTION_TANKS]\nFORMULA X 2*TSS\n";
        std::ofstream(model,std::ios::app)<<"\n[PROCESS_COMPONENTS]\norg.hydrocouple.openswmm.reactions config=\"model.rxn\"\n";
    }
    void runUncoupledAndCompare() {
        NativeEngine native;nativeStart(native);ready();
        double elapsed=0;int guard=0;
        do { ASSERT_EQ(swmm_engine_step(native.value,&elapsed),0); }while(elapsed>0 && ++guard<10000);
        ASSERT_LT(guard,10000);ASSERT_EQ(swmm_engine_end(native.value),0);ASSERT_EQ(swmm_engine_report(native.value),0);ASSERT_EQ(swmm_engine_close(native.value),0);
        guard=0;while(component->status()==Status::Updated && ++guard<10000)component->update();
        ASSERT_EQ(component->status(),Status::Done)<<diagnostics(*component);component->finish();
        ASSERT_EQ(component->status(),Status::Finished)<<diagnostics(*component);
        EXPECT_EQ(bytes(directory/"native/model.out"),bytes(directory/"wrapped/model.out"));
    }
};
}
TEST_F(EngineComponent, ConfiguredMetadataDoesNotCreateRunOutputs) {
    initialize(true);EXPECT_FALSE(fs::exists(directory/"wrapped"));
    const auto metadata=Json::parse(read<std::string>(output(*component,"model_metadata")));
    EXPECT_TRUE(metadata.at("runnable"));EXPECT_EQ(metadata.at("nodes").size(),2u);EXPECT_EQ(metadata.at("links").size(),1u);
    EXPECT_EQ(metadata.at("pollutants")[1]["concentration_units"],"ug/L");
    EXPECT_EQ(metadata.at("instance_id"),"network");EXPECT_EQ(component->outputs().size(),17u);
    auto* quantity=dynamic_cast<HC::IQuantity*>(output(*component,"epoch_julian_day")->valueDefinition());
    ASSERT_NE(quantity,nullptr);EXPECT_EQ(quantity->valueKind(),HC::ValueKind::Intensive);EXPECT_DOUBLE_EQ(quantity->unit()->conversionFactorToSI(),86400);
    EXPECT_FALSE(component->capabilities().contains(HC::Capability::Checkpointing));
}
TEST_F(EngineComponent, AuthoredExtraOutputIsReportedAndRefusedBeforeExecution) {
    std::ofstream(model,std::ios::app)<<"\n[FILES]\nSAVE OUTFLOWS \"extra-output.iface\"\n";
    initialize();const auto metadata=Json::parse(read<std::string>(output(*component,"model_metadata")));
    EXPECT_FALSE(metadata.at("runnable"));EXPECT_FALSE(component->validate().empty());
    component->prepare();EXPECT_EQ(component->status(),Status::Failed);
    EXPECT_FALSE(fs::exists(directory/"wrapped"));EXPECT_FALSE(fs::exists(directory/"extra-output.iface"));
}
TEST_F(EngineComponent, UncoupledNativeAndWrappedOutputAreByteIdentical) { runUncoupledAndCompare(); }
TEST_F(EngineComponent, QualifiedNetworkRolePreservesNativeOutput) {
    configuration["role"]="network_only";runUncoupledAndCompare();
}
TEST_F(EngineComponent, NativeMsxSpeciesRemainPollutantsInWrappedOutFiles) {
    reaction();runUncoupledAndCompare();
    auto handle=swmm_output_open((directory/"wrapped/model.out").string().c_str());ASSERT_NE(handle,nullptr);
    const int pollutants=swmm_output_get_pollut_count(handle);
    EXPECT_EQ(pollutants,5); // TSS, B, native temperature and two reaction species
    EXPECT_STREQ(swmm_output_get_pollut_id(handle,0),"TSS");EXPECT_STREQ(swmm_output_get_pollut_id(handle,1),"B");
    int x=-1,w=-1;
    for(int i=0;i<pollutants;++i) {
        const std::string name=swmm_output_get_pollut_id(handle,i);
        if(name=="X")x=i;if(name=="W")w=i;
    }
    ASSERT_GT(x,1);EXPECT_EQ(w,x+1);EXPECT_EQ(w,pollutants-1);
    const int period=swmm_output_get_period_count(handle)-1;ASSERT_GE(period,0);
    float values[32]{};int count=0;ASSERT_EQ(swmm_output_get_node_attribute(handle,0,period,values,&count),0);
    EXPECT_GT(count,6+x);EXPECT_EQ(values[6+x],0); // zero-volume junction is not a tank reactor
    ASSERT_EQ(swmm_output_get_link_attribute(handle,0,period,values,&count),0);
    EXPECT_GT(count,5+x);EXPECT_NEAR(values[5+x],200.0f,1e-3f);swmm_output_close(handle);
    const auto metadata=Json::parse(read<std::string>(output(*component,"model_metadata")));
    EXPECT_EQ(metadata.at("reaction_species")[0]["id"],"X");EXPECT_EQ(metadata.at("reaction_species")[1]["kind"],"WALL");
}
TEST_F(EngineComponent, TypedIntervalsMatchAuthoritativeNativeAmounts) {
    NativeEngine native;nativeStart(native);ready(true);SWMM_OutfallExportReceipt previous{},receipt{};double oldMass[2]{},mass[2]{};
    for(double time:{60.,120.,600.}) {
        double actual=0;ASSERT_EQ(swmm_engine_advance_to(native.value,time,&actual),0);
        ASSERT_EQ(swmm_node_get_export_receipt(native.value,swmm_node_index(native.value,"O"),&receipt,mass,2),0);
        component->update();ASSERT_NE(component->status(),Status::Failed)<<diagnostics(*component);
        EXPECT_DOUBLE_EQ(read<double>(output(*component,"water_m3")),receipt.exported_m3-previous.exported_m3);
        EXPECT_DOUBLE_EQ(read<double>(output(*component,"enthalpy_j")),receipt.exported_heat_j-previous.exported_heat_j);
        EXPECT_DOUBLE_EQ(read<double>(output(*component,"species.A")),mass[0]-oldMass[0]);
        EXPECT_DOUBLE_EQ(read<double>(output(*component,"species.B")),mass[1]-oldMass[1]);
        previous=receipt;std::copy(mass,mass+2,oldMass);
    }
    ASSERT_EQ(component->status(),Status::Done);component->finish();
    ASSERT_EQ(component->status(),Status::Finished)<<diagnostics(*component);
    ASSERT_EQ(swmm_engine_end(native.value),0);ASSERT_EQ(swmm_engine_report(native.value),0);ASSERT_EQ(swmm_engine_close(native.value),0);
    EXPECT_EQ(bytes(directory/"native/model.out"),bytes(directory/"wrapped/model.out"));
}
TEST_F(EngineComponent, MsxExportIsDiagnosedBeforeNativeAdvancement) {
    reaction();initialize(true);EXPECT_FALSE(component->validate().empty());EXPECT_EQ(component->status(),Status::Invalid);
    EXPECT_FALSE(fs::exists(directory/"wrapped"));
}
TEST_F(EngineComponent, ChangedModelIsRejectedBeforeOutputCreation) {
    initialize();ASSERT_TRUE(component->validate().empty());std::ofstream(model,std::ios::app)<<"\n; changed after configuration\n";
    component->prepare();EXPECT_EQ(component->status(),Status::Failed);EXPECT_FALSE(fs::exists(directory/"wrapped"));
}
TEST_F(EngineComponent, ChangedReactionConfigurationIsRejectedBeforeOutputCreation) {
    reaction();initialize();ASSERT_TRUE(component->validate().empty());
    std::ofstream(directory/"model.rxn",std::ios::app)<<"\n[REACTION_SPECIES]\nBULK CHANGED MG\n";
    component->prepare();EXPECT_EQ(component->status(),Status::Failed);
    EXPECT_FALSE(fs::exists(directory/"wrapped"));
    EXPECT_NE(diagnostics(*component).find("Process configuration changed"),std::string::npos);
}
TEST_F(EngineComponent, ExistingOutputDirectoryIsPreserved) {
    ready();component->finish();const auto before=bytes(directory/"wrapped/model.out");
    auto other=library.create();other->setReferenceDirectory(directory.string());argument(*other,"configuration",configuration);
    other->initialize();ASSERT_TRUE(other->validate().empty());other->prepare();EXPECT_EQ(other->status(),Status::Failed);
    EXPECT_EQ(bytes(directory/"wrapped/model.out"),before);
}
TEST_F(EngineComponent, SeparateInstancesKeepIndependentRuntimeAndOutputPaths) {
    ready();component->update();
    auto other=library.create();other->setReferenceDirectory(directory.string());fs::remove_all(directory/"second");
    auto config=configuration;config["output_directory"]="second";
    argument(*other,"hydrocouple.instance_id",{{"values",{"second-network"}}});argument(*other,"configuration",config);
    other->initialize();ASSERT_TRUE(other->validate().empty());other->prepare();ASSERT_EQ(other->status(),Status::Updated);
    auto* firstTime=dynamic_cast<HC::Temporal::ITimeModelComponent*>(component.get());
    auto* secondTime=dynamic_cast<HC::Temporal::ITimeModelComponent*>(other.get());ASSERT_NE(firstTime,nullptr);ASSERT_NE(secondTime,nullptr);
    EXPECT_GT(firstTime->currentDateTime()->julianDay(),secondTime->currentDateTime()->julianDay());
    other->update();EXPECT_DOUBLE_EQ(firstTime->currentDateTime()->julianDay(),secondTime->currentDateTime()->julianDay());
    EXPECT_EQ(other->id(),"second-network");
}
#ifdef ENGINE_RECEIVER_LIBRARY
namespace {
class EngineReceiver:public EngineComponent {
protected:
    Library receiverLibrary{ENGINE_RECEIVER_LIBRARY};
    std::unique_ptr<HC::IModelComponent> receiver;
    void connectReceiver(double capacity=-1) {
        initialize(true);receiver=receiverLibrary.create();ASSERT_NE(receiver,nullptr);
        Json config={{"schema","hydrocouple.interval-transfer/1"},{"epoch_julian_day",dynamic_cast<HC::Temporal::ITimeModelComponent*>(component.get())->simulationPeriod()->julianDay()},
            {"species",Json::array({{{"id","B"},{"basis","kg"}},{{"id","A"},{"basis","kg"}}})},
            {"expected_transfers",3},{"receipt_file",(directory/"receipts.csv").string()}};
        if(capacity>=0)config["max_water_m3"]=capacity;
        argument(*receiver,"config",config);receiver->initialize();ASSERT_EQ(receiver->status(),Status::Initialized);
        for(auto* input:receiver->inputs()) {
            auto* provider=output(*component,input->id());ASSERT_NE(provider,nullptr);std::string message;
            ASSERT_TRUE(input->canConsume(provider,message))<<input->id()<<": "<<message;provider->addConsumer(input);
        }
        ASSERT_TRUE(component->validate().empty())<<diagnostics(*component);ASSERT_TRUE(receiver->validate().empty())<<diagnostics(*receiver);
        component->prepare();ASSERT_EQ(component->status(),Status::Updated)<<diagnostics(*component);
        receiver->prepare();ASSERT_EQ(receiver->status(),Status::Updated)<<diagnostics(*receiver);
    }
};
}
TEST_F(EngineReceiver, RealReceiverClosesNativeWaterMassAndHeatWithoutDoubleDebit) {
    connectReceiver();
    for(int step=0;step<3;++step) {
        component->update();ASSERT_NE(component->status(),Status::Failed)<<diagnostics(*component);
        receiver->update();ASSERT_NE(receiver->status(),Status::Failed)<<diagnostics(*receiver);
    }
    ASSERT_EQ(component->status(),Status::Done);ASSERT_EQ(receiver->status(),Status::Done);
    component->finish();ASSERT_EQ(component->status(),Status::Finished)<<diagnostics(*component);
    for(const auto& field:{"water_m3","enthalpy_j","species.A","species.B"}) {
        const auto exported=read<double>(output(*component,std::string("exported.")+field));
        EXPECT_GT(exported,0);EXPECT_DOUBLE_EQ(exported,read<double>(output(*receiver,std::string("cumulative.")+field)));
        EXPECT_DOUBLE_EQ(read<double>(output(*component,std::string("unreceived.")+field)),0);
    }
    receiver->finish();
}
TEST_F(EngineReceiver, MissingReceiptStopsBeforeAnotherNativeAdvance) {
    connectReceiver();component->update();const auto token=read<std::string>(output(*component,"transfer_id"));
    component->update();EXPECT_EQ(component->status(),Status::Failed);EXPECT_EQ(read<std::string>(output(*component,"transfer_id")),token);
    EXPECT_FALSE(fs::exists(directory/"wrapped/native_boundary_1.json"));
}
TEST_F(EngineReceiver, RefusedAlreadyDebitedExportRetainsUnreceivedInventoryAndStops) {
    connectReceiver(0);component->update();receiver->update();ASSERT_EQ(receiver->status(),Status::Updated);
    component->update();EXPECT_EQ(component->status(),Status::Failed)<<diagnostics(*component);
    EXPECT_FALSE(fs::exists(directory/"wrapped/native_boundary_1.json"));
    for(const auto& field:{"water_m3","enthalpy_j","species.A","species.B"})
        EXPECT_DOUBLE_EQ(read<double>(output(*component,std::string("exported.")+field)),
                        read<double>(output(*receiver,std::string("cumulative.")+field))+read<double>(output(*component,std::string("unreceived.")+field)));
}
TEST_F(EngineReceiver, PartialAcceptanceRecordsBothAcceptedAndUnreceivedAmounts) {
    connectReceiver(.1);component->update();receiver->update();
    const double accepted=read<double>(output(*receiver,"accepted.water_m3"));
    EXPECT_GT(accepted,0);EXPECT_LT(accepted,read<double>(output(*component,"water_m3")));
    component->update();ASSERT_EQ(component->status(),Status::Failed);
    EXPECT_NE(diagnostics(*component).find("refused part"),std::string::npos);
    EXPECT_FALSE(fs::exists(directory/"wrapped/native_boundary_1.json"));
    const auto ledger=Json::parse(bytes(directory/"wrapped/accepted_0.json"));
    for(const auto& field:{"water_m3","enthalpy_j","species.A","species.B"}) {
        const double received=read<double>(output(*receiver,std::string("accepted.")+field));
        const double remaining=read<double>(output(*component,std::string("unreceived.")+field));
        EXPECT_GT(remaining,0);
        EXPECT_DOUBLE_EQ(read<double>(output(*component,std::string("exported.")+field)),received+remaining);
    }
    EXPECT_DOUBLE_EQ(ledger["accepted_amounts"][0].get<double>(),accepted);
    EXPECT_GT(ledger["unreceived_amounts"][0].get<double>(),0);
}
#endif
