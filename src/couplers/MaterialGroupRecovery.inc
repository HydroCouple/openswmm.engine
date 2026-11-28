// SPDX-License-Identifier: Apache-2.0
// Included after the concrete component, inside its module-private namespace.
// The optional C service never exposes the native engine or widens HC vtables.
struct MaterialGroupRecovery {
    using Component=OpenSWMMEngineComponent;
    using CS=HC::IModelComponent::ComponentStatus;
    struct Error:std::runtime_error { int code;Error(int c,const char* s):runtime_error(s),code(c){} };
    struct NativeDelete { void operator()(SWMM_MaterialCheckpoint p)const noexcept{swmm_split_material_checkpoint_destroy(p);} };
    using Native=std::unique_ptr<SWMM_MaterialCheckpointOpaque,NativeDelete>;
    struct Group {
        Component *network,*surface;
        std::weak_ptr<MaterialLifetime> first,second;
        const bool outputs;
        const Json identity;
        const std::string description;
        Group(Component& n,Component& s,bool out,Json id):network(&n),surface(&s),
            first(n.material_->request.lifetime()),second(s.material_->receipt.lifetime()),
            outputs(out),identity(std::move(id)),description(identity.dump()){}
        ~Group(){for(auto& weak:{first,second})if(auto life=weak.lock();life && life->recoveryOwner==this)life->recoveryOwner=nullptr;}
    };
    struct State {
        const std::string bytes;
        const Json metadata;
        const std::string description;
        Native native;
        bool inUse=false;
        State(std::string b,Json m,Native n):bytes(std::move(b)),metadata(std::move(m)),description(stateDescription(metadata)),native(std::move(n)){}
    };
    static std::string stateDescription(const Json& metadata) {
        const auto& a=metadata.at("observations").at(0);const auto& b=metadata.at("observations").at(1);
        require(a.at("accepted").is_number_unsigned() && a.at("accepted")==b.at("accepted") &&
            a.at("time_s").is_number() && a.at("time_s")==b.at("time_s"),"Collective state observed clocks/counts differ");
        const auto seconds=a.at("time_s").get<double>();
        require(std::isfinite(seconds) && seconds>=0 && a.at("accepted").get<uint64_t>()<=1000000000000ULL,
            "Collective state observed clock/count exceeds its bound");
        auto group=metadata;group.erase("observations");
        return Json{{"schema","openswmm.component-material-state/1"},{"group",group},
            {"accepted_steps",a.at("accepted")},{"accepted_time_s",seconds}}.dump();
    }
    static char* diagnostic()noexcept{static thread_local char text[2048]{};return text;}
    static int failure(int rc,const char* message)noexcept {
        auto* out=diagnostic();size_t n=std::min(std::char_traits<char>::length(message),size_t(2047));
        while(n && (static_cast<unsigned char>(message[n])&0xc0)==0x80)--n;
        std::copy_n(message,n,out);out[n]=0;return rc;
    }
    template<class F>static int call(F&& f)noexcept {
        diagnostic()[0]=0;
        try{f();return SWMM_OK;}
        catch(const Error& e){return failure(e.code,e.what());}
        catch(const std::bad_alloc&){return failure(SWMM_ERR_NOMEM,"Cannot allocate collective component recovery.");}
        catch(const std::invalid_argument& e){return failure(SWMM_ERR_BADPARAM,e.what());}
        catch(const std::exception& e){return failure(SWMM_ERR_INTERNAL,e.what());}
        catch(...){return failure(SWMM_ERR_INTERNAL,"Non-standard collective component recovery exception.");}
    }
    static void require(bool value,const char* why){if(!value)throw std::invalid_argument(why);}
    static void native(int rc){if(rc)throw Error(rc,swmm_split_material_checkpoint_last_error());}
    static Group& group(OpenSWMM_MaterialGroup p){require(p,"Null material group");return *reinterpret_cast<Group*>(p);}
    static State& state(OpenSWMM_MaterialState p){require(p,"Null material state");return *reinterpret_cast<State*>(p);}
    static void live(Group& g) {
        auto n=g.first.lock(),s=g.second.lock();
        require(n && s && n->available && s->available && !n->terminal && !s->terminal &&
            n->recoveryOwner==&g && s->recoveryOwner==&g && n->peer.lock()==s && s->peer.lock()==n,
            "Material recovery group is closed, failed or disconnected");
        // Lifetimes are checked BEFORE any borrowed component is dereferenced.
        require(!g.network->lifecycleActive_ && !g.surface->lifecycleActive_ && !n->busy && !s->busy,
            "Material collective lifecycle is already active");
        g.network->material_->validate();g.surface->material_->validate();
        require(g.network->status()==CS::Updated && g.surface->status()==CS::Updated &&
            g.network->time_==g.surface->time_ && !n->ended && !s->ended,
            "Material recovery requires an idle synchronized Updated pair");
    }
    struct Lease {
        MaterialPhaseLink::Operation material;
        Component::ComponentOperation network,surface;
        explicit Lease(Group& g):material(g.network->material_->operation()),
            network(g.network->lifecycleActive_),surface(g.surface->lifecycleActive_){}
    };
    static Json identity(Component& n,Component& s,bool outputs) {
        return {{"schema","openswmm.component-material-group/1"},{"network",n.id()},{"surface",s.id()},
            {"capacity",n.config_.value("material_capacity",Json(4096))},{"outputs",outputs},
            {"epoch",n.epoch_},{"duration",n.duration_},{"pollutants",n.metadata_.at("pollutants")}};
    }
    static Json enrolledMetadata(Component& c,bool outputs) {
        auto m=c.metadata_;auto& recovery=m["material_collective_recovery"];
        recovery["enrolled"]=true;recovery["outputs"]=outputs;recovery["text_reports"]=false;
        m["result_files"].erase("report");
        if(!outputs)m["result_files"]=Json::object();
        return m;
    }
    static std::string* metadataView(Component& c) {
        for(auto& out:c.outputs_)if(out->id()=="model_metadata")return &out->text;
        throw std::logic_error("Missing component metadata view");
    }
    static int enroll(void* network,void* surface,int outputs,OpenSWMM_MaterialGroup* result) {
        return call([&]{
            require(network && surface && result && (outputs==0 || outputs==1),"Invalid collective enrollment arguments");
            auto* n=dynamic_cast<Component*>(static_cast<HC::IModelComponent*>(network));
            auto* s=dynamic_cast<Component*>(static_cast<HC::IModelComponent*>(surface));
            require(n && s && n!=s && n->material_ && s->material_ && !n->material_->surface && s->material_->surface,
                "Collective recovery requires this module's network and surface components in that order");
            auto a=n->material_->request.lifetime(),b=s->material_->receipt.lifetime();
            require(n->status()==CS::Valid && s->status()==CS::Valid && !n->lifecycleActive_ && !s->lifecycleActive_ &&
                !a->prepared && !b->prepared && !a->recoveryOwner && !b->recoveryOwner &&
                a->available && b->available && !a->terminal && !b->terminal,
                "Enroll one collective owner before either component prepares");
            n->material_->validate();s->material_->validate();
            require(a->peer.lock()==b && b->peer.lock()==a && n->material_->request.provider()==&s->material_->receipt &&
                n->id()!=s->id() && !n->id().empty() && !s->id().empty() &&
                n->modelBytes_==s->modelBytes_ && n->epoch_==s->epoch_ && n->duration_==s->duration_ &&
                n->config_.value("material_capacity",Json(4096))==s->config_.value("material_capacity",Json(4096)) &&
                n->output_.lexically_normal()!=s->output_.lexically_normal(),"Material group membership/model/capacity/output ownership differs");
            require(!outputs || s->config_.value("surface_results",true),"Collective output recovery requires surface_results");
            require((!n->materialRecovery_ || n->materialRecoveryOutputs_==bool(outputs)) &&
                (!s->materialRecovery_ || s->materialRecoveryOutputs_==bool(outputs)),"Material recovery enrollment mode is already fixed");
            auto pair=n->material_->operation();
            auto g=std::make_unique<Group>(*n,*s,outputs!=0,identity(*n,*s,outputs!=0));
            auto nm=enrolledMetadata(*n,outputs),sm=enrolledMetadata(*s,outputs);
            auto nv=nm.dump(),sv=sm.dump();auto* no=metadataView(*n);auto* so=metadataView(*s);
            n->metadata_.swap(nm);s->metadata_.swap(sm);no->swap(nv);so->swap(sv);
            n->materialRecovery_=s->materialRecovery_=true;n->materialRecoveryOutputs_=s->materialRecoveryOutputs_=outputs!=0;
            a->recoveryOwner=b->recoveryOwner=g.get();*result=reinterpret_cast<OpenSWMM_MaterialGroup>(g.release());
        });
    }
    static uint32_t crc(std::string_view bytes)noexcept {
        uint32_t value=~uint32_t(0);for(unsigned char c:bytes){value^=c;for(int k=0;k<8;++k)value=(value>>1)^(0xedb88320u&uint32_t(-int(value&1)));}return ~value;
    }
    static void number(std::string& s,uint32_t n){for(int k=0;k<4;++k)s.push_back(char(n>>(8*k)));}
    static uint32_t number(std::string_view s,size_t at){uint32_t n=0;for(int k=0;k<4;++k)n|=uint32_t(static_cast<unsigned char>(s[at+k]))<<(8*k);return n;}
    static std::unique_ptr<State> decode(const void* data,size_t size) {
        require(data && size>=20 && size<=OPENSWMM_COMPONENT_RECOVERY_MAX_BYTES,"Invalid collective state input/size");
        std::string bytes(static_cast<const char*>(data),size);std::string_view all(bytes);
        const auto meta=number(all,8),payload=number(all,12);
        require(all.substr(0,8)=="OSWMGR01" && meta>0 && meta<=1024u*1024u && payload>0 &&
            payload<=SWMM_MATERIAL_CHECKPOINT_MAX_BYTES && size_t(meta)+payload==size-20 &&
            crc(all.substr(20))==number(all,16),"Collective state framing/checksum differs");
        std::vector<std::set<std::string>> keys;
        auto json=Json::parse(all.substr(20,meta),[&](int depth,Json::parse_event_t event,Json& value){
            require(depth<=16,"Collective state JSON exceeds its depth bound");
            if(event==Json::parse_event_t::object_start)keys.emplace_back();
            if(event==Json::parse_event_t::key)require(!keys.empty() && keys.back().insert(value.get<std::string>()).second,"Duplicate collective metadata key");
            if(event==Json::parse_event_t::object_end)keys.pop_back();
            return true;
        });
        require(json.is_object() && json.size()==9 && json.at("schema")=="openswmm.component-material-group/1" &&
            json.at("observations").is_array() && json.at("observations").size()==2,"Collective state metadata shape differs");
        SWMM_MaterialCheckpoint raw=nullptr;native(swmm_split_material_checkpoint_from_bytes(bytes.data()+20+meta,payload,&raw));
        Native owned(raw);return std::make_unique<State>(std::move(bytes),std::move(json),std::move(owned));
    }
    static void histories(Group& g,const MaterialPhaseLink::Observation& n,const MaterialPhaseLink::Observation& s) {
        const auto cap=g.identity.at("capacity").get<uint64_t>();
        require(n.accepted==s.accepted && n.time==s.time && n.time<=g.network->duration_ && n.observed==s.observed &&
            s.legs<=cap && s.outer[2]==s.accepted && s.observed[2]-s.empty<=s.legs && std::all_of(n.outer.begin(),n.outer.end(),[](auto v){return v==0;}),
            "Collective role observer histories differ");
    }
    static int describe(OpenSWMM_MaterialGroup value,const char** data,size_t* size) {
        return call([&]{auto& g=group(value);require(data && size,"Null group description output");*data=g.description.data();*size=g.description.size();});
    }
    static int prepare(OpenSWMM_MaterialGroup value) {
        return call([&]{auto& g=group(value);live(g);auto& n=*g.network;auto& s=*g.surface;
            require(n.time_==0 && !n.materialRecoveryEnabled_,"Collective preparation requires one fresh zero-time pair");
            Lease lease(g);n.startMaterialNetwork();n.recordMaterialState(false);s.recordMaterialState(false);
        });
    }
    static int capture(OpenSWMM_MaterialGroup value,OpenSWMM_MaterialState* result) {
        return call([&]{require(result,"Null collective state output");auto& g=group(value);live(g);Lease lease(g);
            auto& n=*g.network;auto& s=*g.surface;
            require(n.materialRecoveryEnabled_,"Prepare the collective group before capture");
            auto meta=g.identity;meta["observations"]={n.material_->observation(),s.material_->observation()};
            const auto no=n.material_->prepareObservation(meta["observations"][0]),so=s.material_->prepareObservation(meta["observations"][1]);
            histories(g,no,so);require(no.time==n.time_,"Component observer/native time differs");
            auto begin=Component::pairedStatus(n,s,CS::Updated,CS::Checkpointing),end=Component::pairedStatus(n,s,CS::Checkpointing,CS::Updated);
            begin.publish(n,s);
            std::unique_ptr<State> captured;
            try {
                SWMM_MaterialCheckpoint raw=nullptr;native(swmm_split_material_checkpoint_capture(n.engine_,&raw));Native owned(raw);
                const void* data=nullptr;size_t size=0;native(swmm_split_material_checkpoint_data(raw,&data,&size));
                auto json=meta.dump();require(json.size()<=1024u*1024u,"Collective observer metadata exceeds its bound");
                std::string payload=json;payload.append(static_cast<const char*>(data),size);
                std::string bytes="OSWMGR01";number(bytes,uint32_t(json.size()));number(bytes,uint32_t(size));number(bytes,crc(payload));bytes+=payload;
                require(bytes.size()<=OPENSWMM_COMPONENT_RECOVERY_MAX_BYTES,"Collective state exceeds its complete size bound");
                captured=std::make_unique<State>(std::move(bytes),std::move(meta),std::move(owned));
            } catch(...) {end.publish(n,s);throw;}
            end.publish(n,s);
            *result=reinterpret_cast<OpenSWMM_MaterialState>(captured.release());
        });
    }
    static Json restoredView(MaterialPhaseLink& link,const MaterialPhaseLink::Observation& o) {
        auto j=link.snapshot();j["accepted_time_s"]=o.time;j["accepted_boundaries"]=o.accepted;
        j["observed_offer"]=o.observed[1];j["observed_proof"]=o.observed[2];j["observed_ack"]=o.observed[3];
        j["applied_legs_observed"]=o.legs;j["drain_legs_observed"]=o.drain;j["spill_legs_observed"]=o.spill;
        j["empty_windows_observed"]=o.empty;j["zero_capacity_offers_observed"]=o.zeroCapacity;
        j["conductance_observed"]=o.outer[1];j["advance_observed"]=o.outer[2];j["report_observed"]=o.outer[8];
        j["last_window"]=o.last;j["last_window_historical"]=o.historical;return j;
    }
    static int restore(OpenSWMM_MaterialGroup value,OpenSWMM_MaterialState checkpoint) {
        return call([&]{auto& g=group(value);auto& saved=state(checkpoint);live(g);auto& n=*g.network;auto& s=*g.surface;
            require(n.time_==0 && s.time_==0 && n.material_->observation().at("accepted")==0 &&
                s.material_->observation().at("accepted")==0,"Collective restore requires two fresh components before any update");
            auto meta=saved.metadata;const auto observations=meta.at("observations");meta.erase("observations");
            require(meta==g.identity,"Collective stable members/model/output policy differs");
            auto no=n.material_->prepareObservation(observations.at(0)),so=s.material_->prepareObservation(observations.at(1));histories(g,no,so);
            require(n.materialRecoveryEnabled_ && !saved.inUse,"Prepare an idle collective group before restore");
            Lease lease(g);saved.inUse=true;
            struct Pin { State& s;~Pin(){s.inUse=false;} } pin{saved};
            // Stage every allocation and both read-only views before native publication.
            auto nv=restoredView(*n.material_,no).dump(),sv=restoredView(*s.material_,so).dump();
            auto* nf=n.fields_.at("material_session");auto* sf=s.fields_.at("material_session");
            auto begin=Component::pairedStatus(n,s,CS::Updated,CS::Checkpointing),end=Component::pairedStatus(n,s,CS::Checkpointing,CS::Updated);
            begin.publish(n,s);
            try {native(swmm_split_material_checkpoint_restore_at(n.engine_,saved.native.get(),no.time));}
            catch(...) {end.publish(n,s);throw;}
            n.time_=s.time_=no.time;n.material_->installObservation(std::move(no));s.material_->installObservation(std::move(so));
            nf->text.swap(nv);sf->text.swap(sv);end.publish(n,s);
        });
    }
    static int importState(const void* data,size_t size,OpenSWMM_MaterialState* result) {
        return call([&]{require(result,"Null collective imported state output");auto saved=decode(data,size);*result=reinterpret_cast<OpenSWMM_MaterialState>(saved.release());});
    }
    static int data(OpenSWMM_MaterialState value,const void** data,size_t* size) {
        return call([&]{auto& s=state(value);require(data && size,"Null collective state data output");*data=s.bytes.data();*size=s.bytes.size();});
    }
    static int describeState(OpenSWMM_MaterialState value,const char** data,size_t* size) {
        return call([&]{auto& s=state(value);require(data && size,"Null collective state description output");
            *data=s.description.data();*size=s.description.size();});
    }
    static void destroyGroup(OpenSWMM_MaterialGroup value) {
        auto* g=reinterpret_cast<Group*>(value);if(!g)return;
        for(auto& weak:{g->first,g->second})if(auto life=weak.lock();life && life->busy) {
            failure(SWMM_ERR_LIFECYCLE,"Cannot release an active material group");return;
        }
        delete g;
    }
    static void destroyState(OpenSWMM_MaterialState value) {
        auto* s=reinterpret_cast<State*>(value);if(!s)return;
        if(s->inUse){failure(SWMM_ERR_LIFECYCLE,"Cannot release an active collective state");return;}
        delete s;
    }
    static const char* lastError(){return diagnostic();}
    static const OpenSWMM_MaterialRecoveryV1* service(uint32_t version,size_t size)noexcept {
#ifndef OPENSWMM_COMPONENT_2D_RESULTS
        (void)version;(void)size;return nullptr;
#else
        static const OpenSWMM_MaterialRecoveryV1 api{sizeof(OpenSWMM_MaterialRecoveryV1),1,enroll,describe,prepare,capture,restore,importState,data,destroyGroup,destroyState,lastError};
        static const OpenSWMM_MaterialRecoveryV2 v2{{sizeof(OpenSWMM_MaterialRecoveryV2),2,enroll,describe,prepare,capture,restore,importState,data,destroyGroup,destroyState,lastError},describeState};
        if(version==2 && size>=sizeof(v2))return &v2.base;
        return version==1 && size>=sizeof(api)?&api:nullptr;
#endif
    }
};
