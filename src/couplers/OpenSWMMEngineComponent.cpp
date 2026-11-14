// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
// Generic SDK-free engine wrapper. The older FVQual-specific module remains a
// separate compatibility adapter. All physical work uses the public C API.

/**
 * @file      OpenSWMMEngineComponent.cpp
 * @brief     SDK-independent component wrapper around the OpenSWMM engine.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "ComponentBase.hpp"
#include "WaterPhaseLink.hpp"
#include "MaterialPhaseLink.hpp"
#include <openswmm/components/openswmm_component_recovery.h>
#include <hydrocouplecomponentabi.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_spatial.h>
#include <openswmm/engine/openswmm_hotstart.h>
#include <openswmm/engine/openswmm_checkpoint.h>
#include <openswmm/engine/openswmm_snow.h>
#ifdef OPENSWMM_HAS_2D
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_gw2d.h>
#include <openswmm/engine/openswmm_gw_transport.h>
#include <openswmm/engine/openswmm_surface_ownership.h>
#endif
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <cstdlib>
#include <optional>

namespace openswmm::couplers {
namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
constexpr auto profile = "hydrocouple.interval-transfer/1";
/**
 * @brief Convert UTF8 bytes to a native filesystem path without filesystem access.
 * @param[in] value Stable byte view, read/copied synchronously; no lifetime retained.
 * @return Owning native path via u8string, no normalization/existence check.
 * Conversion/allocation may throw.
 */
fs::path pathFromText(std::string_view value) {
    return fs::path(std::u8string(value.begin(), value.end()));
}
/**
 * @brief Convert a native filesystem path to owning UTF8 byte text.
 * @param[in] path Stable native path.
 * @return Byte string from u8string, no existence/absolute/canonical check.
 * Conversion/allocation may throw.
 */
std::string pathText(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
/**
 * @brief Read local file bytes into an owning string for immutable-source comparison/JSON
 * parsing.
 * @param[in] path Native filesystem path, opened binary for read.
 * @return Byte sequence read through stream iterators; no encoding/newline conversion.
 * @throws std::runtime_error Open failure; allocation/stream exceptions under configured policy
 * propagate.
 * @note Does not separately test stream state after iterator read, so unexpected IO failure may
 * return partial data later rejected by parse/byte comparison. No file mutation or engine/clock
 * advance.
 */
std::string readFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot read " + pathText(path));
    return {std::istreambuf_iterator<char>(file), {}};
}
/**
 * @brief Convert a nonzero native engine status into an exception with engine diagnostic text.
 * @param[in] engine Valid live handle whose last-error string is readable for failure.
 * @param[in] code Native status;0 returns, nonzero throws std::runtime_error with code/message.
 * @note Does not clear native status, change engine lifecycle or take handle ownership. String
 * allocation may throw.
 */
void check(SWMM_Engine engine, int code) {
    if (code)
        throw std::runtime_error("SWMM (" + std::to_string(code) +
                                 "): " + swmm_get_last_error_msg(engine));
}
struct EngineHandle {
    SWMM_Engine value = swmm_engine_create();
    /**
     * @brief Create an owned metadata-inspection native engine handle.
     * @throws std::runtime_error Native allocation returns null; no model opened yet.
     * @note Destructor closes/destroys handle, no ownership transfer; use local exclusive RAII
     * lifecycle.
     */
    EngineHandle() {
        if (!value)
            throw std::runtime_error("Cannot create OpenSWMM engine");
    }
    /**
     * @brief Close then destroy the owned native inspection handle.
     * @note Close status ignored, no report/end request or exception conversion. Caller keeps
     * exclusive handle and no borrowed API pointers alive after destruction.
     */
    ~EngineHandle() {
        swmm_engine_close(value);
        swmm_engine_destroy(value);
    }
};
/**
 * @brief Read a null-terminated native text getter with dynamically growing caller scratch.
 * @tparam Getter Callable accepting writable char pointer and signed capacity; must check/throw
 * its own status.
 * @param[in] getter Synchronous callable, writes a valid NUL-terminated string even when
 * truncated.
 * @return Owning text after result length<capacity-1, starting256 bytes and doubling for full
 * buffer.
 * @note No maximum growth/overflow guard; getter must eventually fit and never leave
 * unterminated data. Allocation/getter exceptions propagate, no pointer retained after call.
 * Repeated getter calls may repeat its own cursor/read effects; wrapper itself owns no
 * engine/clock state.
 */
template <class Getter> std::string readText(Getter getter) {
    std::vector<char> buffer(256);
    for (;;) {
        getter(buffer.data(), static_cast<int>(buffer.size()));
        const auto length = std::strlen(buffer.data());
        if (length < buffer.size() - 1)
            return {buffer.data(), length};
        buffer.resize(buffer.size() * 2);
    }
}
class EngineConfiguration final : public Item, public HC::IArgument {
public:
    /**
     * @brief Construct required native-engine configuration argument and borrowed owner
     * association.
     * @param[in] owner Nonnull component pointer, must outlive argument. No engine opened or
     * model initialized; metadata describes openswmm.component/1 schema,
     * construction/description may allocate.
     */
    explicit EngineConfiguration(ComponentBase* owner)
        : Item("configuration", owner, HC::DataKind::String), owner_(owner) {
        setCaption("OpenSWMM model configuration");
        setDescription(
            "openswmm.component/1: model, role, output_directory, optional outfalls and exchange_times_s");
    }
    /**
     * @brief Read OpenSWMM component argument metadata.
     * @return Configuration argument role.
     * @note Copied text/containers may allocate; no file access, engine advancement or
     * notifications.
     */
    ArgumentRole role() const override { return ArgumentRole::Configuration; }
    /**
     * @brief Read OpenSWMM component argument metadata.
     * @return False; a configuration must be supplied before successful preparation.
     * @note Copied text/containers may allocate; no file access, engine advancement or
     * notifications.
     */
    bool isOptional() const override { return false; }
    /**
     * @brief Read OpenSWMM component argument metadata.
     * @return True outside owner Created status.
     * @note Copied text/containers may allocate; no file access, engine advancement or
     * notifications.
     */
    bool isReadOnly() const override { return owner_->status() != ComponentBase::CS::Created; }
    /**
     * @brief Read OpenSWMM component argument metadata.
     * @return Stored canonical inline JSON or local file text.
     * @note Copied text/containers may allocate; no file access, engine advancement or
     * notifications.
     */
    std::string toString() const override { return text; }
    /**
     * @brief Perform the argument persistence hook without writing a file.
     * @note No-op; host calls serialize for reusable configuration text. No model/clock
     * mutation.
     */
    void saveData() override {}
    /**
     * @brief Read OpenSWMM component argument metadata.
     * @return One JSON component-configuration file selector filter.
     * @note Copied text/containers may allocate; no file access, engine advancement or
     * notifications.
     */
    std::vector<std::string> fileFilters() const override {
        return {"OpenSWMM component configuration (*.json)"};
    }
    /**
     * @brief Read OpenSWMM component argument metadata.
     * @return Empty vector; component-data-item initialization is unsupported.
     * @note Copied text/containers may allocate; no file access, engine advancement or
     * notifications.
     */
    std::vector<const std::type_info*> validComponentDataItemTypes() const override { return {}; }
    /**
     * @brief Test supported local configuration input/serialization representations.
     * @param[in] type Argument type selector.
     * @return True for JSON or File only; no value/path/lifecycle validation, allocation or
     * mutation.
     */
    bool isValidArgType(ArgumentInputType type) const override {
        return type == ArgumentInputType::JSON || type == ArgumentInputType::File;
    }
    /**
     * @brief Read OpenSWMM component argument metadata.
     * @return Last successful JSON/File input type, default JSON.
     * @note Copied text/containers may allocate; no file access, engine advancement or
     * notifications.
     */
    ArgumentInputType currentArgumentInputType() const override { return type_; }
    /**
     * @brief Install inline openswmm.component/1 JSON or a local configuration-file reference.
     * @param[in] value JSON object with schema openswmm.component/1, JSON \@uri file object, or
     * File path.
     * @param[in] type JSON/File only; containing owner must be Created.
     * @param[out] message Empty success, caught std::exception diagnostic on refusal.
     * @return True after argument update, false on bad schema/type/lifecycle/empty/remote
     * path/JSON.
     * @note Strips file:// prefix, rejects remaining ://. Inline schema object retained and
     * canonicalized; file reference not read/validated until load. Catch appends owner Error.
     * Exceptions can follow text/ inline partial changes; error reporting can itself throw. No
     * physical/clock advance, exclusive setup.
     */
    bool initialize(const std::string& value, ArgumentInputType type,
                    std::string& message) override {
        try {
            if (isReadOnly() || !isValidArgType(type))
                throw std::invalid_argument(
                    "Set configuration before initialization using JSON or File");
            if (type == ArgumentInputType::JSON) {
                const auto parsed = Json::parse(value);
                if (parsed.is_object() && parsed.contains("schema")) {
                    if (parsed.at("schema") != "openswmm.component/1")
                        throw std::invalid_argument("Use schema openswmm.component/1");
                    inline_ = parsed;
                    text = parsed.dump();
                    type_ = type;
                    message.clear();
                    return true;
                }
                text = parsed.at("@uri").get<std::string>();
            } else
                text = value;
            if (text.starts_with("file://"))
                text.erase(0, 7);
            if (text.empty() || text.find("://") != std::string::npos)
                throw std::invalid_argument("Choose a local configuration file");
            inline_ = nullptr;
            type_ = type;
            message.clear();
            return true;
        } catch (const std::exception& error) {
            message = error.what();
            owner_->reportError(message);
            return false;
        }
    }
    /**
     * @brief Refuse configuration initialization from another component data item.
     * @param[out] message Receives instruction to use local file/JSON; unnamed source ignored.
     * @return Always false, no source buffer read, physical advance or configuration change.
     * Diagnostic assignment may throw. Appends owner Error entry.
     */
    bool initialize(const HC::IComponentDataItem&, std::string& message) override {
        message = "Use JSON or a local configuration file";
        owner_->reportError(message);
        return false;
    }
    /**
     * @brief Assign a local configuration file path from a validated scalar host string buffer.
     * @param[in] buffer Readable Host/None rank1 String descriptor, shape[count0], live
     * std::string for nonempty request; descriptor memory remains caller-owned and stable
     * through call.
     * @param[in] start One scalar offset0..1.
     * @param[in] count One fitting extent0..1;0 validates then succeeds without
     * configuration/lifecycle check.
     * @param[out] message Optional validation/initialize diagnostic, empty success.
     * @return True for accepted file-path assignment/empty slice, false on validation/initialize
     * refusal. Delegates nonempty string to File initialize, not inline JSON. No file parsing or
     * physical clock advance here; string/diagnostic operations may throw after partial argument
     * change. Exclusive setup.
     */
    bool setValuesFrom(const HC::BufferDescriptor& buffer, std::span<const int64_t> start,
                       std::span<const int64_t> count, std::string* message = nullptr) override {
        if (!valid(buffer, start, count, message))
            return false;
        if (!count[0])
            return true;
        std::string diagnostic;
        const bool result = initialize(*static_cast<std::string*>(buffer.data),
                                       ArgumentInputType::File, diagnostic);
        if (message)
            *message = diagnostic;
        return result;
    }
    /**
     * @brief Serialize inline configuration or local file reference without saving a file.
     * @param[in] type JSON or File; inline configuration only supports JSON.
     * @param[out] value Canonical inline JSON, JSON \@uri object for file reference, or bare
     * File path; unsupported representation preserves previous value.
     * @param[out] message Empty success or representation diagnostic.
     * @return True for supported representation, false otherwise. No read/parse of referenced
     * file, engine/clock advance or argument mutation. Allocation/JSON exceptions propagate;
     * stable configuration required.
     */
    bool serialize(ArgumentInputType type, std::string& value,
                   std::string& message) const override {
        if (!isValidArgType(type) || (!inline_.is_null() && type != ArgumentInputType::JSON)) {
            message = "Inline configuration serializes as JSON; file configuration as JSON or File";
            return false;
        }
        value = type == ArgumentInputType::JSON
                    ? (inline_.is_null() ? Json{{"@uri", text}}.dump() : inline_.dump())
                    : text;
        message.clear();
        return true;
    }
    /**
     * @brief Load configured JSON and establish its filesystem base for relative model/output
     * paths.
     * @param[out] base Initially absolute owner referenceDirectory (or current dot); file input
     * updates to configuration file parent. Can change before later read/parse failure.
     * @return Owning inline JSON copy or parsed local file object, not full component schema
     * validation.
     * @throws std::invalid_argument Empty chosen configuration; filesystem/read/JSON/allocation
     * errors propagate.
     * @note Owner must remain alive/stable. No engine open, physical/clock advance or file
     * write; local path and returned document are caller-owned, failure has no transactional
     * base rollback.
     */
    Json load(fs::path& base) const {
        base = fs::absolute(pathFromText(
            owner_->referenceDirectory().empty() ? "." : owner_->referenceDirectory()));
        if (!inline_.is_null())
            return inline_;
        if (text.empty())
            throw std::invalid_argument("Choose an OpenSWMM model configuration");
        auto path = pathFromText(text);
        if (path.is_relative())
            path = base / path;
        base = path.parent_path();
        return Json::parse(readFile(path));
    }

private:
    ComponentBase* owner_;
    Json inline_;
    ArgumentInputType type_ = ArgumentInputType::JSON;
};

