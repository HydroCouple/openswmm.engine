// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      OpenSWMMModelComponent.cpp
 * @brief     Component wrapper for OpenSWMM model configuration.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

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
using Json = nlohmann::json;
namespace fs = std::filesystem;
class Configuration final : public Item, public HC::IArgument {
public:
    Json inlineConfiguration;
    /**
     * @brief Construct required reservoir mapping argument with borrowed model association.
     * @param[in] c Nonowned component pointer, must outlive argument; metadata only, no
     * model/file open. Construction and property signal snapshot may allocate.
     */
    explicit Configuration(HC::IModelComponent* c)
        : Item("configuration", c, HC::DataKind::String) {
        setCaption("SWMM model and reservoir mapping");
        setDescription(
            "Model file, elapsed exchange times, ownership, explicit outfalls/columns, temperature basis and species mapping.");
    }
    /**
     * @brief Read reservoir-coupling argument metadata.
     * @return Configuration argument role.
     * @note Copied text/containers may allocate; no file access, engine advancement or
     * notifications.
     */
    ArgumentRole role() const override { return ArgumentRole::Configuration; }
    /**
     * @brief Read reservoir-coupling argument metadata.
     * @return False; a configuration must be supplied before successful preparation.
     * @note Copied text/containers may allocate; no file access, engine advancement or
     * notifications.
     */
    bool isOptional() const override { return false; }
    /**
     * @brief Read reservoir-coupling argument metadata.
     * @return False unconditionally; this argument does not enforce lifecycle immutability.
     * @note Copied text/containers may allocate; no file access, engine advancement or
     * notifications.
     */
    bool isReadOnly() const override { return false; }
    /**
     * @brief Read reservoir-coupling argument metadata.
     * @return Stored local file text; inline setup clears text, so this can be empty with valid
     * inline configuration.
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
     * @brief Read reservoir-coupling argument metadata.
     * @return One JSON SWMM coupling setup file selector filter.
     * @note Copied text/containers may allocate; no file access, engine advancement or
     * notifications.
     */
    std::vector<std::string> fileFilters() const override {
        return {"SWMM coupling setup (*.json)"};
    }
    /**
     * @brief Read reservoir-coupling argument metadata.
     * @return Empty vector; component-data-item initialization is unsupported.
     * @note Copied text/containers may allocate; no file access, engine advancement or
     * notifications.
     */
    std::vector<const std::type_info*> validComponentDataItemTypes() const override { return {}; }
    /**
     * @brief Test supported local configuration input/serialization representations.
     * @param[in] t Argument type selector.
     * @return True for JSON or File only; no value/path/lifecycle validation, allocation or
     * mutation.
     */
    bool isValidArgType(ArgumentInputType t) const override {
        return t == ArgumentInputType::JSON || t == ArgumentInputType::File;
    }
    /**
     * @brief Read reservoir-coupling argument metadata.
     * @return Last successful JSON/File input type, default JSON.
     * @note Copied text/containers may allocate; no file access, engine advancement or
     * notifications.
     */
    ArgumentInputType currentArgumentInputType() const override { return type_; }
    /**
     * @brief Install reservoir-coupling inline setup or a local setup-file reference.
     * @param[in] value JSON openswmm.reservoir-coupling/1.0 object, \@uri object, first
     * values/array string, or File path. Inline object must declare swmm-network/fvqual-receiver
     * ownership and network_only role.
     * @param[in] type JSON/File only, no lifecycle guard in this argument.
     * @param[out] message Empty success or caught std::exception diagnostic.
     * @return True after assignment, false unsupported/bad JSON/ownership/remote URI. Empty
     * local path can be accepted here then refused by loadConfiguration. Strips file://, does
     * not read file at this stage.
     * @note Repeated assignment may replace configuration even after initialization metadata
     * says writable. No Error entry emitted here, physical/clock advance or transactional
     * rollback; allocation may leave partial argument.
     */
    bool initialize(const std::string& value, ArgumentInputType type,
                    std::string& message) override {
        try {
            if (!isValidArgType(type))
                throw std::invalid_argument("Use a local coupling setup file");
            auto candidate = value;
            if (type == ArgumentInputType::JSON) {
                const auto j = Json::parse(value);
                if (j.is_object() &&
                    j.value("schema", std::string()) == "openswmm.reservoir-coupling/1.0") {
                    if (j.at("ownership") != "swmm-network/fvqual-receiver" ||
                        j.at("role") != "network_only")
                        throw std::invalid_argument(
                            "Declare separate SWMM network and FVQual receiver ownership");
                    inlineConfiguration = j;
                    text.clear();
                    type_ = type;
                    message.clear();
                    return true;
                }
                candidate = j.contains("@uri")
                                ? j.at("@uri").get<std::string>()
                                : (j.is_object() ? j.at("values") : j).at(0).get<std::string>();
            }
            if (candidate.starts_with("file://"))
                candidate.erase(0, 7);
            if (candidate.find("://") != std::string::npos)
                throw std::invalid_argument("Remote configuration is not supported");
            inlineConfiguration = nullptr;
            text = std::move(candidate);
            type_ = type;
            message.clear();
            return true;
        } catch (const std::exception& e) {
            message = e.what();
            return false;
        }
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
        if (count[0] == 0)
            return true;
        std::string result;
        const bool ok = initialize(*static_cast<const std::string*>(buffer.data),
                                   ArgumentInputType::File, result);
        if (message)
            *message = result;
        return ok;
    }
    /**
     * @brief Refuse configuration initialization from another component data item.
     * @param[out] message Receives instruction to use local file/JSON; unnamed source ignored.
     * @return Always false, no source buffer read, physical advance or configuration change.
     * Diagnostic assignment may throw. Does not append component Error entry itself.
     */
    bool initialize(const HC::IComponentDataItem&, std::string& message) override {
        message = "Use a setup file";
        return false;
    }
    /**
     * @brief Serialize inline configuration or local file reference without saving a file.
     * @param[in] t JSON or File; inline configuration only supports JSON.
     * @param[out] value Canonical inline JSON, JSON \@uri object for file reference, or bare
     * File path; unsupported representation preserves previous value.
     * @param[out] message Empty success or representation diagnostic.
     * @return True for supported representation, false otherwise. No read/parse of referenced
     * file, engine/clock advance or argument mutation. Allocation/JSON exceptions propagate;
     * stable configuration required.
     */
    bool serialize(ArgumentInputType t, std::string& value, std::string& message) const override {
        if (!isValidArgType(t)) {
            message = "Unsupported argument format";
            return false;
        }
        if (!inlineConfiguration.is_null()) {
            if (t != ArgumentInputType::JSON) {
                message = "Inline configuration serializes as JSON";
                return false;
            }
            value = inlineConfiguration.dump();
            message.clear();
            return true;
        }
        value = t == ArgumentInputType::JSON ? Json{{"@uri", text}}.dump() : text;
        message.clear();
        return true;
    }

private:
    ArgumentInputType type_ = ArgumentInputType::JSON;
};
class OpenSWMMModelComponent final : public ComponentBase,
                                     public HC::Temporal::ITimeModelComponent {
public:
    /**
     * @brief Construct a Created native SWMM component with owned argument/output metadata.
     * @param[in] info Borrowed factory metadata pointer, must outlive component; no ownership
     * transfer.
     * @note Initial ID openswmm may be replaced by host identity argument before initialization.
     * No native engine/file open or physical time advance; metadata allocation may throw.
     */
    explicit OpenSWMMModelComponent(HC::IModelComponentInfo* info)
        : ComponentBase("openswmm", info), configuration_(this),
          frame_("outfall_transfers", this, HC::DataKind::String),
          water_("exported_water_m3", this, HC::DataKind::Float64, "m3", 3),
          heat_("exported_heat_j", this, HC::DataKind::Float64, "J", 2, 1, -2) {
        setCaption("OpenSWMM network");
        frame_.setCaption("Outfall transfers awaiting receiver receipt");
        frame_.setDescription(
            "hydrocouple.integrated-inflow/1.0; interval amounts already debited by SWMM. One acknowledged receiver only.");
        water_.setCaption("Cumulative mapped outfall water");
        heat_.setCaption("Cumulative mapped outfall heat");
    }
    /**
     * @brief Close/destroy any owned runtime engine when component wrapper is destroyed.
     * @note Does not finish/report model or validate pending receiver receipt automatically.
     * Borrowed output/argument/clock pointers become invalid; host detaches consumers before
     * owner destruction.
     */
    ~OpenSWMMModelComponent() override { close(); }
    /**
     * @brief Enumerate host identity and model-configuration argument pointers.
     * @return Owning vector of nonowned argument pointers, component lifetime; no extra lifetime
     * extension. No file read/lifecycle/clock advance; vector allocation may throw, host keeps
     * owner stable.
     */
    std::vector<HC::IArgument*> arguments() const override {
        auto result = ComponentBase::arguments();
        result.push_back(const_cast<Configuration*>(&configuration_));
        return result;
    }
    /**
     * @brief Enumerate currently constructed output item pointers without refreshing native
     * state.
     * @return Owning vector of borrowed pointers, component lifetime and until reinitialization
     * clears dynamic items. Do not delete pointers; no producer advance or ownership transfer.
     * Vector allocation can throw; host serializes initialization/output-list reads.
     */
    std::vector<HC::IOutput*> outputs() const override {
        std::vector<HC::IOutput*> out{const_cast<Output*>(&frame_), const_cast<Output*>(&water_),
                                      const_cast<Output*>(&heat_)};
        for (const auto& item : massOutputs_)
            out.push_back(item.get());
        return out;
    }
    /**
     * @brief Reset wrapper-owned transfer history and dynamic mappings before configuration
     * validation.
     * @note Enters Initializing, closes old engine, clears
     * config/frames/counters/elapsed/cumulative scalar outputs/mappings/output path/schedule and
     * dynamic species ports, then Initialized. Argument and host identity remain; native model
     * is not opened yet. Caught std::exception fails, partial resets retained; no native elapsed
     * advancement. Host detaches borrowed dynamic outputs before reinitialization.
     */
    void initialize() override {
        try {
            transition(CS::Initializing);
            close();
            config_ = Json();
            frames_ = Json::array();
            sequence_ = 0;
            time_ = 0;
            water_.number = 0;
            heat_.number = 0;
            frame_.text.clear();
            massOutputs_.clear();
            nativeTargets_.clear();
            mapped_.clear();
            output_.clear();
            times_.clear();
            transition(CS::Initialized);
        } catch (const std::exception& e) {
            fail(e.what());
        }
    }
    /**
     * @brief Load and validate reservoir-coupling setup without opening a runtime SWMM engine.
     * @return Empty on Valid, first caught std::exception message after Invalid transition
     * otherwise.
     * @note Enters Validating, resolves setup/model existence/mapping/schedule and may assign
     * configuration instance ID if host did not. Mutates wrapper config/base/epoch/schedule even
     * on partial failure; no native physical advance. This validate path does not append Error
     * entry separately. Exclusive lifecycle; filesystem/JSON/diagnostic exceptions may propagate
     * during failure reporting.
     */
    std::vector<std::string> validate() override {
        try {
            transition(CS::Validating);
            loadConfiguration();
            transition(CS::Valid);
            return {};
        } catch (const std::exception& e) {
            transition(CS::Invalid, e.what());
            return {e.what()};
        }
    }
    /**
     * @brief Qualify explicit one-way receiver mapping, open native donor and seed export
     * receipts.
     * @note Enters Preparing and reloads setup. At most one direct frame receiver, no
     * adaptation; if wired, requires integrated_inflows ID, temporal epoch/horizon equality and
     * exact receiver mesh geometry/order, column count and scalar acknowledgment token. Creates
     * timestamp/atomic-serial output subdirectory, opens/initializes/starts native engine,
     * validates epoch/horizon/no MSX, complete unique kg-based pollutant mapping and outfall
     * receipt flags/native-or-explicit-fixed heat basis. Writes config then Updated. Caught
     * failure fails/closes but directories/files/partial ports remain; no rollback. Host owns
     * stable receiver lifetimes/config, prepared native source profile and exclusive startup.
     */
    void prepare() override {
        try {
            transition(CS::Preparing);
            loadConfiguration();
            if (frame_.consumers().size() > 1 || !frame_.adaptedOutputs().empty())
                throw std::invalid_argument(
                    "Outfall transfer tuples require one direct acknowledged receiver");
            for (auto* input : frame_.consumers()) {
                if (input->id() != "integrated_inflows")
                    throw std::invalid_argument("Use the FVQual integrated_inflows receiver");
                auto* receiver =
                    dynamic_cast<HC::Temporal::ITimeModelComponent*>(input->modelComponent());
                if (!receiver)
                    throw std::invalid_argument("The receiver must declare physical time");
                if (std::abs(receiver->simulationPeriod()->julianDay() - epoch_) > 1.e-10)
                    throw std::invalid_argument("SWMM and receiver epochs differ");
                if (std::abs(receiver->simulationPeriod()->duration() * 86400.0 - times_.back()) >
                    1.e-6)
                    throw std::invalid_argument("SWMM and receiver exchange horizons differ");
                bool columns = false, ack = false;
                for (auto* output : input->modelComponent()->outputs()) {
                    if (output->id() == "temperature" && output->shape().size() == 2) {
                        columns =
                            output->shape()[0] == config_.at("receiver_columns").get<int64_t>();
                        const auto* spatial =
                            dynamic_cast<HC::Spatial::IPolyhedralSurfaceComponentDataItem*>(output);
                        if (!spatial || !spatial->polyhedralSurface() ||
                            !spatial->polyhedralSurface()->meshView())
                            throw std::invalid_argument(
                                "Receiver must publish its mesh for mapping verification");
                        const auto* mesh = spatial->polyhedralSurface()->meshView();
                        const Json geometry = {{"x", mesh->nodeX()},
                                               {"y", mesh->nodeY()},
                                               {"z", mesh->nodeZ()},
                                               {"offsets", mesh->faceNodeOffsets()},
                                               {"nodes", mesh->faceNodes()}};
                        if (geometry != config_.at("receiver_geometry"))
                            throw std::invalid_argument(
                                "Receiver mesh geometry/order differs from the explicit mapping");
                    }
                    if (output->id() == "accepted_transfer_id")
                        ack = output->dataKind() == HC::DataKind::String &&
                              output->shape() == std::vector<int64_t>{1};
                }
                if (!columns || !ack)
                    throw std::invalid_argument(
                        "Receiver columns or transfer acknowledgment do not match the mapping");
            }
            static std::atomic<unsigned> serial{0};
            const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
            output_ = base_ / config_.value("output_directory", std::string("swmm_output"));
            output_ /= std::to_string(stamp) + "-" + std::to_string(++serial);
            fs::create_directories(output_);
            engine_ = swmm_engine_create();
            if (!engine_)
                throw std::runtime_error("Cannot create SWMM engine");
            const auto model = resolve(config_.at("model").get<std::string>());
            check(swmm_engine_open(engine_, model.string().c_str(),
                                   (output_ / "donor.rpt").string().c_str(),
                                   (output_ / "donor.out").string().c_str(), nullptr));
            // This role is explicit: native runoff/network are retained, while
            // the source's integrated surface is disabled before initialization.
            if (config_.at("role") == "network_only")
                check(swmm_options_set(engine_, "IGNORE_2D", "YES"));
            check(swmm_engine_initialize(engine_));
            check(swmm_engine_start(engine_, 1));
            double start;
            check(swmm_get_start_time(engine_, &start));
            // SWMM DateTime is days since 1899-12-30, not a Julian day.
            if (std::abs(start + 2415018.5 - epoch_) > 1.e-10)
                throw std::invalid_argument("Configured epoch does not match SWMM model start");
            double end;
            check(swmm_get_end_time(engine_, &end));
            if (times_.back() > (end - start) * 86400.0 + 1.e-6)
                throw std::invalid_argument("Exchange horizon exceeds SWMM duration");
            if (swmm_reaction_species_count(engine_) > 0)
                throw std::invalid_argument(
                    "MSX outfall export is not qualified by this component");
            const int np = swmm_pollutant_count(engine_);
            const auto species = config_.value("species", Json::object());
            if (static_cast<int>(species.size()) != np)
                throw std::invalid_argument("Map every native pollutant explicitly");
            std::set<std::string> targets;
            for (int p = 0; p < np; ++p) {
                const std::string native = swmm_pollutant_id(engine_, p);
                const auto target = species.at(native).get<std::string>();
                int units;
                check(swmm_pollutant_get_units(engine_, p, &units));
                if (units == 2 || target.empty() || !targets.insert(target).second)
                    throw std::invalid_argument(
                        "Use unique kg-based species; count concentrations need a separate physical conversion");
                nativeTargets_.push_back(target);
                auto item = std::make_unique<Output>("exported_species_" + target + "_kg", this,
                                                     HC::DataKind::Float64, "kg", 0, 1);
                item->setCaption("Cumulative exported " + target);
                massOutputs_.push_back(std::move(item));
            }
            mapped_.clear();
            std::set<int> nodes;
            for (const auto& entry : config_.at("outfalls")) {
                const auto id = entry.at("id").get<std::string>();
                const int n = swmm_node_index(engine_, id.c_str());
                if (n < 0 || !nodes.insert(n).second)
                    throw std::invalid_argument("Unknown or duplicate mapped outfall: " + id);
                Mapped m;
                m.node = n;
                m.definition = entry;
                m.amounts.resize(np);
                check(swmm_node_get_export_receipt(engine_, n, &m.receipt, m.amounts.data(), np));
                if (np && !(m.receipt.flags & 2))
                    throw std::invalid_argument(
                        "Only LEGACY pollutant export accounting is qualified");
                if (entry.at("temperature").at("mode") == "native" && !(m.receipt.flags & 4))
                    throw std::invalid_argument(
                        "Native outfall heat is unavailable; explicitly select a fixed temperature assumption");
                mapped_.push_back(std::move(m));
            }
            writeJson(output_ / "configuration.json", config_);
            transition(CS::Updated, "SWMM prepared with explicit one-way outfall ownership");
        } catch (const std::exception& e) {
            fail(e.what());
            close();
        }
    }
    /**
     * @brief Advance native donor to next scheduled boundary and publish mapped already-debited
     * export frame.
     * @note Unnamed requested-output list ignored. Requires Updated/live engine/remaining
     * schedule; verifies previous token receipt then Updating, advances to exact elapsed
     * boundary within1e-8s, records native boundary before status check. Differences cumulative
     * water m3/species kg/native heat J, rejects reversal/negative or nonfinite inventories;
     * fixed-temperature mode substitutes volume*1000*4186*T J relative to0degC with explicit
     * provenance. Positive mapping weights distribute tuple to receiver columns/placement,
     * writes packet/updates cumulative outputs then elapsed/sequence and Updated/Done. Catches
     * std::exception, fails/closes without rolling back native debit, history or artifacts; host
     * must schedule receiver once per frame before next donor update/finish. Exclusive lifecycle
     * required.
     */
    void update(const std::vector<HC::IOutput*>& = {}) override {
        try {
            if (status() != CS::Updated || !engine_ || sequence_ >= times_.size())
                throw std::logic_error("SWMM is not ready to advance");
            // After irreversible donor advancement, require the preceding
            // receiver receipt before producing another export. Never retry a
            // failed consumer by advancing the donor a second time.
            verifyReceipt();
            transition(CS::Updating);
            const double end = times_.at(sequence_);
            double actual;
            const int advanceCode = swmm_engine_advance_to(engine_, end, &actual);
            // Keep the native boundary ledger even when mapping/refusal or a
            // receiver fails. The donor cannot roll back physical advancement.
            recordNativeBoundary(end, advanceCode);
            check(advanceCode);
            if (std::abs(actual - end) > 1.e-8)
                throw std::runtime_error("SWMM did not land on the exact exchange boundary");
            Json rows = Json::array();
            double water = 0, heat = 0;
            std::vector<double> masses(nativeTargets_.size());
            for (auto& m : mapped_) {
                SWMM_OutfallExportReceipt current;
                std::vector<double> amounts(nativeTargets_.size());
                check(swmm_node_get_export_receipt(engine_, m.node, &current, amounts.data(),
                                                   amounts.size()));
                if (current.reversed_m3 > m.receipt.reversed_m3 + 1.e-12)
                    throw std::runtime_error(
                        "Outfall reversal requires an unsupported feedback protocol");
                const double volume = current.exported_m3 - m.receipt.exported_m3;
                if (!std::isfinite(volume) || volume < 0)
                    throw std::runtime_error("Invalid cumulative outfall volume");
                double energy = current.exported_heat_j - m.receipt.exported_heat_j;
                const auto& temperature = m.definition.at("temperature");
                if (temperature.at("mode") == "fixed")
                    energy = volume * 1000.0 * 4186.0 * temperature.at("value_degC").get<double>();
                if (!std::isfinite(energy))
                    throw std::runtime_error("Outfall heat is nonfinite");
                Json species = Json::object();
                for (std::size_t p = 0; p < amounts.size(); ++p) {
                    const double amount = amounts[p] - m.amounts[p];
                    if (!std::isfinite(amount) || amount < 0)
                        throw std::runtime_error("Invalid cumulative pollutant mass");
                    species[nativeTargets_[p]] = amount;
                    masses[p] += amount;
                }
                for (const auto& target : m.definition.at("targets")) {
                    const double weight = target.at("weight");
                    Json parts = species;
                    for (auto& amount : parts)
                        amount = amount.get<double>() * weight;
                    rows.push_back(
                        {{"column", target.at("column")},
                         {"volume_m3", volume * weight},
                         {"heat_j", energy * weight},
                         {"placement", m.definition.value("placement", std::string("uniform"))},
                         {"species_mass_kg", parts},
                         {"source_object", m.definition.at("id")}});
                }
                water += volume;
                heat += energy;
                m.receipt = current;
                m.amounts = std::move(amounts);
            }
            std::ostringstream id;
            id << "swmm-outfalls/" << this->id() << '/' << std::setw(8) << std::setfill('0')
               << sequence_;
            Json frame = {{"id", id.str()}, {"start_s", time_}, {"end_s", end}, {"inflows", rows}};
            frames_.push_back(frame);
            Json packet = frame;
            packet["schema"] = "hydrocouple.integrated-inflow/1.0";
            packet["epoch_julian_day"] = epoch_;
            packet["ownership"] = "already_debited";
            packet["units"] = {{"time", "elapsed_s"},
                               {"water", "m3"},
                               {"heat", "J_relative_to_0_degC"},
                               {"species", "kg"}};
            // Persist the authoritative debit before offering it to a receiver.
            writeJson(output_ / ("export_" + std::to_string(sequence_) + ".json"), packet);
            frame_.text = packet.dump();
            water_.number += water;
            heat_.number += heat;
            for (std::size_t p = 0; p < masses.size(); ++p)
                massOutputs_[p]->number += masses[p];
            time_ = end;
            ++sequence_;
            transition(sequence_ == times_.size() ? CS::Done : CS::Updated,
                       "Outfall interval exported once");
        } catch (const std::exception& e) {
            fail(e.what());
            close();
        }
    }
    /**
     * @brief Verify final acknowledgment, end/report donor and write reusable inflow-series
     * artifact.
     * @note Failed path skips receipt check; enters Finishing, ends/reports engine when present,
     * serializes all retained frames/species metadata, closes and Finished. No explicit Finished
     * idempotence guard, so host calls once under valid transition policy. Caught failure
     * fails/closes with partial file/output retained. No rollback or receiver advancement;
     * borrowed connections live through acknowledgment.
     */
    void finish() override {
        try {
            const bool failed = status() == CS::Failed;
            if (!failed)
                verifyReceipt();
            transition(CS::Finishing);
            if (engine_) {
                check(swmm_engine_end(engine_));
                check(swmm_engine_report(engine_));
            }
            if (!output_.empty()) {
                Json species = Json::object();
                for (const auto& s : nativeTargets_)
                    species[s] = {{"units", "kg/m3"}, {"initial", 0.0}};
                writeJson(output_ / "inflow_series.json", {{"schema", "fvqual.inflow-series/1.0"},
                                                           {"units",
                                                            {{"time", "elapsed_s"},
                                                             {"water", "m3"},
                                                             {"heat", "J_relative_to_0_degC"},
                                                             {"species", "kg"}}},
                                                           {"epoch_julian_day", epoch_},
                                                           {"species", species},
                                                           {"frames", frames_}});
            }
            close();
            transition(CS::Finished);
        } catch (const std::exception& e) {
            fail(e.what());
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
        clock_.day = epoch_ + time_ / 86400.0;
        return &clock_;
    }
    /**
     * @brief Borrow configured absolute start/exchange horizon metadata.
     * @return Nonowned component Clock span, start epoch astronomical Julian day and duration
     * days from last exchange endpoint (0 before schedule).; next query overwrites view. No
     * physical advancement, allocation or ownership transfer; component lifetime and serialized
     * metadata access required.
     */
    HC::Temporal::ITimeSpan* simulationPeriod() const override {
        period_.day = epoch_;
        period_.span = times_.empty() ? 0 : times_.back() / 86400.0;
        return &period_;
    }
    /**
     * @brief Read the host scheduling target as absolute astronomical Julian day.
     * @return Next agreed exchange endpoint when Updated; otherwise current epoch+elapsed/86400.
     *
     * No native advancement or allocation; prepared schedule/sequence must remain stable through
     * query.
     */
    double nextDateTimeJulianDay() const override {
        return epoch_ + ((sequence_ < times_.size() && status() == CS::Updated) ? times_[sequence_]
                                                                                : time_) /
                            86400.0;
    }

private:
    struct Mapped {
        int node = -1;
        Json definition;
        SWMM_OutfallExportReceipt receipt{};
        std::vector<double> amounts;
    };
    Configuration configuration_;
    Output frame_, water_, heat_;
    std::vector<std::unique_ptr<Output>> massOutputs_;
    SWMM_Engine engine_ = nullptr;
    Json config_, frames_ = Json::array();
    fs::path base_, output_;
    std::vector<double> times_;
    std::vector<Mapped> mapped_;
    std::vector<std::string> nativeTargets_;
    std::size_t sequence_ = 0;
    double time_ = 0, epoch_ = 2440587.5;
    mutable Clock clock_, period_;
    /**
     * @brief Resolve model/configured path text against this component's established base.
     * @param[in] value Path text, absolute retained, relative joined to base.
     * @return Owning native path using platform narrow-string conversion; no forced
     * absolute/canonical normalization. No existence/file access or state mutation; filesystem
     * conversion/allocation may throw.
     */
    fs::path resolve(const std::string& value) const {
        fs::path path(value);
        return path.is_absolute() ? path : base_ / path;
    }
    /**
     * @brief Write a pretty JSON diagnostic/transfer artifact to a selected local path.
     * @param[in] path Destination opened/truncated; caller owns isolated output hierarchy.
     * @param[in] j Stable serializable JSON, newline appended.
     * @throws std::runtime_error Open/write failure; JSON/allocation errors propagate.
     * @note No atomic replacement/cleanup on failure, partial file may remain. Does not advance
     * native model or modify packet; caller serializes artifact writes.
     */
    static void writeJson(const fs::path& path, const Json& j) {
        std::ofstream f(path);
        f << j.dump(2) << '\n';
        if (!f)
            throw std::runtime_error("Cannot record donor transfers");
    }
    /**
     * @brief Throw on a nonzero owned-engine native status with diagnostic and code.
     * @param[in] code 0 returns, nonzero throws std::runtime_error; null error text uses Unknown
     * error.
     * @note Requires live compatible owned engine for error lookup, no clear/status/physical
     * mutation. String allocation may throw; caller decides failure/close policy.
     */
    void check(int code) const {
        if (code)
            throw std::runtime_error("SWMM: " +
                                     std::string(swmm_get_last_error_msg(engine_)
                                                     ? swmm_get_last_error_msg(engine_)
                                                     : "Unknown error") +
                                     " (" + std::to_string(code) + ")");
    }
    /**
     * @brief Release the owned runtime engine if present and clear its handle.
     * @note Calls native close then destroy, ignores close error status. Does not report/end or
     * consume pending receiver acknowledgment. No-op when absent; no throw wrapper, exclusive
     * owner lifecycle, all borrowed native API pointers invalid after close.
     */
    void close() noexcept {
        if (engine_) {
            swmm_engine_close(engine_);
            swmm_engine_destroy(engine_);
            engine_ = nullptr;
        }
    }
    /**
     * @brief Transition component to Failed with a diagnostic unless already Failed/Finished.
     * @param[in] message Failure text copied into lifecycle event/fatal error entry.
     * @note Does not itself close runtime engine; callers decide cleanup. Transition/signal
     * allocation can throw; no physical rollback or native clock change, exclusive component
     * lifecycle.
     */
    void fail(const std::string& message) {
        if (status() != CS::Failed && status() != CS::Finished)
            transition(CS::Failed, message);
    }
    /**
     * @brief Record attempted boundary and cumulative native outfall receipts before status
     * handling.
     * @param[in] requested Target elapsed seconds, retained even if native advance failed.
     * @param[in] advanceCode Native advance result for diagnostic artifact.
     * @note Best-effort elapsed query uses prior time fallback; each outfall query status/flags
     * and cumulative m3/reversed m3/native J/species kg stored, mapped target names retained.
     * Does not difference/consume receipts or advance physical state. JSON/file errors throw
     * after attempted advance and may mask its original error; valid runtime/receipt mappings
     * and exclusive output phase required.
     */
    void recordNativeBoundary(double requested, int advanceCode) const {
        double elapsed = time_;
        (void)swmm_engine_get_elapsed_seconds(engine_, &elapsed);
        Json rows = Json::array();
        for (const auto& mapped : mapped_) {
            SWMM_OutfallExportReceipt receipt{};
            std::vector<double> amounts(nativeTargets_.size());
            const int result = swmm_node_get_export_receipt(engine_, mapped.node, &receipt,
                                                            amounts.data(), amounts.size());
            Json species = Json::object();
            for (std::size_t p = 0; p < amounts.size(); ++p)
                species[nativeTargets_[p]] = amounts[p];
            rows.push_back({{"outfall", mapped.definition.at("id")},
                            {"receipt_code", result},
                            {"flags", receipt.flags},
                            {"cumulative_water_m3", receipt.exported_m3},
                            {"cumulative_reversed_m3", receipt.reversed_m3},
                            {"cumulative_native_heat_j", receipt.exported_heat_j},
                            {"cumulative_species_kg", species}});
        }
        writeJson(output_ / ("native_boundary_" + std::to_string(sequence_) + ".json"),
                  {{"schema", "openswmm.outfall-boundary-ledger/1.0"},
                   {"requested_end_s", requested},
                   {"native_elapsed_s", elapsed},
                   {"advance_code", advanceCode},
                   {"outfalls", rows}});
    }
    /**
     * @brief Require matching receiver token for the last published frame before further donor
     * advance.
     * @note Only when sequence>0 and frame has consumer. Reads first consumer owner's
     * accepted_transfer_id String host scalar; requires previous frame ID and receiver not
     * Failed, writes acknowledgment artifact. Does not validate accepted amounts/proportions
     * here (typed engine component has stronger amount receipt profile), clear frame or advance
     * receiver/donor. Invalid/missing receipt throws, live receiver/ output pointees and
     * exclusive wiring phase required; diagnostic writes can partially fail.
     */
    void verifyReceipt() {
        if (sequence_ && !frame_.consumers().empty()) {
            auto* consumer = frame_.consumers().front()->modelComponent();
            std::string accepted;
            bool found = false;
            for (auto* o : consumer->outputs())
                if (o->id() == "accepted_transfer_id") {
                    const int64_t shape[1] = {1}, start[1] = {0}, count[1] = {1};
                    HC::BufferDescriptor b;
                    b.data = &accepted;
                    b.kind = HC::DataKind::String;
                    b.rank = 1;
                    b.shape = shape;
                    b.itemSizeBytes = sizeof(std::string);
                    std::string message;
                    found = o->getValuesInto(b, start, count, &message);
                    break;
                }
            if (!found || accepted != frames_.back().at("id").get<std::string>() ||
                consumer->status() == CS::Failed)
                throw std::runtime_error(
                    "The preceding donor export has no matching receiver receipt; advancement stopped");
            writeJson(output_ / ("accepted_" + std::to_string(sequence_ - 1) + ".json"),
                      {{"schema", "openswmm.receiver-acknowledgment/1.0"},
                       {"transfer_id", accepted},
                       {"receiver", consumer->id()},
                       {"end_s", time_},
                       {"ownership", "already_debited"}});
        }
    }
    /**
     * @brief Load and qualify one-way reservoir coupling schema, timeline and conservative
     * column mapping.
     * @note Reads inline or local file (base reference directory/config parent), requires schema
     * openswmm.reservoir-coupling/1.0, network_only role and swmm-network/fvqual-receiver
     * ownership. Stores finite epoch Julian day and strictly increasing positive finite
     * elapsed-second schedule; requires existing model, positive integer receiver columns and
     * nonempty outfall map. Optional instance ID must agree with host-assigned identity or
     * replaces unassigned raw ID. Each outfall target has unique valid0-based column and
     * positive finite weight, sum1 within1e-12; heat native or finite fixed degC with
     * provenance, placement uniform/surface/bed. Does not open model, confirm native outfall IDs
     * or verify receiver geometry; prepare owns those checks. Throws on
     * refusal/read/JSON/allocation, retaining partial config/base/identity/time changes; no
     * physical advancement, exclusive configuration phase.
     */
    void loadConfiguration() {
        if (!configuration_.inlineConfiguration.is_null()) {
            base_ = fs::absolute(referenceDirectory().empty() ? fs::path(".")
                                                              : fs::path(referenceDirectory()));
            config_ = configuration_.inlineConfiguration;
        } else {
            if (configuration_.text.empty())
                throw std::invalid_argument("Choose a SWMM coupling setup file");
            auto path = fs::path(configuration_.text);
            if (path.is_relative())
                path = fs::path(referenceDirectory()) / path;
            base_ = fs::absolute(path).parent_path();
            std::ifstream f(path);
            if (!f)
                throw std::invalid_argument("Cannot read coupling setup: " + path.string());
            f >> config_;
        }
        if (config_.at("schema") != "openswmm.reservoir-coupling/1.0" ||
            config_.at("ownership") != "swmm-network/fvqual-receiver" ||
            config_.at("role") != "network_only")
            throw std::invalid_argument(
                "Declare network_only and separate receiving-reservoir ownership");
        epoch_ = config_.at("epoch_julian_day");
        if (!std::isfinite(epoch_))
            throw std::invalid_argument("Epoch must be finite");
        if (config_.contains("instance_id")) {
            const auto instance = config_.at("instance_id").get<std::string>();
            if (instance.empty())
                throw std::invalid_argument("Instance ID must not be empty");
            if (hasAssignedInstanceIdentity() && instance != id())
                throw std::invalid_argument(
                    "Configuration instance_id conflicts with the host-assigned identity");
            if (!hasAssignedInstanceIdentity())
                setInstanceId(instance); // legacy direct-harness configuration
        }
        times_ = config_.at("exchange_times_s").get<std::vector<double>>();
        double last = 0;
        for (double t : times_) {
            if (!std::isfinite(t) || t <= last)
                throw std::invalid_argument("Exchange times must strictly increase from zero");
            last = t;
        }
        if (times_.empty() || !fs::is_regular_file(resolve(config_.at("model").get<std::string>())))
            throw std::invalid_argument("Choose a model and nonempty schedule");
        if (!config_.at("receiver_columns").is_number_integer())
            throw std::invalid_argument("Receiver column count must be an integer");
        const int64_t columns = config_.at("receiver_columns");
        if (columns <= 0)
            throw std::invalid_argument("Receiver column count must be positive");
        if (!config_.at("outfalls").is_array() || config_.at("outfalls").empty())
            throw std::invalid_argument("Map at least one outfall");
        for (const auto& row : config_.at("outfalls")) {
            double sum = 0;
            std::set<int64_t> used;
            for (const auto& target : row.at("targets")) {
                if (!target.at("column").is_number_integer())
                    throw std::invalid_argument("Receiver column indices must be integers");
                const int64_t c = target.at("column");
                const double w = target.at("weight");
                if (c < 0 || c >= columns || !used.insert(c).second || !std::isfinite(w) || w <= 0)
                    throw std::invalid_argument("Invalid or repeated receiver column/weight");
                sum += w;
            }
            if (std::abs(sum - 1) > 1.e-12)
                throw std::invalid_argument(
                    "Each outfall's conservative mapping weights must sum to one");
            const auto& temp = row.at("temperature");
            const std::string mode = temp.at("mode");
            if (mode != "native" && mode != "fixed")
                throw std::invalid_argument(
                    "Select native heat or an explicit fixed-temperature assumption");
            if (mode == "fixed" && (!std::isfinite(temp.at("value_degC").get<double>()) ||
                                    temp.value("provenance", std::string()).empty()))
                throw std::invalid_argument(
                    "Fixed temperature needs a finite Celsius value and provenance");
            const auto placement = row.value("placement", std::string("uniform"));
            if (placement != "uniform" && placement != "surface" && placement != "bed")
                throw std::invalid_argument("Use uniform, surface or bed placement");
        }
    }
};
class OpenSWMMInfo final : public Identity, public HC::IModelComponentInfo {
public:
    /**
     * @brief Construct one-way native-network reservoir-coupling factory metadata.
     * @note Stores canonical plugin ID/caption/description, no native engine instance or run
     * created; string/property-signal allocation may throw.
     */
    OpenSWMMInfo() : Identity("openswmm.model", "OpenSWMM network") {
        setDescription(
            "Native catchment/network with conservative external outfall receipts. One-way, CPU, no checkpoint or stage feedback.");
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
     * @param[in] v Path text copied verbatim, no filesystem/URI validation.
     * @note No reload, signal or native lifecycle change; string allocation may throw, host
     * serializes metadata setup.
     */
    void setLibraryFilePath(const std::string& v) override { library_ = v; }
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
     * @return Owning set of declared discovery tags: SWMM,network,outfalls,one-way.
     * @note Copies may allocate; no component creation, file access, physical/clock or registry
     * mutation.
     */
    std::set<std::string> tags() const override {
        return {"SWMM", "network", "outfalls", "one-way"};
    }
    /**
     * @brief Create a fresh Created model component owned by caller.
     * @return Unique ownership of new OpenSWMMModelComponent through IModelComponent. Instance
     * borrows this info factory, which must outlive every created component. No native model
     * open or physical time advance; allocation/construction exceptions propagate. Host assigns
     * stable identity and configuration, then drives lifecycle; caller destroys after
     * detach/finish.
     */
    std::unique_ptr<HC::IModelComponent> createComponentInstance() override {
        return std::make_unique<OpenSWMMModelComponent>(this);
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
HYDROCOUPLE_DECLARE_COMPONENT(openswmm::couplers::OpenSWMMInfo)
