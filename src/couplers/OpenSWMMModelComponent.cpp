// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "ComponentBase.hpp"
#include <hydrocouplecomponentabi.h>
#include <hydrocouplespatial.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_pollutants.h>
#include <openswmm/engine/openswmm_reactions.h>
#include <openswmm/engine/openswmm_model.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace openswmm::couplers {
using Json=nlohmann::json;
namespace fs=std::filesystem;
class Configuration final : public Item, public HC::IArgument {
public:
    Json inlineConfiguration;
    explicit Configuration(HC::IModelComponent* c) : Item("configuration",c,HC::DataKind::String) {
        setCaption("SWMM model and reservoir mapping");
        setDescription("Model file, elapsed exchange times, ownership, explicit outfalls/columns, temperature basis and species mapping.");
    }
    ArgumentRole role() const override { return ArgumentRole::Configuration; }
    bool isOptional() const override { return false; }
    bool isReadOnly() const override { return false; }
    std::string toString() const override { return text; }
    void saveData() override {}
    std::vector<std::string> fileFilters() const override { return {"SWMM coupling setup (*.json)"}; }
    std::vector<const std::type_info*> validComponentDataItemTypes() const override { return {}; }
    bool isValidArgType(ArgumentInputType t) const override { return t==ArgumentInputType::JSON || t==ArgumentInputType::File; }
    ArgumentInputType currentArgumentInputType() const override { return type_; }
    bool initialize(const std::string& value,ArgumentInputType type,std::string& message) override {
        try {
            if(!isValidArgType(type)) throw std::invalid_argument("Use a local coupling setup file");
            auto candidate=value;
            if(type==ArgumentInputType::JSON) {
                const auto j=Json::parse(value);
                if (j.is_object() && j.value("schema",std::string()) == "openswmm.reservoir-coupling/1.0") {
                    if (j.at("ownership") != "swmm-network/fvqual-receiver" || j.at("role") != "network_only")
                        throw std::invalid_argument("Declare separate SWMM network and FVQual receiver ownership");
                    inlineConfiguration = j; text.clear(); type_ = type; message.clear(); return true;
                }
                candidate=j.contains("@uri")?j.at("@uri").get<std::string>():
                    (j.is_object()?j.at("values"):j).at(0).get<std::string>();
            }
            if(candidate.starts_with("file://")) candidate.erase(0,7);
            if(candidate.find("://")!=std::string::npos) throw std::invalid_argument("Remote configuration is not supported");
            inlineConfiguration = nullptr; text=std::move(candidate); type_=type; message.clear(); return true;
        } catch(const std::exception& e) { message=e.what(); return false; }
    }
    bool setValuesFrom(const HC::BufferDescriptor& buffer,std::span<const int64_t> start,
        std::span<const int64_t> count,std::string* message=nullptr) override {
        if(!valid(buffer,start,count,message))return false;
        if(count[0]==0)return true;
        std::string result;
        const bool ok=initialize(*static_cast<const std::string*>(buffer.data),ArgumentInputType::File,result);
        if(message)*message=result;return ok;
    }
    bool initialize(const HC::IComponentDataItem&,std::string& message) override { message="Use a setup file"; return false; }
    bool serialize(ArgumentInputType t,std::string& value,std::string& message) const override {
        if(!isValidArgType(t)) { message="Unsupported argument format"; return false; }
        if (!inlineConfiguration.is_null()) {
            if (t != ArgumentInputType::JSON) { message="Inline configuration serializes as JSON"; return false; }
            value=inlineConfiguration.dump(); message.clear(); return true;
        }
        value=t==ArgumentInputType::JSON?Json{{"@uri",text}}.dump():text; message.clear(); return true;
    }
private: ArgumentInputType type_=ArgumentInputType::JSON;
};
class OpenSWMMModelComponent final : public ComponentBase, public HC::Temporal::ITimeModelComponent {
public:
    explicit OpenSWMMModelComponent(HC::IModelComponentInfo* info)
        : ComponentBase("openswmm",info),configuration_(this),frame_("outfall_transfers",this,HC::DataKind::String),
          water_("exported_water_m3",this,HC::DataKind::Float64,"m3",3),
          heat_("exported_heat_j",this,HC::DataKind::Float64,"J",2,1,-2) {
        setCaption("OpenSWMM network");
        frame_.setCaption("Outfall transfers awaiting receiver receipt");
        frame_.setDescription("hydrocouple.integrated-inflow/1.0; interval amounts already debited by SWMM. One acknowledged receiver only.");
        water_.setCaption("Cumulative mapped outfall water"); heat_.setCaption("Cumulative mapped outfall heat");
    }
    ~OpenSWMMModelComponent() override { close(); }
    std::vector<HC::IArgument*> arguments() const override {
        auto result=ComponentBase::arguments(); result.push_back(const_cast<Configuration*>(&configuration_)); return result;
    }
    std::vector<HC::IOutput*> outputs() const override {
        std::vector<HC::IOutput*> out{const_cast<Output*>(&frame_),const_cast<Output*>(&water_),const_cast<Output*>(&heat_)};
        for(const auto& item:massOutputs_)out.push_back(item.get()); return out;
    }
    void initialize() override {
        try {
            transition(CS::Initializing); close(); config_=Json(); frames_=Json::array();
            sequence_=0; time_=0; water_.number=0; heat_.number=0; frame_.text.clear();
            massOutputs_.clear(); nativeTargets_.clear(); mapped_.clear(); output_.clear(); times_.clear(); transition(CS::Initialized);
        } catch(const std::exception& e) { fail(e.what()); }
    }
    std::vector<std::string> validate() override {
        try { transition(CS::Validating); loadConfiguration(); transition(CS::Valid); return {}; }
        catch(const std::exception& e) { transition(CS::Invalid,e.what()); return {e.what()}; }
    }
    void prepare() override {
        try {
            transition(CS::Preparing); loadConfiguration();
            if(frame_.consumers().size()>1 || !frame_.adaptedOutputs().empty())
                throw std::invalid_argument("Outfall transfer tuples require one direct acknowledged receiver");
            for(auto* input:frame_.consumers()) {
                if(input->id()!="integrated_inflows") throw std::invalid_argument("Use the FVQual integrated_inflows receiver");
                auto* receiver=dynamic_cast<HC::Temporal::ITimeModelComponent*>(input->modelComponent());
                if(!receiver) throw std::invalid_argument("The receiver must declare physical time");
                if(std::abs(receiver->simulationPeriod()->julianDay()-epoch_)>1.e-10)
                    throw std::invalid_argument("SWMM and receiver epochs differ");
                if(std::abs(receiver->simulationPeriod()->duration()*86400.0-times_.back())>1.e-6)
                    throw std::invalid_argument("SWMM and receiver exchange horizons differ");
                bool columns=false,ack=false;
                for(auto* output:input->modelComponent()->outputs()) {
                    if(output->id()=="temperature" && output->shape().size()==2) {
                        columns=output->shape()[0]==config_.at("receiver_columns").get<int64_t>();
                        const auto* spatial=dynamic_cast<HC::Spatial::IPolyhedralSurfaceComponentDataItem*>(output);
                        if(!spatial || !spatial->polyhedralSurface() || !spatial->polyhedralSurface()->meshView())
                            throw std::invalid_argument("Receiver must publish its mesh for mapping verification");
                        const auto* mesh=spatial->polyhedralSurface()->meshView();
                        const Json geometry={{"x",mesh->nodeX()},{"y",mesh->nodeY()},{"z",mesh->nodeZ()},
                            {"offsets",mesh->faceNodeOffsets()},{"nodes",mesh->faceNodes()}};
                        if(geometry!=config_.at("receiver_geometry"))
                            throw std::invalid_argument("Receiver mesh geometry/order differs from the explicit mapping");
                    }
                    if(output->id()=="accepted_transfer_id") ack=output->dataKind()==HC::DataKind::String && output->shape()==std::vector<int64_t>{1};
                }
                if(!columns || !ack) throw std::invalid_argument("Receiver columns or transfer acknowledgment do not match the mapping");
            }
            static std::atomic<unsigned> serial{0};
            const auto stamp=std::chrono::system_clock::now().time_since_epoch().count();
            output_=base_/config_.value("output_directory",std::string("swmm_output"));
            output_/=std::to_string(stamp)+"-"+std::to_string(++serial);
            fs::create_directories(output_);
            engine_=swmm_engine_create(); if(!engine_) throw std::runtime_error("Cannot create SWMM engine");
            const auto model=resolve(config_.at("model").get<std::string>());
            check(swmm_engine_open(engine_,model.string().c_str(),(output_/"donor.rpt").string().c_str(),
                (output_/"donor.out").string().c_str(),nullptr));
            // This role is explicit: native runoff/network are retained, while
            // the source's integrated surface is disabled before initialization.
            if(config_.at("role")=="network_only") check(swmm_options_set(engine_,"IGNORE_2D","YES"));
            check(swmm_engine_initialize(engine_)); check(swmm_engine_start(engine_,1));
            double start; check(swmm_get_start_time(engine_,&start));
            // SWMM DateTime is days since 1899-12-30, not a Julian day.
            if(std::abs(start+2415018.5-epoch_)>1.e-10) throw std::invalid_argument("Configured epoch does not match SWMM model start");
            double end; check(swmm_get_end_time(engine_,&end));
            if(times_.back()>(end-start)*86400.0+1.e-6) throw std::invalid_argument("Exchange horizon exceeds SWMM duration");
            if(swmm_reaction_species_count(engine_)>0) throw std::invalid_argument("MSX outfall export is not qualified by this component");
            const int np=swmm_pollutant_count(engine_);
            const auto species=config_.value("species",Json::object());
            if(static_cast<int>(species.size())!=np) throw std::invalid_argument("Map every native pollutant explicitly");
            std::set<std::string> targets;
            for(int p=0;p<np;++p) {
                const std::string native=swmm_pollutant_id(engine_,p);
                const auto target=species.at(native).get<std::string>();
                int units; check(swmm_pollutant_get_units(engine_,p,&units));
                if(units==2 || target.empty() || !targets.insert(target).second)
                    throw std::invalid_argument("Use unique kg-based species; count concentrations need a separate physical conversion");
                nativeTargets_.push_back(target);
                auto item=std::make_unique<Output>("exported_species_"+target+"_kg",this,HC::DataKind::Float64,"kg",0,1);
                item->setCaption("Cumulative exported "+target); massOutputs_.push_back(std::move(item));
            }
            mapped_.clear(); std::set<int> nodes;
            for(const auto& entry:config_.at("outfalls")) {
                const auto id=entry.at("id").get<std::string>(); const int n=swmm_node_index(engine_,id.c_str());
                if(n<0 || !nodes.insert(n).second) throw std::invalid_argument("Unknown or duplicate mapped outfall: "+id);
                Mapped m; m.node=n; m.definition=entry; m.amounts.resize(np);
                check(swmm_node_get_export_receipt(engine_,n,&m.receipt,m.amounts.data(),np));
                if(np && !(m.receipt.flags&2)) throw std::invalid_argument("Only LEGACY pollutant export accounting is qualified");
                if(entry.at("temperature").at("mode")=="native" && !(m.receipt.flags&4))
                    throw std::invalid_argument("Native outfall heat is unavailable; explicitly select a fixed temperature assumption");
                mapped_.push_back(std::move(m));
            }
            writeJson(output_/"configuration.json",config_);
            transition(CS::Updated,"SWMM prepared with explicit one-way outfall ownership");
        } catch(const std::exception& e) { fail(e.what()); close(); }
    }
    void update(const std::vector<HC::IOutput*>& ={}) override {
        try {
            if(status()!=CS::Updated || !engine_ || sequence_>=times_.size()) throw std::logic_error("SWMM is not ready to advance");
            // After irreversible donor advancement, require the preceding
            // receiver receipt before producing another export. Never retry a
            // failed consumer by advancing the donor a second time.
            verifyReceipt();
            transition(CS::Updating);
            const double end=times_.at(sequence_); double actual;
            const int advanceCode=swmm_engine_advance_to(engine_,end,&actual);
            // Keep the native boundary ledger even when mapping/refusal or a
            // receiver fails. The donor cannot roll back physical advancement.
            recordNativeBoundary(end,advanceCode);
            check(advanceCode);
            if(std::abs(actual-end)>1.e-8) throw std::runtime_error("SWMM did not land on the exact exchange boundary");
            Json rows=Json::array(); double water=0,heat=0; std::vector<double> masses(nativeTargets_.size());
            for(auto& m:mapped_) {
                SWMM_OutfallExportReceipt current; std::vector<double> amounts(nativeTargets_.size());
                check(swmm_node_get_export_receipt(engine_,m.node,&current,amounts.data(),amounts.size()));
                if(current.reversed_m3>m.receipt.reversed_m3+1.e-12) throw std::runtime_error("Outfall reversal requires an unsupported feedback protocol");
                const double volume=current.exported_m3-m.receipt.exported_m3;
                if(!std::isfinite(volume) || volume<0) throw std::runtime_error("Invalid cumulative outfall volume");
                double energy=current.exported_heat_j-m.receipt.exported_heat_j;
                const auto& temperature=m.definition.at("temperature");
                if(temperature.at("mode")=="fixed") energy=volume*1000.0*4186.0*temperature.at("value_degC").get<double>();
                if(!std::isfinite(energy)) throw std::runtime_error("Outfall heat is nonfinite");
                Json species=Json::object();
                for(std::size_t p=0;p<amounts.size();++p) {
                    const double amount=amounts[p]-m.amounts[p];
                    if(!std::isfinite(amount)||amount<0) throw std::runtime_error("Invalid cumulative pollutant mass");
                    species[nativeTargets_[p]]=amount; masses[p]+=amount;
                }
                for(const auto& target:m.definition.at("targets")) {
                    const double weight=target.at("weight"); Json parts=species;
                    for(auto& amount:parts) amount=amount.get<double>()*weight;
                    rows.push_back({{"column",target.at("column")},{"volume_m3",volume*weight},{"heat_j",energy*weight},
                        {"placement",m.definition.value("placement",std::string("uniform"))},{"species_mass_kg",parts},
                        {"source_object",m.definition.at("id")}});
                }
                water+=volume; heat+=energy; m.receipt=current; m.amounts=std::move(amounts);
            }
            std::ostringstream id; id<<"swmm-outfalls/"<<this->id()<<'/'<<std::setw(8)<<std::setfill('0')<<sequence_;
            Json frame={{"id",id.str()},{"start_s",time_},{"end_s",end},{"inflows",rows}};
            frames_.push_back(frame);
            Json packet=frame; packet["schema"]="hydrocouple.integrated-inflow/1.0";
            packet["epoch_julian_day"]=epoch_; packet["ownership"]="already_debited";
            packet["units"]={{"time","elapsed_s"},{"water","m3"},{"heat","J_relative_to_0_degC"},{"species","kg"}};
            // Persist the authoritative debit before offering it to a receiver.
            writeJson(output_/("export_"+std::to_string(sequence_)+".json"),packet);
            frame_.text=packet.dump(); water_.number+=water; heat_.number+=heat;
            for(std::size_t p=0;p<masses.size();++p) massOutputs_[p]->number+=masses[p];
            time_=end; ++sequence_; transition(sequence_==times_.size()?CS::Done:CS::Updated,"Outfall interval exported once");
        } catch(const std::exception& e) { fail(e.what()); close(); }
    }
    void finish() override {
        try {
            const bool failed=status()==CS::Failed;
            if(!failed) verifyReceipt();
            transition(CS::Finishing);
            if(engine_) { check(swmm_engine_end(engine_)); check(swmm_engine_report(engine_)); }
            if(!output_.empty()) {
                Json species=Json::object();
                for(const auto& s:nativeTargets_) species[s]={{"units","kg/m3"},{"initial",0.0}};
                writeJson(output_/"inflow_series.json",{{"schema","fvqual.inflow-series/1.0"},
                    {"units",{{"time","elapsed_s"},{"water","m3"},{"heat","J_relative_to_0_degC"},{"species","kg"}}},
                    {"epoch_julian_day",epoch_},{"species",species},{"frames",frames_}});
            }
            close(); transition(CS::Finished);
        } catch(const std::exception& e) { fail(e.what()); close(); }
    }
    HC::Temporal::IDateTime* currentDateTime() const override { clock_.day=epoch_+time_/86400.0; return &clock_; }
    HC::Temporal::ITimeSpan* simulationPeriod() const override { period_.day=epoch_; period_.span=times_.empty()?0:times_.back()/86400.0; return &period_; }
    double nextDateTimeJulianDay() const override {
        return epoch_+((sequence_<times_.size() && status()==CS::Updated)?times_[sequence_]:time_)/86400.0;
    }
private:
    struct Mapped { int node=-1; Json definition; SWMM_OutfallExportReceipt receipt{}; std::vector<double> amounts; };
    Configuration configuration_; Output frame_,water_,heat_; std::vector<std::unique_ptr<Output>> massOutputs_;
    SWMM_Engine engine_=nullptr; Json config_,frames_=Json::array(); fs::path base_,output_;
    std::vector<double> times_; std::vector<Mapped> mapped_; std::vector<std::string> nativeTargets_;
    std::size_t sequence_=0; double time_=0,epoch_=2440587.5; mutable Clock clock_,period_;
    fs::path resolve(const std::string& value) const { fs::path path(value); return path.is_absolute()?path:base_/path; }
    static void writeJson(const fs::path& path,const Json& j) { std::ofstream f(path); f<<j.dump(2)<<'\n'; if(!f)throw std::runtime_error("Cannot record donor transfers"); }
    void check(int code) const { if(code)throw std::runtime_error("SWMM: "+std::string(swmm_get_last_error_msg(engine_)?swmm_get_last_error_msg(engine_):"Unknown error")+" ("+std::to_string(code)+")"); }
    void close() noexcept { if(engine_) { swmm_engine_close(engine_); swmm_engine_destroy(engine_); engine_=nullptr; } }
    void fail(const std::string& message) { if(status()!=CS::Failed && status()!=CS::Finished) transition(CS::Failed,message); }
    void recordNativeBoundary(double requested,int advanceCode) const {
        double elapsed=time_;
        (void)swmm_engine_get_elapsed_seconds(engine_,&elapsed);
        Json rows=Json::array();
        for(const auto& mapped:mapped_) {
            SWMM_OutfallExportReceipt receipt{};std::vector<double> amounts(nativeTargets_.size());
            const int result=swmm_node_get_export_receipt(engine_,mapped.node,&receipt,amounts.data(),amounts.size());
            Json species=Json::object();
            for(std::size_t p=0;p<amounts.size();++p)species[nativeTargets_[p]]=amounts[p];
            rows.push_back({{"outfall",mapped.definition.at("id")},{"receipt_code",result},{"flags",receipt.flags},
                {"cumulative_water_m3",receipt.exported_m3},{"cumulative_reversed_m3",receipt.reversed_m3},
                {"cumulative_native_heat_j",receipt.exported_heat_j},{"cumulative_species_kg",species}});
        }
        writeJson(output_/("native_boundary_"+std::to_string(sequence_)+".json"),
            {{"schema","openswmm.outfall-boundary-ledger/1.0"},{"requested_end_s",requested},
             {"native_elapsed_s",elapsed},{"advance_code",advanceCode},{"outfalls",rows}});
    }
    void verifyReceipt() {
        if(sequence_ && !frame_.consumers().empty()) {
                auto* consumer=frame_.consumers().front()->modelComponent();
                std::string accepted; bool found=false;
                for(auto* o:consumer->outputs()) if(o->id()=="accepted_transfer_id") {
                    const int64_t shape[1]={1},start[1]={0},count[1]={1};
                    HC::BufferDescriptor b; b.data=&accepted; b.kind=HC::DataKind::String; b.rank=1; b.shape=shape;
                    b.itemSizeBytes=sizeof(std::string); std::string message;
                    found=o->getValuesInto(b,start,count,&message); break;
                }
                if(!found || accepted!=frames_.back().at("id").get<std::string>() || consumer->status()==CS::Failed)
                    throw std::runtime_error("The preceding donor export has no matching receiver receipt; advancement stopped");
                writeJson(output_/("accepted_"+std::to_string(sequence_-1)+".json"),
                    {{"schema","openswmm.receiver-acknowledgment/1.0"},{"transfer_id",accepted},
                     {"receiver",consumer->id()},{"end_s",time_},{"ownership","already_debited"}});
            }
    }
    void loadConfiguration() {
        if (!configuration_.inlineConfiguration.is_null()) {
            base_ = fs::absolute(referenceDirectory().empty() ? fs::path(".") : fs::path(referenceDirectory()));
            config_ = configuration_.inlineConfiguration;
        } else {
            if(configuration_.text.empty()) throw std::invalid_argument("Choose a SWMM coupling setup file");
            auto path=fs::path(configuration_.text);
            if(path.is_relative()) path=fs::path(referenceDirectory())/path;
            base_=fs::absolute(path).parent_path(); std::ifstream f(path);
            if(!f)throw std::invalid_argument("Cannot read coupling setup: "+path.string()); f>>config_;
        }
        if(config_.at("schema")!="openswmm.reservoir-coupling/1.0" || config_.at("ownership")!="swmm-network/fvqual-receiver" ||
           config_.at("role")!="network_only") throw std::invalid_argument("Declare network_only and separate receiving-reservoir ownership");
        epoch_=config_.at("epoch_julian_day"); if(!std::isfinite(epoch_))throw std::invalid_argument("Epoch must be finite");
        if(config_.contains("instance_id")) {
            const auto instance=config_.at("instance_id").get<std::string>();
            if(instance.empty())throw std::invalid_argument("Instance ID must not be empty");
            if(hasAssignedInstanceIdentity() && instance!=id())
                throw std::invalid_argument("Configuration instance_id conflicts with the host-assigned identity");
            if(!hasAssignedInstanceIdentity())setInstanceId(instance); // legacy direct-harness configuration
        }
        times_=config_.at("exchange_times_s").get<std::vector<double>>();
        double last=0; for(double t:times_) { if(!std::isfinite(t)||t<=last)throw std::invalid_argument("Exchange times must strictly increase from zero"); last=t; }
        if(times_.empty() || !fs::is_regular_file(resolve(config_.at("model").get<std::string>())))throw std::invalid_argument("Choose a model and nonempty schedule");
        if(!config_.at("receiver_columns").is_number_integer())throw std::invalid_argument("Receiver column count must be an integer");
        const int64_t columns=config_.at("receiver_columns"); if(columns<=0)throw std::invalid_argument("Receiver column count must be positive");
        if(!config_.at("outfalls").is_array() || config_.at("outfalls").empty())throw std::invalid_argument("Map at least one outfall");
        for(const auto& row:config_.at("outfalls")) {
            double sum=0; std::set<int64_t> used;
            for(const auto& target:row.at("targets")) {
                if(!target.at("column").is_number_integer())throw std::invalid_argument("Receiver column indices must be integers");
                const int64_t c=target.at("column"); const double w=target.at("weight");
                if(c<0 || c>=columns || !used.insert(c).second || !std::isfinite(w) || w<=0)throw std::invalid_argument("Invalid or repeated receiver column/weight"); sum+=w;
            }
            if(std::abs(sum-1)>1.e-12)throw std::invalid_argument("Each outfall's conservative mapping weights must sum to one");
            const auto& temp=row.at("temperature"); const std::string mode=temp.at("mode");
            if(mode!="native" && mode!="fixed")throw std::invalid_argument("Select native heat or an explicit fixed-temperature assumption");
            if(mode=="fixed" && (!std::isfinite(temp.at("value_degC").get<double>()) || temp.value("provenance",std::string()).empty()))
                throw std::invalid_argument("Fixed temperature needs a finite Celsius value and provenance");
            const auto placement=row.value("placement",std::string("uniform"));
            if(placement!="uniform" && placement!="surface" && placement!="bed")throw std::invalid_argument("Use uniform, surface or bed placement");
        }
    }
};
class OpenSWMMInfo final : public Identity, public HC::IModelComponentInfo {
public:
    OpenSWMMInfo() : Identity("openswmm.model","OpenSWMM network") { setDescription("Native catchment/network with conservative external outfall receipts. One-way, CPU, no checkpoint or stage feedback."); }
    std::string libraryFilePath() const override { return library_; }
    void setLibraryFilePath(const std::string& v) override { library_=v; }
    std::string iconFilePath() const override { return {}; }
    std::string developer() const override { return "Caleb Buahin"; }
    std::vector<std::string> documentation() const override { return {}; }
    std::string license() const override { return "Apache-2.0"; }
    std::string copyright() const override { return "Copyright 2026 Caleb Buahin"; }
    std::string url() const override { return "https://hydrocouple.org/projects/openswmm"; }
    std::string email() const override { return {}; }
    std::string version() const override { return "6.0.0-alpha.4"; }
    std::set<std::string> tags() const override { return {"SWMM","network","outfalls","one-way"}; }
    std::unique_ptr<HC::IModelComponent> createComponentInstance() override { return std::make_unique<OpenSWMMModelComponent>(this); }
    std::vector<HC::IAdaptedOutputFactory*> adaptedOutputFactories() const override { return {}; }
private: std::string library_;
};
}
HYDROCOUPLE_DECLARE_COMPONENT(openswmm::couplers::OpenSWMMInfo)