struct MaterialGroupRecovery;
class OpenSWMMEngineComponent final : public ComponentBase,
                                      public HC::Temporal::ITimeModelComponent {
public:
    /**
     * @brief Construct a Created native SWMM component with owned argument/output metadata.
     * @param[in] info Borrowed factory metadata pointer, must outlive component; no ownership
     * transfer.
     * @note Initial ID openswmm-engine may be replaced by host identity argument before
     * initialization. No native engine/file open or physical time advance; metadata allocation
     * may throw.
     */
    explicit OpenSWMMEngineComponent(HC::IModelComponentInfo* info)
        : ComponentBase("openswmm-engine", info), configuration_(this) {}
    /**
     * @brief Close/destroy any owned runtime engine when component wrapper is destroyed.
     * @note Does not finish/report model or validate pending receiver receipt automatically.
     * Borrowed output/argument/clock pointers become invalid; host detaches consumers before
     * owner destruction.
     */
    ~OpenSWMMEngineComponent() override { close(); }
    /**
     * @brief Enumerate host identity and model-configuration argument pointers.
     * @return Owning vector of nonowned argument pointers, component lifetime; no extra lifetime
     * extension. No file read/lifecycle/clock advance; vector allocation may throw, host keeps
     * owner stable.
     */
    std::vector<HC::IArgument*> arguments() const override {
        auto result = ComponentBase::arguments();
        result.push_back(const_cast<EngineConfiguration*>(&configuration_));
        return result;
    }
    /**
     * @brief Enumerate currently constructed output item pointers without refreshing native
     * state.
     * @return Owning vector of borrowed pointers, component lifetime and until reinitialization
     * clears dynamic items. Do not delete pointers; no producer advance or ownership transfer.
     * Vector allocation can throw; host serializes initialization/output-list reads.
     */
    std::vector<HC::IInput*> inputs() const override {
        if (material_ && !material_->surface) return {&material_->request};
        if (split_ && !split_->surface) return {&split_->request};
        return {};
    }
    std::vector<HC::IOutput*> outputs() const override {
        std::vector<HC::IOutput*> result;
        for (const auto& item : outputs_)
            result.push_back(item.get());
        if (split_ && split_->surface) result.push_back(&split_->receipt);
        if (material_ && material_->surface) result.push_back(&material_->receipt);
        return result;
    }
    /**
     * @brief Discover immutable configured model metadata and construct optional typed outfall
     * transfer ports.
     * @note Enters Initializing, closes old handle and clears outputs/fields/species/receipts;
     * loads schema openswmm.component/1, validates complete_model/network_only role and chosen
     * fresh output path text. Uses local metadata-only engine and byte-compares model
     * before/after discovery, snapshots process configuration bytes, publishes model_metadata
     * and enters Initialized even if runnable restrictions were collected (validate/prepare
     * enforce them). No runtime physical advance or output directory creation yet. Caught
     * std::exception clears ports/closes engine then fails; partial metadata may remain and
     * transition/diagnostic allocation can propagate. Host supplies stable paths/files/identity.
     */
    void initialize() override {
        if(rejectLifecycleReentry())return;
        if (material_ && material_->connected()) {
            reportError("Disconnect inactive material ports before reinitializing"); return;
        }
        ComponentOperation call(lifecycleActive_);
        std::optional<MaterialPhaseLink::Operation> operation;
        try {
            transition(CS::Initializing);
            if(!close())throw std::runtime_error("Native material close refused; handle retained");
            outputs_.clear();
            fields_.clear();
            species_.clear();
            outfalls_.clear();
            sequence_ = 0;
            time_ = 0;
            receiver_ = nullptr;
            pendingToken_.clear();
            reasons_.clear();
            componentFiles_.clear();
            materialOutputOwned_ = false;
            materialRecovery_=materialRecoveryOutputs_=materialRecoveryEnabled_=false;
            config_ = configuration_.load(base_);
            if (config_.at("schema") != "openswmm.component/1")
                throw std::invalid_argument("Use schema openswmm.component/1");
            for (const auto& [key, value] : config_.items())
                if (key != "schema" && key != "role" && key != "model" &&
                    key != "output_directory" && key != "outfalls" &&
                    key != "exchange_times_s" && key != "managed_session" && key != "surface_results" && key != "material_capacity")
                    throw std::invalid_argument("Unknown component configuration key: " + key);
            managed_ = config_.value("managed_session", false);
            ownershipToken_.clear();
            role_ = config_.value("role", std::string("complete_model"));
            if (config_.contains("surface_results") &&
                ((role_ != "integrated2d" && role_ != "split_material_surface") || !config_.at("surface_results").is_boolean()))
                throw std::invalid_argument("surface_results is a Boolean for a surface-owner role");
            split_.reset();
            material_.reset();
            const bool materialRole = role_ == "split_material_network" || role_ == "split_material_surface";
            if (config_.contains("material_capacity") && !materialRole)
                throw std::invalid_argument("material_capacity requires a material split role");
            if (materialRole) {
#ifdef OPENSWMM_COMPONENT_2D_RESULTS
                if (managed_ || config_.contains("outfalls") || config_.contains("exchange_times_s"))
                    throw std::invalid_argument("Material roles require native scheduling without other session/export owners");
                const auto capacity = config_.value("material_capacity",Json(4096));
                if (!capacity.is_number_integer() || capacity <= 0 || capacity > 65536)
                    throw std::invalid_argument("material_capacity must be an integer from 1 to 65536");
                material_ = std::make_unique<MaterialPhaseLink>(this,role_ == "split_material_surface",capacity.get<std::uint64_t>());
                operation.emplace(material_->operation());
                material_->request.finishAt = [this](double at) {
                    if (lifecycleActive_ || !material_->authorizedFinish() || status()!=CS::Updated || time_!=at)
                        throw std::invalid_argument("Material final settlement requires its shared accepted boundary");
                    ComponentOperation call(lifecycleActive_);
                    try { transition(CS::Updating); startMaterialNetwork();material_->finishNetwork(engine_); recordMaterialState(true); transition(CS::Done); }
                    catch(const std::exception& ex){fail(ex.what());recordMaterialFailure();throw;}
                };
                material_->receipt.serve = [this](const HC::IInput* query) {
                    try {material_->evaluate(engine_,query);}
                    catch(const std::exception& ex){fail(ex.what());throw;}
                };
                material_->publish = [this](double at,bool ended) {
                    time_=at;recordMaterialState(false);
                    if(ended && status()==CS::Updated){transition(CS::Updating);transition(CS::Done);}
                };
                material_->receipt.observe = [this]{recordMaterialState(true);};
#else
                throw std::invalid_argument("Material component roles require the CPU surface and HDF5 build");
#endif
            } else if (role_ == "split_network" || role_ == "integrated2d") {
#ifdef OPENSWMM_COMPONENT_2D_RESULTS
                if (managed_ || config_.contains("outfalls") || config_.contains("exchange_times_s"))
                    throw std::invalid_argument("Split water roles require native phase scheduling without outfall exports or managed sessions");
                split_ = std::make_unique<WaterPhaseLink>(this,role_ == "integrated2d");
                split_->request.finalizeAt = [this](double at) {
                    if (status() != CS::Updated || time_ != at)
                        throw std::invalid_argument("Final settlement requires a shared accepted boundary");
                    try {
                        transition(CS::Updating);
                        split_->finishNetwork(engine_);
                        transition(CS::Done);
                    } catch (const std::exception& ex) { fail(ex.what()); throw; }
                };
                split_->receipt.serve = [this](const HC::IInput* query) {
                    try { split_->evaluate(engine_,query,time_); }
                    catch (const std::exception& ex) { fail(ex.what()); throw; }
                };
#else
                throw std::invalid_argument("Split roles require the CPU surface and HDF5 build");
#endif
            } else if (role_ != "complete_model" && role_ != "network_only")
                throw std::invalid_argument("Use complete_model or network_only");
            model_ = resolve(config_.at("model").get<std::string>());
            output_ = resolve(config_.at("output_directory").get<std::string>());
            if (config_.at("output_directory").get<std::string>().empty())
                throw std::invalid_argument("Choose a fresh output_directory for this instance");
            modelBytes_ = readFile(model_);
            EngineHandle inspection;
            check(inspection.value,
                  swmm_engine_open_metadata(inspection.value, pathText(model_).c_str()));
            discover(inspection.value);
            if (readFile(model_) != modelBytes_)
                throw std::runtime_error(
                    "Model changed during metadata discovery; initialize again");
            auto* metadata = add("model_metadata", HC::DataKind::String);
            metadata->text = metadata_.dump();
            if(material_){auto* state=add("material_session",HC::DataKind::String);state->text=material_->snapshot().dump();fields_["material_session"]=state;}
            if (!outfalls_.empty())
                createTransferPorts();
            transition(CS::Initialized);
        } catch (const std::exception& error) {
            outputs_.clear();
            fields_.clear();
            close();
            fail(error.what());
        }
    }
    /**
     * @brief Check configured execution restrictions and reciprocal complete typed transfer
     * connections.
     * @return Empty diagnostic vector after Valid transition, or first exception message after
     * Error entry and Invalid transition. Receiver pointer may be partially resolved on refusal.
     * @note Enters Validating, no native run/clock/file write. Requires Initialized lifecycle
     * and live connected pointees; callbacks/diagnostic allocation can throw, host serializes
     * wiring and lifecycle.
     */
    std::vector<std::string> validate() override {
        if(rejectLifecycleReentry())return {"Component lifecycle is already active"};
        if(material_ && material_->ready()) {
            reportError("Material validation requires an inactive unprepared component");
            return {"Material validation requires an inactive unprepared component"};
        }
        ComponentOperation call(lifecycleActive_);
        std::optional<MaterialPhaseLink::Operation> operation;
        try {
            if(material_)operation.emplace(material_->operation());
            transition(CS::Validating);
            if (!reasons_.empty())
                throw std::invalid_argument(reasons_.front());
            validateConnections();
            transition(CS::Valid);
            return {};
        } catch (const std::exception& error) {
            reportError(error.what());
            transition(CS::Invalid, error.what());
            return {error.what()};
        }
    }
    /**
     * @brief Open/start a fresh runtime engine and arm qualified cumulative export receipts.
     * @note Enters Preparing, rejects restrictions/wiring changes and changed model/process
     * bytes. Requires fresh output directory (existing refused), opens native report/results,
     * applies IGNORE_2D for network_only, initializes/starts and checks selected outfall receipt
     * flags7. Writes configuration/metadata JSON then Updated. No elapsed physical advancement
     * beyond native start. Caught std::exception fails/closes, but created
     * directory/files/partial artifacts remain; no rollback. Exclusive host lifecycle and
     * immutable configuration; output/connection pointers kept alive through run.
     */
    void prepare() override {
        if(rejectLifecycleReentry())return;
        if(material_ && material_->ready()) {
            reportError("Material component is active or already prepared");return;
        }
        ComponentOperation call(lifecycleActive_);
        std::optional<MaterialPhaseLink::Operation> operation;
        try {
            if(material_){operation.emplace(material_->operation());material_->freeze();}
            transition(CS::Preparing);
            if (!reasons_.empty())
                throw std::invalid_argument(reasons_.front());
            validateConnections();
            if (readFile(model_) != modelBytes_)
                throw std::runtime_error(
                    "Model changed after initialization; create a new configured instance");
            for (const auto& [path, contents] : componentFiles_)
                if (readFile(path) != contents)
                    throw std::runtime_error(
                        "Process configuration changed after initialization; create a new configured instance: " +
                        pathText(path));
            if (surfaceExecution_) {
                const char* backend = std::getenv("OPENSWMM_2D_BACKEND");
                if (backend && backend[0] && std::string(backend) != "cpu" &&
                    std::string(backend) != "CPU")
                    throw std::invalid_argument("Surface component requires CPU backend override or no override");
            }
            if (!fs::create_directories(output_))
                throw std::invalid_argument(
                    "output_directory already exists; choose a fresh directory for each run/instance");
            materialOutputOwned_ = material_ != nullptr;
            engine_ = swmm_engine_create();
            if (!engine_)
                throw std::runtime_error("Cannot create runtime engine");
            check(engine_, swmm_engine_open(engine_, pathText(model_).c_str(),
                                            materialRecovery_ ? nullptr : pathText(output_ / "model.rpt").c_str(),
                                            (split_ && split_->surface) || (material_ && (material_->surface || (materialRecovery_ && !materialRecoveryOutputs_))) ? nullptr : pathText(output_ / "model.out").c_str(), nullptr));
            if(material_)
                check(engine_,swmm_split_material_configure(engine_,material_->surface?SWMM_SPLIT_SURFACE:SWMM_SPLIT_NETWORK));
            if (split_)
                check(engine_, swmm_split_configure(engine_, split_->surface ? SWMM_SPLIT_SURFACE : SWMM_SPLIT_NETWORK));
            if (role_ == "network_only")
                check(engine_, swmm_options_set(engine_, "IGNORE_2D", "YES"));
#ifdef OPENSWMM_HAS_2D
            if (surfaceExecution_ && config_.value("surface_results",true) && (!materialRecovery_ || materialRecoveryOutputs_))
                check(engine_, swmm_2d_set_results_file(
                                   engine_, pathText(output_ / "model.2d.h5").c_str()));
#endif
            if (managed_) {
                SWMM_CheckpointSessionOptions options{};
                options.struct_size = sizeof(options);
                options.schema_version = 1;
                options.output = surfaceExecution_ ? SWMM_CHECKPOINT_OUTPUT_COMBINED
                                                  : SWMM_CHECKPOINT_OUTPUT_BINARY;
                options.ownership_token = ownershipToken_.empty() ? nullptr : ownershipToken_.c_str();
                const int code = swmm_checkpoint_start_session(engine_, &options);
                if (code)
                    throw std::runtime_error(swmm_checkpoint_last_error_msg());
            } else {
                check(engine_, swmm_engine_initialize(engine_));
                if(material_){
                    if(materialRecovery_ && materialRecoveryOutputs_)
                        checkMaterialRecovery(swmm_split_material_checkpoint_prepare_outputs(engine_));
                    if(material_->surface)material_->prepareSurface(engine_);else material_->preparedNetwork(engine_);
                }
                else if (!split_) check(engine_, swmm_engine_start(engine_, 1));
                else if (split_->surface) split_->prepareSurface(engine_);
            }
            for (auto& outfall : outfalls_) {
                outfall.amounts.assign(species_.size(), 0);
                check(engine_, swmm_node_get_export_receipt(
                                   engine_, outfall.node, &outfall.receipt, outfall.amounts.data(),
                                   static_cast<int>(outfall.amounts.size())));
                if ((outfall.receipt.flags & 7u) != 7u)
                    throw std::invalid_argument(
                        "Selected outfall requires qualified LEGACY pollutant and native heat receipts");
            }
            write("configuration.json", config_);
            write("metadata.json", metadata_);
            recordMaterialState(true);
            transition(CS::Updated);
        } catch (const std::exception& error) {
            fail(error.what());
            recordMaterialFailure();
            close();
        }
    }
    /**
     * @brief Advance the native model once and publish an already-debited conservative export
     * interval.
     * @note Unnamed requested-output list ignored; only Updated accepted (otherwise Error and
     * return). Verifies prior receiver receipt before Updating. Uncoupled profile calls one
     * native step and publishes elapsed/Done. Coupled profile advances to exact next
     * elapsed-seconds endpoint, records native boundary before checking status, differences
     * cumulative water m3/enthalpy J/pollutant kg, refuses any reversal or invalid amount,
     * writes export JSON and typed fields/cumulative/unreceived outputs. New token
     * instance:sequence must be acknowledged before next advance/finish. Energy may be signed,
     * other amounts nonnegative. Updates time/sequence and Updated/Done; caught failure
     * fails/closes without rolling back native debits, receipts, fields or written artifacts.
     * Exclusive once-only host scheduling.
     */
    void update(const std::vector<HC::IOutput*>& = {}) override {
        if(rejectLifecycleReentry())return;
        if (status() != CS::Updated) {
            reportError("Engine component is not ready to update");
            return;
        }
        ComponentOperation call(lifecycleActive_);
        std::optional<MaterialPhaseLink::Operation> operation;
        try {
            if(material_){operation.emplace(material_->operation());material_->validate();}
            verifyReceipt();
            transition(CS::Updating);
            if(material_){
                if(material_->surface){transition(material_->ended()?CS::Done:CS::Updated);return;}
                startMaterialNetwork();double elapsed=0;
                check(engine_,swmm_engine_step(engine_,&elapsed));check(engine_,swmm_engine_get_elapsed_seconds(engine_,&time_));
                if(elapsed==0)material_->finishNetwork(engine_);else material_->accepted(time_);
                recordMaterialState(elapsed==0);transition(elapsed==0?CS::Done:CS::Updated);return;
            }
            if (split_ && split_->surface) {
                transition(split_->ended ? CS::Done : CS::Updated);
                return;
            }
            if (split_) split_->startNetwork(engine_);
            if (outfalls_.empty()) {
                double elapsed = 0;
                check(engine_, swmm_engine_step(engine_, &elapsed));
                check(engine_, swmm_engine_get_elapsed_seconds(engine_, &time_));
                if (split_ && elapsed == 0) split_->finishNetwork(engine_);
                transition(elapsed == 0 ? CS::Done : CS::Updated);
                return;
            }
            const double end = times_.at(sequence_);
            double actual = 0;
            const int code = swmm_engine_advance_to(engine_, end, &actual);
            recordNativeBoundary(end, code);
            check(engine_, code);
            if (actual != end)
                throw std::runtime_error("Engine did not reach the agreed interval endpoint");
            std::vector<double> amounts(2 + species_.size(), 0);
            for (auto& outfall : outfalls_) {
                SWMM_OutfallExportReceipt receipt{};
                std::vector<double> mass(species_.size());
                check(engine_,
                      swmm_node_get_export_receipt(engine_, outfall.node, &receipt, mass.data(),
                                                   static_cast<int>(mass.size())));
                if (receipt.reversed_m3 != outfall.receipt.reversed_m3)
                    throw std::runtime_error(
                        "Reverse outfall flow needs a bidirectional contract; this profile stopped");
                amounts[0] += receipt.exported_m3 - outfall.receipt.exported_m3;
                amounts[1] += receipt.exported_heat_j - outfall.receipt.exported_heat_j;
                for (size_t i = 0; i < mass.size(); ++i)
                    amounts[i + 2] += mass[i] - outfall.amounts[i];
                outfall.receipt = receipt;
                outfall.amounts = std::move(mass);
            }
            for (size_t i = 0; i < amounts.size(); ++i)
                if (!std::isfinite(amounts[i]) || (i != 1 && amounts[i] < 0))
                    throw std::runtime_error("Invalid native export amount");
            pendingToken_ = id() + ":" + std::to_string(sequence_ + 1);
            pending_ = amounts;
            Json row = {{"schema", profile},
                        {"transfer_id", pendingToken_},
                        {"instance_id", id()},
                        {"epoch_julian_day", epoch_},
                        {"start_s", time_},
                        {"end_s", end},
                        {"ownership", "already_debited"},
                        {"amounts", amounts}};
            write("export_" + std::to_string(sequence_) + ".json", row);
            fields_.at("transfer_id")->text = pendingToken_;
            fields_.at("epoch_julian_day")->number = epoch_;
            fields_.at("interval_start_s")->number = time_;
            fields_.at("interval_end_s")->number = end;
            for (size_t i = 0; i < amounts.size(); ++i) {
                fields_.at(amountField(i))->number = amounts[i];
                fields_.at("exported." + amountField(i))->number += amounts[i];
                fields_.at("unreceived." + amountField(i))->number = receiver_ ? amounts[i] : 0;
            }
            time_ = end;
            ++sequence_;
            transition(sequence_ == times_.size() ? CS::Done : CS::Updated);
        } catch (const std::exception& error) {
            fail(error.what());
            recordMaterialFailure();
            close();
        }
    }
    /**
     * @brief Validate last receiver receipt then end/report and close the native runtime engine.
     * @note Finished no-op. Failed path skips receipt and native end/report, otherwise checks
     * acknowledgment; transitions Finishing then Finished after close. Caught std::exception
     * fails/closes; written artifacts and physical state not rolled back. Must run before
     * destruction for final native report, exclusive lifecycle and live receiver pointers until
     * acknowledgment check completes.
     */
    void finish() override {
        if(rejectLifecycleReentry())return;
        if (status() == CS::Finished)
            return;
        ComponentOperation call(lifecycleActive_);
        std::optional<MaterialPhaseLink::Operation> operation;
        try {
            if(material_)operation.emplace(material_->operation());
            const bool failed = status() == CS::Failed || (material_ && material_->terminal());
            if (!failed)
                verifyReceipt();
            transition(CS::Finishing);
            if (engine_ && !failed) {
                if(material_){if(material_->surface)material_->finishSurface(time_);else {startMaterialNetwork();material_->finishNetwork(engine_);}}
                else if (!split_) check(engine_, swmm_engine_end(engine_));
                else if (!split_->surface) split_->finishNetwork(engine_);
                else split_->finishSurface(time_);
                check(engine_, swmm_engine_report(engine_));
            }
            if(!close()){fail("Native material close refused; handle retained: "+std::string(swmm_get_last_error_msg(engine_)));recordMaterialState(true);return;}
            recordMaterialState(true);
            transition(CS::Finished);
        } catch (const std::exception& error) {
            fail(error.what());
            recordMaterialFailure();
            close();
        }
    }
    /**
     * @brief Borrow the current absolute physical-time metadata view.
     * @return Nonowned mutable Clock pointer after refreshing day=epoch Julian day+elapsed
     * seconds/86400; component owns view, next query can overwrite contents. No native
     * advancement or allocation. Host serializes mutable metadata queries and owner lifetime.
     */
    HC::Temporal::IDateTime* currentDateTime() const override {
        clock_.day = epoch_ + time_ / 86400.;
        return &clock_;
    }
    /**
     * @brief Borrow the configured absolute start/duration metadata view.
     * @return Nonowned component Clock span, start epoch astronomical Julian day and duration
     * days from native duration; next query overwrites view. No physical advancement, allocation
     * or ownership transfer; component lifetime and serialized metadata access required.
     */
    HC::Temporal::ITimeSpan* simulationPeriod() const override {
        period_.day = epoch_;
        period_.span = duration_ / 86400.;
        return &period_;
    }
    /**
     * @brief Read the host scheduling target as absolute astronomical Julian day.
     * @return Next agreed exchange endpoint when Updated; otherwise current epoch+elapsed/86400.
     * Uncoupled engine uses min(native duration,elapsed+routingStep). No native advancement or
     * allocation; prepared schedule/sequence must remain stable through query.
     */
    double nextDateTimeJulianDay() const override {
        if (status() != CS::Updated || (split_ && split_->surface) || (material_ && material_->surface))
            return epoch_ + time_ / 86400.;
        return epoch_ + (outfalls_.empty() ? std::min(duration_, time_ + routingStep_)
                                           : times_.at(sequence_)) /
                            86400.;
    }

private:
    struct ComponentOperation {
        bool& active;
        explicit ComponentOperation(bool& flag):active(flag){active=true;}
        ~ComponentOperation(){active=false;}
    };
    friend struct MaterialGroupRecovery;
    bool materialRecovery_=false, materialRecoveryOutputs_=false, materialRecoveryEnabled_=false;
    bool lifecycleActive_=false;
    bool rejectLifecycleReentry(){
        if(lifecycleActive_ || (material_ && material_->busy())){reportError("Component lifecycle is already active");return true;}
        return false;
    }
    struct Outfall {
        int node;
        SWMM_OutfallExportReceipt receipt{};
        std::vector<double> amounts;
    };
    EngineConfiguration configuration_;
    SWMM_Engine engine_ = nullptr;
    HC::IModelComponent* receiver_ = nullptr;
    std::vector<std::unique_ptr<Output>> outputs_;
    std::map<std::string, Output*> fields_;
    Json config_, metadata_;
    fs::path base_, model_, output_;
    std::string role_, modelBytes_, pendingToken_;
    std::string ownershipToken_;
    bool managed_ = false;
    std::unique_ptr<WaterPhaseLink> split_;
    std::unique_ptr<MaterialPhaseLink> material_;
    bool materialOutputOwned_ = false;
    bool surfaceExecution_ = false;
    std::map<fs::path, std::string> componentFiles_;
    std::vector<std::string> species_, reasons_;
    std::vector<Outfall> outfalls_;
    std::vector<double> times_, pending_;
    size_t sequence_ = 0;
    double epoch_ = 2440587.5, duration_ = 0, time_ = 0, routingStep_ = 0;
    mutable Clock clock_, period_;
    /**
     * @brief Resolve model/configured path text against this component's established base.
     * @param[in] value Path text, absolute retained, relative joined to base.
     * @return Owning UTF8-converted absolute native path. No existence/file access or state
     * mutation; filesystem conversion/allocation may throw.
     */
    fs::path resolve(const std::string& value) const {
        const auto p = pathFromText(value);
        return fs::absolute(p.is_absolute() ? p : base_ / p);
    }
    /**
     * @brief Release the owned runtime engine if present and clear its handle.
     * @note Material close refusal retains its handle. Other roles preserve their existing close
     * behavior. Does not report/end or
     * consume pending receiver acknowledgment. No-op when absent; no throw wrapper, exclusive
     * owner lifecycle, all borrowed native API pointers invalid after close.
     */
    bool close() noexcept {
        if (engine_) {
            const int code=swmm_engine_close(engine_);
            if(material_ && code!=SWMM_OK)return false;
            if(material_)material_->release();
            swmm_engine_destroy(engine_);
            engine_ = nullptr;
        }else if(material_)material_->release();
        return true;
    }
    static void checkMaterialRecovery(int code) {
        if(code)throw std::runtime_error(swmm_split_material_checkpoint_last_error());
    }
    void startMaterialNetwork() {
        if(material_->ended())return;
        material_->startNetwork(engine_,!materialRecovery_ || materialRecoveryOutputs_);
        if(materialRecovery_ && !materialRecoveryEnabled_) {
            checkMaterialRecovery(swmm_split_material_checkpoint_enable(engine_,materialRecoveryOutputs_));
            materialRecoveryEnabled_=true;
        }
    }
    void recordMaterialState(bool persist) {
        if(!material_)return;const auto snapshot=material_->snapshot();
        if(const auto it=fields_.find("material_session");it!=fields_.end())it->second->text=snapshot.dump();
        if(persist && materialOutputOwned_)write("material-session.json",snapshot);
    }
    void recordMaterialFailure()noexcept {
        if(!material_)return;
        try{recordMaterialState(true);material_->observePeer();}catch(...){}
    }
    /**
     * @brief Transition component to Failed with a diagnostic unless already Failed/Finished.
     * @param[in] message Failure text copied into lifecycle event/fatal error entry.
     * @note Does not itself close runtime engine; callers decide cleanup. Transition/signal
     * allocation can throw; no physical rollback or native clock change, exclusive component
     * lifecycle.
     */
    void fail(const std::string& message) {
        if(material_)material_->fail(message);
        if (status() != CS::Failed && status() != CS::Finished)
            transition(CS::Failed, material_ && !material_->error().empty() ? material_->error() :
                split_ && !split_->error.empty() ? message + ": " + split_->error : message);
    }
    /**
     * @brief Write one JSON boundary/configuration artifact under the component output
     * directory.
     * @param[in] name Relative artifact name chosen by internal caller, no path traversal
     * filtering here.
     * @param[in] value Stable JSON copied to pretty text, newline appended.
     * @throws std::runtime_error Open/write stream failure; JSON/allocation errors propagate.
     * @note Opens/truncates destination, no atomic replacement or rollback; partial file may
     * remain. No native water/clock change; caller owns isolated output directory and serialized
     * artifact writes.
     */
    void write(const std::string& name, const Json& value) const {
        std::ofstream file(output_ / name);
        file << value.dump(2) << '\n';
        if (!file)
            throw std::runtime_error("Cannot write component boundary ledger");
    }
    /**
     * @brief Read a native engine option into an owning dynamically sized string.
     * @param[in] engine Live metadata/runtime handle, caller retains ownership.
     * @param[in] key Nonnull null-terminated supported option name, read synchronously.
     * @return Owning text using readText/check; native errors throw. No wrapper clock/option
     * mutation; getter must NUL-terminate truncated results, allocation can throw.
     */
    std::string option(SWMM_Engine engine, const char* key) const {
        return readText([&](char* buffer, int length) {
            check(engine, swmm_options_get(engine, key, buffer, length));
        });
    }
#ifdef OPENSWMM_COMPONENT_2D_RESULTS
    /**
     * @brief Admit the bounded native MESH groundwater owner without changing configuration.
     * @param[in] engine Borrowed parsed handle; getters do not prepare or advance it.
     * @param[in] rows Authored aquifer row count.
     * @return Whether global and row settings fit the qualified water-only profile.
     * @note Native getter errors throw through check/readText; no output or state mutation.
     */
    bool groundwaterWaterProfile(SWMM_Engine engine, int rows) const {
        auto aquifer = [&](const char* key) {
            return readText([&](char* b, int n) {
                check(engine, swmm_gw2d_option_get(engine, key, b, n));
            });
        };
        auto transport = [&](const char* key) {
            return readText([&](char* b, int n) {
                check(engine, swmm_gw_transport_option_get(engine, key, b, n));
            });
        };
        if (rows < 1 || aquifer("MODE") != "MESH" ||
            option(engine, "FLOW_ROUTING") != "DYNWAVE" ||
            transport("THERMAL_MODEL") != "WATER_ONLY" || transport("CONDUCTION") != "NO")
            return false;
        for (const char* key :
             {"TRANSPORT_POLLUTANTS", "TRANSPORT_MSX", "TRANSPORT_AGE", "TRANSPORT_TEMPERATURE"})
            if (transport(key) != "NO") return false;
        // The managed native builder owns the broader source/Richards/closure
        // profiles and retains its own compatibility checks before initialization.
        if (managed_ || split_)
            return true;
        if (aquifer("CLOSURE") != "CLOSED_FORM" ||
            aquifer("SOIL_CHAR") != "GARDNER" || aquifer("GW_ET") != "NONE" ||
            aquifer("NODE_ENROLMENT") != "ROWS" || option(engine, "FLOW_ROUTING") != "DYNWAVE" ||
            swmm_subcatch_count(engine) || swmm_lid_usage_count(engine) ||
            transport("THERMAL_MODEL") != "WATER_ONLY" || transport("CONDUCTION") != "NO")
            return false;
        for (const char* key :
             {"TRANSPORT_POLLUTANTS", "TRANSPORT_MSX", "TRANSPORT_AGE", "TRANSPORT_TEMPERATURE"})
            if (transport(key) != "NO") return false;
        for (int row = 0; row < rows; ++row)
            for (const auto& property : std::vector<std::pair<const char*, int>>{
                     {"CLOSURE", SWMM_GW2D_CLOSURE_CLOSED_FORM}, {"SOIL_CHAR", SWMM_GW2D_SOIL_GARDNER}}) {
                double authored = 0, value = 0;
                const auto flag = std::string(property.first) + "_SET";
                check(engine, swmm_gw2d_row_get_property(engine, row, flag.c_str(), &authored));
                check(engine, swmm_gw2d_row_get_property(engine, row, property.first, &value));
                if (authored && value != property.second) return false;
            }
        for (int node = 0; node < swmm_node_count(engine); ++node) {
            int lid = -1;
            double saturation = 0;
            check(engine, swmm_node_get_lid(engine, node, &lid, &saturation));
            if (lid >= 0) return false;
        }
        return true;
    }
#endif
    /**
     * @brief Map interval tuple position to typed amount field name.
     * @param[in] index 0 water_m3,1 enthalpy_j,>=2 species name at index-2.
     * @return Owning field key; invalid species offset throws std::out_of_range. No state
     * mutation or unit conversion; string allocation may throw.
     */
    std::string amountField(size_t index) const {
        return index == 0   ? "water_m3"
               : index == 1 ? "enthalpy_j"
                            : "species." + species_.at(index - 2);
    }
    /**
     * @brief Create/retain an owned scalar output item with declared physical metadata.
     * @param[in] id Field ID, included in interval-profile description.
     * @param[in] type String or Float64 supported scalar data kind.
     * @param[in] unit Unit text, no numerical conversion.
     * @param[in] l Length dimension power.
     * @param[in] m Mass dimension power.
     * @param[in] t Time dimension power.
     * @param[in] kind Intensive/extensive quantity semantics.
     * @param[in] factor Multiplicative SI conversion factor, offset0.
     * @param[in] conceptURI Vocabulary URI of the quantity (a QUDT quantity kind), or empty.
     * @return Borrowed output pointer, owner vector retains object until clear/destruction; no
     * transfer. Allocation/description signal/append can throw, no engine/clock advance. Caller
     * updates fields map separately.
     */
    Output* add(const std::string& id, HC::DataKind type = HC::DataKind::Float64,
                std::string unit = "unitless", double l = 0, double m = 0, double t = 0,
                HC::ValueKind kind = HC::ValueKind::Extensive, double factor = 1,
                std::string conceptURI = {}) {
        auto item = std::make_unique<Output>(id, this, type, unit, l, m, t, kind, factor,
                                             std::move(conceptURI));
        auto* result = item.get();
        result->setDescription(std::string(profile) + ";field=" + id);
        outputs_.push_back(std::move(item));
        return result;
    }
    /**
     * @brief Construct typed interval token/time/current/cumulative/unreceived amount outputs.
     * @note Requires stable species list and empty matching output-field namespace. Owns created
     * outputs; fields_ stores borrowed pointers. Water m3, enthalpy J, pollutant kg; epoch days
     * has86400 SI scale, interval endpoints seconds. Repetition replaces map pointers while
     * older outputs remain listed; caller uses once per cleared initialization. Allocation may
     * leave partial ports, no physical advance.
     */
    void createTransferPorts() {
        fields_["transfer_id"] = add("transfer_id", HC::DataKind::String);
        for (const auto& field : {std::string("epoch_julian_day"), std::string("interval_start_s"),
                                  std::string("interval_end_s")})
            fields_[field] =
                add(field, HC::DataKind::Float64, field == "epoch_julian_day" ? "day" : "s", 0, 0,
                    1, HC::ValueKind::Intensive, field == "epoch_julian_day" ? 86400 : 1,
                    qudtQuantityKind("Time"));
        for (size_t i = 0; i < species_.size() + 2; ++i)
            for (const auto& prefix : {"", "exported.", "unreceived."}) {
                const std::string field = prefix + amountField(i);
                // Water volume, enthalpy and species mass, each named by its QUDT kind.
                fields_[field] = i == 0   ? add(field, HC::DataKind::Float64, "m3", 3, 0, 0,
                                                HC::ValueKind::Extensive, 1,
                                                qudtQuantityKind("Volume"))
                                 : i == 1 ? add(field, HC::DataKind::Float64, "J", 2, 1, -2,
                                                HC::ValueKind::Extensive, 1,
                                                qudtQuantityKind("Enthalpy"))
                                          : add(field, HC::DataKind::Float64, "kg", 0, 1, 0,
                                                HC::ValueKind::Extensive, 1,
                                                qudtQuantityKind("Mass"));
            }
    }
    /**
     * @brief Read metadata and execution qualification from a metadata-only native engine.
     * @param[in] engine Live opened metadata inspection handle, caller retains/closes it after
     * discovery.
     * @note Populates epoch astronomical Julian day from SWMM serial days, rounded millisecond
     * duration, routing step s, native IDs/transport matrix/process bytes/geometry and runnable
     * restriction reasons. Resolves selected unique external outfalls, qualified LEGACY
     * heat/pollutant SI receipts and finite strictly increasing exchange schedule ending at
     * duration. Current profile rejects runtime plugins, unqualified spatial profiles/output-path
     * collisions, network_only watershed, MSX/count-based outfall exports. Does not itself open
     * runtime/start/advance or write files; reads process configuration bytes. Errors throw with
     * partial metadata/selection retained, exclusive initialization and immutable source files.
     */
    void discover(SWMM_Engine engine);
    /**
     * @brief Populate parsed geometry, watershed/LID/snow and optional surface/GW catalogues.
     * @param[in] engine Live metadata handle with stable native IDs/options; all API failures
     * checked.
     * @note Node/link/subcatch coordinates remain as-authored with explicit project-length
     * label/CRS; missing endpoint coordinates null. 2D vertices/coupling areas converted to m/m2
     * and mesh indices0-based, GW parameter values retain project units with labels. Optional
     * compile branch reports availability, not qualified spatial runtime. No model
     * initialization/physical advance; JSON allocation/API errors can leave partial metadata.
     * Caller owns metadata lifecycle and native handle.
     */
    void discoverGeometry(SWMM_Engine engine);
    /**
     * @brief Append authored boundary/source records through parsed-stage public APIs.
     * @note Values retain explicitly labelled native units except constant surface stages,
     * converted with the mesh scale. Series/curve references are not sampled. No runtime
     * initialization or inventory transfer occurs; unsupported execution stays refused.
     */
    void discoverBoundaries(SWMM_Engine engine);
    /**
     * @brief Resolve a single direct receiving owner for the complete typed interval tuple.
     * @note Clears receiver_; disconnected tuple permitted. Any connected field requires exactly
     * one same-ID reciprocal provider link for every token/time/amount field, no adapted
     * outputs, one other live owner. Receiver acknowledgment outputs must scalar String token
     * and Float64 SI extensive amounts with matching unit caption/dimensions/no offset. Does not
     * connect/update receiver itself. Throws on first incompatibility after partial pointer
     * resolution; host serializes wiring/lifetimes.
     */
    void validateConnections();
    /**
     * @brief Check and record the preceding already-debited export's full receiver acceptance.
     * @note No receiver/pending token returns. Reads receiver scalar outputs without asking it
     * to advance; token must match, receiver not Failed, accepted water finite in0..pending and
     * all signed energy/ nonnegative species exactly same fraction (exact floating equality).
     * Publishes residual/unreceived and writes accepted JSON before refusing partial acceptance;
     * stops run because native debit cannot be undone. Full acceptance clears pending tuple. No
     * native advancement; buffer/API/file/allocation errors throw with partial
     * residual/artifacts retained. Live reciprocal receiver and exclusive phase required.
     */
    void verifyReceipt();
    /**
     * @brief Persist native cumulative outfall receipt diagnostics immediately after attempted
     * advancement.
     * @param[in] end Requested elapsed boundary s.
     * @param[in] code Native advance status retained even on failure.
     * @note Reads elapsed best-effort (fallback previous time), stores each receipt query
     * status/flags, water/reversal m3, native enthalpy J and species kg, writes sequence
     * artifact before check throws. Does not validate/consume/difference receipts or change
     * physical state. Output/JSON errors may throw and mask original advance status, partial
     * file retained; valid runtime handle/selected IDs required.
     */
    void recordNativeBoundary(double end, int code) const;
};

