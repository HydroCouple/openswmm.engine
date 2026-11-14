// SPDX-License-Identifier: Apache-2.0
#include "ProcessComponentHost.hpp"
#include "PluginFactory.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/FileIO.hpp"
#include <openswmm/engine/openswmm_engine.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <source_location>
#include <stdexcept>
#ifdef OPENSWMM_HAS_HYDROCOUPLE
#include "../../couplers/ComponentBase.hpp"
#ifdef OPENSWMM_HAS_2D
#include <openswmm/engine/openswmm_2d.h>
#endif
#endif

namespace openswmm::components {
bool foreign_component(const ProcessComponentSpec& spec) {
    if (spec.id.find_first_of("/\\") != std::string::npos ||
        spec.id.ends_with(".so") || spec.id.ends_with(".dylib") || spec.id.ends_with(".dll"))
        return true;
    return std::any_of(spec.args.begin(), spec.args.end(), [](const auto& item) {
        return item.first == "library";
    });
}

#ifdef OPENSWMM_HAS_HYDROCOUPLE
namespace {
namespace HC = HydroCouple;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using CS = HC::IModelComponent::ComponentStatus;
using couplers::Output;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("Native component host: " + message);
}
void checked(void* engine, int code, const std::source_location& call = std::source_location::current()) {
    require(code == 0, "native API code " + std::to_string(code) + " at line " +
        std::to_string(call.line()) + ": " + swmm_get_last_error_msg(engine));
}
template<class Function> void foreignBoundary(Function call) {
    try { call(); }
    catch (const std::exception&) { throw; }
    catch (...) { throw std::runtime_error("Native component host: foreign component threw a nonstandard exception"); }
}
std::string fileText(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "cannot read " + io::path_utf8(path));
    return {std::istreambuf_iterator<char>(stream), {}};
}
void saveJson(const fs::path& path, const Json& value) {
    std::ofstream stream(path);
    stream << value.dump(2) << '\n';
    stream.flush();
    require(bool(stream), "cannot write " + io::path_utf8(path));
}
std::string option(void* engine, const char* name) {
    char text[256]{};
    checked(engine, swmm_options_get(engine, name, text, sizeof(text)));
    return text;
}
template<class T> T scalar(HC::IModelComponent& component, const std::string& name) {
    const int64_t zero = 0, one = 1;
    T result{};
    HC::BufferDescriptor buffer{&result, HC::Helpers::DataKindOf<T>::kind, 1, &one, nullptr, sizeof(T)};
    for (auto* output : component.outputs())
        if (output && output->id() == name) {
            std::string message;
            const bool read = output->getValuesInto(buffer, {&zero, 1}, {&one, 1}, &message);
            require(read, "cannot read " + name + ": " + message);
            return result;
        }
    throw std::runtime_error("Native component host: missing receipt " + name);
}

class NativeDonor final : public couplers::ComponentBase {
public:
    explicit NativeDonor(const std::string& id) : ComponentBase(id, nullptr) {}
    std::vector<HC::IOutput*> outputs() const override {
        std::vector<HC::IOutput*> result;
        for (const auto& [id, field] : fields) result.push_back(field.get());
        return result;
    }
    void initialize() override { transition(CS::Initializing); transition(CS::Initialized); }
    std::vector<std::string> validate() override { transition(CS::Validating); transition(CS::Valid); return {}; }
    void prepare() override { transition(CS::Preparing); transition(CS::Updated); }
    void update(const std::vector<HC::IOutput*>& = {}) override {}
    void finish() override { transition(CS::Finishing); transition(CS::Finished); }
    void publish(bool done) { transition(CS::Updating); transition(done ? CS::Done : CS::Updated); }
    void add(const std::string& id, HC::DataKind kind, const char* unit = "unitless",
             double length = 0, double mass = 0, double time = 0,
             HC::ValueKind semantics = HC::ValueKind::Intensive, double factor = 1) {
        auto field = std::make_unique<Output>(id, this, kind, unit, length, mass, time, semantics, factor);
        field->setDescription("hydrocouple.interval-transfer/1;field=" + id);
        fields.emplace(id, std::move(field));
    }
    std::map<std::string, std::unique_ptr<Output>> fields;
};

