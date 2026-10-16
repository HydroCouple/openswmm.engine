// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
// Generic SDK-free engine wrapper. The older FVQual-specific module remains a
// separate compatibility adapter. All physical work uses the public C API.
#include "ComponentBase.hpp"
#include <hydrocouplecomponentabi.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_spatial.h>
#include <openswmm/engine/openswmm_hotstart.h>
#ifdef OPENSWMM_HAS_2D
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_gw2d.h>
#endif
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

namespace openswmm::couplers {
namespace {
using Json=nlohmann::json;
namespace fs=std::filesystem;
constexpr auto profile="hydrocouple.interval-transfer/1";
fs::path pathFromText(std::string_view value) { return fs::path(std::u8string(value.begin(),value.end())); }
std::string pathText(const fs::path& path) { const auto text=path.u8string(); return {text.begin(),text.end()}; }
std::string readFile(const fs::path& path) {
    std::ifstream file(path,std::ios::binary);
    if(!file)throw std::runtime_error("Cannot read "+pathText(path));
    return {std::istreambuf_iterator<char>(file),{}};
}
void check(SWMM_Engine engine,int code) {
    if(code)throw std::runtime_error("SWMM ("+std::to_string(code)+"): "+swmm_get_last_error_msg(engine));
}
struct EngineHandle {
    SWMM_Engine value=swmm_engine_create();
    EngineHandle() { if(!value)throw std::runtime_error("Cannot create OpenSWMM engine"); }
    ~EngineHandle() { swmm_engine_close(value); swmm_engine_destroy(value); }
};
template<class Getter> std::string readText(Getter getter) {
    std::vector<char> buffer(256);
    for(;;) {
        getter(buffer.data(),static_cast<int>(buffer.size()));
        const auto length=std::strlen(buffer.data());
        if(length<buffer.size()-1)return {buffer.data(),length};
        buffer.resize(buffer.size()*2);
    }
}
class EngineConfiguration final:public Item,public HC::IArgument {
public:
    explicit EngineConfiguration(ComponentBase* owner):Item("configuration",owner,HC::DataKind::String),owner_(owner) {
        setCaption("OpenSWMM model configuration");
        setDescription("openswmm.component/1: model, role, output_directory, optional outfalls and exchange_times_s");
    }
    ArgumentRole role() const override { return ArgumentRole::Configuration; }
    bool isOptional() const override { return false; }
    bool isReadOnly() const override { return owner_->status()!=ComponentBase::CS::Created; }
    std::string toString() const override { return text; }
    void saveData() override {}
    std::vector<std::string> fileFilters() const override { return {"OpenSWMM component configuration (*.json)"}; }
    std::vector<const std::type_info*> validComponentDataItemTypes() const override { return {}; }
    bool isValidArgType(ArgumentInputType type) const override { return type==ArgumentInputType::JSON || type==ArgumentInputType::File; }
    ArgumentInputType currentArgumentInputType() const override { return type_; }
    bool initialize(const std::string& value,ArgumentInputType type,std::string& message) override {
        try {
            if(isReadOnly() || !isValidArgType(type))throw std::invalid_argument("Set configuration before initialization using JSON or File");
            if(type==ArgumentInputType::JSON) {
                const auto parsed=Json::parse(value);
                if(parsed.is_object() && parsed.contains("schema")) {
                    if(parsed.at("schema")!="openswmm.component/1")throw std::invalid_argument("Use schema openswmm.component/1");
                    inline_=parsed; text=parsed.dump(); type_=type; message.clear(); return true;
                }
                text=parsed.at("@uri").get<std::string>();
            } else text=value;
            if(text.starts_with("file://"))text.erase(0,7);
            if(text.empty() || text.find("://")!=std::string::npos)throw std::invalid_argument("Choose a local configuration file");
            inline_=nullptr; type_=type; message.clear(); return true;
        } catch(const std::exception& error) { message=error.what(); owner_->reportError(message); return false; }
    }
    bool initialize(const HC::IComponentDataItem&,std::string& message) override {
        message="Use JSON or a local configuration file"; owner_->reportError(message); return false;
    }
    bool setValuesFrom(const HC::BufferDescriptor& buffer,std::span<const int64_t> start,std::span<const int64_t> count,std::string* message=nullptr) override {
        if(!valid(buffer,start,count,message))return false;
        if(!count[0])return true;
        std::string diagnostic;
        const bool result=initialize(*static_cast<std::string*>(buffer.data),ArgumentInputType::File,diagnostic);
        if(message)*message=diagnostic; return result;
    }
    bool serialize(ArgumentInputType type,std::string& value,std::string& message) const override {
        if(!isValidArgType(type) || (!inline_.is_null() && type!=ArgumentInputType::JSON)) {
            message="Inline configuration serializes as JSON; file configuration as JSON or File"; return false;
        }
        value=type==ArgumentInputType::JSON?(inline_.is_null()?Json{{"@uri",text}}.dump():inline_.dump()):text;
        message.clear(); return true;
    }
    Json load(fs::path& base) const {
        base=fs::absolute(pathFromText(owner_->referenceDirectory().empty()?".":owner_->referenceDirectory()));
        if(!inline_.is_null())return inline_;
        if(text.empty())throw std::invalid_argument("Choose an OpenSWMM model configuration");
        auto path=pathFromText(text); if(path.is_relative())path=base/path;
        base=path.parent_path(); return Json::parse(readFile(path));
    }
private:
    ComponentBase* owner_; Json inline_;
    ArgumentInputType type_=ArgumentInputType::JSON;
};

class OpenSWMMEngineComponent final:public ComponentBase,public HC::Temporal::ITimeModelComponent {
public:
    explicit OpenSWMMEngineComponent(HC::IModelComponentInfo* info):ComponentBase("openswmm-engine",info),configuration_(this) {}
    ~OpenSWMMEngineComponent() override { close(); }
    std::vector<HC::IArgument*> arguments() const override {
        auto result=ComponentBase::arguments();result.push_back(const_cast<EngineConfiguration*>(&configuration_));return result;
    }
    std::vector<HC::IOutput*> outputs() const override {
        std::vector<HC::IOutput*> result;for(const auto& item:outputs_)result.push_back(item.get());return result;
    }
    void initialize() override {
        try {
            transition(CS::Initializing); close(); outputs_.clear(); fields_.clear(); species_.clear(); outfalls_.clear();
            sequence_=0;time_=0;receiver_=nullptr;pendingToken_.clear();reasons_.clear();componentFiles_.clear();
            config_=configuration_.load(base_);
            if(config_.at("schema")!="openswmm.component/1")throw std::invalid_argument("Use schema openswmm.component/1");
            role_=config_.value("role",std::string("complete_model"));
            if(role_!="complete_model" && role_!="network_only")throw std::invalid_argument("Use complete_model or network_only");
            model_=resolve(config_.at("model").get<std::string>());
            output_=resolve(config_.at("output_directory").get<std::string>());
            if(config_.at("output_directory").get<std::string>().empty())throw std::invalid_argument("Choose a fresh output_directory for this instance");
            modelBytes_=readFile(model_); EngineHandle inspection;
            check(inspection.value,swmm_engine_open_metadata(inspection.value,pathText(model_).c_str()));
            discover(inspection.value);
            if(readFile(model_)!=modelBytes_)throw std::runtime_error("Model changed during metadata discovery; initialize again");
            auto* metadata=add("model_metadata",HC::DataKind::String); metadata->text=metadata_.dump();
            if(!outfalls_.empty())createTransferPorts();
            transition(CS::Initialized);
        } catch(const std::exception& error) { outputs_.clear();fields_.clear();close();fail(error.what()); }
    }
    std::vector<std::string> validate() override {
        try {
            transition(CS::Validating);
            if(!reasons_.empty())throw std::invalid_argument(reasons_.front());
            validateConnections(); transition(CS::Valid); return {};
        } catch(const std::exception& error) { reportError(error.what());transition(CS::Invalid,error.what());return {error.what()}; }
    }
    void prepare() override {
        try {
            transition(CS::Preparing);
            if(!reasons_.empty())throw std::invalid_argument(reasons_.front());
            validateConnections();
            if(readFile(model_)!=modelBytes_)throw std::runtime_error("Model changed after initialization; create a new configured instance");
            for(const auto& [path,contents]:componentFiles_)
                if(readFile(path)!=contents)throw std::runtime_error("Process configuration changed after initialization; create a new configured instance: "+pathText(path));
            if(!fs::create_directories(output_))throw std::invalid_argument("output_directory already exists; choose a fresh directory for each run/instance");
            engine_=swmm_engine_create();if(!engine_)throw std::runtime_error("Cannot create runtime engine");
            check(engine_,swmm_engine_open(engine_,pathText(model_).c_str(),pathText(output_/"model.rpt").c_str(),pathText(output_/"model.out").c_str(),nullptr));
            if(role_=="network_only")check(engine_,swmm_options_set(engine_,"IGNORE_2D","YES"));
            check(engine_,swmm_engine_initialize(engine_));check(engine_,swmm_engine_start(engine_,1));
            for(auto& outfall:outfalls_) {
                outfall.amounts.assign(species_.size(),0);
                check(engine_,swmm_node_get_export_receipt(engine_,outfall.node,&outfall.receipt,outfall.amounts.data(),static_cast<int>(outfall.amounts.size())));
                if((outfall.receipt.flags&7u)!=7u)throw std::invalid_argument("Selected outfall requires qualified LEGACY pollutant and native heat receipts");
            }
            write("configuration.json",config_);write("metadata.json",metadata_);
            transition(CS::Updated);
        } catch(const std::exception& error) { fail(error.what());close(); }
    }
    void update(const std::vector<HC::IOutput*>& ={}) override {
        if(status()!=CS::Updated) { reportError("Engine component is not ready to update");return; }
        try {
            verifyReceipt();transition(CS::Updating);
            if(outfalls_.empty()) {
                double elapsed=0;check(engine_,swmm_engine_step(engine_,&elapsed));
                check(engine_,swmm_engine_get_elapsed_seconds(engine_,&time_));
                transition(elapsed==0?CS::Done:CS::Updated);return;
            }
            const double end=times_.at(sequence_);double actual=0;
            const int code=swmm_engine_advance_to(engine_,end,&actual);
            recordNativeBoundary(end,code);check(engine_,code);
            if(actual!=end)throw std::runtime_error("Engine did not reach the agreed interval endpoint");
            std::vector<double> amounts(2+species_.size(),0);
            for(auto& outfall:outfalls_) {
                SWMM_OutfallExportReceipt receipt{};std::vector<double> mass(species_.size());
                check(engine_,swmm_node_get_export_receipt(engine_,outfall.node,&receipt,mass.data(),static_cast<int>(mass.size())));
                if(receipt.reversed_m3!=outfall.receipt.reversed_m3)throw std::runtime_error("Reverse outfall flow needs a bidirectional contract; this profile stopped");
                amounts[0]+=receipt.exported_m3-outfall.receipt.exported_m3;
                amounts[1]+=receipt.exported_heat_j-outfall.receipt.exported_heat_j;
                for(size_t i=0;i<mass.size();++i)amounts[i+2]+=mass[i]-outfall.amounts[i];
                outfall.receipt=receipt;outfall.amounts=std::move(mass);
            }
            for(size_t i=0;i<amounts.size();++i)if(!std::isfinite(amounts[i]) || (i!=1 && amounts[i]<0))throw std::runtime_error("Invalid native export amount");
            pendingToken_=id()+":"+std::to_string(sequence_+1);pending_=amounts;
            Json row={{"schema",profile},{"transfer_id",pendingToken_},{"instance_id",id()},
                {"epoch_julian_day",epoch_},{"start_s",time_},{"end_s",end},{"ownership","already_debited"},{"amounts",amounts}};
            write("export_"+std::to_string(sequence_)+".json",row);
            fields_.at("transfer_id")->text=pendingToken_;
            fields_.at("epoch_julian_day")->number=epoch_;fields_.at("interval_start_s")->number=time_;fields_.at("interval_end_s")->number=end;
            for(size_t i=0;i<amounts.size();++i) {
                fields_.at(amountField(i))->number=amounts[i];fields_.at("exported."+amountField(i))->number+=amounts[i];
                fields_.at("unreceived."+amountField(i))->number=receiver_?amounts[i]:0;
            }
            time_=end;++sequence_;transition(sequence_==times_.size()?CS::Done:CS::Updated);
        } catch(const std::exception& error) { fail(error.what());close(); }
    }
    void finish() override {
        if(status()==CS::Finished)return;
        try {
            const bool failed=status()==CS::Failed;
            if(!failed)verifyReceipt();
            transition(CS::Finishing);
            if(engine_ && !failed) { check(engine_,swmm_engine_end(engine_));check(engine_,swmm_engine_report(engine_)); }
            close();transition(CS::Finished);
        } catch(const std::exception& error) { fail(error.what());close(); }
    }
    HC::Temporal::IDateTime* currentDateTime() const override { clock_.day=epoch_+time_/86400.;return &clock_; }
    HC::Temporal::ITimeSpan* simulationPeriod() const override { period_.day=epoch_;period_.span=duration_/86400.;return &period_; }
    double nextDateTimeJulianDay() const override {
        if(status()!=CS::Updated)return epoch_+time_/86400.;
        return epoch_+(outfalls_.empty()?std::min(duration_,time_+routingStep_):times_.at(sequence_))/86400.;
    }
private:
    struct Outfall { int node;SWMM_OutfallExportReceipt receipt{};std::vector<double> amounts; };
    EngineConfiguration configuration_;
    SWMM_Engine engine_=nullptr;HC::IModelComponent* receiver_=nullptr;
    std::vector<std::unique_ptr<Output>> outputs_;std::map<std::string,Output*> fields_;
    Json config_,metadata_;fs::path base_,model_,output_;
    std::string role_,modelBytes_,pendingToken_;
    std::map<fs::path,std::string> componentFiles_;
    std::vector<std::string> species_,reasons_;std::vector<Outfall> outfalls_;
    std::vector<double> times_,pending_;
    size_t sequence_=0;double epoch_=2440587.5,duration_=0,time_=0,routingStep_=0;
    mutable Clock clock_,period_;
    fs::path resolve(const std::string& value) const { const auto p=pathFromText(value);return fs::absolute(p.is_absolute()?p:base_/p); }
    void close() noexcept { if(engine_) { swmm_engine_close(engine_);swmm_engine_destroy(engine_);engine_=nullptr; } }
    void fail(const std::string& message) { if(status()!=CS::Failed && status()!=CS::Finished)transition(CS::Failed,message); }
    void write(const std::string& name,const Json& value) const {
        std::ofstream file(output_/name);file<<value.dump(2)<<'\n';if(!file)throw std::runtime_error("Cannot write component boundary ledger");
    }
    std::string option(SWMM_Engine engine,const char* key) const {
        return readText([&](char* buffer,int length){check(engine,swmm_options_get(engine,key,buffer,length));});
    }
    std::string amountField(size_t index) const { return index==0?"water_m3":index==1?"enthalpy_j":"species."+species_.at(index-2); }
    Output* add(const std::string& id,HC::DataKind type=HC::DataKind::Float64,std::string unit="unitless",double l=0,double m=0,double t=0,HC::ValueKind kind=HC::ValueKind::Extensive,double factor=1) {
        auto item=std::make_unique<Output>(id,this,type,unit,l,m,t,kind,factor);auto* result=item.get();
        result->setDescription(std::string(profile)+";field="+id);outputs_.push_back(std::move(item));return result;
    }
    void createTransferPorts() {
        fields_["transfer_id"]=add("transfer_id",HC::DataKind::String);
        for(const auto& field:{std::string("epoch_julian_day"),std::string("interval_start_s"),std::string("interval_end_s")})
            fields_[field]=add(field,HC::DataKind::Float64,field=="epoch_julian_day"?"day":"s",0,0,1,HC::ValueKind::Intensive,field=="epoch_julian_day"?86400:1);
        for(size_t i=0;i<species_.size()+2;++i)for(const auto& prefix:{"","exported.","unreceived."}) {
            const std::string field=prefix+amountField(i);
            fields_[field]=i==0?add(field,HC::DataKind::Float64,"m3",3):i==1?add(field,HC::DataKind::Float64,"J",2,1,-2):add(field,HC::DataKind::Float64,"kg",0,1);
        }
    }
    void discover(SWMM_Engine engine);
    void validateConnections();
    void verifyReceipt();
    void recordNativeBoundary(double end,int code) const;
};

void OpenSWMMEngineComponent::discover(SWMM_Engine engine) {
    double start=0,end=0;check(engine,swmm_get_start_time(engine,&start));check(engine,swmm_get_end_time(engine,&end));
    epoch_=start+2415018.5;duration_=std::round((end-start)*86400000.)/1000.;
    check(engine,swmm_get_routing_step(engine,&routingStep_));
    if(!std::isfinite(duration_) || duration_<=0)throw std::invalid_argument("Model must have a positive finite duration");
    metadata_={{"schema","openswmm.component-metadata/1"},{"instance_id",id()},{"role",role_},
        {"epoch_julian_day",epoch_},{"duration_s",duration_},{"model",pathText(model_)},
        {"nodes",Json::array()},{"links",Json::array()},{"pollutants",Json::array()},
        {"reaction_species",Json::array()},{"transport",Json::array()},
        {"process_components",Json::array()},{"subcatchment_count",swmm_subcatch_count(engine)},
        {"quality_solver",option(engine,"QUALITY_SOLVER")},{"flow_units",option(engine,"FLOW_UNITS")},
        {"checkpointing",false},{"cloneable",false},{"legacy_aquifer_count",swmm_aquifer_count(engine)}};
    for(int i=0;i<swmm_node_count(engine);++i) {
        int type=0;check(engine,swmm_node_get_type(engine,i,&type));
        metadata_["nodes"].push_back({{"id",swmm_node_id(engine,i)},{"type",type}});
    }
    for(int i=0;i<swmm_link_count(engine);++i) {
        int from=-1,to=-1;check(engine,swmm_link_get_from_node(engine,i,&from));check(engine,swmm_link_get_to_node(engine,i,&to));
        metadata_["links"].push_back({{"id",swmm_link_id(engine,i)},
            {"from",swmm_node_id(engine,from)},{"to",swmm_node_id(engine,to)}});
    }
    const int np=swmm_pollutant_count(engine);
    for(int i=0;i<np;++i) {
        int units=-1;check(engine,swmm_pollutant_get_units(engine,i,&units));
        const std::string name=swmm_pollutant_id(engine,i);species_.push_back(name);
        metadata_["pollutants"].push_back({{"id",name},{"concentration_units",units==0?"mg/L":units==1?"ug/L":"#/L"},
            {"export_amount_units",units==2?"counts":"kg"}});
    }
    const int nr=swmm_reaction_species_count(engine);
    for(int i=0;i<nr;++i) {
        std::vector<char> name(256),units(128);int wall=0;double atol=0,rtol=0;
        for(;;) {
            check(engine,swmm_reaction_species_get(engine,i,name.data(),static_cast<int>(name.size()),&wall,units.data(),static_cast<int>(units.size()),&atol,&rtol));
            const bool fullName=std::strlen(name.data())==name.size()-1,fullUnits=std::strlen(units.data())==units.size()-1;
            if(!fullName && !fullUnits)break;
            if(fullName)name.resize(name.size()*2);if(fullUnits)units.resize(units.size()*2);
        }
        metadata_["reaction_species"].push_back({{"id",name.data()},{"kind",wall?"WALL":"BULK"},
            {"declared_units",units.data()},{"atol",atol},{"rtol",rtol}});
    }
    SWMM_TransportMatrix matrix{};check(engine,swmm_get_transport_matrix(engine,&matrix));
    for(int domain=0;domain<SWMM_TRANSPORT_DOMAIN_COUNT;++domain)for(int cls=0;cls<SWMM_TRANSPORT_CLASS_COUNT;++cls) {
        const auto& cell=matrix.cell[domain][cls];metadata_["transport"].push_back({
            {"domain",swmm_transport_domain_name(domain)},{"class",swmm_transport_class_name(cls)},
            {"state",cell.state},{"count",cell.count},{"reason",cell.reason}});
    }
    for(int i=0;i<swmm_process_component_count(engine);++i) {
        char ignored[1];
        const auto id=readText([&](char* b,int n){check(engine,swmm_process_component_get(engine,i,b,n,ignored,1,ignored,1));});
        const auto path=readText([&](char* b,int n){check(engine,swmm_process_component_get(engine,i,ignored,1,ignored,1,b,n));});
        metadata_["process_components"].push_back({{"id",id},{"configuration_file",path}});
        if(!path.empty())componentFiles_[pathFromText(path)]=readFile(pathFromText(path));
    }
    if(role_=="network_only" && swmm_subcatch_count(engine)>0)
        reasons_.push_back("network_only requires a model without subcatchments; use complete_model to retain native watershed ownership");
    int pluginCount=0;check(engine,swmm_plugins_count(engine,&pluginCount));
    if(pluginCount)reasons_.push_back("Runtime [PLUGINS] require separate configured-metadata qualification in this component profile");
    int vertices=0,spatialRows=0;bool spatialConfigured=false;std::string groundwaterMode="unavailable";
#ifdef OPENSWMM_HAS_2D
    (void)swmm_2d_vertex_count(engine,&vertices);check(engine,swmm_gw2d_row_count(engine,&spatialRows));
    groundwaterMode=readText([&](char* b,int n){check(engine,swmm_gw2d_option_get(engine,"MODE",b,n));});
    spatialConfigured=readText([&](char* b,int n){check(engine,swmm_gw2d_option_get(engine,"CONFIGURED",b,n));})=="YES";
#endif
    metadata_["surface_vertex_count"]=vertices;metadata_["explicit_groundwater_row_count"]=spatialRows;
    metadata_["explicit_groundwater_mode"]=groundwaterMode;metadata_["explicit_groundwater_configured"]=spatialConfigured;
    if(vertices || spatialConfigured || spatialRows)reasons_.push_back("Spatial surface/groundwater execution is pending qualification; use native execution for this model");
    // Retain ordinary model input files. Additional authored output channels
    // need their own isolated-output mapping before independent instances run.
    auto fileField=[&](const char* key){return readText([&](char* b,int n){check(engine,swmm_files_get(engine,key,b,n));});};
    if(!fileField("OUTFLOWS_PATH").empty())reasons_.push_back("Model-declared SAVE OUTFLOWS needs output-path isolation before component execution");
    int hotstarts=0;check(engine,swmm_hotstart_saves_count(engine,&hotstarts));
    if(hotstarts)reasons_.push_back("Model-declared SAVE HOTSTART needs output-path isolation before component execution");
    for(const char* mode:{"RAINFALL_MODE","RUNOFF_MODE","RDII_MODE"})
        if(fileField(mode)=="SAVE")reasons_.push_back(std::string("Model-declared ")+mode+" SAVE needs output-path isolation");
    for(int i=0;i<swmm_lid_usage_count(engine);++i) {
        const auto owner=std::to_string(i);
        char ignored[1];
        const auto path=readText([&](char* b,int n){check(engine,swmm_file_path_get(engine,SWMM_FILE_LID_REPORT,owner.c_str(),ignored,1,b,n));});
        if(!path.empty())reasons_.push_back("Detailed LID reports need output-path isolation before component execution");
    }
#ifdef OPENSWMM_HAS_2D
    char ignored[1];
    const auto surfacePath=readText([&](char* b,int n){check(engine,swmm_file_path_get(engine,SWMM_FILE_OUTPUT_2D,nullptr,ignored,1,b,n));});
    if(!surfacePath.empty())reasons_.push_back("Model-declared 2D results need output-path isolation before component execution");
#endif
    const auto selected=config_.value("outfalls",Json::array());
    if(!selected.is_array())throw std::invalid_argument("outfalls must be an array of stable native outfall IDs");
    std::set<int> seen;
    for(const auto& value:selected) {
        const std::string name=value.get<std::string>();const int node=swmm_node_index(engine,name.c_str());
        int type=-1,route=-1;
        if(node<0 || !seen.insert(node).second)throw std::invalid_argument("Unknown or repeated outfall: "+name);
        check(engine,swmm_node_get_type(engine,node,&type));
        if(type!=SWMM_NODE_OUTFALL)throw std::invalid_argument("Selected export is not an outfall: "+name);
        check(engine,swmm_node_get_outfall_route_to(engine,node,&route));
        if(route>=0)throw std::invalid_argument("Selected outfall is retained by a native subcatchment: "+name);
        outfalls_.push_back({node,{},{}});
    }
    if(!outfalls_.empty()) {
        if(nr)reasons_.push_back("MSX outfall amount receipts are not yet qualified; MSX remains supported in uncoupled native output");
        if(option(engine,"QUALITY_SOLVER")!="LEGACY" || option(engine,"IGNORE_QUALITY")=="YES")
            reasons_.push_back("Outfall export requires the qualified LEGACY quality profile");
        if(option(engine,"HEAT_TRANSPORT")!="YES")reasons_.push_back("This interval profile requires native HEAT_TRANSPORT for enthalpy receipts");
        for(const auto& row:metadata_["pollutants"]) {
            const auto name=row.at("id").get<std::string>();
            if(row.at("export_amount_units")!="kg")reasons_.push_back("Count-based pollutant "+name+" needs an explicit physical-mass conversion");
            if(name.empty() || !std::all_of(name.begin(),name.end(),[](unsigned char c){
                return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.';
            }))reasons_.push_back("Pollutant ID is outside the interval reference profile: "+name);
        }
        times_=config_.at("exchange_times_s").get<std::vector<double>>();double last=0;
        for(double time:times_) {
            if(!std::isfinite(time) || time<=last || time>duration_+1e-6)throw std::invalid_argument("Exchange times must increase from zero within the model duration");
            last=time;
        }
        if(times_.empty() || std::abs(times_.back()-duration_)>1e-6)throw std::invalid_argument("The final exchange must equal the native model duration");
        times_.back()=duration_;
    } else times_.clear();
    metadata_["selected_outfalls"]=selected;metadata_["interval_profile"]=outfalls_.empty()?Json(nullptr):Json(profile);
    metadata_["execution_restrictions"]=reasons_;metadata_["runnable"]=reasons_.empty();
}

void OpenSWMMEngineComponent::validateConnections() {
    receiver_=nullptr;if(outfalls_.empty())return;
    std::vector<Output*> ports;
    for(const auto& name:{"transfer_id","epoch_julian_day","interval_start_s","interval_end_s"})ports.push_back(fields_.at(name));
    for(size_t i=0;i<species_.size()+2;++i)ports.push_back(fields_.at(amountField(i)));
    bool connected=false;
    for(auto* output:ports)connected=connected || !output->consumers().empty() || !output->adaptedOutputs().empty();
    if(!connected)return;
    for(auto* output:ports) {
        const auto consumers=output->consumers();
        if(consumers.size()!=1 || !output->adaptedOutputs().empty() || consumers[0]->id()!=output->id() || consumers[0]->provider()!=output)
            throw std::invalid_argument("Connect each interval field once to one direct receiver");
        auto* owner=consumers[0]->modelComponent();
        if(!owner || owner==this || (receiver_ && receiver_!=owner))throw std::invalid_argument("A complete interval tuple must have one receiving owner");
        receiver_=owner;
    }
    auto require=[&](const std::string& name,HC::DataKind kind)->HC::IOutput* {
        for(auto* item:receiver_->outputs())if(item && item->id()==name && item->shape()==std::vector<int64_t>{1} && item->dataKind()==kind)return item;
        throw std::invalid_argument("Receiver must publish scalar "+name);
    };
    require("accepted_transfer_id",HC::DataKind::String);
    for(size_t i=0;i<species_.size()+2;++i) {
        const auto field=amountField(i);const auto* output=require("accepted."+field,HC::DataKind::Float64);
        const auto* expected=dynamic_cast<HC::IQuantity*>(fields_.at(field)->valueDefinition());
        const auto* actual=dynamic_cast<HC::IQuantity*>(output->valueDefinition());
        if(!actual || !actual->unit() || !actual->unit()->dimensions() || actual->valueKind()!=expected->valueKind() ||
            actual->unit()->caption()!=expected->unit()->caption() || actual->unit()->conversionFactorToSI()!=1 || actual->unit()->offsetToSI()!=0)
            throw std::invalid_argument("Receiver acknowledgment has incompatible units/basis: "+field);
        for(int axis=0;axis<=int(HC::IUnitDimensions::FundamentalUnitDimension::PlaneAngle);++axis) {
            const auto dimension=static_cast<HC::IUnitDimensions::FundamentalUnitDimension>(axis);
            if(actual->unit()->dimensions()->power(dimension)!=expected->unit()->dimensions()->power(dimension))
                throw std::invalid_argument("Receiver acknowledgment has incompatible dimensions: "+field);
        }
    }
}

void OpenSWMMEngineComponent::verifyReceipt() {
    if(!receiver_ || pendingToken_.empty())return;
    auto read=[&](const std::string& name,HC::DataKind kind,void* value,int64_t size) {
        const int64_t one=1,zero=0;HC::BufferDescriptor buffer{value,kind,1,&one,nullptr,size};std::string diagnostic;
        for(auto* item:receiver_->outputs())if(item && item->id()==name) {
            if(!item->getValuesInto(buffer,{&zero,1},{&one,1},&diagnostic))throw std::runtime_error("Cannot read receiver receipt: "+diagnostic);
            return;
        }
        throw std::runtime_error("Receiver receipt is missing: "+name);
    };
    std::string token;read("accepted_transfer_id",HC::DataKind::String,&token,sizeof(token));
    if(token!=pendingToken_ || receiver_->status()==CS::Failed)throw std::runtime_error("The preceding native export has no valid matching receipt; engine advancement stopped");
    std::vector<double> accepted(pending_.size()),remaining(pending_.size());
    for(size_t i=0;i<accepted.size();++i)read("accepted."+amountField(i),HC::DataKind::Float64,&accepted[i],sizeof(double));
    if(!std::isfinite(accepted[0]) || accepted[0]<0 || accepted[0]>pending_[0])throw std::runtime_error("Receiver accepted an invalid water volume");
    const double fraction=pending_[0]>0?accepted[0]/pending_[0]:1;
    for(size_t i=0;i<accepted.size();++i) {
        if(!std::isfinite(accepted[i]) || accepted[i]!=pending_[i]*fraction)throw std::runtime_error("Receiver receipt changed native species/energy proportions");
        remaining[i]=pending_[i]-accepted[i];
    }
    for(size_t i=0;i<remaining.size();++i)fields_.at("unreceived."+amountField(i))->number=remaining[i];
    write("accepted_"+std::to_string(sequence_-1)+".json",{{"transfer_id",token},{"receiver",receiver_->id()},
        {"ownership","native_export_already_debited"},{"accepted_amounts",accepted},{"unreceived_amounts",remaining}});
    if(fraction!=1)throw std::runtime_error("Receiver refused part of an already-debited native export; residual is recorded as unreceived and this run stopped");
    pendingToken_.clear();pending_.clear();
}

void OpenSWMMEngineComponent::recordNativeBoundary(double end,int code) const {
    Json rows=Json::array();double elapsed=time_;(void)swmm_engine_get_elapsed_seconds(engine_,&elapsed);
    for(const auto& outfall:outfalls_) {
        SWMM_OutfallExportReceipt receipt{};std::vector<double> mass(species_.size());
        const int result=swmm_node_get_export_receipt(engine_,outfall.node,&receipt,mass.data(),static_cast<int>(mass.size()));
        rows.push_back({{"outfall",swmm_node_id(engine_,outfall.node)},{"code",result},{"flags",receipt.flags},
            {"exported_water_m3",receipt.exported_m3},{"reversed_water_m3",receipt.reversed_m3},
            {"exported_enthalpy_j",receipt.exported_heat_j},{"species_kg",mass}});
    }
    write("native_boundary_"+std::to_string(sequence_)+".json",{{"requested_end_s",end},{"elapsed_s",elapsed},{"advance_code",code},{"outfalls",rows}});
}

class OpenSWMMEngineInfo final:public Identity,public HC::IModelComponentInfo {
public:
    OpenSWMMEngineInfo():Identity("org.hydrocouple.openswmm.engine","OpenSWMM engine") {
        setDescription("SDK-free native engine with configured metadata and typed external-outfall exports. Initial CPU 1D profile; spatial execution/restart remain separate qualifications.");
    }
    std::string libraryFilePath() const override { return library_; }
    void setLibraryFilePath(const std::string& value) override { library_=value; }
    std::string iconFilePath() const override { return {}; }
    std::string developer() const override { return "Caleb Buahin"; }
    std::vector<std::string> documentation() const override { return {}; }
    std::string license() const override { return "Apache-2.0"; }
    std::string copyright() const override { return "Copyright 2026 Caleb Buahin"; }
    std::string url() const override { return "https://hydrocouple.org/projects/openswmm"; }
    std::string email() const override { return {}; }
    std::string version() const override { return "6.0.0-alpha.4"; }
    std::set<std::string> tags() const override { return {"SWMM","native","configured-metadata","outfall-receipts"}; }
    std::unique_ptr<HC::IModelComponent> createComponentInstance() override { return std::make_unique<OpenSWMMEngineComponent>(this); }
    std::vector<HC::IAdaptedOutputFactory*> adaptedOutputFactories() const override { return {}; }
private:
    std::string library_;
};
}
}
HYDROCOUPLE_DECLARE_COMPONENT(openswmm::couplers::OpenSWMMEngineInfo)