void OpenSWMMEngineComponent::discover(SWMM_Engine engine) {
    surfaceExecution_ = false;
    double start = 0, end = 0;
    check(engine, swmm_get_start_time(engine, &start));
    check(engine, swmm_get_end_time(engine, &end));
    epoch_ = start + 2415018.5;
    duration_ = std::round((end - start) * 86400000.) / 1000.;
    check(engine, swmm_get_routing_step(engine, &routingStep_));
    if (!std::isfinite(duration_) || duration_ <= 0)
        throw std::invalid_argument("Model must have a positive finite duration");
    metadata_ = {{"schema", "openswmm.component-metadata/1"},
                 {"instance_id", id()},
                 {"role", role_},
                 {"epoch_julian_day", epoch_},
                 {"duration_s", duration_},
                 {"model", pathText(model_)},
                 {"nodes", Json::array()},
                 {"links", Json::array()},
                 {"pollutants", Json::array()},
                 {"reaction_species", Json::array()},
                 {"transport", Json::array()},
                 {"process_components", Json::array()},
                 {"subcatchment_count", swmm_subcatch_count(engine)},
                 {"quality_solver", option(engine, "QUALITY_SOLVER")},
                 {"flow_units", option(engine, "FLOW_UNITS")},
                 {"checkpointing", false},
                 {"cloneable", false},
                 {"legacy_aquifer_count", swmm_aquifer_count(engine)}};
    for (int i = 0; i < swmm_node_count(engine); ++i) {
        int type = 0;
        check(engine, swmm_node_get_type(engine, i, &type));
        metadata_["nodes"].push_back({{"id", swmm_node_id(engine, i)}, {"type", type}});
    }
    for (int i = 0; i < swmm_link_count(engine); ++i) {
        int from = -1, to = -1;
        check(engine, swmm_link_get_from_node(engine, i, &from));
        check(engine, swmm_link_get_to_node(engine, i, &to));
        metadata_["links"].push_back({{"id", swmm_link_id(engine, i)},
                                      {"from", swmm_node_id(engine, from)},
                                      {"to", swmm_node_id(engine, to)}});
    }
    const int np = swmm_pollutant_count(engine);
    for (int i = 0; i < np; ++i) {
        int units = -1;
        check(engine, swmm_pollutant_get_units(engine, i, &units));
        const std::string name = swmm_pollutant_id(engine, i);
        species_.push_back(name);
        metadata_["pollutants"].push_back({{"id", name},
                                           {"concentration_units", units == 0   ? "mg/L"
                                                                   : units == 1 ? "ug/L"
                                                                                : "#/L"},
                                           {"export_amount_units", units == 2 ? "counts" : "kg"}});
    }
    const int nr = swmm_reaction_species_count(engine);
    for (int i = 0; i < nr; ++i) {
        std::vector<char> name(256), units(128);
        int wall = 0;
        double atol = 0, rtol = 0;
        for (;;) {
            check(engine, swmm_reaction_species_get(
                              engine, i, name.data(), static_cast<int>(name.size()), &wall,
                              units.data(), static_cast<int>(units.size()), &atol, &rtol));
            const bool fullName = std::strlen(name.data()) == name.size() - 1,
                       fullUnits = std::strlen(units.data()) == units.size() - 1;
            if (!fullName && !fullUnits)
                break;
            if (fullName)
                name.resize(name.size() * 2);
            if (fullUnits)
                units.resize(units.size() * 2);
        }
        metadata_["reaction_species"].push_back({{"id", name.data()},
                                                 {"kind", wall ? "WALL" : "BULK"},
                                                 {"declared_units", units.data()},
                                                 {"atol", atol},
                                                 {"rtol", rtol}});
    }
    SWMM_TransportMatrix matrix{};
    check(engine, swmm_get_transport_matrix(engine, &matrix));
    for (int domain = 0; domain < SWMM_TRANSPORT_DOMAIN_COUNT; ++domain)
        for (int cls = 0; cls < SWMM_TRANSPORT_CLASS_COUNT; ++cls) {
            const auto& cell = matrix.cell[domain][cls];
            metadata_["transport"].push_back({{"domain", swmm_transport_domain_name(domain)},
                                              {"class", swmm_transport_class_name(cls)},
                                              {"state", cell.state},
                                              {"count", cell.count},
                                              {"reason", cell.reason}});
        }
    for (int i = 0; i < swmm_process_component_count(engine); ++i) {
        char ignored[1];
        const auto id = readText([&](char* b, int n) {
            check(engine, swmm_process_component_get(engine, i, b, n, ignored, 1, ignored, 1));
        });
        const auto path = readText([&](char* b, int n) {
            check(engine, swmm_process_component_get(engine, i, ignored, 1, ignored, 1, b, n));
        });
        Json arguments = Json::array();
        bool foreign = id.find_first_of("/\\") != std::string::npos || id.ends_with(".so") ||
                       id.ends_with(".dll") || id.ends_with(".dylib");
        const int count = swmm_process_component_argument_count(engine, i);
        if (count < 0) throw std::runtime_error("Cannot inspect process component arguments");
        for (int argument = 0; argument < count; ++argument) {
            const auto key = readText([&](char* b, int n) {
                check(engine, swmm_process_component_argument_get(engine, i, argument, b, n, ignored, 1));
            });
            const auto value = readText([&](char* b, int n) {
                check(engine, swmm_process_component_argument_get(engine, i, argument, ignored, 1, b, n));
            });
            arguments.push_back({{"name", key}, {"value", value}});
            foreign = foreign || key == "library";
        }
        metadata_["process_components"].push_back({{"id", id}, {"configuration_file", path},
                                                   {"arguments", arguments}, {"foreign", foreign}});
        if (foreign)
            reasons_.push_back("Native hosted components require separate nested graph ownership/recovery qualification in the engine wrapper");
        if (!path.empty())
            componentFiles_[pathFromText(path)] = readFile(pathFromText(path));
    }
    if (role_ == "network_only" && swmm_subcatch_count(engine) > 0)
        reasons_.push_back(
            "network_only requires a model without subcatchments; use complete_model to retain native watershed ownership");
    int pluginCount = 0;
    check(engine, swmm_plugins_count(engine, &pluginCount));
    if (pluginCount)
        reasons_.push_back(
            "Runtime [PLUGINS] require separate configured-metadata qualification in this component profile");
    int vertices = 0, spatialRows = 0;
    bool spatialConfigured = false;
    std::string groundwaterMode = "unavailable";
#ifdef OPENSWMM_HAS_2D
    (void)swmm_2d_vertex_count(engine, &vertices);
    check(engine, swmm_gw2d_row_count(engine, &spatialRows));
    groundwaterMode = readText(
        [&](char* b, int n) { check(engine, swmm_gw2d_option_get(engine, "MODE", b, n)); });
    spatialConfigured = readText([&](char* b, int n) {
                            check(engine, swmm_gw2d_option_get(engine, "CONFIGURED", b, n));
                        }) == "YES";
#endif
    metadata_["surface_vertex_count"] = vertices;
    metadata_["explicit_groundwater_row_count"] = spatialRows;
    metadata_["explicit_groundwater_mode"] = groundwaterMode;
    metadata_["explicit_groundwater_configured"] = spatialConfigured;
    const bool groundwaterConfigured = spatialConfigured || spatialRows;
    bool groundwaterExecution = false;
    bool meshTransportExecution = false;
    if (vertices && !material_) {
        auto surfaceOption = [&](const char* key) {
            return readText([&](char* b, int n) {
                check(engine, swmm_options_get_ext(engine, key, b, n));
            });
        };
#ifdef OPENSWMM_COMPONENT_2D_RESULTS
        surfaceExecution_ = (role_ == "complete_model" || split_) &&
                            option(engine, "IGNORE_2D") != "YES" &&
                            option(engine, "IGNORE_QUALITY") == "YES" &&
                            option(engine, "HEAT_TRANSPORT") != "YES" && nr == 0 &&
                            surfaceOption("BACKEND") == "CPU" &&
                            surfaceOption("RECONSTRUCTION_ORDER") == "1" &&
                            surfaceOption("TRANSPORT_POLLUTANTS") == "NO" &&
                            surfaceOption("TRANSPORT_MSX") == "NO" &&
                            surfaceOption("TRANSPORT_AGE") == "NO" &&
                            surfaceOption("TRANSPORT_TEMPERATURE") == "NO" &&
                            surfaceOption("BED_HEAT_MODEL") == "NONE";
        if (groundwaterConfigured) {
            groundwaterExecution = surfaceExecution_ && groundwaterWaterProfile(engine, spatialRows);
            surfaceExecution_ = groundwaterExecution;
        }
        // Native managed sessions already own matched mesh/GW tracer, age and
        // porous-ground heat profiles. No watershed or network is duplicated.
        auto groundwaterTransport = [&](const char* key) {
            return readText([&](char* b, int n) {
                check(engine, swmm_gw_transport_option_get(engine, key, b, n));
            });
        };
        meshTransportExecution = managed_ && role_ == "complete_model" &&
            groundwaterConfigured && spatialRows > 0 && groundwaterMode == "MESH" &&
            swmm_node_count(engine) == 0 && swmm_link_count(engine) == 0 &&
            swmm_subcatch_count(engine) == 0 && nr == 0 &&
            option(engine, "IGNORE_2D") != "YES" && surfaceOption("BACKEND") == "CPU" &&
            surfaceOption("RECONSTRUCTION_ORDER") == "1" &&
            ((option(engine, "HEAT_TRANSPORT") == "YES" &&
              surfaceOption("BED_HEAT_MODEL") == "GROUND_COLUMN" &&
              surfaceOption("HEAT_MODEL") == "COUPLED" &&
              groundwaterTransport("THERMAL_MODEL") == "POROUS_EQUILIBRIUM") ||
             (option(engine, "HEAT_TRANSPORT") != "YES" &&
              surfaceOption("BED_HEAT_MODEL") == "NONE")) &&
            surfaceOption("TRANSPORT_MSX") == "NO" &&
            groundwaterTransport("TRANSPORT_MSX") == "NO";
        if (meshTransportExecution) surfaceExecution_ = groundwaterExecution = true;
#endif
        if (!surfaceExecution_)
            reasons_.push_back("Surface component requires a qualified complete-model CPU first-order water or managed matched mesh/GW transport profile and HDF5 results writer");
    }
    if (groundwaterConfigured && !groundwaterExecution)
        reasons_.push_back("Explicit groundwater requires a qualified MESH water profile or managed matched mesh/GW transport profile");
    if (split_) {
        if (!vertices || swmm_split_validate_input(engine) != SWMM_OK)
            reasons_.push_back(std::string("Split input: ") + swmm_get_last_error_msg(engine));
        surfaceExecution_ = split_->surface;
    }
    if(material_){
        if(!vertices || swmm_split_material_validate_input(engine)!=SWMM_OK)
            reasons_.push_back(std::string("Material input: ")+swmm_split_material_last_error());
        surfaceExecution_=material_->surface;
    }
    metadata_["managed_session"] = managed_;
    if (managed_ && (!config_.value("outfalls", Json::array()).empty() || role_ != "complete_model"))
        reasons_.push_back("Managed sessions require complete_model without external outfall ports");
    if (managed_ && vertices && !groundwaterExecution)
        reasons_.push_back("Managed spatial sessions require a qualified native mesh aquifer profile");
    metadata_["execution_profile"] = meshTransportExecution ? "native_cpu_managed_transport/1"
                                     : groundwaterExecution && managed_ ? "native_cpu_managed_water/1"
                                     : groundwaterExecution ? "native_cpu_integrated_water/1"
                                     : surfaceExecution_ ? "native_cpu_surface_water/1" : "native_cpu_1d/1";
    metadata_["result_files"] = {{"report", "model.rpt"}, {"binary", "model.out"}};
    if (surfaceExecution_)
        metadata_["result_files"]["surface"] = "model.2d.h5";
    if (split_) {
        metadata_["execution_profile"] = "openswmm.node-surface-water/1";
        metadata_["phase_port"] = waterPhasePort;
        metadata_["phase_packet_schemas"] = {"openswmm.node-surface-water/1", "openswmm.node-surface-water/2", "openswmm.node-surface-water/3", "openswmm.node-surface-water/4", "openswmm.node-surface-water/5", "openswmm.node-surface-water/6", "openswmm.node-surface-water/7", "openswmm.node-surface-water/8", "openswmm.node-surface-water/9"};
        metadata_["groundwater_owner"] = "integrated2d";
        metadata_["watershed_owner"] = "network";
        metadata_["split_source_scope"] = "ordinary runoff and conventional LID outlets through native network routing";
        if (split_->surface) metadata_["result_files"].erase("binary");
        if (!config_.value("surface_results",true)) metadata_["result_files"].erase("surface");
    }
    if(material_){
        metadata_["execution_profile"]="openswmm.passive-material-pair/10";
        metadata_["phase_port"]=materialPhasePort;
        metadata_["phase_packet_schemas"]={"openswmm.node-surface-water/10"};
        metadata_["material_trust"]="bound_trusted_in_process";
        metadata_["material_native_volume_units"]="ft3";
        metadata_["material_native_amount_basis"]="declared_concentration*ft3";
        metadata_["material_journal_capacity"]=config_.value("material_capacity",Json(4096));
        metadata_["receiving_quality_owner"]="split_material_network";
        metadata_["surface_material_owner"]="split_material_surface";
        metadata_["material_checkpointing"]=false;
        metadata_["material_collective_recovery"]={{"service",OPENSWMM_COMPONENT_RECOVERY_SYMBOL},
            {"version",OPENSWMM_COMPONENT_RECOVERY_VERSION},{"enrolled",false},{"scope","exclusive_pair_fresh_restore"}};
        if(material_->surface)metadata_["result_files"].erase("binary");
        if(!config_.value("surface_results",true))metadata_["result_files"].erase("surface");
    }
    // Retain ordinary model input files. Additional authored output channels
    // need their own isolated-output mapping before independent instances run.
    auto fileField = [&](const char* key) {
        return readText([&](char* b, int n) { check(engine, swmm_files_get(engine, key, b, n)); });
    };
    if (!fileField("OUTFLOWS_PATH").empty())
        reasons_.push_back(
            "Model-declared SAVE OUTFLOWS needs output-path isolation before component execution");
    int hotstarts = 0;
    check(engine, swmm_hotstart_saves_count(engine, &hotstarts));
    if (hotstarts)
        reasons_.push_back(
            "Model-declared SAVE HOTSTART needs output-path isolation before component execution");
    for (const char* mode : {"RAINFALL_MODE", "RUNOFF_MODE", "RDII_MODE"})
        if (fileField(mode) == "SAVE")
            reasons_.push_back(std::string("Model-declared ") + mode +
                               " SAVE needs output-path isolation");
    for (int i = 0; i < swmm_lid_usage_count(engine); ++i) {
        const auto owner = std::to_string(i);
        char ignored[1];
        const auto path = readText([&](char* b, int n) {
            check(engine, swmm_file_path_get(engine, SWMM_FILE_LID_REPORT, owner.c_str(), ignored,
                                             1, b, n));
        });
        if (!path.empty())
            reasons_.push_back(
                "Detailed LID reports need output-path isolation before component execution");
    }
#ifdef OPENSWMM_HAS_2D
    char ignored[1];
    const auto surfacePath = readText([&](char* b, int n) {
        check(engine, swmm_file_path_get(engine, SWMM_FILE_OUTPUT_2D, nullptr, ignored, 1, b, n));
    });
    if (!surfacePath.empty() && (!surfaceExecution_ || material_))
        reasons_.push_back(
            "Model-declared 2D results need output-path isolation before component execution");
#endif
    const auto selected = config_.value("outfalls", Json::array());
    if (!selected.is_array())
        throw std::invalid_argument("outfalls must be an array of stable native outfall IDs");
    std::set<int> seen;
    for (const auto& value : selected) {
        const std::string name = value.get<std::string>();
        const int node = swmm_node_index(engine, name.c_str());
        int type = -1, route = -1;
        if (node < 0 || !seen.insert(node).second)
            throw std::invalid_argument("Unknown or repeated outfall: " + name);
        check(engine, swmm_node_get_type(engine, node, &type));
        if (type != SWMM_NODE_OUTFALL)
            throw std::invalid_argument("Selected export is not an outfall: " + name);
        check(engine, swmm_node_get_outfall_route_to(engine, node, &route));
        if (route >= 0)
            throw std::invalid_argument("Selected outfall is retained by a native subcatchment: " +
                                        name);
        outfalls_.push_back({node, {}, {}});
    }
    if (!outfalls_.empty()) {
        if (surfaceExecution_)
            reasons_.push_back("External outfall exports with spatial execution require separate receipt qualification");
        if (nr)
            reasons_.push_back(
                "MSX outfall amount receipts are not yet qualified; MSX remains supported in uncoupled native output");
        if (option(engine, "QUALITY_SOLVER") != "LEGACY" ||
            option(engine, "IGNORE_QUALITY") == "YES")
            reasons_.push_back("Outfall export requires the qualified LEGACY quality profile");
        if (option(engine, "HEAT_TRANSPORT") != "YES")
            reasons_.push_back(
                "This interval profile requires native HEAT_TRANSPORT for enthalpy receipts");
        for (const auto& row : metadata_["pollutants"]) {
            const auto name = row.at("id").get<std::string>();
            if (row.at("export_amount_units") != "kg")
                reasons_.push_back("Count-based pollutant " + name +
                                   " needs an explicit physical-mass conversion");
            if (name.empty() || !std::all_of(name.begin(), name.end(), [](unsigned char c) {
                    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
                }))
                reasons_.push_back("Pollutant ID is outside the interval reference profile: " +
                                   name);
        }
        times_ = config_.at("exchange_times_s").get<std::vector<double>>();
        double last = 0;
        for (double time : times_) {
            if (!std::isfinite(time) || time <= last || time > duration_ + 1e-6)
                throw std::invalid_argument(
                    "Exchange times must increase from zero within the model duration");
            last = time;
        }
        if (times_.empty() || std::abs(times_.back() - duration_) > 1e-6)
            throw std::invalid_argument("The final exchange must equal the native model duration");
        times_.back() = duration_;
    } else
        times_.clear();
    metadata_["selected_outfalls"] = selected;
    metadata_["interval_profile"] = outfalls_.empty() ? Json(nullptr) : Json(profile);
    discoverGeometry(engine);
    metadata_["execution_restrictions"] = reasons_;
    metadata_["runnable"] = reasons_.empty();
}