struct Binding {
    void* engine;
    std::string id, configBytes;
    fs::path configPath, directory;
    Json config, diagnostics = Json::array();
    // Donor outlives every foreign input wired to it.
    NativeDonor donor;
    std::unique_ptr<HC::IModelComponent> receiver;
    std::vector<int> outfalls;
    std::vector<std::string> amounts{"water_m3", "enthalpy_j"};
    std::vector<double> times, previous, previousReverse;
    size_t sequence = 0;
    double epoch = 0, last = 0;
    bool prepared = false, finished = false;

    void cleanupReceiver() noexcept {
        if (!receiver) return;
        try {
            const auto state = receiver->status();
            if (state != CS::Created && state != CS::Finished) receiver->finish();
        } catch (...) {} // Preserve the original failure while releasing foreign resources.
    }

    void drain(const char* phase) {
        std::string failure;
        for (const auto& error : receiver->errors(true)) {
            diagnostics.push_back({{"phase", phase}, {"source", error.source},
                {"code", error.code}, {"severity", int(error.severity)}, {"message", error.message}});
            if (error.severity >= HC::ErrorEntry::Severity::Error)
                failure += error.source + ": " + std::to_string(error.code) + ": " + error.message + "; ";
        }
        require(failure.empty() && receiver->status() != CS::Failed, id + " " + phase + ": " + failure);
    }
    void setArgument(const std::string& name, const Json& value, const fs::path& base) {
        HC::IArgument* selected = nullptr;
        for (auto* argument : receiver->arguments())
            if (argument && argument->id() == name) selected = argument;
        require(selected != nullptr, "unknown argument " + name + " on " + id);
        auto type = HC::IArgument::ArgumentInputType::JSON;
        auto text = value.dump();
        if (value.is_object() && value.contains("@uri")) {
            require(value.size() == 1, "file argument envelope must contain only @uri");
            type = HC::IArgument::ArgumentInputType::File;
            text = io::path_utf8(fs::absolute(base / io::utf8_path(value.at("@uri").get<std::string>())));
        }
        require(selected->isValidArgType(type), "unsupported input type for argument " + name);
        std::string message;
        const bool initialized = selected->initialize(text, type, message);
        require(initialized, "argument " + name + ": " + message);
        drain("argument");
    }
    Binding(void* native, const ProcessComponentSpec& spec, PluginFactory& libraries,
            const fs::path& base, std::set<int>& owned, std::set<fs::path>& destinations)
        : engine(native), id(spec.id), donor(id + ".native") {
        std::map<std::string, std::string> arguments;
        for (const auto& [key, value] : spec.args) {
            require(key == "library" || key == "component", "unknown hosted row key " + key);
            require(arguments.emplace(key, value).second, "duplicate hosted row key " + key);
        }
        require(arguments.contains("library") && arguments.contains("component"),
                "use instance library=path component=type config=file.json");
        const auto library = fs::weakly_canonical(base / io::utf8_path(arguments.at("library")));
        const auto type = arguments.at("component");
        require(type != "org.hydrocouple.openswmm.engine" && type != "openswmm.model",
                "recursive native-engine hosting is not supported");
        std::string loadError;
        libraries.load_library(io::path_utf8(library), [&](const auto& message) { loadError = message; });
        HC::IModelComponentInfo* info = nullptr;
        for (const auto& record : PluginFactory::component_libraries()) {
            if (record.path == io::path_utf8(library)) {
                require(record.load_error.empty(), record.load_error);
                require(record.id == type, "library component type differs from " + type);
                info = dynamic_cast<HC::IModelComponentInfo*>(static_cast<HC::IComponentInfo*>(record.info));
            }
        }
        require(info != nullptr, loadError.empty() ? "no compatible model component in " + io::path_utf8(library) : loadError);
        configPath = fs::absolute(base / io::utf8_path(spec.config_path));
        configBytes = fileText(configPath);
        config = Json::parse(configBytes);
        require(config.at("schema") == "openswmm.hosted-component/1", "invalid hosting schema");
        require(config.at("profile") == "hydrocouple.interval-transfer/1", "unsupported transfer profile");
        for (const auto& [key, value] : config.items())
            require(key == "schema" || key == "profile" || key == "output_directory" ||
                    key == "arguments" || key == "connections" || key == "exports", "unknown hosting key " + key);
        const auto destination = config.at("output_directory").get<std::string>();
        require(!destination.empty(), "choose a fresh hosting output_directory");
        directory = fs::absolute(configPath.parent_path() / io::utf8_path(destination));
        require(!fs::exists(directory), "hosting output_directory already exists");
        require(option(engine, "QUALITY_SOLVER") == "LEGACY" && option(engine, "IGNORE_QUALITY") != "YES" &&
                option(engine, "HEAT_TRANSPORT") == "YES" && swmm_reaction_species_count(engine) == 0,
                "outfall profile requires native LEGACY mass pollutants and heat without MSX");
#ifdef OPENSWMM_HAS_2D
        int vertices = 0;
        const int mesh = swmm_2d_vertex_count(engine, &vertices);
        if (mesh != SWMM_ERR_BADPARAM) checked(engine, mesh); // no parsed mesh is BADPARAM
        require(vertices == 0, "spatial hosted exports need separate receipt qualification");
#endif
        double start = 0, end = 0;
        checked(engine, swmm_get_start_time(engine, &start));
        checked(engine, swmm_get_end_time(engine, &end));
        epoch = start + 2415018.5;
        const double duration = std::round((end - start) * 86400000.) / 1000.;
        times = config.at("exports").at("exchange_times_s").get<std::vector<double>>();
        double prior = 0;
        for (double value : times) {
            require(std::isfinite(value) && value > prior && value <= duration &&
                    std::abs(value * 1000 - std::round(value * 1000)) < 1e-7,
                    "exchange endpoints must increase on the native millisecond grid");
            prior = value;
        }
        require(!times.empty() && times.back() == duration, "last exchange must equal native duration");
        for (const auto& name : config.at("exports").at("outfalls")) {
            const auto nativeId = name.get<std::string>();
            const int node = swmm_node_index(engine, nativeId.c_str());
            int nodeType = -1, route = -1;
            require(node >= 0 && owned.insert(node).second, "unknown or multiply owned outfall " + nativeId);
            checked(engine, swmm_node_get_type(engine, node, &nodeType));
            checked(engine, swmm_node_get_outfall_route_to(engine, node, &route));
            require(nodeType == SWMM_NODE_OUTFALL && route < 0, "export needs an external native outfall");
            outfalls.push_back(node);
        }
        require(!outfalls.empty(), "choose at least one external outfall");
        require(destinations.insert(fs::weakly_canonical(directory)).second,
                "receiving instances require separate output directories");
        for (const auto& [key, value] : config.at("exports").items())
            require(key == "outfalls" || key == "exchange_times_s", "unknown exports key " + key);
        donor.add("transfer_id", HC::DataKind::String);
        donor.add("epoch_julian_day", HC::DataKind::Float64, "day", 0, 0, 1, HC::ValueKind::Intensive, 86400);
        donor.add("interval_start_s", HC::DataKind::Float64, "s", 0, 0, 1);
        donor.add("interval_end_s", HC::DataKind::Float64, "s", 0, 0, 1);
        donor.add("water_m3", HC::DataKind::Float64, "m3", 3, 0, 0, HC::ValueKind::Extensive);
        donor.add("enthalpy_j", HC::DataKind::Float64, "J", 2, 1, -2, HC::ValueKind::Extensive);
        for (int species = 0; species < swmm_pollutant_count(engine); ++species) {
            int units = -1;
            checked(engine, swmm_pollutant_get_units(engine, species, &units));
            require(units == 0 || units == 1, "count-based species needs an explicit mass basis");
            const std::string field = "species." + std::string(swmm_pollutant_id(engine, species));
            amounts.push_back(field);
            donor.add(field, HC::DataKind::Float64, "kg", 0, 1, 0, HC::ValueKind::Extensive);
        }
        previous.assign(amounts.size(), 0);
        previousReverse.assign(outfalls.size(), 0);
        try {
            receiver = info->createComponentInstance();
            require(bool(receiver), "component factory returned null");
            receiver->setReferenceDirectory(io::path_utf8(directory));
            setArgument("hydrocouple.instance_id", {{"values", {id}}}, configPath.parent_path());
            for (const auto& [name, value] : config.at("arguments").items()) {
                require(name != "hydrocouple.instance_id", "instance identity belongs to its process row");
                setArgument(name, value, configPath.parent_path());
            }
            receiver->initialize();
            drain("initialize");
            require(receiver->status() == CS::Initialized, "foreign component did not initialize");
            std::set<std::string> sources, targets;
            for (const auto& connection : config.at("connections")) {
                require(connection.is_object() && connection.size() == 2,
                        "connections must contain only output and input");
                const auto source = connection.at("output").get<std::string>();
                const auto target = connection.at("input").get<std::string>();
                require(source == target && sources.insert(source).second && targets.insert(target).second,
                        "connect every interval field once, without adapters or renaming");
                require(donor.fields.contains(source), "unknown native output " + source);
                HC::IInput* input = nullptr;
                for (auto* candidate : receiver->inputs())
                    if (candidate && candidate->id() == target) input = candidate;
                require(input != nullptr, "unknown receiving input " + target);
                donor.fields.at(source)->addConsumer(input);
                drain("connect");
            }
            require(sources.size() == donor.fields.size(), "connect the entire interval tuple");
            donor.initialize();
            (void)donor.validate();
            const auto validation = receiver->validate();
            drain("validate");
            require(validation.empty() && receiver->status() == CS::Valid,
                    validation.empty() ? "invalid receiving component" : validation.front());
            for (const auto& amount : amounts) {
                const auto* expected = dynamic_cast<HC::IQuantity*>(donor.fields.at(amount)->valueDefinition());
                const HC::IQuantity* actual = nullptr;
                for (auto* output : receiver->outputs())
                    if (output && output->id() == "accepted." + amount && output->shape() == std::vector<int64_t>{1} &&
                        output->dataKind() == HC::DataKind::Float64)
                        actual = dynamic_cast<HC::IQuantity*>(output->valueDefinition());
                require(actual && actual->unit() && actual->unit()->dimensions() &&
                    actual->valueKind() == HC::ValueKind::Extensive && actual->unit()->caption() == expected->unit()->caption() &&
                    actual->unit()->conversionFactorToSI() == 1 && actual->unit()->offsetToSI() == 0,
                    "incompatible receipt units/basis for " + amount);
                for (int axis = 0; axis <= int(HC::IUnitDimensions::FundamentalUnitDimension::PlaneAngle); ++axis) {
                    const auto dimension = static_cast<HC::IUnitDimensions::FundamentalUnitDimension>(axis);
                    require(actual->unit()->dimensions()->power(dimension) == expected->unit()->dimensions()->power(dimension),
                            "incompatible receipt dimensions for " + amount);
                }
            }
            (void)scalar<std::string>(*receiver, "accepted_transfer_id");
        } catch (...) {
            cleanupReceiver();
            throw;
        }
    }
    void start() {
        require(fileText(configPath) == configBytes, "hosting configuration changed after open");
        require(fs::create_directories(directory), "choose a fresh hosting output_directory");
        donor.prepare();
        receiver->prepare();
        drain("prepare");
        require(receiver->status() == CS::Updated, "receiver is not prepared");
        prepared = true;
        saveJson(directory / "configuration.json", config);
        // Query capability and baseline only after the native engine is RUNNING.
        for (size_t i = 0; i < outfalls.size(); ++i) {
            SWMM_OutfallExportReceipt receipt{};
            std::vector<double> species(amounts.size() - 2);
            checked(engine, swmm_node_get_export_receipt(engine, outfalls[i], &receipt, species.data(), int(species.size())));
            require((receipt.flags & 7u) == 7u, "native export receipts lack the required water/mass/heat profile");
            previous[0] += receipt.exported_m3;
            previous[1] += receipt.exported_heat_j;
            previousReverse[i] = receipt.reversed_m3;
            for (size_t k = 0; k < species.size(); ++k) previous[k + 2] += species[k];
        }
    }
    double next() const { return sequence < times.size() ? times[sequence] : std::numeric_limits<double>::infinity(); }
    void accept(double seconds) {
        if (seconds < next()) return;
        require(prepared && seconds == next(), "native step crossed the agreed transfer endpoint");
        std::vector<double> cumulative(amounts.size(), 0), offered(amounts.size()), accepted(amounts.size()), pending(amounts.size());
        Json native = Json::array();
        for (size_t i = 0; i < outfalls.size(); ++i) {
            SWMM_OutfallExportReceipt receipt{};
            std::vector<double> species(amounts.size() - 2);
            checked(engine, swmm_node_get_export_receipt(engine, outfalls[i], &receipt, species.data(), int(species.size())));
            native.push_back({{"outfall", swmm_node_id(engine, outfalls[i])}, {"water_m3", receipt.exported_m3},
                {"enthalpy_j", receipt.exported_heat_j}, {"reversed_m3", receipt.reversed_m3}, {"species_kg", species}});
            require(receipt.reversed_m3 == previousReverse[i], "reverse outfall flow requires a bidirectional profile");
            cumulative[0] += receipt.exported_m3;
            cumulative[1] += receipt.exported_heat_j;
            for (size_t k = 0; k < species.size(); ++k) cumulative[k + 2] += species[k];
        }
        const std::string token = id + ":" + std::to_string(sequence + 1);
        donor.fields.at("transfer_id")->text = token;
        donor.fields.at("epoch_julian_day")->number = epoch;
        donor.fields.at("interval_start_s")->number = last;
        donor.fields.at("interval_end_s")->number = seconds;
        for (size_t k = 0; k < offered.size(); ++k) {
            offered[k] = cumulative[k] - previous[k];
            require(std::isfinite(offered[k]) && (k == 1 || offered[k] >= 0), "invalid native export amount");
            donor.fields.at(amounts[k])->number = offered[k];
        }
        const auto ledger = directory / ("transfer_" + std::to_string(sequence) + ".json");
        Json row{{"schema", "hydrocouple.interval-transfer/1"}, {"transfer_id", token}, {"receiver", id},
            {"epoch_julian_day", epoch}, {"start_s", last}, {"end_s", seconds},
            {"ownership", "native_export_already_debited"}, {"fields", amounts},
            {"offered", offered}, {"accepted", nullptr}, {"pending", offered}, {"native", native}};
        saveJson(ledger, row);
        donor.publish(sequence + 1 == times.size());
        receiver->update();
        drain("update");
        require(scalar<std::string>(*receiver, "accepted_transfer_id") == token, "missing or stale receipt token");
        for (size_t k = 0; k < amounts.size(); ++k) accepted[k] = scalar<double>(*receiver, "accepted." + amounts[k]);
        require(std::isfinite(accepted[0]) && accepted[0] >= 0 && accepted[0] <= offered[0], "invalid accepted water");
        const double fraction = offered[0] > 0 ? accepted[0] / offered[0] : 1;
        for (size_t k = 0; k < amounts.size(); ++k) {
            require(std::isfinite(accepted[k]) && accepted[k] == offered[k] * fraction, "receipt changed native proportions");
            pending[k] = offered[k] - accepted[k];
        }
        row["accepted"] = accepted;
        row["pending"] = pending;
        row["diagnostics"] = diagnostics;
        saveJson(ledger, row);
        require(fraction == 1, "receiver refused an already-debited export; pending amounts retained, native advancement stopped");
        previous = cumulative;
        last = seconds;
        ++sequence;
        require(receiver->status() == (sequence == times.size() ? CS::Done : CS::Updated), "receiver completion differs from transfer schedule");
    }
    void finish() {
        if (!prepared || finished) return;
        if (sequence != times.size()) {
            double seconds = 0;
            checked(engine, swmm_engine_get_elapsed_seconds(engine, &seconds));
            std::vector<double> pending(amounts.size(), 0);
            for (int outfall : outfalls) {
                SWMM_OutfallExportReceipt receipt{};
                std::vector<double> species(amounts.size() - 2);
                checked(engine, swmm_node_get_export_receipt(engine, outfall, &receipt,
                    species.data(), int(species.size())));
                pending[0] += receipt.exported_m3;
                pending[1] += receipt.exported_heat_j;
                for (size_t k = 0; k < species.size(); ++k) pending[k + 2] += species[k];
            }
            for (size_t k = 0; k < pending.size(); ++k) pending[k] -= previous[k];
            const Json cancelled{{"schema", "hydrocouple.interval-transfer/1"},
                {"status", "cancelled"}, {"receiver", id}, {"start_s", last}, {"end_s", seconds},
                {"fields", amounts}, {"pending", pending}, {"ownership", "native_export_already_debited"}};
            saveJson(directory / "cancelled.json", cancelled);
        }
        receiver->finish();
        drain("finish");
        require(receiver->status() == CS::Finished, "receiving component did not finish");
        donor.finish();
        finished = true;
    }
    ~Binding() {
        if (!finished) cleanupReceiver();
    }
};
}

