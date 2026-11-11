// SPDX-License-Identifier: Apache-2.0
// Fresh executable host of the actual factory/module optional service; no SDK.
#include <hydrocouplecomponentabi.h>
#include <hydrocoupletemporal.h>
#include <openswmm/components/openswmm_component_recovery.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_passive_exchange.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <functional>
#include <cmath>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
namespace HC=HydroCouple;
namespace fs=std::filesystem;
using Json=nlohmann::json;
using Status=HC::IModelComponent::ComponentStatus;
const OpenSWMM_MaterialRecoveryV1* api=nullptr;
int journalCapacity=64;
const OpenSWMM_MaterialRecoveryV2* observationApi=nullptr;
void require(bool ok,const char* expr,int line){if(!ok)throw std::runtime_error(std::string("line ")+std::to_string(line)+": "+expr+"; "+(api?api->last_error():"no service"));}
#define CHECK(x) require(bool(x),#x,__LINE__)
std::string bytes(const fs::path& p){std::ifstream f(p,std::ios::binary);CHECK(f.good());return {std::istreambuf_iterator<char>(f),{}};}
void save(const fs::path& p,const std::string& s){std::ofstream f(p,std::ios::binary);f.write(s.data(),s.size());CHECK(f.good());}
std::string path(const fs::path& p){const auto text=p.u8string();return {text.begin(),text.end()};}
class Library {
#ifdef _WIN32
    HMODULE handle_=LoadLibraryA(ENGINE_COMPONENT_LIBRARY);
    void* symbol(const char* s){return reinterpret_cast<void*>(GetProcAddress(handle_,s));}
#else
    void* handle_=dlopen(ENGINE_COMPONENT_LIBRARY,RTLD_NOW|RTLD_LOCAL);
    void* symbol(const char* s){return handle_?dlsym(handle_,s):nullptr;}
#endif
public:
    Library(){
        CHECK(handle_);auto abi=reinterpret_cast<HydroCoupleComponentAbiFn>(symbol(HYDROCOUPLE_COMPONENT_ABI_SYMBOL));
        CHECK(abi && std::string(abi())==HYDROCOUPLE_COMPONENT_ABI_STAMP);
        auto service=reinterpret_cast<OpenSWMM_MaterialRecoveryFn>(symbol(OPENSWMM_COMPONENT_RECOVERY_SYMBOL));CHECK(service);
        CHECK(!service(0,sizeof(*api)));CHECK(!service(2,sizeof(*api)));CHECK(!service(1,sizeof(*api)-1));
        observationApi=reinterpret_cast<const OpenSWMM_MaterialRecoveryV2*>(service(2,sizeof(OpenSWMM_MaterialRecoveryV2)));
        CHECK(observationApi && observationApi->base.version==2 && observationApi->base.struct_size==sizeof(*observationApi));
        CHECK(!service(3,sizeof(*observationApi)));
        api=service(1,sizeof(*api));CHECK(api && api->version==1 && api->struct_size==sizeof(*api));
        auto g=reinterpret_cast<OpenSWMM_MaterialGroup>(uintptr_t(1));auto state=reinterpret_cast<OpenSWMM_MaterialState>(uintptr_t(2));
        const void* data=reinterpret_cast<void*>(uintptr_t(3));const char* text=reinterpret_cast<const char*>(uintptr_t(4));size_t size=555;
        CHECK(api->enroll(nullptr,nullptr,0,&g)!=0 && g==reinterpret_cast<OpenSWMM_MaterialGroup>(uintptr_t(1)));
        CHECK(api->capture(nullptr,&state)!=0 && state==reinterpret_cast<OpenSWMM_MaterialState>(uintptr_t(2)));
        CHECK(api->import_state(nullptr,1,&state)!=0 && state==reinterpret_cast<OpenSWMM_MaterialState>(uintptr_t(2)));
        CHECK(api->describe(nullptr,&text,&size)!=0 && text==reinterpret_cast<const char*>(uintptr_t(4)) && size==555);
        CHECK(api->data(nullptr,&data,&size)!=0 && data==reinterpret_cast<void*>(uintptr_t(3)) && size==555);
        CHECK(api->prepare(nullptr)!=0 && api->restore(nullptr,nullptr)!=0);api->destroy_group(nullptr);api->destroy_state(nullptr);
    }
    ~Library(){if(handle_){
#ifdef _WIN32
        FreeLibrary(handle_);
#else
        dlclose(handle_);
#endif
    }}
    std::unique_ptr<HC::IModelComponent> create(){
        auto entry=reinterpret_cast<HydroCoupleComponentInfoFn>(symbol(HYDROCOUPLE_COMPONENT_INFO_SYMBOL));CHECK(entry);
        auto* info=dynamic_cast<HC::IModelComponentInfo*>(entry());CHECK(info);return info->createComponentInstance();
    }
};
void argument(HC::IModelComponent& c,const std::string& id,const Json& value){for(auto* a:c.arguments())if(a->id()==id){std::string why;CHECK(a->initialize(value.dump(),HC::IArgument::ArgumentInputType::JSON,why));return;}CHECK(false);}
HC::IOutput* output(HC::IModelComponent& c,const std::string& id){for(auto* o:c.outputs())if(o->id()==id)return o;throw std::runtime_error("Missing output "+id);}
Json view(HC::IModelComponent& c,const std::string& id="material_session"){
    std::string text,why;const int64_t zero=0,one=1;HC::BufferDescriptor b{&text,HC::DataKind::String,1,&one,nullptr,sizeof(text)};
    CHECK(output(c,id)->getValuesInto(b,{&zero,1},{&one,1},&why));return Json::parse(text);
}
double day(HC::IModelComponent& c){return dynamic_cast<HC::Temporal::ITimeModelComponent&>(c).currentDateTime()->julianDay();}
using Event=const std::shared_ptr<HC::IComponentStatusChangeEventArgs>&;
struct Observer:HC::ISlot<Event>{std::function<void(Event)> call;void operator()(const HC::ISignal<Event>&,Event e)override{call(e);}};
std::string stateBytes(OpenSWMM_MaterialState s){const void* data=nullptr;size_t size=0;CHECK(api->data(s,&data,&size)==0);return {static_cast<const char*>(data),size};}
uint32_t crc(std::string_view s){uint32_t v=~uint32_t(0);for(unsigned char c:s){v^=c;for(int i=0;i<8;++i)v=(v>>1)^(0xedb88320u&uint32_t(-int(v&1)));}return ~v;}
uint32_t readNumber(const std::string& s,size_t at){uint32_t n=0;for(int i=0;i<4;++i)n|=uint32_t(static_cast<unsigned char>(s.at(at+i)))<<(8*i);return n;}
void number(std::string& s,uint32_t n){for(int i=0;i<4;++i)s.push_back(char(n>>(8*i)));}
std::string rawMetadata(const std::string& good,const std::string& meta) {
    const auto m=readNumber(good,8);auto payload=meta+good.substr(20+m);std::string result="OSWMGR01";
    number(result,uint32_t(meta.size()));number(result,uint32_t(good.size()-20-m));number(result,crc(payload));return result+payload;
}
std::string editedState(const std::string& good,const std::function<void(Json&)>& edit){
    const auto m=readNumber(good,8);auto j=Json::parse(good.substr(20,m));
    edit(j);
    return rawMetadata(good,j.dump());
}
std::string damagedNative(std::string good,bool outputs) {
    const size_t offset=20+readNumber(good,8);auto raw=good.substr(offset);
    auto length=[&](size_t at){uint64_t v=0;for(int i=0;i<8;++i)v|=uint64_t(static_cast<unsigned char>(raw.at(at+i)))<<(8*i);return size_t(v);};
    size_t at=8+length(0)+12;const auto model=length(at);at+=8+model;
    const size_t checksumAt=at,bodyAt=at+12;CHECK(bodyAt+length(at+4)==raw.size());
    if(outputs){const auto hdf=raw.find(std::string("\211HDF\r\n\032\n",8),bodyAt);CHECK(hdf!=raw.npos);raw[hdf]=0;}
    else {CHECK(raw.substr(bodyAt+8,7)=="network");raw[bodyAt+8]='N';}
    const auto sum=crc(std::string_view(raw).substr(bodyAt));for(int i=0;i<4;++i)raw[checksumAt+i]=char(sum>>(8*i));
    good.replace(offset,raw.size(),raw);const auto outer=crc(std::string_view(good).substr(20));for(int i=0;i<4;++i)good[16+i]=char(outer>>(8*i));return good;
}
struct Pair {
    std::unique_ptr<HC::IModelComponent> n,s;
    OpenSWMM_MaterialGroup group=nullptr;
    fs::path dir;
    int callbacks=0,restoreCallbacks=0,failures=0;
    bool watchRestore=false;
    OpenSWMM_MaterialState restoring=nullptr;
    Pair(Library& l,const fs::path& input,fs::path root,bool outputs,bool sourceFirst,bool prepareNow=true):dir(std::move(root)) {
        for(bool surface:{false,true}) {
            auto c=l.create();const std::string id=surface?"surface":"network";
            argument(*c,"hydrocouple.instance_id",{{"values",{id}}});
            Json cfg={{"schema","openswmm.component/1"},{"role",surface?"split_material_surface":"split_material_network"},
                {"model",path(input)},{"output_directory",path(dir/id)},{"material_capacity",journalCapacity}};
            if(surface)cfg["surface_results"]=true;argument(*c,"configuration",cfg);c->initialize();CHECK(c->status()==Status::Initialized);
            CHECK(!dynamic_cast<HC::ICheckpointableModelComponent*>(c.get()));
            CHECK(!c->capabilities().contains(HC::Capability::Checkpointing));
            (surface?s:n)=std::move(c);
        }
        output(*s,"openswmm.node-surface-material.v10")->addConsumer(n->inputs().front());
        CHECK(n->validate().empty() && s->validate().empty());
        auto sentinel=reinterpret_cast<OpenSWMM_MaterialGroup>(uintptr_t(1));
        CHECK(api->enroll(s.get(),n.get(),outputs,&sentinel)!=0 && sentinel==reinterpret_cast<OpenSWMM_MaterialGroup>(uintptr_t(1)));
        CHECK(api->enroll(n.get(),s.get(),2,&sentinel)!=0);
        CHECK(api->enroll(n.get(),s.get(),outputs,&group)==0);
        CHECK(api->enroll(n.get(),s.get(),outputs,&sentinel)!=0 && sentinel==reinterpret_cast<OpenSWMM_MaterialGroup>(uintptr_t(1)));
        const char* description=nullptr;size_t size=0;CHECK(api->describe(group,&description,&size)==0);
        fs::create_directories(dir);save(dir/"group.json",std::string(description,size));
        bool refused=!output(*s,"openswmm.node-surface-material.v10")->removeConsumer(n->inputs().front());
        CHECK(refused && n->inputs().front()->provider()==output(*s,"openswmm.node-surface-material.v10"));
        CHECK(!n->inputs().front()->setProvider(nullptr));
        if(!prepareNow)return;
        auto observer=std::make_shared<Observer>();observer->call=[this](Event e){
            ++callbacks;
            if(n->status()==Status::Checkpointing || s->status()==Status::Checkpointing){
                if(n->status()!=s->status())++failures;
                auto sentinel=reinterpret_cast<OpenSWMM_MaterialState>(uintptr_t(1));
                if(api->capture(group,&sentinel)==0 || sentinel!=reinterpret_cast<OpenSWMM_MaterialState>(uintptr_t(1)))++failures;
                if(api->prepare(group)==0)++failures;
                const auto state=n->status();n->update();s->update();n->prepare();s->finish();if(n->status()!=state || s->status()!=state)++failures;
                api->destroy_group(group); // Must refuse rather than invalidate the active lease.
                if(restoring){if(api->restore(group,restoring)==0)++failures;api->destroy_state(restoring);const void* data=nullptr;size_t size=0;if(api->data(restoring,&data,&size)!=0)++failures;}
            }
            if(watchRestore && e->status()==Status::Updated){
                ++restoreCallbacks;
                if(day(*n)!=day(*s) || view(*n).at("accepted_time_s")!=view(*s).at("accepted_time_s"))++failures;
                throw std::runtime_error("Injected postcommit listener failure");
            }
        };
        n->connect(observer);s->connect(observer);
        (sourceFirst?s:n)->prepare();(sourceFirst?n:s)->prepare();
        if(n->status()!=Status::Updated || s->status()!=Status::Updated){for(auto& e:n->errors())std::cerr<<e.message<<'\n';for(auto& e:s->errors())std::cerr<<e.message<<'\n';}
        CHECK(n->status()==Status::Updated && s->status()==Status::Updated);
        CHECK(api->enroll(n.get(),s.get(),outputs,&sentinel)!=0);
        CHECK(api->prepare(group)==0);CHECK(api->prepare(group)!=0);
        CHECK(view(*n).at("bound")==true && view(*s).at("bound")==true);
        for(auto* c:{n.get(),s.get()}){
            auto m=view(*c,"model_metadata");CHECK(m.at("checkpointing")==false && m.at("material_checkpointing")==false);
            CHECK(m.at("material_collective_recovery").at("enrolled")==true);
            CHECK(!m.at("result_files").contains("report"));
        }
        CHECK(!fs::exists(dir/"network/model.rpt") && !fs::exists(dir/"surface/model.rpt"));
        if(!outputs)CHECK(!fs::exists(dir/"network/model.out") && !fs::exists(dir/"surface/model.2d.h5"));
    }
    ~Pair(){if(group)api->destroy_group(group);}
    OpenSWMM_MaterialState capture(int step,const std::string& prefix,bool outputs){
        OpenSWMM_MaterialState state=nullptr;int rc=api->capture(group,&state);
        if(rc){CHECK(((outputs && step>8) || (journalCapacity==6 && step>6)) && state==nullptr);return nullptr;}
        save(dir/(prefix+"-"+std::to_string(step)+".bin"),stateBytes(state));
        save(dir/(prefix+"-"+std::to_string(step)+"-views.json"),Json::array({view(*n),view(*s)}).dump(2));
        return state;
    }
    void step(){s->update();n->update();if(n->status()!=Status::Updated){for(auto& e:n->errors())std::cerr<<e.message<<'\n';}CHECK(n->status()==Status::Updated && s->status()==Status::Updated);CHECK(day(*n)==day(*s));}
    void finish(){n->finish();s->finish();CHECK(n->status()==Status::Finished && s->status()==Status::Finished);CHECK(failures==0);}
};
struct Native {
    SWMM_Engine e=swmm_engine_create();
    ~Native(){swmm_engine_destroy(e);}
    void check(int rc){if(rc)throw std::runtime_error(swmm_get_last_error_msg(e));}
};
int main(int argc,char** argv){try {
    CHECK(argc==7);Library library;std::string action=argv[1];const bool tail=action.starts_with("--tail-");
    if(tail){journalCapacity=6;action=action=="--tail-original"?"--original":"--restore";}const fs::path model=fs::absolute(argv[2]),dir=fs::absolute(argv[3]),parent=fs::absolute(argv[6]);
    const bool outputs=std::stoi(argv[4]);const int captureAt=std::stoi(argv[5]),last=tail?80:outputs?12:8;
    fs::create_directories(dir);
    if(action=="--original") {
        Pair a(library,model,dir/"frozen-a",outputs,false,false),b(library,model,dir/"frozen-b",outputs,false,false);
        bool refused=false;try{output(*a.s,"openswmm.node-surface-material.v10")->addConsumer(b.n->inputs().front());}catch(...){refused=true;}
        CHECK(refused && a.n->inputs().front()->provider()==output(*a.s,"openswmm.node-surface-material.v10") && b.n->inputs().front()->provider()==output(*b.s,"openswmm.node-surface-material.v10"));
        // Early finish must honor the enrolled mode even before service preparation.
        if(!outputs)for(bool sourceFirst:{false,true}) {
            Pair early(library,model,dir/(sourceFirst?"early-surface":"early-network"),false,false,false);
            early.n->prepare();early.s->prepare();(sourceFirst?early.s:early.n)->finish();(sourceFirst?early.n:early.s)->finish();
            CHECK(early.n->status()==Status::Finished && early.s->status()==Status::Finished);
            CHECK(!fs::exists(early.dir/"network/model.out") && !fs::exists(early.dir/"network/model.rpt") && !fs::exists(early.dir/"surface/model.2d.h5"));
        }
    }
    Pair pair(library,model,dir,outputs,captureAt==6);
    if(action=="--original") {
        Native ref;ref.check(swmm_engine_open(ref.e,path(model).c_str(),nullptr,outputs?path(dir/"native.out").c_str():nullptr,nullptr));
        if(outputs)ref.check(swmm_2d_set_results_file(ref.e,path(dir/"native.h5").c_str()));
        ref.check(swmm_engine_initialize(ref.e));ref.check(swmm_engine_start(ref.e,outputs));ref.check(swmm_passive_exchange_enable(ref.e,journalCapacity,0));
        OpenSWMM_MaterialState retained=nullptr;
        for(int k=0;k<=last;++k){if(k){pair.step();double elapsed=0;ref.check(swmm_engine_step(ref.e,&elapsed));double t=0;ref.check(swmm_engine_get_elapsed_seconds(ref.e,&t));CHECK(t==view(*pair.n).at("accepted_time_s"));}
            auto state=pair.capture(k,"original",outputs);if(k==captureAt)retained=state;else api->destroy_state(state);
        }
        CHECK(retained);
        if(tail){const auto v=Json::parse(bytes(dir/("original-"+std::to_string(captureAt)+"-views.json"))).at(1);
            CHECK(v.at("empty_windows_observed").get<int>()>=2 && v.at("last_window").at("receipt")!=v.at("last_window").at("ordinal"));}
        ref.check(swmm_engine_end(ref.e));ref.check(swmm_engine_close(ref.e));pair.finish();
        const auto before=stateBytes(retained);pair.s.reset();pair.n.reset();CHECK(stateBytes(retained)==before);
        const char* summary=nullptr;size_t length=0;CHECK(observationApi->describe_state(retained,&summary,&length)==0);
        auto descriptor=Json::parse(std::string(summary,length));CHECK(descriptor.at("accepted_steps")==captureAt);
        CHECK(descriptor.at("group").at("network")=="network" && descriptor.at("group").at("surface")=="surface");
        CHECK(descriptor.at("group").at("outputs")==outputs);
        const char* sentinelText=reinterpret_cast<const char*>(uintptr_t(1));size_t sentinelSize=789;
        CHECK(observationApi->describe_state(nullptr,&sentinelText,&sentinelSize)!=0 && sentinelText==reinterpret_cast<const char*>(uintptr_t(1)) && sentinelSize==789);
        auto sentinel=reinterpret_cast<OpenSWMM_MaterialState>(uintptr_t(1));CHECK(api->capture(pair.group,&sentinel)!=0 && sentinel==reinterpret_cast<OpenSWMM_MaterialState>(uintptr_t(1)));
        auto buffer=before;OpenSWMM_MaterialState imported=nullptr;CHECK(api->import_state(buffer.data(),buffer.size(),&imported)==0);buffer.assign(buffer.size(),'!');CHECK(stateBytes(imported)==before);
        for(size_t n:{size_t(0),size_t(19),before.size()-1}){CHECK(api->import_state(before.data(),n,&sentinel)!=0 && sentinel==reinterpret_cast<OpenSWMM_MaterialState>(uintptr_t(1)));}
        auto bad=before;bad.back()^=1;CHECK(api->import_state(bad.data(),bad.size(),&sentinel)!=0 && sentinel==reinterpret_cast<OpenSWMM_MaterialState>(uintptr_t(1)));
        const auto m=readNumber(before,8);auto meta=before.substr(20,m);
        for(const auto& wrongMeta:{std::string("{\"network\":\"duplicate\",")+meta.substr(1),
            std::string("{\"deep\":")+std::string(30,'[')+"0"+std::string(30,']')+","+meta.substr(1)}) {
            const auto bad=rawMetadata(before,wrongMeta);
            CHECK(api->import_state(bad.data(),bad.size(),&sentinel)!=0 && sentinel==reinterpret_cast<OpenSWMM_MaterialState>(uintptr_t(1)));
        }
        api->destroy_state(imported);api->destroy_state(retained);
    } else {
        CHECK(action=="--restore");auto buffer=bytes(parent/("original-"+std::to_string(captureAt)+".bin"));OpenSWMM_MaterialState state=nullptr;
        CHECK(api->import_state(buffer.data(),buffer.size(),&state)==0);buffer.assign(buffer.size(),'!');
        const auto n0=view(*pair.n),s0=view(*pair.s);std::string binary0=outputs?bytes(dir/"network/model.out"):"",hdf0=outputs?bytes(dir/"surface/model.2d.h5"):"";
        for(int fault=0;fault<14;++fault) {
            auto bad=editedState(stateBytes(state),[&](Json& j){auto& o=j["observations"][1];
                if(fault==0){const double t=std::nextafter(o["time_s"].get<double>(),INFINITY);j["observations"][0]["time_s"]=t;o["time_s"]=t;}
                if(fault==1)j["network"]="different-network";
                if(fault==2)j["outputs"]=!outputs;
                if(fault==3)o["outer"][2]=o["accepted"].get<uint64_t>()+1;
                if(fault==4){o["legs"]=uint64_t(65);o["drain"]=uint64_t(65);o["spill"]=uint64_t(0);}
                if(fault==5)o["last"]["session"]="01";
                if(fault==6)o["last"]["receipt"]="0";
                if(fault==7)o["last"]["outer_phase"]=3;
                if(fault==8)o["last"]["outer_sequence"]="99999999999";
                if(fault==9)o["last"]["stage"]=2;
                if(fault==10){o["last"].erase("receipt");o["last"]["unknown"]=Json::object();}
                if(fault==13)o["last"]["receipt"]=std::to_string(std::stoull(o["last"]["receipt"].get<std::string>())+1);
                if(fault==11){o["observed"][2]=o["observed"][2].get<uint64_t>()+1;}
            });
            if(fault==12)bad=damagedNative(stateBytes(state),outputs);
            OpenSWMM_MaterialState wrong=nullptr;CHECK(api->import_state(bad.data(),bad.size(),&wrong)==0);
            pair.watchRestore=fault==0;pair.restoring=wrong;CHECK(api->restore(pair.group,wrong)!=0);pair.restoring=nullptr;pair.watchRestore=false;
            CHECK(view(*pair.n)==n0 && view(*pair.s)==s0 && pair.n->status()==Status::Updated && pair.s->status()==Status::Updated);
            if(outputs)CHECK(bytes(dir/"network/model.out")==binary0 && bytes(dir/"surface/model.2d.h5")==hdf0);
            api->destroy_state(wrong);
        }
        pair.watchRestore=true;pair.restoring=state;CHECK(api->restore(pair.group,state)==0);pair.restoring=nullptr;pair.watchRestore=false;
        CHECK(pair.restoreCallbacks==4 && pair.failures==0);CHECK(view(*pair.s).at("last_window_historical")==true);
        for(int k=captureAt;k<=last;++k){if(k>captureAt)pair.step();auto out=pair.capture(k,"restored",outputs);api->destroy_state(out);}
        CHECK(api->restore(pair.group,state)!=0);api->destroy_state(state);pair.finish();
    }
    save(dir/"result.json",Json{{"callbacks",pair.callbacks},{"observer_failures",pair.failures},{"continued_steps",action=="--restore"?last-captureAt:last},
        {"fresh_process",action=="--restore"},{"native_text_reports",false}}.dump(2));return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