// Parsed authoring only: no kernel initialization, mapping resolution or time
// advancement. Native surface/GW physics continue to share one internal owner.
void OpenSWMMEngineComponent::discoverGeometry(SWMM_Engine engine) {
    auto nodeId = [&](int i) -> Json {
        return i < 0 ? Json(nullptr) : Json(swmm_node_id(engine, i));
    };
    auto subcatchId = [&](int i) -> Json {
        return i < 0 ? Json(nullptr) : Json(swmm_subcatch_id(engine, i));
    };
    const auto crs =
        readText([&](char* b, int n) { check(engine, swmm_spatial_get_crs(engine, b, n)); });
    const auto flow = metadata_.at("flow_units").get<std::string>();
    const bool us = flow == "CFS" || flow == "GPM" || flow == "MGD";
    metadata_["spatial"] = {{"crs", crs.empty() ? Json(nullptr) : Json(crs)},
                            {"coordinate_units", "as_authored"},
                            {"project_length_units", us ? "ft" : "m"},
                            {"catalogue_stage", "parsed"},
                            {"mesh_index_base", 0}};
    std::vector<bool> located(swmm_node_count(engine));
    for (int i = 0; i < swmm_node_count(engine); ++i) {
        int present = 0;
        check(engine, swmm_spatial_node_has_coord(engine, i, &present));
        located[i] = present != 0;
        Json xy = nullptr;
        if (present) {
            double x = 0, y = 0;
            check(engine, swmm_spatial_get_node_coord(engine, i, &x, &y));
            xy = {x, y};
        }
        metadata_["nodes"][i]["coordinates"] = xy;
    }
    for (int i = 0; i < swmm_link_count(engine); ++i) {
        int n = 0, from = -1, to = -1;
        check(engine, swmm_spatial_get_link_vertex_count(engine, i, &n));
        check(engine, swmm_link_get_from_node(engine, i, &from));
        check(engine, swmm_link_get_to_node(engine, i, &to));
        std::vector<double> x(n), y(n);
        Json coordinates = Json::array();
        if (n)
            check(engine, swmm_spatial_get_link_vertices(engine, i, x.data(), y.data(), n));
        for (int j = 0; j < n; ++j) {
            const bool missing =
                (j == 0 && from >= 0 && !located[from]) || (j == n - 1 && to >= 0 && !located[to]);
            coordinates.push_back(missing ? Json(nullptr) : Json{x[j], y[j]});
        }
        metadata_["links"][i]["coordinates"] = coordinates;
    }
    metadata_["subcatchments"] = Json::array();
    Json legacy = {
        {"owner", "native_watershed"}, {"aquifers", Json::array()}, {"connections", Json::array()}};
    for (int i = 0; i < swmm_aquifer_count(engine); ++i)
        legacy["aquifers"].push_back(swmm_aquifer_id(engine, i));
    for (int i = 0; i < swmm_subcatch_count(engine); ++i) {
        int n = 0, node = -1, subcatch = -1, aquifer = -1, gwNode = -1;
        check(engine, swmm_spatial_get_subcatch_polygon_count(engine, i, &n));
        std::vector<double> x(n), y(n);
        Json polygon = Json::array();
        if (n)
            check(engine, swmm_spatial_get_subcatch_polygon(engine, i, x.data(), y.data(), n));
        for (int j = 0; j < n; ++j)
            polygon.push_back({x[j], y[j]});
        check(engine, swmm_subcatch_get_outlet(engine, i, &node));
        check(engine, swmm_subcatch_get_outlet_subcatch(engine, i, &subcatch));
        check(engine, swmm_subcatch_get_aquifer(engine, i, &aquifer));
        check(engine, swmm_subcatch_get_gw_node(engine, i, &gwNode));
        const char* pack = swmm_subcatch_get_snowpack(engine, i);
        metadata_["subcatchments"].push_back(
            {{"id", subcatchId(i)},
             {"polygon", polygon},
             {"outlet_node", nodeId(node)},
             {"outlet_subcatchment", subcatchId(subcatch)},
             {"snowpack", pack && *pack ? Json(pack) : Json(nullptr)}});
        if (aquifer >= 0)
            legacy["connections"].push_back({{"subcatchment", subcatchId(i)},
                                             {"aquifer", swmm_aquifer_id(engine, aquifer)},
                                             {"node", nodeId(gwNode)}});
    }
    legacy["configured"] = !legacy["connections"].empty();
    metadata_["legacy_groundwater"] = legacy;
    int snowModel = 0;
    check(engine, swmm_snow_get_model(engine, &snowModel));
    metadata_["snow"] = {{"owner", "native_watershed"},
                         {"default_model", snowModel == SWMM_SNOW_UEB ? "UEB" : "DEGREE_DAY"},
                         {"packs", Json::array()}};
    for (int i = 0; i < swmm_snowpack_count(engine); ++i) {
        SWMM_UebSnowpack pack{};
        check(engine, swmm_snowpack_get_ueb(engine, i, &pack));
        const int model = pack.model == SWMM_SNOW_DEFAULT ? snowModel : pack.model;
        metadata_["snow"]["packs"].push_back(
            {{"id", swmm_snowpack_id(engine, i)},
             {"model", model == SWMM_SNOW_UEB ? "UEB" : "DEGREE_DAY"}});
    }
    metadata_["lids"] = {{"controls", Json::array()},
                         {"watershed_usage", Json::array()},
                         {"node_assignments", Json::array()},
                         {"node_outlets", Json::array()}};
    for (int i = 0; i < swmm_lid_count(engine); ++i) {
        int type = 0;
        check(engine, swmm_lid_get_type(engine, i, &type));
        SWMM_LidRichardsOptions options{};
        check(engine, swmm_lid_richards_options_get(engine, i, &options));
        metadata_["lids"]["controls"].push_back(
            {{"id", swmm_lid_id(engine, i)},
             {"type", type},
             {"node_flow_model", options.model ? "RICHARDS" : "CLASSICAL"}});
    }
    for (int i = 0; i < swmm_lid_usage_count(engine); ++i) {
        int subcatch = -1, lid = -1, number = 0, toPerv = 0;
        double area = 0;
        check(engine, swmm_lid_usage_get(engine, i, &subcatch, &lid, &number, &area, nullptr,
                                         nullptr, nullptr, &toPerv, nullptr));
        metadata_["lids"]["watershed_usage"].push_back({{"index", i},
                                                        {"subcatchment", subcatchId(subcatch)},
                                                        {"control", swmm_lid_id(engine, lid)},
                                                        {"number", number},
                                                        {"unit_area_project", area},
                                                        {"to_pervious", toPerv != 0}});
    }
    for (int i = 0; i < swmm_node_count(engine); ++i) {
        int lid = -1, bottom = 0;
        double saturation = 0;
        check(engine, swmm_node_get_lid(engine, i, &lid, &saturation));
        if (lid < 0)
            continue;
        check(engine, swmm_node_get_lid_bottom(engine, i, &bottom));
        metadata_["lids"]["node_assignments"].push_back(
            {{"node", nodeId(i)},
             {"control", swmm_lid_id(engine, lid)},
             {"bottom", bottom == SWMM_LID_BOTTOM_MESH_AQUIFER ? "MESH_AQUIFER"
                        : bottom == SWMM_LID_BOTTOM_SEALED     ? "SEALED"
                                                               : "CURRENT"}});
    }
    for (int i = 0; i < swmm_link_count(engine); ++i) {
        int layer = 0, top = 0;
        check(engine, swmm_lid_node_outlet_get(engine, i, &layer, &top));
        if (layer)
            metadata_["lids"]["node_outlets"].push_back(
                {{"link", swmm_link_id(engine, i)}, {"layer", layer},
                 {"anchor", top ? "TOP" : "BOTTOM"}});
    }
    Json surface = {{"owner", "native_integrated2d"},
                    {"vertices_m", Json::array()},
                    {"cells", Json::array()},
                    {"vertex_connections", Json::array()},
                    {"cell_connections", Json::array()},
                    {"boundaries", Json::array()},
                    {"boundary_quality", Json::array()}};
    Json gw = {{"owner", "native_integrated2d"},
               {"internal_exchange", "native_surface_groundwater"},
               {"configured", metadata_["explicit_groundwater_configured"]},
               {"mode", metadata_["explicit_groundwater_mode"]},
               {"rows", Json::array()},
               {"node_beds", Json::array()},
               {"link_beds", Json::array()},
               {"boundaries", Json::array()},
               {"boundary_quality", Json::array()},
               {"sources", Json::array()}};
#ifdef OPENSWMM_HAS_2D
    surface["available"] = true;
    gw["available"] = true;
    gw["geometry_source"] = metadata_["explicit_groundwater_mode"] == "PER_SUBCATCH"
                                ? "subcatchments"
                                : "surface.cells";
    gw["options"] = Json::object();
    for (const char* key :
         {"NODE_ENROLMENT", "LINK_SEEPAGE", "GW_ET", "GW_ET_EFFECTIVE", "LINK_SEEPAGE_EFFECTIVE",
          "GW_CLOCK", "GW_BASE_STEP", "GW_MAX_STEP", "GW_BUDGET_FRACTION", "GW_HEAD_TOLERANCE",
          "GW_BINDS_SURFACE_DT", "CLOSURE", "SOIL_CHAR"})
        gw["options"][key] = readText(
            [&](char* b, int n) { check(engine, swmm_gw2d_option_get(engine, key, b, n)); });
    gw["transport_options"] = Json::object();
    for (const char* key : {"TRANSPORT_POLLUTANTS", "TRANSPORT_MSX", "TRANSPORT_AGE", "TRANSPORT_TEMPERATURE",
                            "THERMAL_MODEL", "CONDUCTION"})
        gw["transport_options"][key] = readText([&](char* b, int n) {
            check(engine, swmm_gw_transport_option_get(engine, key, b, n));
        });
    const int vertices = metadata_["surface_vertex_count"].get<int>();
    if (vertices) {
        int ownerCount = 0;
        check(engine, swmm_surface_owner_get(engine, nullptr, 0, &ownerCount));
        surface["ownership"] = {{"authored_count", ownerCount}, {"objects", Json::array()},
                                 {"shares", Json::array()}, {"revision", nullptr}};
        if (ownerCount) {
            int objects = 0, shares = 0, cells = 0, valid = 0;
            char token[32]{}, diagnostic[4096]{};
            check(engine, swmm_surface_owner_preview(engine, nullptr, -1, nullptr, 0, &objects,
                nullptr, 0, &shares, nullptr, 0, &cells, &valid, token, sizeof(token),
                diagnostic, sizeof(diagnostic), nullptr, nullptr));
            std::vector<SWMM_SurfaceOwnerObject> ownerObjects(objects);
            std::vector<SWMM_SurfaceOwnerShare> ownerShares(shares);
            check(engine, swmm_surface_owner_preview(engine, nullptr, -1, ownerObjects.data(), objects,
                &objects, ownerShares.data(), shares, &shares, nullptr, 0, &cells, &valid,
                token, sizeof(token), diagnostic, sizeof(diagnostic), nullptr, nullptr));
            auto& ownership = surface["ownership"];
            ownership["revision"] = token;
            ownership["valid"] = valid != 0;
            ownership["diagnostics"] = diagnostic;
            for (const auto& object : ownerObjects)
                ownership["objects"].push_back({{"subcatchment", object.name},
                    {"reviewed", object.reviewed != 0}, {"status", object.status},
                    {"declared_area_m2", object.declared_area}, {"inside_area_m2", object.inside_area},
                    {"outside_area_m2", object.outside_area}, {"reason", object.reason}});
            for (const auto& share : ownerShares)
                ownership["shares"].push_back({{"subcatchment", subcatchId(share.subcatch)},
                    {"cell", share.cell}, {"weather_area_m2", share.weather_area},
                    {"pervious_area_m2", share.pervious_area},
                    {"impervious_area_m2", share.impervious_area}, {"lid_area_m2", share.lid_area},
                    {"native_lid_area_m2", share.native_lid_area}});
            if (managed_ || split_) {
                if (!valid)
                    reasons_.push_back(std::string("Invalid native surface ownership: ") + diagnostic);
                else
                    ownershipToken_ = token;
                if (split_) {
                    metadata_["watershed_owners"]={{"ordinary","network"},{"reviewed","integrated2d"}};
                    if (split_->surface && !config_.value("surface_results",true))
                        reasons_.push_back("Reviewed split source ownership requires surface_results=true");
                }
            } else
                reasons_.push_back("Reviewed surface ownership requires managed_session=true");
        }
        double scale = 1;
        check(engine, swmm_2d_geometry_scale_to_si(engine, &scale));
        surface["source_scale_to_m"] = scale;
        for (int i = 0; i < vertices; ++i) {
            double x = 0, y = 0, z = 0;
            check(engine, swmm_2d_vertex_get_xyz(engine, i, &x, &y, &z));
            surface["vertices_m"].push_back({x * scale, y * scale, z * scale});
            int node = -1;
            check(engine, swmm_2d_vertex_get_coupled_node(engine, i, &node));
            if (node >= 0) {
                double cd = 0, area = 0;
                check(engine, swmm_2d_get_vertex_coupling_cd(engine, i, &cd));
                check(engine, swmm_2d_get_vertex_coupling_area(engine, i, &area));
                surface["vertex_connections"].push_back({{"vertex", i},
                                                         {"node", nodeId(node)},
                                                         {"cd", cd},
                                                         {"area_m2", area * scale * scale}});
            }
        }
        int cells = 0, rows = 0;
        check(engine, swmm_2d_cell_count(engine, &cells));
        for (int i = 0; i < cells; ++i) {
            int v[4], n = 0;
            check(engine, swmm_2d_cell_get_vertices(engine, i, v, &n));
            surface["cells"].push_back({{"index", i}, {"vertices", std::vector<int>(v, v + n)}});
        }
        check(engine, swmm_2d_triangle_coupling_rows(engine, &rows));
        for (int i = 0; i < rows; ++i) {
            int cell = -1, node = -1;
            double cd = 0, area = 0;
            check(engine, swmm_2d_get_triangle_coupling_row(engine, i, &cell, &node, &cd, &area));
            surface["cell_connections"].push_back({{"cell", cell},
                                                   {"node", nodeId(node)},
                                                   {"cd", cd},
                                                   {"area_m2", area * scale * scale}});
        }
    }
    const int rowCount = metadata_["explicit_groundwater_row_count"].get<int>();
    for (int i = 0; i < rowCount; ++i) {
        int scope = 0, cell = -1;
        double ks = 0, zs = 0, ts = 0, tr = 0, alpha = 0;
        auto tag = readText([&](char* b, int n) {
            check(engine,
                  swmm_gw2d_row_get(engine, i, &scope, b, n, &cell, &ks, &zs, &ts, &tr, &alpha));
        });
        gw["rows"].push_back({{"index", i},
                              {"scope", scope == SWMM_GW2D_SCOPE_CELL  ? "CELL"
                                        : scope == SWMM_GW2D_SCOPE_TAG ? "TAG"
                                                                       : "GLOBAL"},
                              {"tag", tag},
                              {"cell", scope == SWMM_GW2D_SCOPE_CELL ? Json(cell) : Json(nullptr)},
                              {"ks_project", ks},
                              {"thickness_project", zs},
                              {"theta_s", ts},
                              {"theta_r", tr},
                              {"alpha_project", alpha}});
    }
    int nodes = 0, links = 0;
    check(engine, swmm_gw2d_node_count(engine, &nodes));
    check(engine, swmm_gw2d_link_count(engine, &links));
    for (int i = 0; i < nodes; ++i) {
        int cell = -1, locate = 0, exchange = 0, automatic = 0;
        double kc = 0, dc = 0, area = 0;
        auto name = readText([&](char* b, int n) {
            check(engine, swmm_gw2d_node_get(engine, i, b, n, &cell, &kc, &dc, &area));
        });
        check(engine, swmm_gw2d_node_get_flags(engine, i, &locate, &exchange, &automatic));
        gw["node_beds"].push_back({{"node", name},
                                   {"cell", locate ? Json(nullptr) : Json(cell)},
                                   {"locate_at_initialize", locate != 0},
                                   {"exchange", exchange != 0},
                                   {"automatic", automatic != 0},
                                   {"kc_project", kc},
                                   {"thickness_project", dc},
                                   {"area_project", area}});
    }
    for (int i = 0; i < links; ++i) {
        int exchange = 0;
        double kc = 0, dc = 0;
        auto name = readText([&](char* b, int n) {
            check(engine, swmm_gw2d_link_get(engine, i, b, n, &kc, &dc, &exchange));
        });
        gw["link_beds"].push_back({{"link", name},
                                   {"exchange", exchange != 0},
                                   {"kc_project", kc},
                                   {"thickness_project", dc}});
    }
#else
    surface["available"] = false;
    gw["available"] = false;
    gw["geometry_source"] = nullptr;
#endif
    metadata_["surface"] = surface;
    metadata_["explicit_groundwater"] = gw;
    discoverBoundaries(engine);
}