struct ProcessComponentHost::Impl {
    std::vector<std::unique_ptr<Binding>> bindings;
    const SimulationContext* context = nullptr;
    std::vector<ProcessComponentSpec> expected;
};
ProcessComponentHost::ProcessComponentHost(void* engine, const SimulationContext& context,
    PluginFactory& libraries, const std::string& model_directory) : impl_(std::make_unique<Impl>()) {
    std::set<int> owned;
    std::set<fs::path> destinations;
    impl_->context = &context;
    impl_->expected = context.process_component_specs;
    foreignBoundary([&] {
        for (const auto& spec : context.process_component_specs)
            if (foreign_component(spec))
                impl_->bindings.push_back(std::make_unique<Binding>(engine, spec, libraries,
                    fs::absolute(io::utf8_path(model_directory)), owned, destinations));
    });
}
ProcessComponentHost::~ProcessComponentHost() = default;
void ProcessComponentHost::start() {
    foreignBoundary([&] {
        const auto& current = impl_->context->process_component_specs;
        require(current.size() == impl_->expected.size(), "process rows changed after open; reopen the model");
        for (size_t k = 0; k < current.size(); ++k)
            require(current[k].id == impl_->expected[k].id && current[k].config_path == impl_->expected[k].config_path &&
                    current[k].args == impl_->expected[k].args, "process rows changed after open; reopen the model");
        for (auto& binding : impl_->bindings) binding->start();
    });
}
double ProcessComponentHost::nextBoundary() const {
    double next = std::numeric_limits<double>::infinity();
    for (auto& binding : impl_->bindings) next = std::min(next, binding->next());
    return next;
}
void ProcessComponentHost::acceptBoundary(double seconds) { foreignBoundary([&] { for (auto& binding : impl_->bindings) binding->accept(seconds); }); }
void ProcessComponentHost::finish() { foreignBoundary([&] { for (auto& binding : impl_->bindings) binding->finish(); }); }
#else
struct ProcessComponentHost::Impl {};
ProcessComponentHost::ProcessComponentHost(void*, const SimulationContext&, PluginFactory&, const std::string&) {
    throw std::runtime_error("Foreign process components require OPENSWMM_WITH_HYDROCOUPLE");
}
ProcessComponentHost::~ProcessComponentHost() = default;
void ProcessComponentHost::start() {}
double ProcessComponentHost::nextBoundary() const { return std::numeric_limits<double>::infinity(); }
void ProcessComponentHost::acceptBoundary(double) {}
void ProcessComponentHost::finish() {}
#endif
}
