// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ComponentBase.hpp"
#include <openswmm/engine/openswmm_split_material.h>
#include <openswmm/engine/openswmm_split_packet.h>
#include <array>
#include <functional>

namespace openswmm::couplers {
inline constexpr char materialPhasePort[]="openswmm.node-surface-material.v10";
struct MaterialLifetime {
    explicit MaterialLifetime(bool source,std::string name):surface(source),id(std::move(name)){}
    const bool surface;
    const std::string id;
    std::weak_ptr<MaterialLifetime> peer;
    const void* recoveryOwner=nullptr;
    bool available=true,prepared=false,ready=false,busy=false,bound=false,ended=false,terminal=false,finishing=false,authorizedFinish=false;
    std::string error;
    bool frozen()const noexcept{return recoveryOwner || busy||(prepared&&!ended&&!terminal);}
};
class MaterialReceipt;
class MaterialRequest final:public Item,public HC::IInput {
public:
    MaterialRequest(HC::IModelComponent* owner,std::shared_ptr<MaterialLifetime> life):Item(materialPhasePort,owner,HC::DataKind::String),life_(std::move(life)){}
    HC::IOutput* provider()const override{return provider_;}
    bool canConsume(HC::IOutput*,std::string&)const override;
    bool setProvider(HC::IOutput*)override;
    bool setValuesFrom(const HC::BufferDescriptor&,std::span<const int64_t>,std::span<const int64_t>,std::string* message)override {
        if(message)*message="Material phases are issued by the native network clock";return false;
    }
    std::shared_ptr<MaterialLifetime> lifetime()const{return life_;}
    std::function<void(double)> finishAt;
    void finishFrom(const std::shared_ptr<MaterialLifetime>& source,double time){
        auto peer=life_->peer.lock();
        if(!finishAt||peer!=source||!source->finishing||!source->busy||!life_->busy||life_->terminal)
            throw std::invalid_argument("Material FINISH requires its active finishing surface");
        life_->authorizedFinish=true;
        struct Guard{bool& flag;~Guard(){flag=false;}} guard{life_->authorizedFinish};finishAt(time);
    }
private:
    std::shared_ptr<MaterialLifetime> life_;
    HC::IOutput* provider_=nullptr;
};
class MaterialReceipt final:public Output {
    std::vector<HC::IInput*> connected_;
    bool dispatching_=false;
public:
    MaterialReceipt(HC::IModelComponent* owner,std::shared_ptr<MaterialLifetime> life):Output(materialPhasePort,owner,HC::DataKind::String),life_(std::move(life)){}
    std::shared_ptr<MaterialLifetime> lifetime()const{return life_;}
    std::function<void(const HC::IInput*)> serve;
    std::function<void(SWMM_Engine,std::uint64_t)> bind;
    std::function<std::string(const HC::IInput*,int,const char*,std::size_t)> stage;
    std::function<void(double,bool)> accept;
    std::function<void()> observe;
    std::vector<HC::IInput*> consumers()const override{return connected_;}
    void addConsumer(HC::IInput* input,const HC::IIdentity* providerRole=nullptr)override {
        const auto connected=consumers();if(std::find(connected.begin(),connected.end(),input)!=connected.end())return;
        // A material phase pairs with one native MaterialRequest: never a multi-input role.
        if(providerRole||life_->prepared||life_->frozen()||!life_->available||!connected.empty()||!dynamic_cast<MaterialRequest*>(input))
            throw std::invalid_argument("Material output requires one unfrozen native consumer");
        std::string why;if(!input->canConsume(this,why)||!input->setProvider(this))throw std::invalid_argument("Cannot wire material endpoint: "+why);
        try{connected_.push_back(input);}catch(...){input->setProvider(nullptr);throw;}
    }
    bool removeConsumer(HC::IInput* input)override {
        auto peer=life_->peer.lock();if(life_->frozen()||(peer&&peer->frozen()))return false;
        const auto it=std::find(connected_.begin(),connected_.end(),input);if(it==connected_.end())return false;
        if(peer&&input&&input->provider()==this&&!input->setProvider(nullptr))return false;
        connected_.erase(it);life_->peer.reset();return true;
    }
    void addAdaptedOutput(HC::IAdaptedOutput* output)override {
        if(output)throw std::invalid_argument("Material phases cannot use adapted outputs");
    }
    bool removeAdaptedOutput(HC::IAdaptedOutput*)override{return false;}
    void validateQuery(const HC::IInput* query)const {
        auto peer=life_->peer.lock();const auto connected=consumers();
        if(!life_->available||!peer||!peer->available||!query||connected.size()!=1||connected[0]!=query||
            query->provider()!=this||!adaptedOutputs().empty()||query->id()!=materialPhasePort)
            throw std::invalid_argument("Material endpoint requires its live reciprocal direct connection");
    }
    void updateValues(const HC::IInput* query)override {
        validateQuery(query);auto peer=life_->peer.lock();
        if(!dispatching_||!life_->busy||!peer->busy||!serve)throw std::invalid_argument("Material query is outside its single native dispatch");
        dispatching_=false;
        serve(query);
    }
    void dispatch(const HC::IInput* query){
        if(dispatching_)throw std::invalid_argument("Material dispatch is already active");
        dispatching_=true;struct Guard{bool& flag;~Guard(){flag=false;}} guard{dispatching_};updateValues(query);
    }
private:
    std::shared_ptr<MaterialLifetime> life_;
};
inline bool MaterialRequest::canConsume(HC::IOutput* output,std::string& why)const {
    auto* receipt=dynamic_cast<MaterialReceipt*>(output);auto peer=receipt?receipt->lifetime():nullptr;
    if(!receipt||!peer||!peer->surface||life_->surface||!life_->available||!peer->available||
        output->modelComponent()==modelComponent()||output->modelComponent()->id()==modelComponent()->id()||
        output->id()!=id()||output->dataKind()!=HC::DataKind::String||output->shape()!=shape()||!output->adaptedOutputs().empty()){
        why="Choose the distinct native material surface endpoint";return false;
    }
    const auto consumers=output->consumers();auto existing=peer->peer.lock();
    if((!consumers.empty()&&(consumers.size()!=1||consumers[0]!=this))||(existing&&existing!=life_)){
        why="Material surface already belongs to another consumer";return false;
    }
    why.clear();return true;
}
inline bool MaterialRequest::setProvider(HC::IOutput* output){
    if(output==provider_)return true;auto peer=life_->peer.lock();
    if(output&&provider_)return false;
    if(life_->frozen()||(peer&&peer->frozen()))return false;
    if(!output){if(peer)peer->peer.reset();life_->peer.reset();provider_=nullptr;return true;}
    if(life_->prepared)return false;
    std::string why;if(!canConsume(output,why))return false;
    auto target=static_cast<MaterialReceipt*>(output)->lifetime();life_->peer=target;target->peer=life_;provider_=output;return true;
}
class MaterialPhaseLink {
    using Json=nlohmann::json;
    struct Frame{SWMM_SplitFrame value=nullptr;~Frame(){swmm_split_frame_destroy(value);}};
    std::shared_ptr<MaterialLifetime> life_;
    const std::uint64_t capacity_;
    SWMM_Engine engine_=nullptr;
    MaterialReceipt* pinned_=nullptr;
    std::string response_;
    std::array<std::uint64_t,4> observed_{};
    std::array<std::uint64_t,9> outer_{};
    std::uint64_t accepted_=0,legs_=0,drain_=0,spill_=0,empty_=0,zeroCapacity_=0;
    Json lastWindow_=Json::object();
    double acceptedTime_=0;
    bool lastWindowHistorical_=false;
    static void checked(SWMM_Engine e,int rc){if(rc)throw std::runtime_error("Material phase: "+std::to_string(rc)+" "+swmm_get_last_error_msg(e)+" "+swmm_split_material_last_error());}
    static std::string read(const HC::IComponentDataItem& item){std::string text;const int64_t zero=0;
        if(!HC::Helpers::getValue(item,text,{&zero,1})||text.empty())throw std::invalid_argument("Material endpoint has no prepared packet");return text;}
    static std::string encode(SWMM_Engine e,SWMM_SplitFrame frame){char* raw=nullptr;checked(e,swmm_split_material_packet_encode(e,frame,&raw));
        std::unique_ptr<char,decltype(&swmm_split_packet_free)> out(raw,swmm_split_packet_free);return raw;}
    MaterialReceipt& provider()const {
        auto peer=life_->peer.lock();
        if(!life_->available||!peer||!peer->available||peer->terminal)
            throw std::invalid_argument("Material peer is closed or terminal");
        auto* result=dynamic_cast<MaterialReceipt*>(request.provider());
        if(!result||result->lifetime()!=peer||(pinned_&&pinned_!=result))throw std::invalid_argument("Material provider generation changed");
        result->validateQuery(&request);return *result;
    }
    static int exchange(SWMM_Engine e,int,SWMM_SplitFrame in,SWMM_SplitFrame* out,void* user){auto& link=*static_cast<MaterialPhaseLink*>(user);
        try{auto& peer=link.provider();link.request.text=encode(e,in);peer.dispatch(&link.request);auto text=read(peer);
            checked(e,swmm_split_material_packet_decode(e,text.data(),text.size(),out));return SWMM_OK;}
        catch(const std::exception& ex){link.fail(ex.what());return SWMM_ERR_PLUGIN;}
        catch(...){link.fail("Non-standard material boundary exception");return SWMM_ERR_PLUGIN;}
    }
    static int transport(SWMM_Engine,int stage,const char* bytes,std::size_t size,const char** out,std::size_t* length,void* user){auto& link=*static_cast<MaterialPhaseLink*>(user);
        try{auto& peer=link.provider();if(!peer.stage)throw std::invalid_argument("Material endpoint has no stage dispatcher");
            link.response_=peer.stage(&link.request,stage,bytes,size);++link.observed_.at(stage);*out=link.response_.data();*length=link.response_.size();return SWMM_OK;}
        catch(const std::exception& ex){link.fail(ex.what());return SWMM_ERR_PLUGIN;}
        catch(...){link.fail("Non-standard material transport exception");return SWMM_ERR_PLUGIN;}
    }
    std::string serveStage(const HC::IInput* query,int stage,const char* bytes,std::size_t size){
        receipt.validateQuery(query);auto peer=life_->peer.lock();
        if(!started||!life_->busy||!peer->busy||life_->terminal||stage<1||stage>3)throw std::invalid_argument("Material stage outside active bound operation");
        char* raw=nullptr;checked(engine_,swmm_split_material_packet_copy(engine_,bytes,size,&raw));
        std::unique_ptr<char,decltype(&swmm_split_packet_free)> value(raw,swmm_split_packet_free);
        const auto packet=Json::parse(raw);if(packet.at("stage")!=stage)throw std::invalid_argument("Material stage mismatch");
        lastWindowHistorical_=false;++observed_[stage];const auto& allowance=packet.at("body").at("view").at("allowance");
        lastWindow_={{"session",packet.at("session")},{"ordinal",allowance.at("ordinal")},{"receipt",allowance.at("receipt")},
            {"outer_sequence",packet.at("outer_sequence")},{"outer_phase",packet.at("outer_phase")},{"stage",stage}};
        if(stage==SWMM_MATERIAL_OFFER&&allowance.at("leg_limit")=="0")++zeroCapacity_;
        if(stage==SWMM_MATERIAL_PROOF){const auto& proof=packet.at("body").at("proof").at("legs");legs_+=proof.size();if(proof.empty())++empty_;
            for(const auto& leg:proof)if(leg.at("drain").get<bool>())++drain_;else ++spill_;}
        return raw;
    }
public:
    class Operation {
        std::shared_ptr<MaterialLifetime> own_,peer_;
    public:
        explicit Operation(std::shared_ptr<MaterialLifetime> own):own_(std::move(own)),peer_(own_->peer.lock()){
            if(own_->busy||(peer_&&peer_->busy))throw std::invalid_argument("Material lifecycle is already active");
            own_->busy=true;if(peer_)peer_->busy=true;
        }
        Operation(const Operation&)=delete;Operation& operator=(const Operation&)=delete;
        Operation(Operation&& other)noexcept:own_(std::move(other.own_)),peer_(std::move(other.peer_)){}
        ~Operation(){if(own_)own_->busy=false;if(peer_)peer_->busy=false;}
    };
    MaterialPhaseLink(HC::IModelComponent* owner,bool isSurface,std::uint64_t capacity):life_(std::make_shared<MaterialLifetime>(isSurface,owner->id())),capacity_(capacity),
        surface(isSurface),request(owner,life_),receipt(owner,life_){
        receipt.bind=[this](SWMM_Engine network,std::uint64_t capacity){if(!surface||!started||!life_->ready||life_->bound||!life_->busy||capacity!=capacity_)throw std::invalid_argument("Material surface is not ready for matching exclusive binding");
            checked(network,swmm_split_material_bind(network,engine_,capacity_));life_->bound=true;};
        receipt.stage=[this](const HC::IInput* query,int stage,const char* bytes,std::size_t size){return serveStage(query,stage,bytes,size);};
        receipt.accept=[this](double time,bool ended){acceptedTime_=time;if(!ended)++accepted_;life_->ended=ended;
            if(publish)publish(time,ended);};
    }
    const bool surface;
    MaterialRequest request;
    MaterialReceipt receipt;
    bool started=false;
    std::function<void(double,bool)> publish;
    Operation operation(){return Operation(life_);}
    bool busy()const noexcept{return life_->busy;}
    bool ready()const noexcept{return life_->ready;}
    bool ended()const noexcept{return life_->ended;}
    bool terminal()const noexcept{return life_->terminal;}
    bool authorizedFinish()const noexcept{return life_->authorizedFinish;}
    bool connected()const{return surface?!receipt.consumers().empty():request.provider()!=nullptr;}
    const std::string& error()const noexcept{return life_->error;}
    void freeze(){life_->prepared=true;}
    void fail(const std::string& text){
        if(life_->error.empty())life_->error=text;life_->terminal=true;
        if(auto peer=life_->peer.lock()){if(peer->error.empty())peer->error=text;peer->terminal=true;}
    }
    void validate()const {
        if(life_->terminal||!life_->available)throw std::invalid_argument("Material endpoint is closed or terminal");
        if(surface){auto peer=life_->peer.lock();if(!peer||!peer->available)throw std::invalid_argument("Material consumer generation expired");
            const auto c=receipt.consumers();if(c.size()!=1)throw std::invalid_argument("Connect one material consumer");receipt.validateQuery(c[0]);}
        else (void)provider();
    }
    void preparedNetwork(SWMM_Engine e){engine_=e;life_->ready=true;}
    void prepareSurface(SWMM_Engine e){engine_=e;Frame geometry;checked(e,swmm_split_geometry(e,&geometry.value));receipt.text=encode(e,geometry.value);
        checked(e,swmm_engine_start(e,0));started=true;life_->ready=true;}
    void startNetwork(SWMM_Engine e,bool saveResults=true){
        if(started){validate();return;}auto& peer=provider();auto state=peer.lifetime();
        if(!life_->ready||!state->ready||state->bound)throw std::invalid_argument("Both material components must prepare before advancement");
        const auto geometry=read(peer);Frame frame;checked(e,swmm_split_material_packet_decode(e,geometry.data(),geometry.size(),&frame.value));
        checked(e,swmm_split_apply_geometry(e,frame.value));checked(e,swmm_split_set_callback(e,exchange,this));checked(e,swmm_engine_start(e,saveResults?1:0));
        checked(e,swmm_split_material_set_transport(e,transport,this));pinned_=&peer;peer.bind(e,capacity_);life_->bound=true;started=true;engine_=e;
    }
    void evaluate(SWMM_Engine e,const HC::IInput* query){
        if(!surface||!started||life_->ended||life_->terminal)throw std::invalid_argument("Material phase outside active surface session");
        receipt.validateQuery(query);auto bytes=read(*query);Frame in,out;
        checked(e,swmm_split_material_packet_decode(e,bytes.data(),bytes.size(),&in.value));const int phase=swmm_split_packet_phase(in.value);
        checked(e,swmm_split_evaluate(e,phase,in.value,&out.value));receipt.text=encode(e,out.value);++outer_.at(phase);
    }
    void accepted(double time){
        auto& peer=provider();acceptedTime_=time;++accepted_;peer.accept(time,false);
    }
    void finishNetwork(SWMM_Engine e){
        if(life_->ended)return;startNetwork(e);checked(e,swmm_engine_end(e));
        life_->ended=true;auto& peer=provider();peer.accept(acceptedTime_,true);
    }
    void finishSurface(double time){
        if(life_->ended)return;validate();auto* input=dynamic_cast<MaterialRequest*>(receipt.consumers().front());
        if(!input)throw std::invalid_argument("Material consumer is not a native network endpoint");
        life_->finishing=true;struct Guard{bool& flag;~Guard(){flag=false;}} guard{life_->finishing};input->finishFrom(life_,time);
        if(!life_->ended)throw std::invalid_argument("Network did not confirm material FINISH");
    }
    void observePeer(){
        if(surface)return;auto peer=life_->peer.lock();if(!peer||!peer->available)return;
        auto* endpoint=dynamic_cast<MaterialReceipt*>(request.provider());
        if(endpoint&&endpoint->lifetime()==peer&&endpoint->observe)endpoint->observe();
    }
    void release()noexcept{
        if(!life_->ended&&!life_->terminal&&life_->bound){
            try{fail("Material endpoint closed before successful finalization");}
            catch(...){life_->terminal=true;if(auto peer=life_->peer.lock())peer->terminal=true;}
        }
        life_->available=false;life_->ready=false;engine_=nullptr;
    }
    struct Observation {
        std::array<std::uint64_t,4> observed{};
        std::array<std::uint64_t,9> outer{};
        std::uint64_t accepted=0,legs=0,drain=0,spill=0,empty=0,zeroCapacity=0;
        Json last=Json::object();
        double time=0;
        bool historical=false;
    };
    Json observation()const {
        return Json{{"observed",observed_},{"outer",outer_},{"accepted",accepted_},{"legs",legs_},
            {"drain",drain_},{"spill",spill_},{"empty",empty_},{"zero_capacity",zeroCapacity_},
            {"last",lastWindow_},{"time_s",acceptedTime_}};
    }
    Observation prepareObservation(const Json& j)const {
        if(!j.is_object() || j.size()!=10 || !j.at("observed").is_array() || j.at("observed").size()!=4 ||
            !j.at("outer").is_array() || j.at("outer").size()!=9)
            throw std::invalid_argument("Material observer shape differs");
        auto count=[](const Json& v){if(!v.is_number_unsigned() || v.get<std::uint64_t>()>1000000000000ULL)
                throw std::invalid_argument("Material observer count exceeds its bound");return v.get<std::uint64_t>();};
        Observation o;
        for(size_t i=0;i<4;++i)o.observed[i]=count(j.at("observed").at(i));
        for(size_t i=0;i<9;++i)o.outer[i]=count(j.at("outer").at(i));
        o.accepted=count(j.at("accepted"));o.legs=count(j.at("legs"));o.drain=count(j.at("drain"));
        o.spill=count(j.at("spill"));o.empty=count(j.at("empty"));o.zeroCapacity=count(j.at("zero_capacity"));
        o.time=j.at("time_s").get<double>();o.last=j.at("last");o.historical=!o.last.empty();
        if(!std::isfinite(o.time) || o.time<0 || (o.time==0)!=(o.accepted==0) || o.observed[0] ||
            o.observed[1]!=o.observed[2] || o.observed[1]!=o.observed[3] || o.legs!=o.drain+o.spill ||
            o.empty>o.observed[2] || o.zeroCapacity>o.observed[1] || !o.last.is_object() ||
            (!surface && (o.legs || o.empty || o.zeroCapacity || !o.last.empty())) ||
            (surface && ((o.observed[3]==0)!=o.last.empty())) || o.outer[3] || o.outer[0] ||
            o.outer[4] || o.outer[5] || o.outer[6] || o.outer[7])
            throw std::invalid_argument("Material observer history is inconsistent");
        if(!o.last.empty()) {
            auto ordinal=[&](const char* key){
                const auto& value=o.last.at(key);
                if(!value.is_string())throw std::invalid_argument("Material observer identity must be decimal text");
                const auto text=value.get<std::string>();
                if(text.empty() || text.size()>20 || text.front()=='0' ||
                    !std::all_of(text.begin(),text.end(),[](char c){return c>='0' && c<='9';}))
                    throw std::invalid_argument("Material observer identity is not canonical");
                const auto n=std::stoull(text);if(std::to_string(n)!=text)throw std::invalid_argument("Material observer identity overflow");return n;
            };
            if(o.last.size()!=6 || !o.last.at("stage").is_number_integer() ||
                !o.last.at("outer_phase").is_number_integer() || o.last.at("stage")!=SWMM_MATERIAL_ACK ||
                (o.last.at("outer_phase")!=2 && o.last.at("outer_phase")!=8) ||
                ordinal("session")==0 || ordinal("receipt")<std::max(uint64_t(1),o.observed[3]-o.empty) ||
                ordinal("receipt")>o.observed[3]-o.empty+uint64_t(o.empty!=0) ||
                ordinal("ordinal")!=o.observed[3] || ordinal("outer_sequence")<o.observed[3] ||
                ordinal("outer_sequence")>o.outer[1]+o.outer[2]+o.outer[8])
                throw std::invalid_argument("Material observer last window differs");
        }
        return o;
    }
    void installObservation(Observation&& o)noexcept {
        observed_=o.observed;outer_=o.outer;accepted_=o.accepted;legs_=o.legs;drain_=o.drain;
        spill_=o.spill;empty_=o.empty;zeroCapacity_=o.zeroCapacity;acceptedTime_=o.time;
        lastWindow_.swap(o.last);lastWindowHistorical_=o.historical;
    }
    Json snapshot()const {
        auto peer=life_->peer.lock();return Json{{"schema","openswmm.material-component-session/1"},{"instance_id",life_->id},
            {"peer",peer?peer->id:std::string()},{"surface",surface},{"ready",life_->ready},{"bound",life_->bound},
            {"finished",life_->ended},{"failed",life_->terminal},{"error",life_->error},{"journal_capacity",capacity_},{"accepted_time_s",acceptedTime_},
            {"accepted_boundaries",accepted_},{"observed_offer",observed_[1]},{"observed_proof",observed_[2]},{"observed_ack",observed_[3]},
            {"applied_legs_observed",legs_},{"drain_legs_observed",drain_},{"spill_legs_observed",spill_},{"empty_windows_observed",empty_},
            {"zero_capacity_offers_observed",zeroCapacity_},{"conductance_observed",outer_[1]},{"advance_observed",outer_[2]},
            {"report_observed",outer_[8]},{"finish_observed",outer_[3]},{"last_window",lastWindow_},{"last_window_historical",lastWindowHistorical_},{"authority","retained_native_in_process"}};
    }
};
}