void OpenSWMMEngineComponent::discoverBoundaries(SWMM_Engine engine) {
#ifdef OPENSWMM_HAS_2D
    auto& surface = metadata_["surface"];
    auto& gw = metadata_["explicit_groundwater"];
    if (!surface["cells"].empty()) {
        // Materialize authored edge slots only; this does not initialize the solver,
        // scale the mesh, resolve runtime owners or advance the inspection handle.
        check(engine, swmm_2d_prepare_for_edit(engine));
        const double scale = surface.at("source_scale_to_m");
        for (const auto& cell : surface["cells"]) {
            const int index = cell.at("index");
            const int nv = cell.at("vertices").size();
            for (int edge = 0; edge < nv; ++edge) {
                int exterior = 0;
                check(engine, swmm_2d_edge_is_exterior(engine, index, edge, &exterior));
                if (!exterior)
                    continue;
                int type = 0;
                check(engine, swmm_2d_get_edge_bc_type(engine, index, edge, &type));
                static constexpr const char* types[] = {
                    "WALL", "NORMAL_FLOW", "SPECIFIED_STAGE", "SPECIFIED_FLOW", "RATING_CURVE"};
                if (type < SWMM_2D_BC_WALL || type > SWMM_2D_BC_RATING_CURVE)
                    throw std::runtime_error("Unknown surface boundary type");
                Json row = {{"cell", index}, {"edge", edge}, {"type", types[type]}};
                double value = 0;
                if (type == SWMM_2D_BC_SPECIFIED_STAGE) {
                    check(engine, swmm_2d_get_edge_bc_head(engine, index, edge, &value));
                    const auto series = readText([&](char* b, int n) {
                        check(engine, swmm_2d_get_edge_bc_tseries_name(engine, index, edge, b, n));
                    });
                    row["timeseries"] = series;
                    if (series.empty())
                        row["head_m"] = value * scale;
                    else
                        row["timeseries_units"] = metadata_["spatial"]["project_length_units"];
                } else if (type == SWMM_2D_BC_SPECIFIED_FLOW) {
                    check(engine, swmm_2d_get_edge_bc_flow(engine, index, edge, &value));
                    const auto series = readText([&](char* b, int n) {
                        check(engine, swmm_2d_get_edge_bc_flow_tseries_name(engine, index, edge, b, n));
                    });
                    row["timeseries"] = series;
                    row["units"] = metadata_["flow_units"].get<std::string>() + "/m";
                    if (series.empty())
                        row["flow_native"] = value;
                } else if (type == SWMM_2D_BC_NORMAL_FLOW) {
                    check(engine, swmm_2d_get_edge_bc_slope(engine, index, edge, &value));
                    row["slope"] = value;
                } else if (type == SWMM_2D_BC_RATING_CURVE) {
                    row["stage_units"] = metadata_["spatial"]["project_length_units"];
                    row["flow_units"] = metadata_["flow_units"].get<std::string>() + "/m";
                    row["rating_curve"] = readText([&](char* b, int n) {
                        check(engine, swmm_2d_get_edge_bc_rating_curve_name(engine, index, edge, b, n));
                    });
                }
                surface["boundaries"].push_back(std::move(row));
            }
        }
    }
    int count = 0;
    check(engine, swmm_2d_boundary_quality_count(engine, &count));
    for (int i = 0; i < count; ++i) {
        int cell = -1, edge = -1;
        double value = 0;
        const auto species = readText([&](char* b, int n) {
            check(engine, swmm_2d_get_boundary_quality(engine, i, &cell, &edge, b, n, &value));
        });
        surface["boundary_quality"].push_back({{"cell", cell}, {"edge", edge},
                                               {"species", species},
                                               {"concentration_native", value}});
    }
    for (int i = 0; i < swmm_gw2d_boundary_count(engine); ++i) {
        SWMM_GwBoundary row{};
        std::vector<char> series(256);
        for (;;) {
            const int code = swmm_gw2d_boundary_get(engine, i, &row, series.data(),
                                                    static_cast<int>(series.size()));
            if (code == SWMM_ERR_BUFSIZE) {
                series.resize(series.size() * 2);
                continue;
            }
            check(engine, code);
            break;
        }
        const bool head = row.kind == SWMM_GW2D_BOUNDARY_HEAD;
        gw["boundaries"].push_back(
            {{"cell", row.cell}, {"edge", row.edge}, {"type", head ? "HEAD" : "FLOW"},
             {"value", row.value},
             {"units", head ? metadata_["spatial"]["project_length_units"] : Json("m3/s")},
             {"timeseries", series.data()}});
    }
    for (int i = 0; i < swmm_gw_boundary_quality_count(engine); ++i) {
        int cell = -1, edge = -1;
        double value = 0;
        char kind[16]{};
        const auto species = readText([&](char* b, int n) {
            check(engine, swmm_gw_boundary_quality_get(engine, i, &cell, &edge, b, n,
                                                       kind, sizeof(kind), &value, nullptr, 0));
        });
        const auto series = readText([&](char* b, int n) {
            check(engine, swmm_gw_boundary_quality_get(engine, i, nullptr, nullptr, nullptr, 0,
                                                       nullptr, 0, nullptr, b, n));
        });
        gw["boundary_quality"].push_back({{"cell", cell}, {"edge", edge},
                                          {"species", species}, {"kind", kind},
                                          {"value_native", value}, {"timeseries", series}});
    }
    for (int i = 0; i < swmm_gw_source_count(engine); ++i) {
        int scope = 0, cell = -1, xy = 0;
        double flow = 0, scale = 1, x = 0, y = 0;
        const auto name = readText([&](char* b, int n) {
            check(engine, swmm_gw_source_get(engine, i, b, n, &scope, nullptr, 0, &cell,
                                              &flow, nullptr, 0));
        });
        const auto tag = readText([&](char* b, int n) {
            check(engine, swmm_gw_source_get(engine, i, nullptr, 0, nullptr, b, n,
                                              nullptr, nullptr, nullptr, 0));
        });
        const auto series = readText([&](char* b, int n) {
            check(engine, swmm_gw_source_get(engine, i, nullptr, 0, nullptr, nullptr, 0,
                                              nullptr, nullptr, b, n));
        });
        check(engine, swmm_gw_source_scale_get(engine, i, &scale));
        check(engine, swmm_gw_source_xy_get(engine, i, &xy, &x, &y));
        Json source = {{"name", name}, {"scope", scope == SWMM_GW2D_SCOPE_CELL ? "CELL"
                                                : scope == SWMM_GW2D_SCOPE_TAG ? "TAG" : "GLOBAL"},
                       {"cell", scope == SWMM_GW2D_SCOPE_CELL ? Json(cell) : Json(nullptr)},
                       {"tag", tag}, {"coordinates", xy ? Json{x, y} : Json(nullptr)},
                       {"flow_m3_s", flow}, {"flow_timeseries", series}, {"scale", scale},
                       {"species_terms", Json::array()}};
        for (int term = 0; term < swmm_gw_source_species_count(engine, i); ++term) {
            double value = 0;
            char kind[16]{};
            const auto species = readText([&](char* b, int n) {
                check(engine, swmm_gw_source_species_get(engine, i, term, b, n, kind,
                                                         sizeof(kind), &value, nullptr, 0));
            });
            const auto timeseries = readText([&](char* b, int n) {
                check(engine, swmm_gw_source_species_get(engine, i, term, nullptr, 0, nullptr,
                                                         0, nullptr, b, n));
            });
            source["species_terms"].push_back({{"species", species}, {"kind", kind},
                                                {"value_native", value},
                                                {"timeseries", timeseries}});
        }
        gw["sources"].push_back(std::move(source));
    }
#else
    (void)engine;
#endif
}

void OpenSWMMEngineComponent::validateConnections() {
    receiver_ = nullptr;
    if(material_)material_->validate();
    if (split_) split_->validate();
    if (outfalls_.empty())
        return;
    std::vector<Output*> ports;
    for (const auto& name :
         {"transfer_id", "epoch_julian_day", "interval_start_s", "interval_end_s"})
        ports.push_back(fields_.at(name));
    for (size_t i = 0; i < species_.size() + 2; ++i)
        ports.push_back(fields_.at(amountField(i)));
    bool connected = false;
    for (auto* output : ports)
        connected = connected || !output->consumers().empty() || !output->adaptedOutputs().empty();
    if (!connected)
        return;
    for (auto* output : ports) {
        const auto consumers = output->consumers();
        if (consumers.size() != 1 || !output->adaptedOutputs().empty() ||
            consumers[0]->id() != output->id() || consumers[0]->provider() != output)
            throw std::invalid_argument("Connect each interval field once to one direct receiver");
        auto* owner = consumers[0]->modelComponent();
        if (!owner || owner == this || (receiver_ && receiver_ != owner))
            throw std::invalid_argument("A complete interval tuple must have one receiving owner");
        receiver_ = owner;
    }
    auto require = [&](const std::string& name, HC::DataKind kind) -> HC::IOutput* {
        for (auto* item : receiver_->outputs())
            if (item && item->id() == name && item->shape() == std::vector<int64_t>{1} &&
                item->dataKind() == kind)
                return item;
        throw std::invalid_argument("Receiver must publish scalar " + name);
    };
    require("accepted_transfer_id", HC::DataKind::String);
    for (size_t i = 0; i < species_.size() + 2; ++i) {
        const auto field = amountField(i);
        const auto* output = require("accepted." + field, HC::DataKind::Float64);
        const auto* expected = dynamic_cast<HC::IQuantity*>(fields_.at(field)->valueDefinition());
        const auto* actual = dynamic_cast<HC::IQuantity*>(output->valueDefinition());
        if (!actual || !actual->unit() || !actual->unit()->dimensions() ||
            actual->valueKind() != expected->valueKind() ||
            actual->unit()->caption() != expected->unit()->caption() ||
            actual->unit()->conversionFactorToSI() != 1 || actual->unit()->offsetToSI() != 0)
            throw std::invalid_argument("Receiver acknowledgment has incompatible units/basis: " +
                                        field);
        for (int axis = 0; axis <= int(HC::IUnitDimensions::FundamentalUnitDimension::PlaneAngle);
             ++axis) {
            const auto dimension = static_cast<HC::IUnitDimensions::FundamentalUnitDimension>(axis);
            if (actual->unit()->dimensions()->power(dimension) !=
                expected->unit()->dimensions()->power(dimension))
                throw std::invalid_argument(
                    "Receiver acknowledgment has incompatible dimensions: " + field);
        }
    }
}

void OpenSWMMEngineComponent::verifyReceipt() {
    if (!receiver_ || pendingToken_.empty())
        return;
    auto read = [&](const std::string& name, HC::DataKind kind, void* value, int64_t size) {
        const int64_t one = 1, zero = 0;
        HC::BufferDescriptor buffer{value, kind, 1, &one, nullptr, size};
        std::string diagnostic;
        for (auto* item : receiver_->outputs())
            if (item && item->id() == name) {
                if (!item->getValuesInto(buffer, {&zero, 1}, {&one, 1}, &diagnostic))
                    throw std::runtime_error("Cannot read receiver receipt: " + diagnostic);
                return;
            }
        throw std::runtime_error("Receiver receipt is missing: " + name);
    };
    std::string token;
    read("accepted_transfer_id", HC::DataKind::String, &token, sizeof(token));
    if (token != pendingToken_ || receiver_->status() == CS::Failed)
        throw std::runtime_error(
            "The preceding native export has no valid matching receipt; engine advancement stopped");
    std::vector<double> accepted(pending_.size()), remaining(pending_.size());
    for (size_t i = 0; i < accepted.size(); ++i)
        read("accepted." + amountField(i), HC::DataKind::Float64, &accepted[i], sizeof(double));
    if (!std::isfinite(accepted[0]) || accepted[0] < 0 || accepted[0] > pending_[0])
        throw std::runtime_error("Receiver accepted an invalid water volume");
    const double fraction = pending_[0] > 0 ? accepted[0] / pending_[0] : 1;
    for (size_t i = 0; i < accepted.size(); ++i) {
        if (!std::isfinite(accepted[i]) || accepted[i] != pending_[i] * fraction)
            throw std::runtime_error("Receiver receipt changed native species/energy proportions");
        remaining[i] = pending_[i] - accepted[i];
    }
    for (size_t i = 0; i < remaining.size(); ++i)
        fields_.at("unreceived." + amountField(i))->number = remaining[i];
    write("accepted_" + std::to_string(sequence_ - 1) + ".json",
          {{"transfer_id", token},
           {"receiver", receiver_->id()},
           {"ownership", "native_export_already_debited"},
           {"accepted_amounts", accepted},
           {"unreceived_amounts", remaining}});
    if (fraction != 1)
        throw std::runtime_error(
            "Receiver refused part of an already-debited native export; residual is recorded as unreceived and this run stopped");
    pendingToken_.clear();
    pending_.clear();
}

void OpenSWMMEngineComponent::recordNativeBoundary(double end, int code) const {
    Json rows = Json::array();
    double elapsed = time_;
    (void)swmm_engine_get_elapsed_seconds(engine_, &elapsed);
    for (const auto& outfall : outfalls_) {
        SWMM_OutfallExportReceipt receipt{};
        std::vector<double> mass(species_.size());
        const int result = swmm_node_get_export_receipt(engine_, outfall.node, &receipt,
                                                        mass.data(), static_cast<int>(mass.size()));
        rows.push_back({{"outfall", swmm_node_id(engine_, outfall.node)},
                        {"code", result},
                        {"flags", receipt.flags},
                        {"exported_water_m3", receipt.exported_m3},
                        {"reversed_water_m3", receipt.reversed_m3},
                        {"exported_enthalpy_j", receipt.exported_heat_j},
                        {"species_kg", mass}});
    }
    write("native_boundary_" + std::to_string(sequence_) + ".json", {{"requested_end_s", end},
                                                                     {"elapsed_s", elapsed},
                                                                     {"advance_code", code},
                                                                     {"outfalls", rows}});
}

#include "MaterialGroupRecovery.inc"

class OpenSWMMEngineInfo final : public Identity, public HC::IModelComponentInfo {
public:
    /**
     * @brief Construct native-engine configured-metadata/outfall receipt factory metadata.
     * @note Stores canonical plugin ID/caption/description, no native engine instance or run
     * created; string/property-signal allocation may throw.
     */
    OpenSWMMEngineInfo() : Identity("org.hydrocouple.openswmm.engine", "OpenSWMM engine") {
        setDescription(
            "SDK-free native engine with configured metadata, CPU 1D, CPU surface-water and bounded MESH groundwater profiles, and typed 1D external-outfall exports. Independent split_network and integrated2d roles support the serial CPU node/surface water phase contract.");
    }
    /**
     * @brief Read declared component-factory libraryFilePath metadata.
     * @return Owning stored library path text copy; no filesystem check.
     * @note Copies may allocate; no component creation, file access, physical/clock or registry
     * mutation.
     */
    std::string libraryFilePath() const override { return library_; }
    /**
     * @brief Store the host-discovered component library location.
     * @param[in] value Path text copied verbatim, no filesystem/URI validation.
     * @note No reload, signal or native lifecycle change; string allocation may throw, host
     * serializes metadata setup.
     */
    void setLibraryFilePath(const std::string& value) override { library_ = value; }
    /**
     * @brief Read declared component-factory iconFilePath metadata.
     * @return Empty string; no bundled icon declared.
     * @note Copies may allocate; no component creation, file access, physical/clock or registry
     * mutation.
     */
    std::string iconFilePath() const override { return {}; }
    /**
     * @brief Read declared component-factory developer metadata.
     * @return Caleb Buahin, declared project developer.
     * @note Copies may allocate; no component creation, file access, physical/clock or registry
     * mutation.
     */
    std::string developer() const override { return "Caleb Buahin"; }
    /**
     * @brief Read declared component-factory documentation metadata.
     * @return Empty vector; no external documentation links declared.
     * @note Copies may allocate; no component creation, file access, physical/clock or registry
     * mutation.
     */
    std::vector<std::string> documentation() const override { return {}; }
    /**
     * @brief Read declared component-factory license metadata.
     * @return Apache-2.0 identifier.
     * @note Copies may allocate; no component creation, file access, physical/clock or registry
     * mutation.
     */
    std::string license() const override { return "Apache-2.0"; }
    /**
     * @brief Read declared component-factory copyright metadata.
     * @return Copyright 2026 Caleb Buahin, preserving original project credit.
     * @note Copies may allocate; no component creation, file access, physical/clock or registry
     * mutation.
     */
    std::string copyright() const override { return "Copyright 2026 Caleb Buahin"; }
    /**
     * @brief Read declared component-factory url metadata.
     * @return Declared HydroCouple OpenSWMM project URL string; no network access.
     * @note Copies may allocate; no component creation, file access, physical/clock or registry
     * mutation.
     */
    std::string url() const override { return "https://hydrocouple.org/projects/openswmm"; }
    /**
     * @brief Read declared component-factory email metadata.
     * @return Empty string; no contact address declared here.
     * @note Copies may allocate; no component creation, file access, physical/clock or registry
     * mutation.
     */
    std::string email() const override { return {}; }
    /**
     * @brief Read declared component-factory version metadata.
     * @return Declared6.0.0-alpha.4 component version string, not a runtime engine query.
     * @note Copies may allocate; no component creation, file access, physical/clock or registry
     * mutation.
     */
    std::string version() const override { return "6.0.0-alpha.4"; }
    /**
     * @brief Read declared component-factory tags metadata.
     * @return Owning set of declared discovery tags:
     * SWMM,native,configured-metadata,outfall-receipts.
     * @note Copies may allocate; no component creation, file access, physical/clock or registry
     * mutation.
     */
    std::set<std::string> tags() const override {
        return {"SWMM", "native", "configured-metadata", "outfall-receipts"};
    }
    /**
     * @brief Create a fresh Created model component owned by caller.
     * @return Unique ownership of new OpenSWMMEngineComponent through IModelComponent. Instance
     * borrows this info factory, which must outlive every created component. No native model
     * open or physical time advance; allocation/construction exceptions propagate. Host assigns
     * stable identity and configuration, then drives lifecycle; caller destroys after
     * detach/finish.
     */
    std::unique_ptr<HC::IModelComponent> createComponentInstance() override {
        return std::make_unique<OpenSWMMEngineComponent>(this);
    }
    /**
     * @brief Read declared component-factory adaptedOutputFactories metadata.
     * @return Empty vector; this plugin exposes no adapted-output factory.
     * @note Copies may allocate; no component creation, file access, physical/clock or registry
     * mutation.
     */
    std::vector<HC::IAdaptedOutputFactory*> adaptedOutputFactories() const override { return {}; }

private:
    std::string library_;
};
}
}
HYDROCOUPLE_DECLARE_COMPONENT(openswmm::couplers::OpenSWMMEngineInfo)
extern "C" HYDROCOUPLE_COMPONENT_EXPORT const OpenSWMM_MaterialRecoveryV1*
openswmm_component_material_recovery(uint32_t version,size_t caller_size) {
    return openswmm::couplers::MaterialGroupRecovery::service(version,caller_size);
}
