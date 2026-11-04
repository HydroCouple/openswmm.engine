// SPDX-License-Identifier: Apache-2.0
// Native/direct-component parity and real receiver acceptance through the ABI.

/**
 * @file      test_engine_component.cpp
 * @brief     Tests for engine component.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>
#include <hydrocouplecomponentabi.h>
#include <hydrocoupletemporal.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_output.h>
#include <openswmm/engine/openswmm_checkpoint.h>
#include <openswmm/engine/openswmm_infrastructure.h>
#ifdef OPENSWMM_HAS_2D
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_split_packet.h>
#include <openswmm/engine/openswmm_gw2d.h>
#include <openswmm/engine/openswmm_output2d.h>
#include <openswmm/engine/openswmm_surface_ownership.h>
#include <openswmm/engine/openswmm_subcatchments.h>
#endif
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
namespace HC = HydroCouple;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Status = HC::IModelComponent::ComponentStatus;
namespace {
class Library {
public:
    /**
     * @brief Load the component library for the fixture lifetime.
     *
     * Owns the resulting loader handle, possibly null on failure; does not throw/report loader failure.
     * POSIX requests immediate local symbol resolution. Loaded functions/objects must not outlive the
     * library.
     *
     * @param[in] path Non-null native library path passed to LoadLibraryA or dlopen.
     */
    explicit Library(const char* path) {
#ifdef _WIN32
        handle_ = LoadLibraryA(path);
#else
        handle_ = dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
    }
    /**
     * @brief Unload a successfully loaded component library.
     *
     * Null is a no-op. Ignores platform unload status; callers must already have destroyed component
     * instances and stopped using borrowed symbols/metadata.
     */
    ~Library() {
        if (!handle_)
            return;
#ifdef _WIN32
        FreeLibrary(handle_);
#else
        dlclose(handle_);
#endif
    }
    /**
     * @brief Look up a named entry point in the loaded library.
     *
     * POSIX checks for a null loader handle; Windows forwards the handle directly to GetProcAddress.
     * Does not cache or own the returned symbol.
     *
     * @param[in] name Non-null exact symbol name.
     * @return Borrowed function/address pointer or nullptr on lookup failure, valid only while the
     *     library remains loaded.
     */
    void* symbol(const char* name) const {
#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(handle_, name));
#else
        return handle_ ? dlsym(handle_, name) : nullptr;
#endif
    }
    /**
     * @brief Create a component only after matching the expected HydroCouple ABI stamp.
     *
     * Calls borrowed ABI/info entry points, requires exact ABI stamp, dynamic-casts metadata then
     * invokes its factory. Assumes a resolved ABI function returns a non-null string. Library must
     * outlive the instance and borrowed metadata; this helper does not release library-owned metadata.
     * @return Owned component instance or empty pointer when symbols/stamp/metadata type/factory fail.
     */
    std::unique_ptr<HC::IModelComponent> create() const {
        auto abi =
            reinterpret_cast<HydroCoupleComponentAbiFn>(symbol(HYDROCOUPLE_COMPONENT_ABI_SYMBOL));
        if (!abi || std::string(abi()) != HYDROCOUPLE_COMPONENT_ABI_STAMP)
            return {};
        auto entry =
            reinterpret_cast<HydroCoupleComponentInfoFn>(symbol(HYDROCOUPLE_COMPONENT_INFO_SYMBOL));
        auto* info = entry ? dynamic_cast<HC::IModelComponentInfo*>(entry()) : nullptr;
        return info ? info->createComponentInstance() : nullptr;
    }

private:
#ifdef _WIN32
    HMODULE handle_ = nullptr;
#else
    void* handle_ = nullptr;
#endif
};
struct NativeEngine {
    SWMM_Engine value = swmm_engine_create();
    /**
     * @brief Destroy the fixture-owned native engine, including any partial lifecycle.
     *
     * Destroy accepts null after failed member initialization. Any borrowed engine state expires;
     * artifacts remain.
     */
    ~NativeEngine() { swmm_engine_destroy(value); }
};
struct NativePhaseTrace {
    Json rows = Json::array();
    bool failed = false;
    static void observe(SWMM_Engine engine, int phase, double start, double end, void* user) noexcept {
        auto& trace = *static_cast<NativePhaseTrace*>(user);
        try {
            double elapsed = 0;
            if (swmm_engine_get_elapsed_seconds(engine, &elapsed)) { trace.failed = true; return; }
            Json row{{"phase", phase}, {"start_s", start}, {"end_s", end}, {"native_time_s", elapsed}};
#ifdef OPENSWMM_HAS_2D
            int cells = 0, layers = 0;
            if (swmm_gw2d_get_dimensions(engine, &cells, &layers)) { trace.failed = true; return; }
            row["groundwater"] = Json::array();
            for (int cell = 0; cell < cells; ++cell) {
                double completed = 0, incoming = 0, outgoing = 0;
                if (swmm_gw2d_get_cell(engine, cell, SWMM_GW2D_VAR_COMPLETED_TIME, &completed) ||
                    swmm_gw2d_get_cell(engine, cell, SWMM_GW2D_VAR_PENDING_IN, &incoming) ||
                    swmm_gw2d_get_cell(engine, cell, SWMM_GW2D_VAR_PENDING_OUT, &outgoing)) {
                    trace.failed = true; return;
                }
                row["groundwater"].push_back({{"cell", cell}, {"completed_s", completed},
                    {"pending_in_m3", incoming}, {"pending_out_m3", outgoing}});
            }
            double storage = 0;
            if (swmm_gw2d_get_ledger(engine, SWMM_GW2D_LED_STORAGE, &storage)) { trace.failed = true; return; }
            row["groundwater_storage_including_pending_m3"] = storage;
            row["joint_lid"] = Json::array();
            for (int node = 0; node < swmm_node_count(engine); ++node) {
                SWMM_LidBottomState state{};
                if (swmm_lid_node_bottom_state_get(engine, node, &state) != SWMM_OK) continue;
                row["joint_lid"].push_back({{"node", swmm_node_id(engine, node)},
                    {"valid_sample", state.valid_sample}, {"aquifer_time_s", state.aquifer_time_s},
                    {"sample_start_s", state.sample_start_s}, {"sample_end_s", state.sample_end_s},
                    {"requested_down_m3", state.requested_down_m3}, {"requested_up_m3", state.requested_up_m3},
                    {"reserved_down_m3", state.reserved_down_m3}, {"reserved_up_m3", state.reserved_up_m3},
                    {"accepted_down_m3", state.accepted_down_m3}, {"accepted_up_m3", state.accepted_up_m3},
                    {"cumulative_down_m3", state.down_cumulative_m3}, {"cumulative_up_m3", state.up_cumulative_m3},
                    {"pending_down_m3", state.pending_down_m3}, {"pending_up_m3", state.pending_up_m3}});
            }
#endif
            trace.rows.push_back(std::move(row));
        } catch (...) { trace.failed = true; }
    }
};
/**
 * @brief Read binary output evidence with a nonfatal open assertion.
 *
 * Records file.good failure but does not stop the caller. Local stream closes at return; allocation
 * exceptions propagate.
 *
 * @param[in] path Borrowed source artifact path.
 * @return Owned bytes; empty/partial result may follow an IO failure.
 */
std::string bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    EXPECT_TRUE(file.good()) << path;
    return {std::istreambuf_iterator<char>(file), {}};
}
/**
 * @brief Find a component output by exact ID.
 *
 * Output lifetime follows the component/catalogue; no ownership transfer or state update occurs.
 *
 * @param[in] component Borrowed component whose output entries must be non-null.
 * @param[in] id Exact case-sensitive output ID.
 * @return Borrowed matching output pointer, or nullptr with a recorded test failure.
 */
HC::IOutput* output(HC::IModelComponent& component, const std::string& id) {
    for (auto* item : component.outputs())
        if (item->id() == id)
            return item;
    ADD_FAILURE() << "Missing output " << id;
    return nullptr;
}
/**
 * @brief Read the first scalar component output into owned storage.
 *
 * Intended T is double or std::string: descriptor selects Float64 for double and String otherwise,
 * with sizeof(T) capacity. Nonfatally asserts getValuesInto success and forwards its diagnostic. No
 * output pointer is retained; mismatched T is not validated here.
 *
 * @param[in] item Borrowed output expected to accept one element at index zero; nullptr throws
 *     runtime_error.
 * @return Owned scalar result of T, default-initialized when a rejected call leaves it unchanged.
 */
template <class T> T read(HC::IComponentDataItem* item) {
    if (!item)
        throw std::runtime_error("Cannot read a missing component output");
    T value{};
    const int64_t one = 1, zero = 0;
    std::string message;
    HC::BufferDescriptor buffer{
        &value,  std::is_same_v<T, double> ? HC::DataKind::Float64 : HC::DataKind::String,
        1,       &one,
        nullptr, sizeof(T)};
    EXPECT_TRUE(item->getValuesInto(buffer, {&zero, 1}, {&one, 1}, &message)) << message;
    return value;
}
/**
 * @brief Initialize a named component argument from serialized JSON.
 *
 * Fatally asserts matching argument initialize success and fails if no ID exists. A fatal assertion
 * stops this helper but leaves any partial component argument mutation; caller relies on the
 * recorded test failure. Serialization exceptions propagate.
 *
 * @param[in,out] component Borrowed component whose matching argument is mutated.
 * @param[in] id Exact argument ID; all returned entries are expected non-null.
 * @param[in] value Borrowed JSON value serialized with dump into argument input.
 */
void argument(HC::IModelComponent& component, const std::string& id, const Json& value) {
    for (auto* item : component.arguments())
        if (item->id() == id) {
            std::string message;
            ASSERT_TRUE(
                item->initialize(value.dump(), HC::IArgument::ArgumentInputType::JSON, message))
                << message;
            return;
        }
    FAIL() << "Missing argument " << id;
}
/**
 * @brief Concatenate component error messages for assertion context.
 *
 * Does not clear errors, update status or retain borrowed entries.
 *
 * @param[in] component Borrowed component whose current errors are copied.
 * @return Owned message string with semicolon-space after each entry, empty when no errors exist.
 */
std::string diagnostics(HC::IModelComponent& component) {
    std::string message;
    for (const auto& error : component.errors())
        message += error.message + "; ";
    return message;
}
class EngineComponent : public ::testing::Test {
protected:
    Library library{ENGINE_COMPONENT_LIBRARY};
    std::unique_ptr<HC::IModelComponent> component;
    fs::path directory, model;
    Json configuration;
    /**
     * @brief Create per-case component evidence and a configured reference model.
     *
     * Creates component_engine/test-name, removes its wrapped subdirectory recursively, overwrites
     * model.inp from the fixed reference deck, creates a component and sets its reference
     * directory/instance ID. Stores complete-model JSON configuration for later initialization. Fatal
     * component creation failure stops setup; filesystem/JSON exceptions propagate. Library member
     * outlives component member destruction.
     */
    void SetUp() override {
        directory = fs::absolute(fs::path("component_engine") /
                                 ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::create_directories(directory);
        fs::remove_all(directory / "wrapped");
        model = directory / "model.inp";
        fs::copy_file("component_metadata/reference.inp", model,
                      fs::copy_options::overwrite_existing);
        component = library.create();
        ASSERT_NE(component, nullptr);
        EXPECT_EQ(component->componentInfo()->id(), "org.hydrocouple.openswmm.engine");
        component->setReferenceDirectory(directory.string());
        argument(*component, "hydrocouple.instance_id", {{"values", {"network"}}});
        configuration = {{"schema", "openswmm.component/1"},
                         {"model", "model.inp"},
                         {"role", "complete_model"},
                         {"output_directory", "wrapped"}};
    }
    /**
     * @brief Initialize the component from configuration and save its metadata catalogue.
     *
     * Requires setup and an uninitialized component. Initializes configuration argument, calls
     * component initialize, fatally requires Initialized status, reads/parses model_metadata JSON and
     * overwrites catalogue.json. File-write outcome is unchecked; JSON/read failures can throw. The
     * catalogue is fixture evidence written by this harness.
     *
     * @param[in] exports Add outfall O and exchange times 60/120/600 s when true.
     */
    void initialize(bool exports = false) {
        if (exports) {
            configuration["outfalls"] = {"O"};
            configuration["exchange_times_s"] = {60, 120, 600};
        }
        argument(*component, "configuration", configuration);
        component->initialize();
        ASSERT_EQ(component->status(), Status::Initialized) << diagnostics(*component);
        // Reviewable configured catalogue, written by the harness, not the component.
        const auto catalogue = Json::parse(read<std::string>(output(*component, "model_metadata")));
        std::ofstream(directory / "catalogue.json") << catalogue.dump(2) << '\n';
    }
    /**
     * @brief Initialize, validate and prepare the component for updates.
     *
     * Requires setup. Fatally requires an empty validation-error list and Updated status after prepare;
     * does not update time. Nested helper assertions record failures but do not automatically return
     * from this outer helper, so callers must honor test-failure evidence.
     *
     * @param[in] exports Forward optional exchange catalogue settings to initialize.
     */
    void ready(bool exports = false) {
        initialize(exports);
        ASSERT_TRUE(component->validate().empty()) << diagnostics(*component);
        component->prepare();
        ASSERT_EQ(component->status(), Status::Updated) << diagnostics(*component);
    }
    /**
     * @brief Open, initialize and start the reference engine beside the component run.
     *
     * Creates native evidence directory and uses model plus native/model.rpt/.out destinations. Fatal
     * assertions check each operation; the NativeEngine owner supplies cleanup after partial failure.
     * Does not step/end here.
     *
     * @param[in,out] native Fixture-owned allocated engine, mutated to a running state with saved
     *     results.
     */
    void nativeStart(NativeEngine& native, bool surface = false, bool managed = false) {
        fs::create_directories(directory / "native");
        if (surface)
            fs::remove(directory / "native/model.2d.h5");
        if (managed) {
            fs::remove(directory / "native/model.rpt");
            fs::remove(directory / "native/model.out");
        }
        ASSERT_EQ(swmm_engine_open(native.value, model.string().c_str(),
                                   (directory / "native/model.rpt").string().c_str(),
                                   (directory / "native/model.out").string().c_str(), nullptr),
                  0) << swmm_get_last_error_msg(native.value);
#ifdef OPENSWMM_HAS_2D
        if (surface)
            ASSERT_EQ(swmm_2d_set_results_file(native.value,
                        (directory / "native/model.2d.h5").string().c_str()), 0);
#endif
        if (managed) {
            char token[32]{}, diagnostic[4096]{};
            int owners = 0, objects = 0, shares = 0, cells = 0, valid = 0;
#ifdef OPENSWMM_HAS_2D
            ASSERT_EQ(swmm_surface_owner_get(native.value, nullptr, 0, &owners), 0);
            if (owners) {
                ASSERT_EQ(swmm_surface_owner_preview(native.value, nullptr, -1, nullptr, 0,
                    &objects, nullptr, 0, &shares, nullptr, 0, &cells, &valid, token,
                    sizeof(token), diagnostic, sizeof(diagnostic), nullptr, nullptr), 0);
                ASSERT_EQ(valid, 1) << diagnostic;
            }
#endif
            SWMM_CheckpointSessionOptions options{};
            options.struct_size = sizeof(options);
            options.schema_version = 1;
            options.output = surface ? SWMM_CHECKPOINT_OUTPUT_COMBINED : SWMM_CHECKPOINT_OUTPUT_BINARY;
            options.ownership_token = owners ? token : nullptr;
            ASSERT_EQ(swmm_checkpoint_start_session(native.value, &options), 0)
                << swmm_checkpoint_last_error_msg();
        } else {
            ASSERT_EQ(swmm_engine_initialize(native.value), 0) << swmm_get_last_error_msg(native.value);
            ASSERT_EQ(swmm_engine_start(native.value, 1), 0);
        }
    }
    /**
     * @brief Rewrite the copied model as a wet, reactive link-level comparison fixture.
     *
     * Requires the known reference deck fragments. Replaces routing/boundary/depth/pollutant/DWF text,
     * writes model.rxn with X=2*TSS pipe/tank formulas, and appends the reaction process row. Local
     * replacement lambdas fatally fail on missing anchors but later outer writes may still occur;
     * stream failures are unchecked. Matching artifacts are intentionally overwritten.
     */
    void reaction() {
        // Wet link level pool: native TSS remains at Cinit=100 and X=2*TSS
        // is observable in the pipe. A zero-volume junction need not react.
        auto text = bytes(model);
        auto replace = [&](const std::string& from, const std::string& to) {
            const auto at = text.find(from);
            ASSERT_NE(at, std::string::npos);
            text.replace(at, from.size(), to);
        };
        replace("FLOW_ROUTING KINWAVE", "FLOW_ROUTING DYNWAVE");
        replace("J 10 10 0 0 0", "J 10 10 1.5 0 0");
        replace("O 9 FREE NO", "O 10 FIXED 11.5 NO");
        replace("[DWF]\nJ FLOW 1\nJ A 100\nJ B 100000\n", "");
        replace("[POLLUTANTS]\nA ", "[POLLUTANTS]\nTSS ");
        std::ofstream(model) << text;
        std::ofstream(directory / "model.rxn")
            << "[REACTION_SPECIES]\nBULK X MG\nWALL W MG\n[REACTION_PIPES]\nFORMULA X 2*TSS\n[REACTION_TANKS]\nFORMULA X 2*TSS\n";
        std::ofstream(model, std::ios::app)
            << "\n[PROCESS_COMPONENTS]\norg.hydrocouple.openswmm.reactions config=\"model.rxn\"\n";
    }
    /**
     * @brief Run native and component models independently and compare output bytes.
     *
     * Requires configured setup, optionally rewritten reaction deck. Starts native and readies
     * component, steps each with a 10000-iteration guard, asserts completion and native
     * end/report/close, then component finish. Nonfatally compares native/model.out and
     * wrapped/model.out bytes. Fatal assertions can leave a partial component lifecycle, while local
     * NativeEngine still destroys its handle.
     */
    void runUncoupledAndCompare(bool surface = false, bool coupled = false, bool managed = false,
                               bool transport = false, bool jointLid = false) {
        NativeEngine native;
        ASSERT_NO_FATAL_FAILURE(nativeStart(native, surface, managed));
        ASSERT_NO_FATAL_FAILURE(ready());
#ifdef OPENSWMM_HAS_2D
        auto transportState = [&]() {
            Json state = Json::array();
            int count = 0, cells = 0;
            EXPECT_EQ(swmm_gw2d_species_count(native.value, &count), 0);
            EXPECT_EQ(swmm_2d_cell_count(native.value, &cells), 0);
            for (int species = 0; species < count; ++species) {
                char name[256]{};
                EXPECT_EQ(swmm_gw2d_species_name(native.value, species, name, sizeof(name)), 0);
                Json row{{"name", name}};
                std::vector<double> values(cells);
                int written = 0;
                EXPECT_EQ(swmm_2d_get_cell_conc(native.value, species, values.data(), cells, &written), 0);
                EXPECT_EQ(written, cells);
                row["surface"] = values;
                for (const auto& [zone, key] : std::vector<std::pair<int, const char*>>{
                         {SWMM_GW2D_ZONE_SAT, "saturated"}, {SWMM_GW2D_ZONE_UNSAT, "unsaturated"}}) {
                    EXPECT_EQ(swmm_gw2d_get_cell_conc(native.value, zone, species, values.data(), cells, &written), 0);
                    EXPECT_EQ(written, cells);
                    row[key] = values;
                }
                for (const auto& [term, key] : std::vector<std::pair<int, const char*>>{
                         {SWMM_GW2D_SPL_INIT, "initial"}, {SWMM_GW2D_SPL_STORAGE, "storage"},
                         {SWMM_GW2D_SPL_INFIL_IN, "infiltration"},
                         {SWMM_GW2D_SPL_AGE_GENERATED, "age_generated"},
                         {SWMM_GW2D_SPL_RESIDUAL, "residual"}}) {
                    double value = 0;
                    EXPECT_EQ(swmm_gw2d_get_species_ledger(native.value, species, term, &value), 0);
                    row[key] = value;
                }
                state.push_back(row);
            }
            return state;
        };
        Json initialTransport;
        if (transport) initialTransport = transportState();
#endif
        NativePhaseTrace trace;
        if (surface && managed)
            ASSERT_EQ(swmm_set_phase_callback(native.value, NativePhaseTrace::observe, &trace), 0);
        double elapsed = 0;
        int guard = 0;
        do {
            ASSERT_EQ(swmm_engine_step(native.value, &elapsed), 0);
        } while (elapsed > 0 && ++guard < 10000);
        ASSERT_LT(guard, 10000);
        if (surface && managed) {
            ASSERT_FALSE(trace.failed);
            ASSERT_FALSE(trace.rows.empty());
            std::ofstream(directory / "native-phase-trace.json") << trace.rows.dump(2);
            int previous = 0;
            for (const auto& row : trace.rows) {
                const int phase = row["phase"];
                EXPECT_GT(phase, previous);
                previous = phase == SWMM_PHASE_STEP_ACCEPTED ? 0 : phase;
                if (phase == SWMM_PHASE_STEP_ACCEPTED)
                    EXPECT_NEAR(row["native_time_s"].get<double>(), row["end_s"].get<double>(), 1e-12);
            }
            EXPECT_EQ(previous, 0);
            ASSERT_EQ(swmm_set_phase_callback(native.value, nullptr, nullptr), 0);
        }
#ifdef OPENSWMM_HAS_2D
        if (jointLid) {
            const int node = swmm_node_index(native.value, "SINK");
            SWMM_LidBottomState state{};
            ASSERT_EQ(swmm_lid_node_bottom_state_get(native.value, node, &state), 0);
            EXPECT_EQ(state.flow_model, 1);
            EXPECT_EQ(state.valid_sample, 1);
            EXPECT_GT(state.down_cumulative_m3 + state.up_cumulative_m3, 0);
            EXPECT_GE(state.pending_down_m3, 0);
            EXPECT_GE(state.pending_up_m3, 0);
            EXPECT_DOUBLE_EQ(state.net_cumulative_m3, state.down_cumulative_m3 - state.up_cumulative_m3);
            EXPECT_LE(state.accepted_down_m3, state.reserved_down_m3);
            EXPECT_LE(state.accepted_up_m3, state.reserved_up_m3);
            SWMM_LidRichardsStatistics statistics{};
            ASSERT_EQ(swmm_lid_richards_statistics_get(native.value, node, &statistics), 0);
            EXPECT_GT(statistics.accepted, 0);
            const Json ledger{{"down_m3", state.down_cumulative_m3}, {"up_m3", state.up_cumulative_m3},
                {"pending_down_m3", state.pending_down_m3}, {"pending_up_m3", state.pending_up_m3},
                {"requested_down_m3", state.requested_down_m3}, {"requested_up_m3", state.requested_up_m3},
                {"accepted_down_m3", state.accepted_down_m3}, {"accepted_up_m3", state.accepted_up_m3},
                {"aquifer_time_s", state.aquifer_time_s}, {"sample_start_s", state.sample_start_s},
                {"sample_end_s", state.sample_end_s}, {"richards_accepted", statistics.accepted},
                {"richards_rejected", statistics.rejected}};
            std::ofstream(directory / "native-joint-lid-ledger.json") << ledger.dump(2);
        }
        if (transport) {
            const auto finalTransport = transportState();
            std::ofstream(directory / "native-transport-ledger.json")
                << Json{{"initial", initialTransport}, {"final", finalTransport}}.dump(2);
            ASSERT_EQ(finalTransport.size(), 3u);
            for (std::size_t species = 0; species < finalTransport.size(); ++species) {
                const auto& row = finalTransport[species];
                for (const auto* domain : {"surface", "saturated", "unsaturated"})
                    for (const auto& value : row[domain]) EXPECT_TRUE(std::isfinite(value.get<double>()));
                if (row["name"] == "__TEMPERATURE__") {
                    // Porous heat also stores energy in soil: C*m3 is not an energy ledger.
                    EXPECT_LT(row["surface"][0].get<double>(), initialTransport[species]["surface"][0].get<double>());
                    EXPECT_GT(row["unsaturated"][0].get<double>(), initialTransport[species]["unsaturated"][0].get<double>());
                } else {
                    EXPECT_GT(row["infiltration"].get<double>(), 0);
                    EXPECT_LE(std::abs(row["residual"].get<double>()), 1e-10 *
                        std::max({1.0, std::abs(row["initial"].get<double>()),
                                  std::abs(row["storage"].get<double>())}) + 1e-12);
                    if (row["name"] == "__WATER_AGE__") {
                        EXPECT_GT(row["age_generated"].get<double>(), 0);
                        EXPECT_GT(row["surface"][0].get<double>(), initialTransport[species]["surface"][0].get<double>());
                    } else {
                        EXPECT_EQ(row["name"], "Dye");
                        EXPECT_GT(row["unsaturated"][0].get<double>(), initialTransport[species]["unsaturated"][0].get<double>());
                    }
                }
            }
        }
        if (coupled) {
            int cells = 0;
            ASSERT_EQ(swmm_2d_cell_count(native.value, &cells), 0);
            std::vector<double> volume(cells);
            ASSERT_EQ(swmm_2d_get_coupling_volume_bulk(native.value, volume.data()), 0);
            double moved = 0;
            for (double value : volume) moved += std::abs(value);
            EXPECT_GT(moved, 0); // exercise physical 1D/2D exchange, not only two independent models
            std::ofstream(directory / "native-coupling.json") << Json(volume).dump(2);
        }
        if (surface && managed) {
            Json ledger;
            for (const auto& [name, term] : std::vector<std::pair<const char*, int>>{
                    {"initial", SWMM_GW2D_LED_INIT_STORAGE}, {"storage", SWMM_GW2D_LED_STORAGE},
                    {"infiltration", SWMM_GW2D_LED_INFIL_IN}, {"node", SWMM_GW2D_LED_NODE},
                    {"link", SWMM_GW2D_LED_LINK}}) {
                double value = 0;
                ASSERT_EQ(swmm_gw2d_get_ledger(native.value, term, &value), 0);
                ledger[name] = value;
            }
            double residual = 0;
            ASSERT_EQ(swmm_gw2d_get_continuity_error(native.value, &residual), 0);
            ledger["residual_m3"] = residual;
            int cells = 0, layers = 0;
            ASSERT_EQ(swmm_gw2d_get_dimensions(native.value, &cells, &layers), 0);
            ledger["closures"] = Json::array();
            for (int cell = 0; cell < cells; ++cell) {
                double closure = -1;
                ASSERT_EQ(swmm_gw2d_get_cell(native.value, cell, SWMM_GW2D_VAR_CLOSURE, &closure), 0);
                ledger["closures"].push_back(closure);
            }
            std::ofstream(directory / "native-managed-ledger.json") << ledger.dump(2);
            EXPECT_GT(ledger["infiltration"].get<double>(), 0);
            EXPECT_LE(std::abs(residual), 1e-10 * std::max({1.0,
                std::abs(ledger["initial"].get<double>()), std::abs(ledger["storage"].get<double>())}) + 1e-12);
        }
#endif
        ASSERT_EQ(swmm_engine_end(native.value), 0);
        ASSERT_EQ(swmm_engine_report(native.value), 0);
        ASSERT_EQ(swmm_engine_close(native.value), 0);
        guard = 0;
        while (component->status() == Status::Updated && ++guard < 10000)
            component->update();
        ASSERT_EQ(component->status(), Status::Done) << diagnostics(*component);
        component->finish();
        ASSERT_EQ(component->status(), Status::Finished) << diagnostics(*component);
        EXPECT_EQ(bytes(directory / "native/model.out"), bytes(directory / "wrapped/model.out"));
    }
};
}
TEST_F(EngineComponent, ConfiguredMetadataDoesNotCreateRunOutputs) {
    initialize(true);
    EXPECT_FALSE(fs::exists(directory / "wrapped"));
    const auto metadata = Json::parse(read<std::string>(output(*component, "model_metadata")));
    EXPECT_TRUE(metadata.at("runnable"));
    EXPECT_EQ(metadata.at("nodes").size(), 2u);
    EXPECT_EQ(metadata.at("links").size(), 1u);
    EXPECT_EQ(metadata.at("pollutants")[1]["concentration_units"], "ug/L");
    EXPECT_EQ(metadata.at("instance_id"), "network");
    EXPECT_EQ(component->outputs().size(), 17u);
    auto* quantity =
        dynamic_cast<HC::IQuantity*>(output(*component, "epoch_julian_day")->valueDefinition());
    ASSERT_NE(quantity, nullptr);
    EXPECT_EQ(quantity->valueKind(), HC::ValueKind::Intensive);
    EXPECT_DOUBLE_EQ(quantity->unit()->conversionFactorToSI(), 86400);
    EXPECT_FALSE(component->capabilities().contains(HC::Capability::Checkpointing));
}
TEST_F(EngineComponent, AuthoredExtraOutputIsReportedAndRefusedBeforeExecution) {
    std::ofstream(model, std::ios::app) << "\n[FILES]\nSAVE OUTFLOWS \"extra-output.iface\"\n";
    initialize();
    const auto metadata = Json::parse(read<std::string>(output(*component, "model_metadata")));
    EXPECT_FALSE(metadata.at("runnable"));
    EXPECT_FALSE(component->validate().empty());
    component->prepare();
    EXPECT_EQ(component->status(), Status::Failed);
    EXPECT_FALSE(fs::exists(directory / "wrapped"));
    EXPECT_FALSE(fs::exists(directory / "extra-output.iface"));
}
TEST_F(EngineComponent, UncoupledNativeAndWrappedOutputAreByteIdentical) {
    runUncoupledAndCompare();
}
#ifdef OPENSWMM_COMPONENT_2D_RESULTS
namespace {
void surfaceWaterModel(const fs::path& model) {
    auto deck = bytes(model);
    deck.replace(deck.find("HEAT_TRANSPORT ON"), 17, "HEAT_TRANSPORT OFF\nIGNORE_QUALITY YES");
    std::ofstream(model) << deck << R"(
[2D_OPTIONS]
BACKEND CPU
MAX_TIMESTEP 1
RECONSTRUCTION_ORDER 1
REPORT_2D YES
OUTPUT_FILE authored-shared.h5
TRANSPORT_POLLUTANTS NO
TRANSPORT_MSX NO
TRANSPORT_AGE NO
TRANSPORT_TEMPERATURE NO
[2D_VERTICES]
0 0 0
10 0 0
10 10 0
0 10 0
20 0 0
20 10 0
[2D_TRIANGLES]
0 1 2 .03 .1
0 2 3 .03 .2
[2D_QUADS]
1 4 5 2 .035 .15
[2D_BOUNDARY_CONDITIONS]
0 2 SPECIFIED_FLOW .001
)";
}
}
TEST_F(EngineComponent, WaterPhasePacketsRefuseMalformedSpatialAndTemporalValues) {
    NativeEngine native;
    const auto deck = fs::path(SPLIT_WITNESS) / "DrainingSurface/model.inp";
    ASSERT_EQ(swmm_engine_open(native.value, deck.string().c_str(), (directory / "phase.rpt").string().c_str(), nullptr, nullptr), 0);
    ASSERT_EQ(swmm_split_configure(native.value, SWMM_SPLIT_SURFACE),0);
    ASSERT_EQ(swmm_engine_initialize(native.value),0);
    SWMM_SplitFrame raw = nullptr;
    ASSERT_EQ(swmm_split_geometry(native.value,&raw),0);
    std::unique_ptr<void,decltype(&swmm_split_frame_destroy)> geometry(raw,swmm_split_frame_destroy);
    char* text = nullptr;
    ASSERT_EQ(swmm_split_packet_encode(native.value,raw,&text),0);
    std::unique_ptr<char,decltype(&swmm_split_packet_free)> storage(text,swmm_split_packet_free);
    const auto original = Json::parse(text);
    auto refused = [&](const Json& packet) {
        const auto json = packet.dump();
        auto out = raw;
        EXPECT_NE(swmm_split_packet_decode(native.value,json.data(),json.size(),&out),0);
        EXPECT_EQ(out,raw);
    };
    auto changed = original;
    changed["node_ids"][0] = "different";
    refused(changed);
    changed = original;
    changed["coupled"] = {1};
    refused(changed);
    changed = original;
    changed["ponded_area_ft2"][0] = -1;
    refused(changed);
    changed = original;
    changed["start_s"] = 1;
    refused(changed);
    changed = original;
    changed["phase"] = 1;
    changed["sequence"] = 1;
    changed["target"] = 1;
    changed["kind"] = "receipt";
    changed["duration_s"] = .31;
    changed["conductance_ft2_per_s"] = {{original.at("node_count"),1.}};
    refused(changed);
    changed["conductance_ft2_per_s"] = {{0,-.5}};
    refused(changed);
}

TEST_F(EngineComponent, OutfallPacketsValidateReservationColumnsAndNodeKinds) {
    NativeEngine native;
    const auto deck = fs::path(SPLIT_WITNESS) / "OutfallWithdrawal/model.inp";
    ASSERT_EQ(swmm_engine_open(native.value,deck.string().c_str(),(directory / "check.rpt").string().c_str(),nullptr,nullptr),0);
    ASSERT_EQ(swmm_split_configure(native.value,SWMM_SPLIT_SURFACE),0);
    ASSERT_EQ(swmm_engine_initialize(native.value),0);
    SWMM_SplitFrame raw=nullptr;
    ASSERT_EQ(swmm_split_geometry(native.value,&raw),0);
    std::unique_ptr<void,decltype(&swmm_split_frame_destroy)> hold(raw,swmm_split_frame_destroy);
    char* text=nullptr;
    ASSERT_EQ(swmm_split_packet_encode(native.value,raw,&text),0);
    std::unique_ptr<char,decltype(&swmm_split_packet_free)> storage(text,swmm_split_packet_free);
    const auto geometry=Json::parse(text);
    ASSERT_EQ(geometry.at("schema"),"openswmm.node-surface-water/2");
    auto refused=[&](Json packet) {
        const auto text=packet.dump();
        auto result=raw;
        EXPECT_NE(swmm_split_packet_decode(native.value,text.data(),text.size(),&result),0);
        EXPECT_EQ(result,raw);
    };
    auto packet=geometry;
    packet["mapped_outfalls"]={0}; // J is a storage, not an outfall
    refused(packet);
    packet=geometry;
    packet["phase"]=4;
    packet["kind"]="receipt";
    packet["duration_s"]=.31;
    packet["sequence"]=1;
    packet["target"]=1;
    packet["outfall_boundaries"]={{0.,1.,-1.}};
    refused(packet);
    packet["outfall_boundaries"]={{0.,1.1,1.}};
    refused(packet);
    packet["outfall_boundaries"]={};
    refused(packet);
    packet["outfall_boundaries"]={{0.,1.,1.}};
    packet["schema"]="openswmm.node-surface-water/1";
    refused(packet);
}
TEST_F(EngineComponent, OutfallAdvanceRequiresThePreparedRoutingDuration) {
    NativeEngine native;
    const auto deck = fs::path(SPLIT_WITNESS) / "OutfallWithdrawal/model.inp";
    ASSERT_EQ(swmm_engine_open(native.value,deck.string().c_str(),(directory / "clock.rpt").string().c_str(),nullptr,nullptr),0);
    ASSERT_EQ(swmm_split_configure(native.value,SWMM_SPLIT_SURFACE),0);
    ASSERT_EQ(swmm_engine_initialize(native.value),0);
    SWMM_SplitFrame raw=nullptr;
    ASSERT_EQ(swmm_split_geometry(native.value,&raw),0);
    std::unique_ptr<void,decltype(&swmm_split_frame_destroy)> hold(raw,swmm_split_frame_destroy);
    char* text=nullptr;
    ASSERT_EQ(swmm_split_packet_encode(native.value,raw,&text),0);
    std::unique_ptr<char,decltype(&swmm_split_packet_free)> storage(text,swmm_split_packet_free);
    auto request=Json::parse(text);
    request["target"]=request["sender"];
    request["sender"]=uint64_t(1000000);
    request["sequence"]=1;
    request["phase"]=4;
    request["kind"]="request";
    request["duration_s"]=.31;
    request["remaining_s"]=0.;
    request["flush_on_report"]=false;
    const auto n=swmm_node_count(native.value);
    for (const char* column : {"head_ft","depth_ft","volume_ft3","rim_volume_ft3",
                              "queued_ft3","direct_ft3","node_inflow_cfs","node_outflow_cfs"})
        request[column]=std::vector<double>(n,0);
    ASSERT_EQ(swmm_engine_start(native.value,0),0);
    auto evaluate = [&](const Json& packet, int expected) {
        const auto value=packet.dump();
        SWMM_SplitFrame in=nullptr, out=nullptr;
        ASSERT_EQ(swmm_split_packet_decode(native.value,value.data(),value.size(),&in),0);
        std::unique_ptr<void,decltype(&swmm_split_frame_destroy)> input(in,swmm_split_frame_destroy);
        EXPECT_EQ(swmm_split_evaluate(native.value,packet.at("phase").get<int>(),in,&out),expected);
        swmm_split_frame_destroy(out);
    };
    ASSERT_NO_FATAL_FAILURE(evaluate(request,0)); // reserves water but leaves the clock fixed
    request["sequence"]=2;
    request["phase"]=2;
    request["duration_s"]=.62;
    ASSERT_NO_FATAL_FAILURE(evaluate(request,SWMM_ERR_BADPARAM));
    double clock=-1;
    ASSERT_EQ(swmm_engine_get_elapsed_seconds(native.value,&clock),0);
    EXPECT_EQ(clock,0);
    double stored=0, drained=0;
    ASSERT_EQ(swmm_2d_get_mass_balance(native.value,nullptr,&stored,nullptr,nullptr,nullptr,nullptr,&drained,nullptr,nullptr,nullptr),0);
    EXPECT_EQ(stored,1.1);
    EXPECT_EQ(drained,0);
}
TEST_F(EngineComponent, SplitRolesRequireConnectedParticipantBeforePreparingOutput) {
    configuration["role"] = "split_network";
    configuration["model"] = (fs::path(SPLIT_WITNESS) / "DrainingSurface/model.inp").string();
    initialize();
    EXPECT_FALSE(component->validate().empty());
    EXPECT_FALSE(fs::exists(directory / "wrapped"));
}
TEST_F(EngineComponent, WatershedPacketsValidateWeatherOwnershipAndJournal) {
    NativeEngine native;
    const auto deck=fs::path(SPLIT_WITNESS) / "WatershedRainfall/model.inp";
    ASSERT_EQ(swmm_engine_open(native.value,deck.string().c_str(),(directory / "weather.rpt").string().c_str(),nullptr,nullptr),0);
    ASSERT_EQ(swmm_split_configure(native.value,SWMM_SPLIT_SURFACE),0);
    ASSERT_EQ(swmm_engine_initialize(native.value),0);
    SWMM_SplitFrame raw=nullptr;
    ASSERT_EQ(swmm_split_geometry(native.value,&raw),0);
    std::unique_ptr<void,decltype(&swmm_split_frame_destroy)> hold(raw,swmm_split_frame_destroy);
    char* text=nullptr;
    ASSERT_EQ(swmm_split_packet_encode(native.value,raw,&text),0);
    std::unique_ptr<char,decltype(&swmm_split_packet_free)> storage(text,swmm_split_packet_free);
    const auto geometry=Json::parse(text);
    ASSERT_EQ(geometry.at("schema"),"openswmm.node-surface-water/3");
    auto refused=[&](const Json& packet) {
        const auto encoded=packet.dump();
        auto result=raw;
        EXPECT_NE(swmm_split_packet_decode(native.value,encoded.data(),encoded.size(),&result),0);
        EXPECT_EQ(result,raw);
    };
    auto packet=geometry;
    packet["schema"]="openswmm.node-surface-water/1";
    refused(packet);
    packet=geometry; packet["source_count"]=2; refused(packet);
    packet=geometry; packet["gage_ids"]={"G2","G"}; refused(packet);
    packet=geometry;
    packet["target"]=packet["sender"];
    packet["sender"]=uint64_t(1000000);
    packet["sequence"]=1;
    packet["phase"]=2;
    packet["kind"]="request";
    packet["duration_s"]=.31;
    packet["remaining_s"]=0.;
    packet["flush_on_report"]=false;
    for (const char* column : {"head_ft","depth_ft","volume_ft3","rim_volume_ft3","queued_ft3","direct_ft3"})
        packet[column]=std::vector<double>(swmm_node_count(native.value),0);
    const Json sample={{"start_s",0.},{"rates",{60.,0.}}};
    packet["weather"]={{"gage_rates",{60.,0.}},{"gage_current",{0,0}},{"evap_ft_per_s",0.},{"samples",{sample}}};
    auto changed=packet;
    changed["weather"]["samples"][0]["rates"]={60.}; refused(changed);
    changed=packet; changed["weather"]["samples"][0]["rates"][0]=-1.; refused(changed);
    changed=packet; changed["weather"]["samples"][0]["start_s"]=1.; refused(changed);
    changed=packet; changed["weather"]["samples"].push_back(sample); refused(changed);
    changed=packet; changed.erase("weather"); refused(changed);
    changed=packet; changed["weather"]["gage_current"]={1000000,0}; refused(changed);
    ASSERT_EQ(swmm_engine_start(native.value,0),0);
    auto evaluate=[&](const Json& request,int expected) {
        const auto encoded=request.dump();
        SWMM_SplitFrame in=nullptr,out=nullptr;
        ASSERT_EQ(swmm_split_packet_decode(native.value,encoded.data(),encoded.size(),&in),0);
        std::unique_ptr<void,decltype(&swmm_split_frame_destroy)> input(in,swmm_split_frame_destroy);
        EXPECT_EQ(swmm_split_evaluate(native.value,2,in,&out),expected) << swmm_get_last_error_msg(native.value);
        swmm_split_frame_destroy(out);
    };
    ASSERT_NO_FATAL_FAILURE(evaluate(packet,0));
    packet["start_s"]=.31;
    packet["sequence"]=2;
    ASSERT_NO_FATAL_FAILURE(evaluate(packet,SWMM_ERR_BADPARAM)); // old journal in a fresh phase
    double elapsed=0;
    ASSERT_EQ(swmm_engine_get_elapsed_seconds(native.value,&elapsed),0);
    EXPECT_EQ(elapsed,.31);
}
TEST_F(EngineComponent, GroundwaterPacketsValidateBudgetsAndConduitIdentity) {
    NativeEngine native;
    const auto deck=fs::path(SPLIT_WITNESS) / "GroundwaterLinkGain/model.inp";
    ASSERT_EQ(swmm_engine_open(native.value,deck.string().c_str(),(directory / "ground.rpt").string().c_str(),nullptr,nullptr),0);
    ASSERT_EQ(swmm_split_configure(native.value,SWMM_SPLIT_SURFACE),0);
    ASSERT_EQ(swmm_engine_initialize(native.value),0);
    SWMM_SplitFrame raw=nullptr;
    ASSERT_EQ(swmm_split_geometry(native.value,&raw),0);
    std::unique_ptr<void,decltype(&swmm_split_frame_destroy)> hold(raw,swmm_split_frame_destroy);
    char* text=nullptr;
    ASSERT_EQ(swmm_split_packet_encode(native.value,raw,&text),0);
    std::unique_ptr<char,decltype(&swmm_split_packet_free)> storage(text,swmm_split_packet_free);
    const auto geometry=Json::parse(text);
    ASSERT_EQ(geometry.at("schema"),"openswmm.node-surface-water/4");
    auto decode=[&](const Json& packet,bool accepted) {
        const auto encoded=packet.dump();
        auto result=raw;
        const auto rc=swmm_split_packet_decode(native.value,encoded.data(),encoded.size(),&result);
        if (accepted) {
            EXPECT_EQ(rc,0) << swmm_get_last_error_msg(native.value);
            if (!rc) swmm_split_frame_destroy(result);
        } else {
            EXPECT_NE(rc,0);
            EXPECT_EQ(result,raw);
        }
    };
    decode(geometry,true);
    auto changed=geometry;
    changed["schema"]="openswmm.node-surface-water/2"; decode(changed,false);
    changed=geometry; changed["conduit_count"]=2; decode(changed,false);
    changed=geometry; changed["conduit_links"]={1}; decode(changed,false);
    changed=geometry; changed["link_ids"]={"other"}; decode(changed,false);
    changed=geometry; changed["aquifer_bed"]={1}; decode(changed,false);
    changed=geometry; changed["groundwater_conduits"]["gain_limit_cfs"]={-1.}; decode(changed,false);
    changed=geometry; changed["groundwater_conduits"]["path_ft"]={0.}; decode(changed,false);
    changed=geometry; changed["groundwater_conduits"]["coupled"]={2}; decode(changed,false);
    changed=geometry; changed["groundwater_ledger_m3"]={0.}; decode(changed,false);
    changed=geometry;
    changed["phase"]=2; changed["kind"]="request"; changed["sequence"]=1;
    changed["target"]=changed["sender"]; changed["sender"]=uint64_t(1000000);
    changed["duration_s"]=.31; changed["remaining_s"]=0.; changed["flush_on_report"]=false;
    for (const char* name : {"head_ft","depth_ft","volume_ft3","rim_volume_ft3","queued_ft3",
                             "direct_ft3","node_inflow_cfs","node_outflow_cfs"})
        changed[name]=std::vector<double>(swmm_node_count(native.value),0.);
    changed["conduit_seepage_cfs"]={-.1};
    decode(changed,true); // accepted signed native rate, one barrel
    changed["conduit_seepage_cfs"]={}; decode(changed,false);
}
TEST_F(EngineComponent, ReviewedSourcePacketsValidateOwnersAndInventories) {
    NativeEngine native;
    const auto deck=fs::path(SPLIT_WITNESS) / "ReviewedFootprintSources/model.inp";
    ASSERT_EQ(swmm_engine_open(native.value,deck.string().c_str(),(directory/"sources.rpt").string().c_str(),nullptr,nullptr),0);
    ASSERT_EQ(swmm_split_configure(native.value,SWMM_SPLIT_SURFACE),0);
    ASSERT_EQ(swmm_2d_set_results_file(native.value,(directory/"sources.h5").string().c_str()),0);
    ASSERT_EQ(swmm_engine_initialize(native.value),0);
    SWMM_SplitFrame raw=nullptr;
    ASSERT_EQ(swmm_split_geometry(native.value,&raw),0);
    std::unique_ptr<void,decltype(&swmm_split_frame_destroy)> hold(raw,swmm_split_frame_destroy);
    char* text=nullptr;
    ASSERT_EQ(swmm_split_packet_encode(native.value,raw,&text),0);
    std::unique_ptr<char,decltype(&swmm_split_packet_free)> storage(text,swmm_split_packet_free);
    const auto geometry=Json::parse(text);
    ASSERT_EQ(geometry.at("schema"),"openswmm.node-surface-water/5");
    auto check=[&](const Json& packet,bool accept) {
        const auto encoded=packet.dump();
        auto decoded=raw;
        const auto rc=swmm_split_packet_decode(native.value,encoded.data(),encoded.size(),&decoded);
        if (accept) { EXPECT_EQ(rc,0) << swmm_get_last_error_msg(native.value); if (!rc) swmm_split_frame_destroy(decoded); }
        else { EXPECT_NE(rc,0); EXPECT_EQ(decoded,raw); }
    };
    check(geometry,true);
    auto changed=geometry;
    changed["schema"]="openswmm.node-surface-water/4"; check(changed,false);
    changed=geometry; changed["source_groups"]={-1,-1}; check(changed,false);
    changed=geometry; changed["source_groups"]={0}; check(changed,false);
    changed=geometry; changed["source_groups"]={0,2}; check(changed,false);
    changed=geometry; changed["subcatchment_ids"]={"B","A"}; check(changed,false);
    changed=geometry; changed["reviewed_sources"]["pending_drain_ft3"]={-1.,0.}; check(changed,false);
    changed=geometry; changed["reviewed_sources"]["held_rate_cfs"]={0.}; check(changed,false);
    changed=geometry; changed["reviewed_sources"]["totals_ft3"]={0.}; check(changed,false);
    changed=geometry; changed["reviewed_sources"]["storage_ft3"]=-1.; check(changed,false);
}
TEST_F(EngineComponent, ReviewedSourcesRefuseMissingMeshResultsBeforePreparingFiles) {
    configuration["model"]=(fs::path(SPLIT_WITNESS)/"ReviewedFootprintSources/model.inp").string();
    configuration["role"]="integrated2d"; configuration["surface_results"]=false;
    initialize();
    EXPECT_FALSE(component->validate().empty());
    component->prepare();
    EXPECT_EQ(component->status(),Status::Failed);
    EXPECT_FALSE(fs::exists(directory/"wrapped"));
}
TEST_F(EngineComponent, JointRichardsPacketsValidateTrialColumnsAndOwnerCohort) {
    NativeEngine net,surface;
    const auto deck=fs::path(SPLIT_WITNESS)/"JointRichardsCompeting/model.inp";
    ASSERT_EQ(swmm_engine_open(net.value,deck.string().c_str(),(directory/"net.rpt").string().c_str(),nullptr,nullptr),0);
    ASSERT_EQ(swmm_engine_open(surface.value,deck.string().c_str(),(directory/"surface.rpt").string().c_str(),nullptr,nullptr),0);
    ASSERT_EQ(swmm_split_configure(net.value,SWMM_SPLIT_NETWORK),0);
    ASSERT_EQ(swmm_split_configure(surface.value,SWMM_SPLIT_SURFACE),0);
    fs::remove(directory/"surface.h5"); // This test owns the writer destination across reruns.
    ASSERT_EQ(swmm_2d_set_results_file(surface.value,(directory/"surface.h5").string().c_str()),0);
    ASSERT_EQ(swmm_engine_initialize(surface.value),0);
    ASSERT_EQ(swmm_engine_initialize(net.value),0);
    SWMM_SplitFrame geometry=nullptr;
    ASSERT_EQ(swmm_split_geometry(surface.value,&geometry),0);
    ASSERT_EQ(swmm_split_apply_geometry(net.value,geometry),0);
    swmm_split_frame_destroy(geometry);
    struct Check { SWMM_Engine surface; bool checked=false; } check{surface.value};
    const auto callback=[](SWMM_Engine engine,int phase,SWMM_SplitFrame request,SWMM_SplitFrame* reply,void* user) -> int {
        auto& check=*static_cast<Check*>(user);
        const int rc=swmm_split_evaluate(check.surface,phase,request,reply);
        if (rc || phase != SWMM_SPLIT_LID_TRIAL || check.checked) return rc;
        check.checked=true;
        char* text=nullptr;
        EXPECT_EQ(swmm_split_packet_encode(engine,*reply,&text),0);
        if (!text) return SWMM_ERR_BADPARAM;
        const auto packet=Json::parse(text); swmm_split_packet_free(text);
        EXPECT_EQ(packet.at("schema"),"openswmm.node-surface-water/6");
        auto decode=[&](const Json& item,bool valid) {
            const auto encoded=item.dump(); auto decoded=*reply;
            const int result=swmm_split_packet_decode(engine,encoded.data(),encoded.size(),&decoded);
            if (valid) { EXPECT_EQ(result,0) << swmm_get_last_error_msg(engine); if (!result) swmm_split_frame_destroy(decoded); }
            else { EXPECT_NE(result,0); EXPECT_EQ(decoded,*reply); }
        };
        decode(packet,true);
        auto changed=packet; changed["schema"]="openswmm.node-surface-water/5"; decode(changed,false);
        changed=packet; changed["lid_nodes"]={0}; decode(changed,false);
        changed=packet; changed["richards_nodes"].erase(0); decode(changed,false);
        changed=packet; changed["richards_nodes"][0]["node"]=999; decode(changed,false);
        changed=packet; changed["richards_nodes"][0]["state"]["richards_water"]={0.}; decode(changed,false);
        changed=packet; changed["richards_nodes"][0]["state"]["richards_water"][1]=-1.; decode(changed,false);
        changed=packet; changed["richards_nodes"][0]["state"]["richards_pressure"][1]=nullptr; decode(changed,false);
        changed=packet; changed["richards_nodes"][0]["theta"]={0.}; decode(changed,false);
        changed=packet; changed["richards_nodes"][0]["hydraulics"][2]=-1.; decode(changed,false);
        changed=packet; changed["richards_nodes"][0]["state"]["richards_report"]["ok"]=false; decode(changed,false);
        changed=packet; changed["richards_nodes"][0]["state"]["richards_mesh_pending_finish"]=false; decode(changed,false);
        changed=packet; changed["richards_nodes"][0]["state"]["richards_mesh_routed"]=true; decode(changed,false);
        return SWMM_OK;
    };
    ASSERT_EQ(swmm_split_set_callback(net.value,callback,&check),0);
    ASSERT_EQ(swmm_engine_start(surface.value,0),0);
    ASSERT_EQ(swmm_engine_start(net.value,0),0);
    double elapsed=0;
    ASSERT_EQ(swmm_engine_step(net.value,&elapsed),0) << swmm_get_last_error_msg(surface.value);
    EXPECT_TRUE(check.checked);
    ASSERT_EQ(swmm_engine_end(net.value),0);
}
TEST_F(EngineComponent, ReviewedReturnPacketsRefuseInvalidInventoryBeforeSurfaceAdvancement) {
    NativeEngine net,surface;
    const auto deck=fs::path(SPLIT_WITNESS)/"ReviewedReturnOrdinary/model.inp";
    ASSERT_EQ(swmm_engine_open(net.value,deck.string().c_str(),(directory/"net.rpt").string().c_str(),nullptr,nullptr),0);
    ASSERT_EQ(swmm_engine_open(surface.value,deck.string().c_str(),(directory/"surface.rpt").string().c_str(),nullptr,nullptr),0);
    ASSERT_EQ(swmm_split_configure(net.value,SWMM_SPLIT_NETWORK),0);
    ASSERT_EQ(swmm_split_configure(surface.value,SWMM_SPLIT_SURFACE),0);
    fs::remove(directory/"surface.h5");
    ASSERT_EQ(swmm_2d_set_results_file(surface.value,(directory/"surface.h5").string().c_str()),0);
    ASSERT_EQ(swmm_engine_initialize(surface.value),0);
    ASSERT_EQ(swmm_engine_initialize(net.value),0);
    SWMM_SplitFrame geometry=nullptr;
    ASSERT_EQ(swmm_split_geometry(surface.value,&geometry),0);
    char* text=nullptr;
    ASSERT_EQ(swmm_split_packet_encode(surface.value,geometry,&text),0);
    const auto original=Json::parse(text); swmm_split_packet_free(text);
    ASSERT_EQ(original["schema"],"openswmm.node-surface-water/7");
    auto decode=[&](const Json& packet) {
        const auto encoded=packet.dump(); auto sentinel=geometry;
        EXPECT_NE(swmm_split_packet_decode(net.value,encoded.data(),encoded.size(),&sentinel),0);
        EXPECT_EQ(sentinel,geometry);
    };
    auto changed=original; changed["schema"]="openswmm.node-surface-water/5"; decode(changed);
    changed=original; changed["outfall_return_targets"]={-1}; decode(changed);
    ASSERT_EQ(swmm_split_apply_geometry(net.value,geometry),0); swmm_split_frame_destroy(geometry);
    struct Check { SWMM_Engine surface; bool checked=false; } check{surface.value};
    const auto callback=[](SWMM_Engine engine,int phase,SWMM_SplitFrame request,SWMM_SplitFrame* reply,void* user) -> int {
        auto& check=*static_cast<Check*>(user);
        if (phase != SWMM_SPLIT_ADVANCE) return swmm_split_evaluate(check.surface,phase,request,reply);
        check.checked=true;
        char* text=nullptr;
        EXPECT_EQ(swmm_split_packet_encode(engine,request,&text),0);
        if (!text) return SWMM_ERR_BADPARAM;
        const auto packet=Json::parse(text); swmm_split_packet_free(text);
        for (const auto& volumes : {Json::array({0.}),Json::array({-1.,0.,0.}),Json::array({nullptr,0.,0.})}) {
            auto bad=packet; bad["outfall_returns_ft3"]=volumes;
            const auto encoded=bad.dump(); auto sentinel=request;
            EXPECT_NE(swmm_split_packet_decode(check.surface,encoded.data(),encoded.size(),&sentinel),0);
            EXPECT_EQ(sentinel,request);
        }
        auto bad=packet;
        const int unowned=swmm_subcatch_index(engine,"CATCH");
        EXPECT_GE(unowned,0); if (unowned < 0) return SWMM_ERR_BADPARAM;
        bad["outfall_returns_ft3"][unowned]=1.;
        const auto encoded=bad.dump(); SWMM_SplitFrame tampered=nullptr;
        const auto rc=swmm_split_packet_decode(check.surface,encoded.data(),encoded.size(),&tampered);
        EXPECT_EQ(rc,0); if (rc) return rc;
        const auto refused=swmm_split_evaluate(check.surface,phase,tampered,reply);
        swmm_split_frame_destroy(tampered); EXPECT_NE(refused,0);
        return refused;
    };
    ASSERT_EQ(swmm_split_set_callback(net.value,callback,&check),0);
    ASSERT_EQ(swmm_engine_start(surface.value,0),0);
    ASSERT_EQ(swmm_engine_start(net.value,0),0);
    double elapsed=0,time=-1;
    EXPECT_NE(swmm_engine_step(net.value,&elapsed),0);
    EXPECT_TRUE(check.checked);
    ASSERT_EQ(swmm_engine_get_elapsed_seconds(surface.value,&time),0); EXPECT_EQ(time,0);
    ASSERT_EQ(swmm_engine_get_elapsed_seconds(net.value,&time),0); EXPECT_EQ(time,0);
}
TEST_F(EngineComponent, SplitWaterComponentsMatchIndependentNativeRuns) {
    const fs::path cases = SPLIT_WITNESS;
    for (const char* name : {"DrainingSurface", "SpillingNetwork", "PerStepExchange",
                             "VariableRouting", "DrySurface", "CompetingWithdrawals",
                             "BinaryOnlyRetainsReportFlushes", "EarlyFinishFlushesPendingBatch",
                             "OutfallDischarge", "OutfallWithdrawal", "OutfallFlapGate", "OutfallOverlap",
                             "OutfallSpreadRelaxation", "OutfallMixedNodeExchange", "OutfallBinaryOnly", "OutfallEarlyFinish",
                             "WatershedRainfall", "WatershedOutfall", "ConventionalLidOutlets", "ConventionalLidOverflow", "ConventionalLidCascade",
                             "WatershedBinaryOnly", "WatershedEarlyFinish",
                             "GroundwaterNodeGain", "GroundwaterNodeLoss", "GroundwaterIndependent", "GroundwaterEnslaved", "GroundwaterSigma", "GroundwaterLinkGain", "GroundwaterLinkLoss", "GroundwaterCompetingLinks", "GroundwaterOneWayLoss", "GroundwaterUSUnits", "GroundwaterBinaryOnly", "GroundwaterEarlyFinish", "GroundwaterWatershed",
                             "ReviewedFootprintSources", "ReviewedFootprintIndependent", "ReviewedFootprintEnslaved", "ReviewedFootprintSigma", "ReviewedFootprintEvaporation", "ReviewedFootprintEarlyFinish",
                             "JointRichardsClosedShared", "JointRichardsClosedIndependent", "JointRichardsEnslavedShared", "JointRichardsEnslavedIndependent", "JointRichardsSigmaShared", "JointRichardsSigmaIndependent", "JointRichardsCompeting", "JointRichardsAquiferGain", "JointRichardsEarlyFinish",
                             "ReviewedMappedDischarge", "ReviewedMappedWithdrawal", "ReviewedMappedFlapGate", "ReviewedMappedRichards", "ReviewedReturnClosedShared", "ReviewedReturnIndependent", "ReviewedReturnEnslaved", "ReviewedReturnSigma", "ReviewedReturnRichards", "ReviewedReturnEarlyFinish", "ReviewedReturnMixedMapped", "ReviewedReturnOrdinary",
                             "ClassicalMeshReviewedShared", "ClassicalMeshReviewedIndependent", "ClassicalMeshReviewedEnslaved", "ClassicalMeshReviewedSigma", "ClassicalMeshReviewedGain", "ClassicalMeshOrdinarySources", "ClassicalMeshConduit", "ClassicalSealedReviewed", "ClassicalCurrentReviewed", "ClassicalMixedRichards", "ClassicalMixedRichardsSigma", "ClassicalEarlyFinish"}) {
        SCOPED_TRACE(name);
        const auto work = directory / "wrapped" / name;
        const auto witness = cases / name;
        auto network = library.create();
        auto surface = library.create();
        ASSERT_TRUE(network && surface);
        const bool mesh = std::string_view(name) != "BinaryOnlyRetainsReportFlushes" && std::string_view(name) != "OutfallBinaryOnly" && std::string_view(name) != "WatershedBinaryOnly" && std::string_view(name) != "GroundwaterBinaryOnly";
        const bool early = std::string_view(name) == "EarlyFinishFlushesPendingBatch" || std::string_view(name) == "OutfallEarlyFinish" || std::string_view(name) == "WatershedEarlyFinish" || std::string_view(name) == "GroundwaterEarlyFinish" || std::string_view(name) == "ReviewedFootprintEarlyFinish" || std::string_view(name) == "JointRichardsEarlyFinish" || std::string_view(name) == "ReviewedReturnEarlyFinish" || std::string_view(name) == "ClassicalEarlyFinish";
        Json netConfig = {{"schema","openswmm.component/1"},{"role","split_network"},
                          {"model",(witness / "model.inp").string()},
                          {"output_directory",(work / "network").string()}};
        auto surfaceConfig = netConfig;
        surfaceConfig["role"] = "integrated2d";
        surfaceConfig["surface_results"] = mesh;
        surfaceConfig["output_directory"] = (work / "surface").string();
        argument(*network,"hydrocouple.instance_id",{{"values",{"network"}}});
        argument(*surface,"hydrocouple.instance_id",{{"values",{"surface"}}});
        argument(*network,"configuration",netConfig);
        argument(*surface,"configuration",surfaceConfig);
        network->initialize();
        surface->initialize();
        ASSERT_EQ(network->status(),Status::Initialized) << diagnostics(*network);
        ASSERT_EQ(surface->status(),Status::Initialized) << diagnostics(*surface);
        ASSERT_EQ(network->inputs().size(),1u);
        auto* provider = output(*surface,"openswmm.node-surface-water.v1");
        ASSERT_NE(provider,nullptr);
        provider->addConsumer(network->inputs().front());
        ASSERT_TRUE(network->validate().empty()) << diagnostics(*network);
        ASSERT_TRUE(surface->validate().empty()) << diagnostics(*surface);
        network->prepare();
        surface->prepare();
        ASSERT_EQ(surface->status(),Status::Updated) << diagnostics(*surface);
        ASSERT_EQ(network->status(),Status::Updated) << diagnostics(*network);
        std::ifstream ledger(witness / "native-ledger.txt");
        ASSERT_TRUE(ledger.is_open());
        std::ifstream ground(witness / "native-groundwater-ledger.txt");
        auto checkLedger = [&] {
            const auto packet = Json::parse(read<std::string>(provider));
            int phase;
            ASSERT_TRUE(ledger >> phase);
            ASSERT_EQ(packet.at("phase"),phase);
            for (const char* key : {"init_storage", "final_storage", "rainfall_in", "coupling_1d_to_2d_in",
                                    "coupling_2d_to_1d_out", "outfall_in", "outfall_out", "boundary_in",
                                    "boundary_out", "evap_out"}) {
                double expected;
                ASSERT_TRUE(ledger >> expected);
                EXPECT_EQ(packet.at("balance").at(key).get<double>(),expected) << key;
            }
            if (ground.is_open()) {
                int gwPhase=-1;
                ASSERT_TRUE(ground >> gwPhase);
                EXPECT_EQ(gwPhase,phase);
                const auto values=packet.at("groundwater_ledger_m3").get<std::vector<double>>();
                ASSERT_EQ(values.size(),16u);
                for (double actual : values) {
                    double expected=0;
                    ASSERT_TRUE(ground >> expected);
                    EXPECT_EQ(expected,actual);
                }
            }
        };
        int sweeps = 0;
        while (network->status() == Status::Updated && ++sweeps < 10000) {
            surface->update();
            network->update();
            ASSERT_NE(network->status(),Status::Failed) << diagnostics(*network);
            ASSERT_NO_FATAL_FAILURE(checkLedger());
            if (early) break;
        }
        ASSERT_LT(sweeps,10000);
        if (!early) ASSERT_EQ(network->status(),Status::Done);
        if (early) {
            surface->finish();
            ASSERT_NO_FATAL_FAILURE(checkLedger());
            network->finish();
        } else {
            network->finish();
            surface->finish();
        }
        std::string extra;
        EXPECT_FALSE(ledger >> extra);
        ASSERT_EQ(network->status(),Status::Finished) << diagnostics(*network);
        ASSERT_EQ(surface->status(),Status::Finished) << diagnostics(*surface);
        ASSERT_TRUE(fs::exists(witness / "native.out"));
        EXPECT_EQ(bytes(work / "network/model.out"),bytes(witness / "native.out"));
        EXPECT_EQ(fs::exists(work / "surface/model.2d.h5"),mesh);
        EXPECT_FALSE(fs::exists(work / "surface/model.out"));
        EXPECT_FALSE(fs::exists(work / "network/model.2d.h5"));
        EXPECT_TRUE(provider->removeConsumer(network->inputs().front()));
    }
}
TEST_F(EngineComponent, ClassicalPacketsValidateFormulationHistoryAndRichardsSubset) {
    NativeEngine net,surface;
    const auto deck=fs::path(SPLIT_WITNESS)/"ClassicalMixedRichards/model.inp";
    ASSERT_EQ(swmm_engine_open(net.value,deck.string().c_str(),(directory/"net.rpt").string().c_str(),nullptr,nullptr),0);
    ASSERT_EQ(swmm_engine_open(surface.value,deck.string().c_str(),(directory/"surface.rpt").string().c_str(),nullptr,nullptr),0);
    ASSERT_EQ(swmm_split_configure(net.value,SWMM_SPLIT_NETWORK),0);
    ASSERT_EQ(swmm_split_configure(surface.value,SWMM_SPLIT_SURFACE),0);
    fs::remove(directory/"surface.h5");
    ASSERT_EQ(swmm_2d_set_results_file(surface.value,(directory/"surface.h5").string().c_str()),0);
    ASSERT_EQ(swmm_engine_initialize(surface.value),0);
    ASSERT_EQ(swmm_engine_initialize(net.value),0);
    SWMM_SplitFrame geometry=nullptr;
    ASSERT_EQ(swmm_split_geometry(surface.value,&geometry),0);
    ASSERT_EQ(swmm_split_apply_geometry(net.value,geometry),0); swmm_split_frame_destroy(geometry);
    struct Check { SWMM_Engine surface; fs::path path; bool checked=false; } check{surface.value,directory};
    const auto callback=[](SWMM_Engine engine,int phase,SWMM_SplitFrame request,SWMM_SplitFrame* reply,void* user) -> int {
        auto& check=*static_cast<Check*>(user);
        const int rc=swmm_split_evaluate(check.surface,phase,request,reply);
        if (rc || phase != SWMM_SPLIT_LID_ACCEPT || check.checked) return rc;
        check.checked=true;
        char* text=nullptr;
        EXPECT_EQ(swmm_split_packet_encode(engine,*reply,&text),0); if (!text) return SWMM_ERR_BADPARAM;
        const auto packet=Json::parse(text); swmm_split_packet_free(text);
        EXPECT_EQ(packet.at("schema"),"openswmm.node-surface-water/8");
        std::ofstream(check.path/"accepted-lid-packet.json") << packet.dump(2);
        int classical=-1;
        for (size_t i=0; i<packet.at("lid_states").size(); ++i)
            if (!packet["lid_states"][i]["richards"].get<bool>()) classical=int(i);
        EXPECT_GE(classical,0); if (classical < 0) return SWMM_ERR_BADPARAM;
        EXPECT_GT(packet["lid_states"][classical]["state"]["captured_flow"].get<double>(),0);
        auto decode=[&](const Json& item,bool valid) {
            const auto encoded=item.dump(); auto decoded=*reply;
            const int result=swmm_split_packet_decode(engine,encoded.data(),encoded.size(),&decoded);
            if (valid) { EXPECT_EQ(result,0) << swmm_get_last_error_msg(engine); if (!result) swmm_split_frame_destroy(decoded); }
            else { EXPECT_NE(result,0); EXPECT_EQ(decoded,*reply); }
        };
        decode(packet,true);
        auto changed=packet; changed["schema"]="openswmm.node-surface-water/7"; decode(changed,false);
        changed=packet; changed["mesh_richards_nodes"]=Json::array(); decode(changed,false);
        changed=packet; changed["lid_states"][classical]["richards"]=true; decode(changed,false);
        changed=packet; changed["lid_states"][classical]["state"]["green_ampt"]["F"]=nullptr; decode(changed,false);
        changed=packet; changed["lid_states"][classical]["state"]["infiltration_cell"]=999; decode(changed,false);
        changed=packet; changed["lid_states"][classical]["theta"]={0.}; decode(changed,false);
        changed=packet; changed["lid_states"][classical]["state"]["richards_water"]={1.}; decode(changed,false);
        return SWMM_OK;
    };
    ASSERT_EQ(swmm_split_set_callback(net.value,callback,&check),0);
    ASSERT_EQ(swmm_engine_start(surface.value,0),0); ASSERT_EQ(swmm_engine_start(net.value,0),0);
    double elapsed=0;
    ASSERT_EQ(swmm_engine_step(net.value,&elapsed),0) << swmm_get_last_error_msg(surface.value);
    EXPECT_TRUE(check.checked);
    ASSERT_EQ(swmm_engine_end(net.value),0);
}
TEST_F(EngineComponent, CompleteSurfaceWaterMatchesNativeWithoutTouchingAuthoredDestination) {
    surfaceWaterModel(model);
    const auto sentinel = directory / "authored-shared.h5";
    std::ofstream(sentinel) << "keep authored destination";
    runUncoupledAndCompare(true);
    EXPECT_EQ(bytes(sentinel), "keep authored destination");
    EXPECT_TRUE(fs::exists(directory / "native/model.2d.h5"));
    EXPECT_TRUE(fs::exists(directory / "wrapped/model.2d.h5"));
    const auto m = Json::parse(read<std::string>(output(*component, "model_metadata")));
    EXPECT_EQ(m["execution_profile"], "native_cpu_surface_water/1");
    EXPECT_EQ(m["result_files"]["surface"], "model.2d.h5");
}

TEST_F(EngineComponent, SurfaceInstancesKeepIndependentResultsWhenAdvancedTogether) {
    surfaceWaterModel(model);
    fs::remove_all(directory / "peer");
    ready();
    auto peer = library.create();
    ASSERT_NE(peer, nullptr);
    peer->setReferenceDirectory(directory.string());
    argument(*peer, "hydrocouple.instance_id", {{"values", {"peer"}}});
    auto config = configuration;
    config["output_directory"] = "peer";
    argument(*peer, "configuration", config);
    peer->initialize();
    ASSERT_EQ(peer->status(), Status::Initialized) << diagnostics(*peer);
    ASSERT_TRUE(peer->validate().empty()) << diagnostics(*peer);
    peer->prepare();
    ASSERT_EQ(peer->status(), Status::Updated) << diagnostics(*peer);
    int guard = 0;
    while ((component->status() == Status::Updated || peer->status() == Status::Updated) &&
           ++guard < 10000) {
        if (component->status() == Status::Updated) component->update();
        if (peer->status() == Status::Updated) peer->update();
    }
    ASSERT_EQ(component->status(), Status::Done) << diagnostics(*component);
    ASSERT_EQ(peer->status(), Status::Done) << diagnostics(*peer);
    component->finish(); peer->finish();
    ASSERT_EQ(component->status(), Status::Finished);
    ASSERT_EQ(peer->status(), Status::Finished);
    EXPECT_EQ(bytes(directory / "wrapped/model.out"), bytes(directory / "peer/model.out"));
    EXPECT_TRUE(fs::exists(directory / "wrapped/model.2d.h5"));
    EXPECT_TRUE(fs::exists(directory / "peer/model.2d.h5"));
    EXPECT_FALSE(fs::exists(directory / "authored-shared.h5"));
}

TEST_F(EngineComponent, CompleteCoupledSurfaceWaterMatchesNativeWithActualExchange) {
    surfaceWaterModel(model);
    std::ofstream(model, std::ios::app) << "\n[STORAGE]\nLake -10 5 2 FUNCTIONAL 0 0 100 0 0\n[2D_TRIANGLE_NODE_MAP]\n0 Lake .6 .01\n";
    runUncoupledAndCompare(true, true);
}

namespace {
class GroundwaterComponent : public EngineComponent {
protected:
    enum class Exchange { Recharge, NodeIn, NodeOut, LinkIn, LinkOut };
    void modelGroundwater(bool independent, Exchange exchange = Exchange::Recharge,
                          const std::string& units = "CMS", bool meshSi = false) {
        const bool link = exchange == Exchange::LinkIn || exchange == Exchange::LinkOut;
        std::ofstream f(model);
        f << "[OPTIONS]\nFLOW_UNITS " << units << R"(
FLOW_ROUTING DYNWAVE
START_DATE 01/01/2026
START_TIME 00:00
END_DATE 01/01/2026
END_TIME 00:10
REPORT_STEP 00:01
ROUTING_STEP 5
IGNORE_QUALITY YES
HEAT_TRANSPORT OFF
[JUNCTIONS]
J -5 6 0 0 0
[OUTFALLS]
O -5.05 FREE NO
[CONDUITS]
C J O 30 .013 0 0
[XSECTIONS]
C CIRCULAR .5 0 0 0 1
[INFLOWS]
J FLOW Inlet FLOW 1 1
[TIMESERIES]
Inlet 0:00 .05
Inlet 0:10 .05
[COORDINATES]
J 1 5
O 9 5
[2D_OPTIONS]
BACKEND CPU
MAX_TIMESTEP 1
RECONSTRUCTION_ORDER 1
REPORT_2D YES
REPORT_2D_VARIABLES ALL
OUTPUT_FILE authored-groundwater.h5
TRANSPORT_POLLUTANTS NO
TRANSPORT_MSX NO
TRANSPORT_AGE NO
TRANSPORT_TEMPERATURE NO
BED_HEAT_MODEL NONE
EVAPORATION NO
[2D_VERTICES]
)";
        if (meshSi) f << ";; UNITS: SI (m)\n";
        f << R"(0 0 0
10 0 0
10 10 0
0 10 0
20 0 0
20 10 0
[2D_TRIANGLES]
0 1 2 .03 .2
0 2 3 .03 .2
[2D_QUADS]
1 4 5 2 .03 .2
[2D_INFILTRATION_DEFAULTS]
* CONSTANT 50 - - - -
[2D_AQUIFER_OPTIONS]
MODE MESH
SOIL_CHAR GARDNER
CLOSURE CLOSED_FORM
NODE_ENROLMENT ROWS
GW_ET NONE
GW_CLOCK )" << (independent ? "INDEPENDENT" : "SHARED") << "\nGW_BASE_STEP .125\nGW_MAX_STEP 8\nLINK_SEEPAGE "
          << (link ? "TWO_WAY" : "NONE") << "\n[2D_AQUIFER]\n* 36 8 .45 .1 2 HG0 "
          << (exchange == Exchange::LinkOut ? 6 : 2) << R"(
[GW_TRANSPORT_OPTIONS]
TRANSPORT_POLLUTANTS NO
TRANSPORT_MSX NO
TRANSPORT_AGE NO
TRANSPORT_TEMPERATURE NO
CONDUCTION NO
THERMAL_MODEL WATER_ONLY
)";
        if (exchange == Exchange::NodeIn || exchange == Exchange::NodeOut) {
            f << "[STORAGE]\nWell -7 8 " << (exchange == Exchange::NodeIn ? 6 : .1)
              << " FUNCTIONAL 0 0 10 0 0\n[2D_AQUIFER_NODE]\nWell 1 KC 360 DC .2 AREA 5\n";
        }
        if (link)
            f << "[LOSSES]\nC 0 0 0 NO 100\n[2D_AQUIFER_LINKS]\nC KC 100 DC .2\n";
    }
    void runGroundwater(bool independent, Exchange exchange = Exchange::Recharge,
                        const std::string& units = "CMS", bool meshSi = false) {
        modelGroundwater(independent, exchange, units, meshSi);
        const auto sentinel = directory / "authored-groundwater.h5";
        std::ofstream(sentinel) << "preserve authored output";
        NativeEngine native;
        ASSERT_NO_FATAL_FAILURE(nativeStart(native, true));
        int active = 0, cells = 0, layers = 0;
        ASSERT_EQ(swmm_gw2d_is_active(native.value, &active), 0);
        ASSERT_EQ(active, 1);
        ASSERT_EQ(swmm_gw2d_get_dimensions(native.value, &cells, &layers), 0);
        ASSERT_EQ(cells, 3);
        ASSERT_GE(layers, 1); // native storage dimension can retain unused sigma slots
        for (int cell = 0; cell < cells; ++cell) {
            double closure = -1;
            ASSERT_EQ(swmm_gw2d_get_cell(native.value, cell, SWMM_GW2D_VAR_CLOSURE, &closure), 0);
            ASSERT_EQ(closure, SWMM_GW2D_CLOSURE_CLOSED_FORM);
        }
        ASSERT_NO_FATAL_FAILURE(ready());
        const auto meta = Json::parse(read<std::string>(output(*component, "model_metadata")));
        EXPECT_EQ(meta["execution_profile"], "native_cpu_integrated_water/1");
        EXPECT_EQ(meta["explicit_groundwater"]["options"]["GW_CLOCK"], independent ? "INDEPENDENT" : "SHARED");
        double elapsed = 0;
        int guard = 0;
        do {
            ASSERT_EQ(swmm_engine_step(native.value, &elapsed), 0) << swmm_get_last_error_msg(native.value);
            component->update();
            ASSERT_NE(component->status(), Status::Failed) << diagnostics(*component);
            ASSERT_EQ(component->status() == Status::Done, elapsed == 0);
        } while (elapsed > 0 && ++guard < 10000);
        ASSERT_LT(guard, 10000);
        Json ledger;
        for (const auto& term : std::vector<std::pair<const char*, int>>{
                 {"initial", SWMM_GW2D_LED_INIT_STORAGE}, {"storage", SWMM_GW2D_LED_STORAGE},
                 {"infiltration", SWMM_GW2D_LED_INFIL_IN}, {"node", SWMM_GW2D_LED_NODE},
                 {"link", SWMM_GW2D_LED_LINK}}) {
            double value = 0;
            ASSERT_EQ(swmm_gw2d_get_ledger(native.value, term.second, &value), 0);
            ledger[term.first] = value;
        }
        double residual = 0;
        ASSERT_EQ(swmm_gw2d_get_continuity_error(native.value, &residual), 0);
        ledger["residual_m3"] = residual;
        const double storage = ledger["storage"], initial = ledger["initial"];
        EXPECT_GT(ledger["infiltration"].get<double>(), 0);
        EXPECT_LE(std::abs(residual), 1e-10 * std::max({1.0, std::abs(storage), std::abs(initial)}) + 1e-12);
        if (exchange == Exchange::NodeIn) EXPECT_LT(ledger["node"].get<double>(), 0);
        if (exchange == Exchange::NodeOut) EXPECT_GT(ledger["node"].get<double>(), 0);
        if (exchange == Exchange::LinkIn) EXPECT_GT(ledger["link"].get<double>(), 0);
        if (exchange == Exchange::LinkOut) EXPECT_LT(ledger["link"].get<double>(), 0);
        std::ofstream(directory / "native-groundwater-ledger.json") << ledger.dump(2);
        ASSERT_EQ(swmm_engine_end(native.value), 0);
        ASSERT_EQ(swmm_engine_report(native.value), 0);
        ASSERT_EQ(swmm_engine_close(native.value), 0);
        component->finish();
        ASSERT_EQ(component->status(), Status::Finished) << diagnostics(*component);
        EXPECT_EQ(bytes(directory / "native/model.out"), bytes(directory / "wrapped/model.out"));
        EXPECT_EQ(bytes(sentinel), "preserve authored output");
        SWMM_Output2D result = nullptr;
        ASSERT_EQ(swmm_output2d_open((directory / "wrapped/model.2d.h5").string().c_str(), &result), 0);
        std::unique_ptr<void, decltype(&swmm_output2d_close)> handle(result, swmm_output2d_close);
        int rank = 0, integer = 0;
        uint64_t shape[8]{}, unlimited = 0;
        ASSERT_EQ(swmm_output2d_shape(result, "Mesh2_face_gw_hg", &rank, shape, &integer, &unlimited), 0);
        ASSERT_EQ(rank, 2);
        ASSERT_GT(shape[0], 1u);
        ASSERT_EQ(shape[1], 3u);
    }
};
}
TEST_F(GroundwaterComponent, GroundwaterSharedRecharge) { runGroundwater(false); }
TEST_F(GroundwaterComponent, GroundwaterIndependentRecharge) { runGroundwater(true); }
TEST_F(GroundwaterComponent, GroundwaterSharedNodeIn) { runGroundwater(false, Exchange::NodeIn); }
TEST_F(GroundwaterComponent, GroundwaterIndependentNodeIn) { runGroundwater(true, Exchange::NodeIn); }
TEST_F(GroundwaterComponent, GroundwaterSharedNodeOut) { runGroundwater(false, Exchange::NodeOut); }
TEST_F(GroundwaterComponent, GroundwaterIndependentNodeOut) { runGroundwater(true, Exchange::NodeOut); }
TEST_F(GroundwaterComponent, GroundwaterSharedLinkIn) { runGroundwater(false, Exchange::LinkIn); }
TEST_F(GroundwaterComponent, GroundwaterIndependentLinkIn) { runGroundwater(true, Exchange::LinkIn); }
TEST_F(GroundwaterComponent, GroundwaterSharedLinkOut) { runGroundwater(false, Exchange::LinkOut); }
TEST_F(GroundwaterComponent, GroundwaterIndependentLinkOut) { runGroundwater(true, Exchange::LinkOut); }
TEST_F(GroundwaterComponent, GroundwaterUsProject) { runGroundwater(false, Exchange::Recharge, "CFS"); }
TEST_F(GroundwaterComponent, GroundwaterUsProjectSiMesh) { runGroundwater(true, Exchange::Recharge, "CFS", true); }
TEST_F(GroundwaterComponent, GroundwaterRowClosureIsNotSilentlyChanged) {
    modelGroundwater(false);
    std::ofstream(model, std::ios::app) << "[2D_AQUIFER]\nCELL 1 36 8 .45 .1 2 CLOSURE SIGMA\n";
    initialize();
    EXPECT_FALSE(component->validate().empty());
    component->prepare();
    EXPECT_EQ(component->status(), Status::Failed);
    EXPECT_FALSE(fs::exists(directory / "wrapped"));
}
TEST_F(GroundwaterComponent, GroundwaterTransportRemainsRefused) {
    modelGroundwater(true);
    std::ofstream(model, std::ios::app) << "[GW_TRANSPORT_OPTIONS]\nTRANSPORT_AGE YES\n";
    initialize();
    EXPECT_FALSE(component->validate().empty());
    component->prepare();
    EXPECT_EQ(component->status(), Status::Failed);
    EXPECT_FALSE(fs::exists(directory / "wrapped"));
}

namespace {
class ManagedComponent : public EngineComponent,
                         public ::testing::WithParamInterface<std::tuple<int, int, bool>> {
protected:
    void managedModel() {
        const auto [closure, lid, independent] = GetParam();
        auto text = bytes("component_profiles/managed_sources.inp");
        if (lid) {
            const std::string from = "SINK 0 100 0";
            text.replace(text.find(from), from.size(), "SINK 0 .325 0");
            text += "[LID_CONTROLS]\nStack NODE\nStack SURFACE 75 .1\n"
                    "Stack MEDIA 150 .45 .2 .08 5 10 75\nStack AGGREGATE 100 .4 100\n"
                    "Stack BOTTOM 0 1\n[LID_NODES]\nSINK Stack .2\n";
            if (lid == 2)
                text += "[LID_RICHARDS]\nStack OPTIONS 4 1e-7 1e-5 30\n"
                        "Stack 2 .03 2 1.6 .5 .0001\nStack 3 .03 2 1.6 .5 .0001\n";
        }
        text += std::string("[2D_AQUIFER_OPTIONS]\nCLOSURE ") +
                (closure == 0 ? "CLOSED_FORM" : closure == 1 ? "ENSLAVED" : "SIGMA") +
                "\nM_LAYERS 4\nGW_CLOCK " + (independent ? "INDEPENDENT" : "SHARED") + "\n";
        std::ofstream(model) << text;
        std::ofstream(directory / "authored-managed.h5") << "preserve managed authoring";
        configuration["managed_session"] = true;
    }
};
}
TEST_P(ManagedComponent, NativeSourceOwners) {
    managedModel();
    ASSERT_NO_FATAL_FAILURE(runUncoupledAndCompare(true, false, true));
    const auto metadata = Json::parse(bytes(directory / "wrapped/metadata.json"));
    EXPECT_EQ(metadata["execution_profile"], "native_cpu_managed_water/1");
    EXPECT_EQ(metadata["surface"]["ownership"]["authored_count"], 2);
    EXPECT_TRUE(metadata["surface"]["ownership"]["valid"].get<bool>());
    EXPECT_FALSE(metadata["surface"]["ownership"]["shares"].empty());
    const auto ledger = Json::parse(bytes(directory / "native-managed-ledger.json"));
    for (const auto& closure : ledger["closures"])
        EXPECT_EQ(closure, std::get<0>(GetParam()));
    EXPECT_EQ(bytes(directory / "authored-managed.h5"), "preserve managed authoring");
}
INSTANTIATE_TEST_SUITE_P(Native, ManagedComponent,
    ::testing::Combine(::testing::Values(0, 1, 2), ::testing::Values(0, 1, 2),
                       ::testing::Bool()));

namespace {
class JointLidComponent : public EngineComponent,
    public ::testing::WithParamInterface<std::tuple<int, bool>> {};
}
TEST_P(JointLidComponent, NativeRichardsAquiferAcceptance) {
    const auto [closure, independent] = GetParam();
    auto text = bytes("component_profiles/managed_sources.inp");
    const std::string from = "SINK 0 100 0";
    text.replace(text.find(from), from.size(), "SINK 0 .325 .024");
    text += "[LID_CONTROLS]\nStack NODE\nStack SURFACE 75 .1\n"
            "Stack MEDIA 150 .45 .2 .08 5 10 75\nStack AGGREGATE 100 .4 100\n"
            "Stack BOTTOM 0 0\n[LID_NODES]\nSINK Stack 1 BOTTOM MESH_AQUIFER\n"
            "[LID_RICHARDS]\nStack OPTIONS 4 1e-7 1e-5 30\n"
            "Stack 2 .03 2 1.6 .5 .0001\nStack 3 .03 2 1.6 .5 .0001\n"
            "[COORDINATES]\nSINK .2 .5\n[2D_AQUIFER_NODE]\nSINK 1 KC 3600 DC .1 AREA 1\n";
    text += std::string("[2D_AQUIFER_OPTIONS]\nCLOSURE ") +
        (closure == 0 ? "CLOSED_FORM" : closure == 1 ? "ENSLAVED" : "SIGMA") +
        "\nM_LAYERS 4\nGW_CLOCK " + (independent ? "INDEPENDENT" : "SHARED") + "\n";
    std::ofstream(model) << text;
    std::ofstream(directory / "authored-managed.h5") << "preserve joint authoring";
    configuration["managed_session"] = true;
    ASSERT_NO_FATAL_FAILURE(runUncoupledAndCompare(true, false, true, false, true));
    EXPECT_EQ(bytes(directory / "authored-managed.h5"), "preserve joint authoring");
}
INSTANTIATE_TEST_SUITE_P(Native, JointLidComponent,
    ::testing::Combine(::testing::Values(0, 1, 2), ::testing::Bool()));

namespace {
class MeshTransportComponent : public EngineComponent,
    public ::testing::WithParamInterface<std::tuple<int, int, bool>> {};
}
TEST_P(MeshTransportComponent, MatchedMeshGroundTransport) {
    const auto [closure, order, independent] = GetParam();
    std::ofstream f(model);
    f << R"([OPTIONS]
FLOW_UNITS CMS
FLOW_ROUTING DYNWAVE
START_DATE 01/01/2026
END_DATE 01/01/2026
END_TIME 00:00:10
ROUTING_STEP .31
REPORT_STEP 00:00:02
IGNORE_QUALITY NO
WATER_AGE ON
HEAT_TRANSPORT YES
[POLLUTANTS]
Dye MG/L 0 0 0 0 NO * 0 0
[2D_OPTIONS]
BACKEND CPU
MAX_TIMESTEP .2
LTS_TIERS 1
EVAPORATION NO
REPORT_2D YES
REPORT_2D_VARIABLES ALL
OUTPUT_FILE authored-managed.h5
TRANSPORT_POLLUTANTS YES
TRANSPORT_MSX NO
TRANSPORT_AGE YES
TRANSPORT_TEMPERATURE YES
HEAT_MODEL COUPLED
BED_HEAT_MODEL GROUND_COLUMN
HEAT_SURFACE_EXCHANGE NO
HEAT_RADIATIVE_EXCHANGE NO
RECONSTRUCTION_ORDER )" << order << R"(
[2D_VERTICES]
0 0 0
2 0 0
2 2 0
0 2 0
[2D_TRIANGLES]
0 1 2 .03 .2
0 2 3 .03 .2
[2D_INFILTRATION_DEFAULTS]
* CONSTANT 50 - - - -
[2D_INITIAL_QUALITY]
* Dye 10
* __WATER_AGE__ 1
* __TEMPERATURE__ 20
[2D_AQUIFER_OPTIONS]
MODE MESH
SOIL_CHAR GARDNER
NODE_ENROLMENT ROWS
LINK_SEEPAGE NONE
GW_ET NONE
M_LAYERS 4
CLOSURE )" << (closure == 0 ? "CLOSED_FORM" : closure == 1 ? "ENSLAVED" : "SIGMA")
      << "\nGW_CLOCK " << (independent ? "INDEPENDENT" : "SHARED") << R"(
GW_BASE_STEP .125
GW_MAX_STEP 8
[2D_AQUIFER]
* 36 2 .45 .1 2 HG0 .5
[GW_TRANSPORT_OPTIONS]
TRANSPORT_POLLUTANTS YES
TRANSPORT_MSX NO
TRANSPORT_AGE YES
TRANSPORT_TEMPERATURE YES
THERMAL_MODEL POROUS_EQUILIBRIUM
CONDUCTION YES
SOIL_ET_HEAT SENSIBLE_ONLY
SURFACE_THERMAL_BC ADIABATIC
DEEP_THERMAL_BC ADIABATIC
[GW_INITIAL_QUALITY]
* SAT Dye 2
* UNSAT Dye 5
* SAT __WATER_AGE__ 0
* UNSAT __WATER_AGE__ 0
* SAT __TEMPERATURE__ 8
* UNSAT __TEMPERATURE__ 8
)";
    f.close();
    std::ofstream(directory / "authored-managed.h5") << "preserve managed transport authoring";
    configuration["managed_session"] = true;
    if (order == 2) {
        // Retain native RK2 heat/independent-clock guards at authoring time.
        initialize();
        EXPECT_FALSE(component->validate().empty());
        component->prepare();
        EXPECT_EQ(component->status(), Status::Failed);
        EXPECT_FALSE(fs::exists(directory / "wrapped"));
        NativeEngine native;
        fs::create_directories(directory / "native");
        fs::remove(directory / "native/model.2d.h5");
        fs::remove(directory / "native/refused.rpt");
        ASSERT_EQ(swmm_engine_open(native.value, model.string().c_str(),
            (directory / "native/refused.rpt").string().c_str(), nullptr, nullptr), 0);
        ASSERT_EQ(swmm_2d_set_results_file(native.value,
            (directory / "native/model.2d.h5").string().c_str()), 0);
        SWMM_CheckpointSessionOptions options{};
        options.struct_size = sizeof(options);
        options.schema_version = 1;
        options.output = SWMM_CHECKPOINT_OUTPUT_MESH;
        EXPECT_NE(swmm_checkpoint_start_session(native.value, &options), 0);
        EXPECT_NE(std::string(swmm_checkpoint_last_error_msg()).find(
            independent ? "GW_CLOCK INDEPENDENT" : "Coupled groundwater RK2"), std::string::npos);
        return;
    }
    ASSERT_NO_FATAL_FAILURE(runUncoupledAndCompare(true, false, true, true));
    const auto metadata = Json::parse(bytes(directory / "wrapped/metadata.json"));
    EXPECT_EQ(metadata["execution_profile"], "native_cpu_managed_transport/1");
    EXPECT_EQ(bytes(directory / "authored-managed.h5"), "preserve managed transport authoring");
}
INSTANTIATE_TEST_SUITE_P(Native, MeshTransportComponent,
    ::testing::Combine(::testing::Values(0, 1, 2), ::testing::Values(1, 2), ::testing::Bool()));

TEST_F(EngineComponent, UnknownConfigurationKeysAreRefused) {
    configuration["mis_spelled_output"] = "somewhere";
    argument(*component, "configuration", configuration);
    component->initialize();
    EXPECT_EQ(component->status(), Status::Failed);
    EXPECT_FALSE(fs::exists(directory / "wrapped"));
}

TEST_F(EngineComponent, SurfaceTransportRemainsRefusedBeforeCreatingRunOutputs) {
    surfaceWaterModel(model);
    std::ofstream(model, std::ios::app) << "\n[2D_OPTIONS]\nTRANSPORT_AGE YES\n";
    initialize();
    EXPECT_FALSE(component->validate().empty());
    component->prepare();
    EXPECT_EQ(component->status(), Status::Failed);
    EXPECT_FALSE(fs::exists(directory / "wrapped"));
    EXPECT_FALSE(fs::exists(directory / "authored-shared.h5"));
}

TEST_F(EngineComponent, NativeSurfaceResultsBindingAddsWriterWhenModelHasNoOutputSlot) {
    surfaceWaterModel(model);
    auto deck = bytes(model);
    const std::string outputLine = "OUTPUT_FILE authored-shared.h5\n";
    deck.erase(deck.find(outputLine), outputLine.size());
    std::ofstream(model) << deck;
    runUncoupledAndCompare(true);
    EXPECT_TRUE(fs::exists(directory / "native/model.2d.h5"));
    EXPECT_TRUE(fs::exists(directory / "wrapped/model.2d.h5"));
}

TEST_F(EngineComponent, NativeSurfaceResultsBindingRefusesLifecycleAndExistingFiles) {
    surfaceWaterModel(model);
    NativeEngine native;
    const auto destination = directory / "fresh.h5";
    fs::remove(destination);
    EXPECT_EQ(swmm_2d_set_results_file(nullptr, destination.string().c_str()), SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_2d_set_results_file(native.value, destination.string().c_str()), SWMM_ERR_LIFECYCLE);
    ASSERT_EQ(swmm_engine_open_metadata(native.value, model.string().c_str()), 0);
    EXPECT_EQ(swmm_2d_set_results_file(native.value, destination.string().c_str()), SWMM_ERR_LIFECYCLE);
    ASSERT_EQ(swmm_engine_close(native.value), 0);
    ASSERT_EQ(swmm_engine_open(native.value, model.string().c_str(), nullptr, nullptr, nullptr), 0);
    EXPECT_EQ(swmm_2d_set_results_file(native.value, nullptr), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_2d_set_results_file(native.value, ""), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_2d_set_results_file(native.value, "relative.h5"), SWMM_ERR_BADPARAM);
    ASSERT_EQ(swmm_2d_set_results_file(native.value, destination.string().c_str()), 0);
    EXPECT_FALSE(fs::exists(destination));
    std::ofstream(destination) << "existing result";
    ASSERT_EQ(swmm_engine_initialize(native.value), 0) << swmm_get_last_error_msg(native.value);
    EXPECT_EQ(swmm_2d_set_results_file(native.value, destination.string().c_str()), SWMM_ERR_LIFECYCLE);
    EXPECT_NE(swmm_engine_start(native.value, 1), 0);
    EXPECT_EQ(bytes(destination), "existing result");
    EXPECT_FALSE(fs::exists(directory / "authored-shared.h5"));
}
#endif
TEST_F(EngineComponent, QualifiedNetworkRolePreservesNativeOutput) {
    configuration["role"] = "network_only";
    runUncoupledAndCompare();
}
TEST_F(EngineComponent, NativeMsxSpeciesRemainPollutantsInWrappedOutFiles) {
    reaction();
    runUncoupledAndCompare();
    auto handle = swmm_output_open((directory / "wrapped/model.out").string().c_str());
    ASSERT_NE(handle, nullptr);
    const int pollutants = swmm_output_get_pollut_count(handle);
    EXPECT_EQ(pollutants, 5); // TSS, B, native temperature and two reaction species
    EXPECT_STREQ(swmm_output_get_pollut_id(handle, 0), "TSS");
    EXPECT_STREQ(swmm_output_get_pollut_id(handle, 1), "B");
    int x = -1, w = -1;
    for (int i = 0; i < pollutants; ++i) {
        const std::string name = swmm_output_get_pollut_id(handle, i);
        if (name == "X")
            x = i;
        if (name == "W")
            w = i;
    }
    ASSERT_GT(x, 1);
    EXPECT_EQ(w, x + 1);
    EXPECT_EQ(w, pollutants - 1);
    const int period = swmm_output_get_period_count(handle) - 1;
    ASSERT_GE(period, 0);
    float values[32]{};
    int count = 0;
    ASSERT_EQ(swmm_output_get_node_attribute(handle, 0, period, values, &count), 0);
    EXPECT_GT(count, 6 + x);
    EXPECT_EQ(values[6 + x], 0); // zero-volume junction is not a tank reactor
    ASSERT_EQ(swmm_output_get_link_attribute(handle, 0, period, values, &count), 0);
    EXPECT_GT(count, 5 + x);
    EXPECT_NEAR(values[5 + x], 200.0f, 1e-3f);
    swmm_output_close(handle);
    const auto metadata = Json::parse(read<std::string>(output(*component, "model_metadata")));
    EXPECT_EQ(metadata.at("reaction_species")[0]["id"], "X");
    EXPECT_EQ(metadata.at("reaction_species")[1]["kind"], "WALL");
}
TEST_F(EngineComponent, TypedIntervalsMatchAuthoritativeNativeAmounts) {
    NativeEngine native;
    nativeStart(native);
    ready(true);
    SWMM_OutfallExportReceipt previous{}, receipt{};
    double oldMass[2]{}, mass[2]{};
    for (double time : {60., 120., 600.}) {
        double actual = 0;
        ASSERT_EQ(swmm_engine_advance_to(native.value, time, &actual), 0);
        ASSERT_EQ(swmm_node_get_export_receipt(native.value, swmm_node_index(native.value, "O"),
                                               &receipt, mass, 2),
                  0);
        component->update();
        ASSERT_NE(component->status(), Status::Failed) << diagnostics(*component);
        EXPECT_DOUBLE_EQ(read<double>(output(*component, "water_m3")),
                         receipt.exported_m3 - previous.exported_m3);
        EXPECT_DOUBLE_EQ(read<double>(output(*component, "enthalpy_j")),
                         receipt.exported_heat_j - previous.exported_heat_j);
        EXPECT_DOUBLE_EQ(read<double>(output(*component, "species.A")), mass[0] - oldMass[0]);
        EXPECT_DOUBLE_EQ(read<double>(output(*component, "species.B")), mass[1] - oldMass[1]);
        previous = receipt;
        std::copy(mass, mass + 2, oldMass);
    }
    ASSERT_EQ(component->status(), Status::Done);
    component->finish();
    ASSERT_EQ(component->status(), Status::Finished) << diagnostics(*component);
    ASSERT_EQ(swmm_engine_end(native.value), 0);
    ASSERT_EQ(swmm_engine_report(native.value), 0);
    ASSERT_EQ(swmm_engine_close(native.value), 0);
    EXPECT_EQ(bytes(directory / "native/model.out"), bytes(directory / "wrapped/model.out"));
}
TEST_F(EngineComponent, MsxExportIsDiagnosedBeforeNativeAdvancement) {
    reaction();
    initialize(true);
    EXPECT_FALSE(component->validate().empty());
    EXPECT_EQ(component->status(), Status::Invalid);
    EXPECT_FALSE(fs::exists(directory / "wrapped"));
}
TEST_F(EngineComponent, ChangedModelIsRejectedBeforeOutputCreation) {
    initialize();
    ASSERT_TRUE(component->validate().empty());
    std::ofstream(model, std::ios::app) << "\n; changed after configuration\n";
    component->prepare();
    EXPECT_EQ(component->status(), Status::Failed);
    EXPECT_FALSE(fs::exists(directory / "wrapped"));
}
TEST_F(EngineComponent, ChangedReactionConfigurationIsRejectedBeforeOutputCreation) {
    reaction();
    initialize();
    ASSERT_TRUE(component->validate().empty());
    std::ofstream(directory / "model.rxn", std::ios::app)
        << "\n[REACTION_SPECIES]\nBULK CHANGED MG\n";
    component->prepare();
    EXPECT_EQ(component->status(), Status::Failed);
    EXPECT_FALSE(fs::exists(directory / "wrapped"));
    EXPECT_NE(diagnostics(*component).find("Process configuration changed"), std::string::npos);
}
TEST_F(EngineComponent, ExistingOutputDirectoryIsPreserved) {
    ready();
    component->finish();
    const auto before = bytes(directory / "wrapped/model.out");
    auto other = library.create();
    other->setReferenceDirectory(directory.string());
    argument(*other, "configuration", configuration);
    other->initialize();
    ASSERT_TRUE(other->validate().empty());
    other->prepare();
    EXPECT_EQ(other->status(), Status::Failed);
    EXPECT_EQ(bytes(directory / "wrapped/model.out"), before);
}
TEST_F(EngineComponent, SeparateInstancesKeepIndependentRuntimeAndOutputPaths) {
    ready();
    component->update();
    auto other = library.create();
    other->setReferenceDirectory(directory.string());
    fs::remove_all(directory / "second");
    auto config = configuration;
    config["output_directory"] = "second";
    argument(*other, "hydrocouple.instance_id", {{"values", {"second-network"}}});
    argument(*other, "configuration", config);
    other->initialize();
    ASSERT_TRUE(other->validate().empty());
    other->prepare();
    ASSERT_EQ(other->status(), Status::Updated);
    auto* firstTime = dynamic_cast<HC::Temporal::ITimeModelComponent*>(component.get());
    auto* secondTime = dynamic_cast<HC::Temporal::ITimeModelComponent*>(other.get());
    ASSERT_NE(firstTime, nullptr);
    ASSERT_NE(secondTime, nullptr);
    EXPECT_GT(firstTime->currentDateTime()->julianDay(),
              secondTime->currentDateTime()->julianDay());
    other->update();
    EXPECT_DOUBLE_EQ(firstTime->currentDateTime()->julianDay(),
                     secondTime->currentDateTime()->julianDay());
    EXPECT_EQ(other->id(), "second-network");
}
#ifdef ENGINE_RECEIVER_LIBRARY
namespace {
class EngineReceiver : public EngineComponent {
protected:
    Library receiverLibrary{ENGINE_RECEIVER_LIBRARY};
    std::unique_ptr<HC::IModelComponent> receiver;
    /**
     * @brief Initialize, connect and prepare a real interval-transfer receiver fixture.
     *
     * Initializes the provider with transfer outputs, creates a receiver from receiverLibrary and
     * configures reversed B/A species order, expected count three and retained receipts.csv. Connects
     * each receiver input to matching provider output, validates/prepares both, and fatally checks
     * statuses. Assertions can leave partial connections/runtime for fixture cleanup. Requires sole
     * ownership and no prior receiver, matching output IDs, and live components for connection
     * lifetime; no update/transfer is executed here.
     *
     * @param[in] capacity Nonnegative maximum accepted water volume m3 for the receiver config;
     *     negative omits the limit.
     */
    void connectReceiver(double capacity = -1) {
        initialize(true);
        receiver = receiverLibrary.create();
        ASSERT_NE(receiver, nullptr);
        Json config = {
            {"schema", "hydrocouple.interval-transfer/1"},
            {"epoch_julian_day", dynamic_cast<HC::Temporal::ITimeModelComponent*>(component.get())
                                     ->simulationPeriod()
                                     ->julianDay()},
            {"species",
             Json::array({{{"id", "B"}, {"basis", "kg"}}, {{"id", "A"}, {"basis", "kg"}}})},
            {"expected_transfers", 3},
            {"receipt_file", (directory / "receipts.csv").string()}};
        if (capacity >= 0)
            config["max_water_m3"] = capacity;
        argument(*receiver, "config", config);
        receiver->initialize();
        ASSERT_EQ(receiver->status(), Status::Initialized);
        for (auto* input : receiver->inputs()) {
            auto* provider = output(*component, input->id());
            ASSERT_NE(provider, nullptr);
            std::string message;
            ASSERT_TRUE(input->canConsume(provider, message)) << input->id() << ": " << message;
            provider->addConsumer(input);
        }
        ASSERT_TRUE(component->validate().empty()) << diagnostics(*component);
        ASSERT_TRUE(receiver->validate().empty()) << diagnostics(*receiver);
        component->prepare();
        ASSERT_EQ(component->status(), Status::Updated) << diagnostics(*component);
        receiver->prepare();
        ASSERT_EQ(receiver->status(), Status::Updated) << diagnostics(*receiver);
    }
};
}
TEST_F(EngineReceiver, RealReceiverClosesNativeWaterMassAndHeatWithoutDoubleDebit) {
    connectReceiver();
    for (int step = 0; step < 3; ++step) {
        component->update();
        ASSERT_NE(component->status(), Status::Failed) << diagnostics(*component);
        receiver->update();
        ASSERT_NE(receiver->status(), Status::Failed) << diagnostics(*receiver);
    }
    ASSERT_EQ(component->status(), Status::Done);
    ASSERT_EQ(receiver->status(), Status::Done);
    component->finish();
    ASSERT_EQ(component->status(), Status::Finished) << diagnostics(*component);
    for (const auto& field : {"water_m3", "enthalpy_j", "species.A", "species.B"}) {
        const auto exported = read<double>(output(*component, std::string("exported.") + field));
        EXPECT_GT(exported, 0);
        EXPECT_DOUBLE_EQ(exported,
                         read<double>(output(*receiver, std::string("cumulative.") + field)));
        EXPECT_DOUBLE_EQ(read<double>(output(*component, std::string("unreceived.") + field)), 0);
    }
    receiver->finish();
}
TEST_F(EngineReceiver, PortableReferenceIdentitiesHaveIndependentValidatedArguments) {
    auto first = receiverLibrary.create(), second = receiverLibrary.create();
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    auto identity = [](HC::IModelComponent& model) -> HC::IArgument* {
        for (auto* item : model.arguments())
            if (item->id() == "hydrocouple.instance_id") return item;
        return nullptr;
    };
    auto* a = identity(*first);
    auto* b = identity(*second);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(read<std::string>(a), first->id());
    EXPECT_TRUE(a->isValidArgType(HC::IArgument::ArgumentInputType::JSON));
    EXPECT_TRUE(a->isValidArgType(HC::IArgument::ArgumentInputType::String));
    EXPECT_FALSE(a->isValidArgType(HC::IArgument::ArgumentInputType::File));
    std::string saved, message;
    ASSERT_TRUE(a->serialize(HC::IArgument::ArgumentInputType::JSON, saved, message));
    EXPECT_EQ(Json::parse(saved)["values"][0], first->id());
    ASSERT_TRUE(a->initialize(Json{{"values", {"river_a"}}}.dump(),
        HC::IArgument::ArgumentInputType::JSON, message));
    ASSERT_TRUE(a->initialize("river_a", HC::IArgument::ArgumentInputType::String, message));
    EXPECT_TRUE(a->isReadOnly());
    const int64_t one = 1, zero = 0;
    std::string id = "river_λ";
    HC::BufferDescriptor buffer{&id, HC::DataKind::String, 1, &one, nullptr, sizeof(id)};
    ASSERT_TRUE(b->setValuesFrom(buffer, {&zero, 1}, {&one, 1}, &message)) << message;
    EXPECT_EQ(first->id(), "river_a");
    EXPECT_EQ(second->id(), "river_λ");
    EXPECT_EQ(read<std::string>(a), "river_a");
    EXPECT_EQ(read<std::string>(b), "river_λ");
    EXPECT_FALSE(a->initialize(*b, message));
    EXPECT_FALSE(a->initialize(Json{{"values", {"other"}}}.dump(),
        HC::IArgument::ArgumentInputType::JSON, message));
    EXPECT_EQ(first->id(), "river_a");
    argument(*first, "config", {{"schema", "hydrocouple.interval-transfer/1"},
        {"epoch_julian_day", 2461041.5}, {"expected_transfers", 1}, {"species", Json::array()},
        {"receipt_file", (directory / "identity-receipts.csv").string()}});
    first->initialize();
    ASSERT_EQ(first->status(), Status::Initialized) << diagnostics(*first);
    EXPECT_TRUE(a->isReadOnly());
    EXPECT_FALSE(a->setValuesFrom(buffer, {&zero, 1}, {&one, 1}, &message));
    ASSERT_TRUE(a->serialize(HC::IArgument::ArgumentInputType::JSON, saved, message));
    EXPECT_EQ(Json::parse(saved)["values"][0], "river_a");
}
TEST_F(EngineReceiver, MissingReceiptStopsBeforeAnotherNativeAdvance) {
    connectReceiver();
    component->update();
    const auto token = read<std::string>(output(*component, "transfer_id"));
    component->update();
    EXPECT_EQ(component->status(), Status::Failed);
    EXPECT_EQ(read<std::string>(output(*component, "transfer_id")), token);
    EXPECT_FALSE(fs::exists(directory / "wrapped/native_boundary_1.json"));
}
TEST_F(EngineReceiver, RefusedAlreadyDebitedExportRetainsUnreceivedInventoryAndStops) {
    connectReceiver(0);
    component->update();
    receiver->update();
    ASSERT_EQ(receiver->status(), Status::Updated);
    component->update();
    EXPECT_EQ(component->status(), Status::Failed) << diagnostics(*component);
    EXPECT_FALSE(fs::exists(directory / "wrapped/native_boundary_1.json"));
    for (const auto& field : {"water_m3", "enthalpy_j", "species.A", "species.B"})
        EXPECT_DOUBLE_EQ(read<double>(output(*component, std::string("exported.") + field)),
                         read<double>(output(*receiver, std::string("cumulative.") + field)) +
                             read<double>(output(*component, std::string("unreceived.") + field)));
}
TEST_F(EngineReceiver, PartialAcceptanceRecordsBothAcceptedAndUnreceivedAmounts) {
    connectReceiver(.1);
    component->update();
    receiver->update();
    const double accepted = read<double>(output(*receiver, "accepted.water_m3"));
    EXPECT_GT(accepted, 0);
    EXPECT_LT(accepted, read<double>(output(*component, "water_m3")));
    component->update();
    ASSERT_EQ(component->status(), Status::Failed);
    EXPECT_NE(diagnostics(*component).find("refused part"), std::string::npos);
    EXPECT_FALSE(fs::exists(directory / "wrapped/native_boundary_1.json"));
    const auto ledger = Json::parse(bytes(directory / "wrapped/accepted_0.json"));
    for (const auto& field : {"water_m3", "enthalpy_j", "species.A", "species.B"}) {
        const double received = read<double>(output(*receiver, std::string("accepted.") + field));
        const double remaining =
            read<double>(output(*component, std::string("unreceived.") + field));
        EXPECT_GT(remaining, 0);
        EXPECT_DOUBLE_EQ(read<double>(output(*component, std::string("exported.") + field)),
                         received + remaining);
    }
    EXPECT_DOUBLE_EQ(ledger["accepted_amounts"][0].get<double>(), accepted);
    EXPECT_GT(ledger["unreceived_amounts"][0].get<double>(), 0);
}

namespace {
class NativeHosting : public EngineReceiver {
protected:
    Json hosting(double capacity = -1, const std::string& library = ENGINE_RECEIVER_LIBRARY) {
        const auto destination = directory / "hosted";
        fs::remove_all(destination);
        Json config{{"schema", "hydrocouple.interval-transfer/1"},
            {"epoch_julian_day", 2461041.5}, {"expected_transfers", 3},
            {"receipt_file", "receipts.csv"},
            {"species", Json::array({{{"id", "B"}, {"basis", "kg"}}, {{"id", "A"}, {"basis", "kg"}}})}};
        if (capacity >= 0) config["max_water_m3"] = capacity;
        Json connections = Json::array();
        for (const auto* name : {"transfer_id", "epoch_julian_day", "interval_start_s", "interval_end_s",
                                 "water_m3", "enthalpy_j", "species.A", "species.B"})
            connections.push_back({{"output", name}, {"input", name}});
        Json document{{"schema", "openswmm.hosted-component/1"},
            {"profile", "hydrocouple.interval-transfer/1"}, {"output_directory", "hosted"},
            {"arguments", {{"config", config}}}, {"connections", connections},
            {"exports", {{"outfalls", {"O"}}, {"exchange_times_s", {60, 120, 600}}}}};
        std::ofstream(directory / "host.json") << document.dump(2);
        std::ofstream(model, std::ios::app) << "\n[PROCESS_COMPONENTS]\nReceivingReach library=\""
            << library << "\" component=org.hydrocouple.openswmm.test.sink config=host.json\n";
        return document;
    }
};
}
TEST_F(NativeHosting, SameRealReceiverMatchesComposerContractAndNativeInventory) {
    ASSERT_NO_FATAL_FAILURE(connectReceiver());
    for (int interval = 0; interval < 3; ++interval) {
        component->update();
        receiver->update();
        ASSERT_NE(receiver->status(), Status::Failed) << diagnostics(*receiver);
    }
    component->finish();
    receiver->finish();
    ASSERT_EQ(component->status(), Status::Finished);
    std::map<std::string, double> expected;
    for (const auto* field : {"water_m3", "enthalpy_j", "species.A", "species.B"})
        expected[field] = read<double>(output(*receiver, std::string("cumulative.") + field));
    hosting();
    NativeEngine native;
    ASSERT_NO_FATAL_FAILURE(nativeStart(native));
    double elapsed = 0;
    int guard = 0;
    do {
        ASSERT_EQ(swmm_engine_step(native.value, &elapsed), 0) << swmm_get_last_error_msg(native.value);
    } while (elapsed > 0 && ++guard < 10000);
    ASSERT_LT(guard, 10000);
    ASSERT_EQ(swmm_engine_end(native.value), 0) << swmm_get_last_error_msg(native.value);
    ASSERT_EQ(swmm_engine_report(native.value), 0);
    ASSERT_EQ(swmm_engine_close(native.value), 0);
    EXPECT_EQ(bytes(directory / "native/model.out"), bytes(directory / "wrapped/model.out"));
    std::map<std::string, double> accepted;
    for (int interval = 0; interval < 3; ++interval) {
        const auto ledger = Json::parse(bytes(directory / "hosted" / ("transfer_" + std::to_string(interval) + ".json")));
        EXPECT_EQ(ledger["receiver"], "ReceivingReach");
        for (size_t k = 0; k < ledger["fields"].size(); ++k) {
            accepted[ledger["fields"][k].get<std::string>()] += ledger["accepted"][k].get<double>();
            EXPECT_EQ(ledger["pending"][k], 0);
        }
    }
    for (const auto& [field, value] : expected) EXPECT_DOUBLE_EQ(accepted.at(field), value) << field;
    std::ofstream(directory / "native-host-totals.json") << Json(accepted).dump(2);
}
TEST_F(NativeHosting, MissingLibraryIsInspectedWithoutLoadingAndFailsBeforeAdvance) {
    hosting(-1, (directory / "missing-component.so").string());
    NativeEngine inspection;
    ASSERT_EQ(swmm_engine_open_metadata(inspection.value, model.string().c_str()), 0);
    EXPECT_FALSE(fs::exists(directory / "hosted"));
    NativeEngine native;
    EXPECT_NE(swmm_engine_open(native.value, model.string().c_str(),
        (directory / "native.rpt").string().c_str(), (directory / "native.out").string().c_str(), nullptr), 0);
    EXPECT_NE(std::string(swmm_get_last_error_msg(native.value)).find("failed to load"), std::string::npos);
    EXPECT_FALSE(fs::exists(directory / "hosted"));
}
TEST_F(NativeHosting, PartialAcceptanceStopsWithExplicitPendingInventory) {
    hosting(.1);
    NativeEngine native;
    ASSERT_NO_FATAL_FAILURE(nativeStart(native));
    double elapsed = 0;
    int code = 0, guard = 0;
    do { code = swmm_engine_step(native.value, &elapsed); }
    while (!code && elapsed > 0 && ++guard < 10000);
    ASSERT_NE(code, 0);
    double seconds = 0;
    ASSERT_EQ(swmm_engine_get_elapsed_seconds(native.value, &seconds), 0);
    EXPECT_EQ(seconds, 60);
    const auto ledger = Json::parse(bytes(directory / "hosted/transfer_0.json"));
    EXPECT_EQ(ledger["accepted"][0], .1);
    EXPECT_GT(ledger["pending"][0].get<double>(), 0);
    EXPECT_FALSE(fs::exists(directory / "hosted/transfer_1.json"));
    EXPECT_NE(swmm_engine_step(native.value, &elapsed), 0);
    double after = 0;
    ASSERT_EQ(swmm_engine_get_elapsed_seconds(native.value, &after), 0);
    EXPECT_EQ(after, seconds);
}
TEST_F(NativeHosting, CancellationRecordsUnsentNativeExportsAndClosesTheReceiver) {
    hosting();
    NativeEngine native;
    ASSERT_NO_FATAL_FAILURE(nativeStart(native));
    double seconds = 0, elapsed = 0;
    while (seconds < 30) {
        ASSERT_EQ(swmm_engine_step(native.value, &elapsed), 0);
        ASSERT_EQ(swmm_engine_get_elapsed_seconds(native.value, &seconds), 0);
    }
    ASSERT_LT(seconds, 60);
    ASSERT_EQ(swmm_engine_end(native.value), 0) << swmm_get_last_error_msg(native.value);
    const auto cancelled = Json::parse(bytes(directory / "hosted/cancelled.json"));
    EXPECT_EQ(cancelled["status"], "cancelled");
    EXPECT_EQ(cancelled["end_s"], seconds);
    EXPECT_GT(cancelled["pending"][0].get<double>(), 0);
    EXPECT_FALSE(fs::exists(directory / "hosted/transfer_0.json"));
}
TEST_F(NativeHosting, AuthoredArgumentsAndHostConfigurationRoundTripWithoutLoading) {
    hosting(-1, (directory / "unavailable.so").string());
    NativeEngine metadata;
    ASSERT_EQ(swmm_engine_open_metadata(metadata.value, model.string().c_str()), 0);
    ASSERT_EQ(swmm_process_component_argument_count(metadata.value, 0), 2);
    char key[128]{}, value[4096]{};
    ASSERT_EQ(swmm_process_component_argument_get(metadata.value, 0, 0, key, sizeof(key), value, sizeof(value)), 0);
    EXPECT_EQ(std::string(key), "library");
    EXPECT_EQ(std::string(value), (directory / "unavailable.so").string());
    ASSERT_EQ(swmm_process_component_argument_set(metadata.value, 0, "test", "roundtrip"), 0);
    EXPECT_EQ(swmm_process_component_argument_count(metadata.value, 0), 3);
    ASSERT_EQ(swmm_process_component_argument_set(metadata.value, 0, "test", nullptr), 0);
    EXPECT_EQ(swmm_process_component_argument_count(metadata.value, 0), 2);
    EXPECT_NE(swmm_process_component_argument_set(metadata.value, 0, "Config", "wrong"), 0);
    const auto destination = directory / "saved";
    fs::create_directories(destination);
    ASSERT_EQ(swmm_model_write(metadata.value, (destination / "model.inp").string().c_str()), 0)
        << swmm_get_last_error_msg(metadata.value);
    EXPECT_EQ(bytes(destination / "host.json"), bytes(directory / "host.json"));
    NativeEngine reopened;
    ASSERT_EQ(swmm_engine_open_metadata(reopened.value, (destination / "model.inp").string().c_str()), 0);
    ASSERT_EQ(swmm_process_component_argument_count(reopened.value, 0), 2);
    EXPECT_FALSE(fs::exists(directory / "hosted"));
    EXPECT_FALSE(fs::exists(destination / "hosted"));
}
TEST_F(NativeHosting, ASecondReceivingOwnerCannotClaimTheSameNativeExport) {
    hosting();
    std::ofstream(model, std::ios::app) << "OtherReach library=\"" << ENGINE_RECEIVER_LIBRARY
        << "\" component=org.hydrocouple.openswmm.test.sink config=host.json\n";
    NativeEngine native;
    EXPECT_NE(swmm_engine_open(native.value, model.string().c_str(),
        (directory / "native.rpt").string().c_str(), (directory / "native.out").string().c_str(), nullptr), 0);
    EXPECT_NE(std::string(swmm_get_last_error_msg(native.value)).find("multiply owned"), std::string::npos);
    EXPECT_FALSE(fs::exists(directory / "hosted"));
}
TEST_F(NativeHosting, ReceivingInstancesNeedSeparateOutputDirectories) {
    auto config = hosting();
    config["exports"]["outfalls"] = {"Other"};
    std::ofstream(directory / "other.json") << config.dump(2);
    std::ofstream(model, std::ios::app) << "\n[OUTFALLS]\nOther 0 FREE NO\n"
        << "[PROCESS_COMPONENTS]\nOtherReach library=\"" << ENGINE_RECEIVER_LIBRARY
        << "\" component=org.hydrocouple.openswmm.test.sink config=other.json\n";
    NativeEngine native;
    EXPECT_NE(swmm_engine_open(native.value, model.string().c_str(),
        (directory / "native.rpt").string().c_str(), (directory / "native.out").string().c_str(), nullptr), 0);
    EXPECT_NE(std::string(swmm_get_last_error_msg(native.value)).find("separate output"), std::string::npos);
    EXPECT_FALSE(fs::exists(directory / "hosted"));
}
TEST_F(NativeHosting, IncompleteTupleIsRefusedBeforePreparingReceiver) {
    auto config = hosting();
    config["connections"].erase(config["connections"].begin());
    std::ofstream(directory / "host.json") << config.dump(2);
    NativeEngine native;
    EXPECT_NE(swmm_engine_open(native.value, model.string().c_str(),
        (directory / "native.rpt").string().c_str(), (directory / "native.out").string().c_str(), nullptr), 0);
    EXPECT_NE(std::string(swmm_get_last_error_msg(native.value)).find("entire interval tuple"), std::string::npos);
    EXPECT_FALSE(fs::exists(directory / "hosted"));
}
TEST_F(NativeHosting, UnknownArgumentIsRefusedBeforePreparingReceiver) {
    auto config = hosting();
    config["arguments"]["typo"] = 1;
    std::ofstream(directory / "host.json") << config.dump(2);
    NativeEngine native;
    EXPECT_NE(swmm_engine_open(native.value, model.string().c_str(),
        (directory / "native.rpt").string().c_str(), (directory / "native.out").string().c_str(), nullptr), 0);
    EXPECT_NE(std::string(swmm_get_last_error_msg(native.value)).find("unknown argument typo"), std::string::npos);
    EXPECT_FALSE(fs::exists(directory / "hosted"));
}
TEST_F(NativeHosting, ConfigurationChangesAfterOpenAreRefusedBeforeAdvance) {
    auto config = hosting();
    NativeEngine native;
    ASSERT_EQ(swmm_engine_open(native.value, model.string().c_str(),
        (directory / "native.rpt").string().c_str(), (directory / "native.out").string().c_str(), nullptr), 0);
    config["arguments"]["scale"] = {{"values", {2}}};
    std::ofstream(directory / "host.json") << config.dump(2);
    ASSERT_EQ(swmm_engine_initialize(native.value), 0);
    EXPECT_NE(swmm_engine_start(native.value, 1), 0);
    EXPECT_NE(std::string(swmm_get_last_error_msg(native.value)).find("configuration changed"), std::string::npos);
    EXPECT_FALSE(fs::exists(directory / "hosted"));
}
#ifdef OPENSWMM_HAS_GEOPACKAGE
TEST_F(NativeHosting, ForeignArgumentsSurviveGeoPackageAndBackToInp) {
    hosting();
    NativeEngine authored;
    ASSERT_EQ(swmm_engine_open_metadata(authored.value, model.string().c_str()), 0);
    const auto destination = directory / "geopackage";
    fs::create_directories(destination);
    const auto package = destination / "model.gpkg";
    fs::remove(package);
    constexpr auto plugin = "org.hydrocouple.openswmm.plugins.geopackage";
    ASSERT_EQ(swmm_model_write_with_plugin(authored.value, package.string().c_str(), plugin), 0)
        << swmm_get_last_error_msg(authored.value);
    NativeEngine reopened;
    // The existing metadata-only entry point reads native INP. The supported
    // GeoPackage plugin route opens a configured binding without preparing it.
    ASSERT_EQ(swmm_engine_open(reopened.value, package.string().c_str(), nullptr, nullptr, plugin), 0)
        << swmm_get_last_error_msg(reopened.value);
    ASSERT_EQ(swmm_process_component_argument_count(reopened.value, 0), 2);
    char key[64]{}, value[4096]{};
    ASSERT_EQ(swmm_process_component_argument_get(reopened.value, 0, 0, key, sizeof(key), value, sizeof(value)), 0);
    EXPECT_EQ(std::string(key), "library");
    EXPECT_EQ(std::string(value), ENGINE_RECEIVER_LIBRARY);
    const auto back = destination / "model.inp";
    ASSERT_EQ(swmm_model_write(reopened.value, back.string().c_str()), 0);
    NativeEngine inspection;
    ASSERT_EQ(swmm_engine_open_metadata(inspection.value, back.string().c_str()), 0)
        << swmm_get_last_error_msg(inspection.value);
    ASSERT_EQ(swmm_process_component_argument_count(inspection.value, 0), 2);
    ASSERT_EQ(swmm_process_component_argument_get(inspection.value, 0, 1, key, sizeof(key), value, sizeof(value)), 0);
    EXPECT_EQ(std::string(key), "component");
    EXPECT_EQ(std::string(value), "org.hydrocouple.openswmm.test.sink");
    EXPECT_FALSE(fs::exists(directory / "hosted"));
    EXPECT_FALSE(fs::exists(destination / "hosted"));
}
#endif
#endif

TEST_F(EngineComponent, GeometryPreservesOriginMissingCoordinatesAndLinkVertices) {
    std::ofstream(model, std::ios::app)
        << "\n[OPTIONS]\nCRS EPSG:2263\n[COORDINATES]\nJ 0 0\n[VERTICES]\nC 5 7\n";
    initialize();
    const auto m = Json::parse(read<std::string>(output(*component, "model_metadata")));
    EXPECT_EQ(m["spatial"]["crs"], "EPSG:2263");
    EXPECT_EQ(m["spatial"]["project_length_units"], "ft");
    EXPECT_EQ(m["nodes"][0]["coordinates"], Json({0, 0}));
    EXPECT_TRUE(m["nodes"][1]["coordinates"].is_null());
    EXPECT_EQ(m["links"][0]["coordinates"], Json({{0, 0}, {5, 7}, nullptr}));
    EXPECT_FALSE(fs::exists(directory / "wrapped"));
}

TEST_F(EngineComponent, WatershedCatalogueSeparatesLegacyGroundwaterSnowAndLidUsage) {
    std::ofstream(model, std::ios::app) << R"(
[RAINGAGES]
G INTENSITY 1:00 1 TIMESERIES Rain
[TIMESERIES]
Rain 0:00 0
[SUBCATCHMENTS]
S G J 1 25 500 .5 0 Pack
[SUBAREAS]
S .01 .1 .05 .05 25 OUTLET
[INFILTRATION]
S 3 .5 4 7 0
[POLYGONS]
S 0 0
S 10 0
S 10 10
S 0 10
[AQUIFERS]
AQ .5 .15 .3 .1 12 15 .35 14 0 0 4 .3
[GROUNDWATER]
S AQ J 6 .1 1 0 0 0 0 *
[SNOWPACKS]
Pack PLOWABLE .001 .002 32 .1 .1 0 .5
Pack IMPERVIOUS .001 .002 32 .1 .1 0 0
Pack PERVIOUS .001 .002 32 .1 .1 0 0
[LID_CONTROLS]
Garden BC
Garden SURFACE 6 .1 .1 1 0
Garden SOIL 12 .45 .2 .1 .5 10 3
Garden STORAGE 12 .75 .5 0
[LID_USAGE]
S Garden 1 100 10 0 10 0
)";
    initialize();
    const auto m = Json::parse(read<std::string>(output(*component, "model_metadata")));
    ASSERT_EQ(m["subcatchments"].size(), 1u);
    EXPECT_EQ(m["subcatchments"][0]["polygon"].size(), 4u);
    EXPECT_EQ(m["subcatchments"][0]["outlet_node"], "J");
    EXPECT_EQ(m["subcatchments"][0]["snowpack"], "Pack");
    EXPECT_TRUE(m["legacy_groundwater"]["configured"]);
    EXPECT_EQ(m["legacy_groundwater"]["connections"][0]["aquifer"], "AQ");
    EXPECT_EQ(m["snow"]["packs"][0]["model"], "DEGREE_DAY");
    EXPECT_EQ(m["lids"]["watershed_usage"][0]["subcatchment"], "S");
    EXPECT_EQ(m["lids"]["watershed_usage"][0]["control"], "Garden");
    EXPECT_FALSE(m["explicit_groundwater"]["configured"]);
    EXPECT_TRUE(m["explicit_groundwater"]["rows"].empty());
    EXPECT_FALSE(fs::exists(directory / "wrapped"));
}

TEST_F(EngineComponent, NodeLidCatalogueRetainsRichardsModelAndSealedBottom) {
    auto deck = bytes(model);
    deck.replace(deck.find("FLOW_ROUTING KINWAVE"), 19, "FLOW_ROUTING DYNWAVE");
    deck.replace(deck.find("HEAT_TRANSPORT ON"), 17, "HEAT_TRANSPORT OFF");
    std::ofstream(model) << deck;
    std::ofstream(model, std::ios::app) << R"(
[STORAGE]
Store 0 2 1 FUNCTIONAL 0 0 100 0 0
[LID_CONTROLS]
Stack NODE
Stack SURFACE 6 .1
Stack MEDIA 12 .45 .2 .08 2 10 3
Stack AGGREGATE 6 .4 100
Stack BOTTOM .5 0
[LID_NODES]
Store Stack 10 BOTTOM SEALED
[ORIFICES]
Drain Store O BOTTOM 0 .6 NO 0
[XSECTIONS]
Drain CIRCULAR .1 0 0 0
[LID_NODE_OUTLETS]
Drain 3 BOTTOM
[LID_RICHARDS]
Stack OPTIONS 4 1e-7 1e-5 30
Stack 2 .03 2 1.6 .5 .0001
Stack 3 .03 2 1.6 .5 .0001
)";
    initialize();
    const auto m = Json::parse(read<std::string>(output(*component, "model_metadata")));
    EXPECT_EQ(m["lids"]["controls"][0]["node_flow_model"], "RICHARDS");
    EXPECT_EQ(m["lids"]["node_assignments"][0],
              Json({{"node", "Store"}, {"control", "Stack"}, {"bottom", "SEALED"}}));
    EXPECT_EQ(m["lids"]["node_outlets"],
              Json::array({{{"link", "Drain"}, {"layer", 3}, {"anchor", "BOTTOM"}}}));
    EXPECT_FALSE(fs::exists(directory / "wrapped"));
}

#ifdef OPENSWMM_HAS_2D
TEST_F(EngineComponent, SurfaceBoundaryCataloguePreservesAuthoredUnitsAndReferences) {
    std::ofstream(model, std::ios::app) << R"(
[2D_OPTIONS]
REPORT_2D NO
[2D_VERTICES]
0 0 10
10 0 10
10 10 10
0 10 10
20 0 10
20 10 10
[2D_TRIANGLES]
0 1 2 .03 0
[2D_QUADS]
1 4 5 2 .03 0
[2D_BOUNDARY_CONDITIONS]
0 1 SPECIFIED_STAGE 12
0 2 NORMAL_FLOW .03
1 0 TS_FLOW Inlet
1 1 RATING_CURVE Rating
[2D_BOUNDARY_QUALITY]
0 1 A 7
[TIMESERIES]
Inlet 0:00 2
Inlet 0:10 3
[CURVES]
Rating RATING 0 0
Rating 1 2
)";
    initialize();
    const auto m = Json::parse(read<std::string>(output(*component, "model_metadata")));
    const auto& rows = m.at("surface").at("boundaries");
    ASSERT_EQ(rows.size(), 5u); // shared interior edge is absent; default WALL is retained
    EXPECT_EQ(rows[0]["cell"], 0);
    EXPECT_EQ(rows[0]["edge"], 1);
    EXPECT_EQ(rows[0]["type"], "SPECIFIED_STAGE");
    EXPECT_DOUBLE_EQ(rows[0]["head_m"], 12 * .3048);
    EXPECT_EQ(rows[1]["type"], "NORMAL_FLOW");
    EXPECT_DOUBLE_EQ(rows[1]["slope"], .03);
    EXPECT_EQ(rows[2]["type"], "SPECIFIED_FLOW");
    EXPECT_EQ(rows[2]["timeseries"], "Inlet");
    EXPECT_EQ(rows[3]["type"], "RATING_CURVE");
    EXPECT_EQ(rows[3]["rating_curve"], "Rating");
    EXPECT_EQ(rows[4]["type"], "WALL");
    EXPECT_EQ(m["surface"]["boundary_quality"][0],
              Json({{"cell", 0}, {"edge", 1}, {"species", "A"},
                    {"concentration_native", 7}}));
    EXPECT_FALSE(m["runnable"]);
    EXPECT_FALSE(fs::exists(directory / "wrapped"));
}

TEST_F(EngineComponent, SiMeshBoundaryCatalogueDoesNotScaleTwice) {
    std::ofstream(model, std::ios::app) << R"(
[2D_OPTIONS]
REPORT_2D NO
[2D_VERTICES]
;; UNITS: SI (m)
0 0 10
10 0 10
10 10 10
[2D_TRIANGLES]
0 1 2 .03 0
[2D_BOUNDARY_CONDITIONS]
0 0 SPECIFIED_STAGE 12
0 1 SPECIFIED_FLOW 2
0 2 TS_STAGE Tailwater
[TIMESERIES]
Tailwater 0:00 12
Tailwater 0:10 13
)";
    initialize();
    const auto m = Json::parse(read<std::string>(output(*component, "model_metadata")));
    const auto& rows = m.at("surface").at("boundaries");
    EXPECT_DOUBLE_EQ(rows[0]["head_m"], 12);
    EXPECT_DOUBLE_EQ(rows[1]["flow_native"], 2);
    EXPECT_EQ(rows[1]["units"], "CFS/m"); // flow signals use project units, even for an SI mesh
    EXPECT_EQ(rows[2]["timeseries"], "Tailwater");
    EXPECT_EQ(rows[2]["timeseries_units"], "ft");
    EXPECT_FALSE(rows[2].contains("head_m"));
    EXPECT_TRUE(m["surface"]["boundary_quality"].empty());
    EXPECT_FALSE(fs::exists(directory / "wrapped"));
}

TEST_F(EngineComponent, SiProjectBoundaryCataloguePreservesProjectSignals) {
    auto deck = bytes(model);
    deck.replace(deck.find("FLOW_UNITS CFS"), 14, "FLOW_UNITS CMS");
    std::ofstream(model) << deck << R"(
[2D_OPTIONS]
REPORT_2D NO
[2D_VERTICES]
0 0 10
10 0 10
10 10 10
[2D_TRIANGLES]
0 1 2 .03 0
[2D_BOUNDARY_CONDITIONS]
0 0 SPECIFIED_STAGE 12
0 1 SPECIFIED_FLOW 2
0 2 TS_STAGE Tailwater
[TIMESERIES]
Tailwater 0:00 12
)";
    initialize();
    const auto m = Json::parse(read<std::string>(output(*component, "model_metadata")));
    const auto& rows = m.at("surface").at("boundaries");
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_DOUBLE_EQ(rows[0]["head_m"], 12);
    EXPECT_DOUBLE_EQ(rows[1]["flow_native"], 2);
    EXPECT_EQ(rows[1]["units"], "CMS/m");
    EXPECT_EQ(rows[2]["timeseries_units"], "m");
    EXPECT_FALSE(rows[2].contains("head_m"));
    EXPECT_FALSE(fs::exists(directory / "wrapped"));
}

TEST_F(EngineComponent, GroundwaterBoundaryAndSourceCatalogueRetainsNativeSemantics) {
    std::ofstream(model, std::ios::app) << R"(
[2D_OPTIONS]
REPORT_2D NO
[2D_VERTICES]
0 0 10
10 0 10
10 10 10
[2D_TRIANGLES]
0 1 2 .03 0 Soil
[2D_AQUIFER]
* 1 4 .45 .1 2
[2D_AQUIFER_OPTIONS]
GW_CLOCK INDEPENDENT
GW_BASE_STEP .125
[2D_AQUIFER_BOUNDARIES]
CELL 1 EDGE 0 HEAD CONST 8
CELL 1 EDGE 1 FLOW SERIES Pump
[GW_BOUNDARY_QUALITY]
1 0 A CONC 3
1 0 A MASSFLUX 2
[GW_SOURCES]
Well XY 2 3 FLOW Pump SCALE .5 A CONC 4 B MASS 2
Global * FLOW 0 A MASS 1
Tagged TAG Soil FLOW -.01
Cell CELL 1 FLOW .001
[TIMESERIES]
Pump 0:00 .001
Pump 0:10 -.001
)";
    const std::string longSeries(300, 'P');
    auto deck = bytes(model);
    for (std::size_t pos = 0; (pos = deck.find("Pump", pos)) != std::string::npos;
         pos += longSeries.size())
        deck.replace(pos, 4, longSeries);
    std::ofstream(model) << deck;
    initialize();
    const auto m = Json::parse(read<std::string>(output(*component, "model_metadata")));
    const auto& gw = m.at("explicit_groundwater");
    EXPECT_EQ(gw["options"]["GW_CLOCK"], "INDEPENDENT");
    EXPECT_DOUBLE_EQ(std::stod(gw["options"]["GW_BASE_STEP"].get<std::string>()), .125);
    ASSERT_EQ(gw.at("boundaries").size(), 2u);
    EXPECT_EQ(gw["boundaries"][0],
              Json({{"cell", 0}, {"edge", 0}, {"type", "HEAD"},
                    {"value", 8}, {"units", "ft"}, {"timeseries", ""}}));
    EXPECT_EQ(gw["boundaries"][1]["type"], "FLOW");
    EXPECT_EQ(gw["boundaries"][1]["units"], "m3/s");
    EXPECT_EQ(gw["boundaries"][1]["timeseries"], longSeries);
    ASSERT_EQ(gw["boundary_quality"].size(), 2u);
    EXPECT_EQ(gw["boundary_quality"][0]["kind"], "CONC");
    EXPECT_EQ(gw["boundary_quality"][1]["kind"], "MASSFLUX");
    const auto& source = gw.at("sources").at(0);
    EXPECT_EQ(source["name"], "Well");
    EXPECT_EQ(source["coordinates"], Json({2, 3}));
    EXPECT_EQ(source["flow_timeseries"], longSeries);
    EXPECT_DOUBLE_EQ(source["scale"], .5);
    ASSERT_EQ(source["species_terms"].size(), 2u);
    EXPECT_EQ(source["species_terms"][0]["species"], "A");
    EXPECT_EQ(source["species_terms"][0]["kind"], "CONC");
    EXPECT_EQ(source["species_terms"][1]["species"], "B");
    EXPECT_EQ(source["species_terms"][1]["kind"], "MASS");
    ASSERT_EQ(gw["sources"].size(), 4u);
    EXPECT_EQ(gw["sources"][1]["scope"], "GLOBAL");
    EXPECT_TRUE(gw["sources"][1]["cell"].is_null());
    EXPECT_EQ(gw["sources"][2]["scope"], "TAG");
    EXPECT_EQ(gw["sources"][2]["tag"], "Soil");
    EXPECT_TRUE(gw["sources"][2]["cell"].is_null());
    EXPECT_EQ(gw["sources"][3]["scope"], "CELL");
    EXPECT_EQ(gw["sources"][3]["cell"], 0);
    EXPECT_FALSE(m["runnable"]);
    EXPECT_FALSE(fs::exists(directory / "wrapped"));
}

TEST_F(EngineComponent, MixedSurfaceGeometryAndRepeatedConnectionsAreReadWithoutPreparing) {
    std::ofstream(model, std::ios::app) << R"(
[2D_OPTIONS]
REPORT_2D NO
[2D_VERTICES]
0 0 10
10 0 10
10 10 10
0 10 10
20 0 10
20 10 10
[2D_TRIANGLES]
0 1 2 .03 0
[2D_QUADS]
1 4 5 2 .03 0
[2D_TRIANGLE_NODE_MAP]
0 J .6 2
0 O .7 3
[2D_VERTEX_NODE_MAP]
3 J .5 4
[2D_AQUIFER]
* 1 4 .45 .1 2
[2D_AQUIFER_NODE]
J AUTO KC 1 DC 2 AREA 3
[2D_AQUIFER_LINKS]
C KC 1 DC 2 EXCHANGE NO
)";
    initialize();
    const auto m = Json::parse(read<std::string>(output(*component, "model_metadata")));
    EXPECT_FALSE(m["runnable"]);
    const auto& surface = m["surface"];
    ASSERT_EQ(surface["vertices_m"].size(), 6u);
    EXPECT_DOUBLE_EQ(surface["vertices_m"][1][0].get<double>(), 3.048);
    EXPECT_EQ(surface["cells"][0]["vertices"], Json({0, 1, 2}));
    EXPECT_EQ(surface["cells"][1]["vertices"], Json({1, 4, 5, 2}));
    ASSERT_EQ(surface["cell_connections"].size(), 2u);
    EXPECT_EQ(surface["cell_connections"][0]["node"], "J");
    EXPECT_EQ(surface["cell_connections"][1]["node"], "O");
    EXPECT_NEAR(surface["cell_connections"][1]["area_m2"].get<double>(), 3 * .3048 * .3048, 1e-14);
    EXPECT_EQ(surface["vertex_connections"][0]["vertex"], 3);
    EXPECT_EQ(surface["vertex_connections"][0]["node"], "J");
    const auto& gw = m["explicit_groundwater"];
    EXPECT_EQ(gw["mode"], "MESH");
    EXPECT_EQ(gw["geometry_source"], "surface.cells");
    EXPECT_TRUE(gw["configured"]);
    EXPECT_TRUE(gw["node_beds"][0]["locate_at_initialize"]);
    EXPECT_TRUE(gw["node_beds"][0]["cell"].is_null());
    EXPECT_FALSE(gw["link_beds"][0]["exchange"]);
    EXPECT_FALSE(m["legacy_groundwater"]["configured"]);
    component->prepare();
    EXPECT_EQ(component->status(), Status::Failed);
    EXPECT_FALSE(fs::exists(directory / "wrapped"));
}

TEST_F(EngineComponent, ExplicitPerSubcatchGroundwaterDoesNotBecomeLegacyGroundwater) {
    std::ofstream(model, std::ios::app)
        << "\n[2D_AQUIFER_OPTIONS]\nMODE PER_SUBCATCH\n[2D_AQUIFER]\n* 1 4 .45 .1 2\n";
    initialize();
    const auto m = Json::parse(read<std::string>(output(*component, "model_metadata")));
    EXPECT_EQ(m["surface_vertex_count"], 0);
    EXPECT_TRUE(m["surface"]["vertices_m"].empty());
    EXPECT_TRUE(m["explicit_groundwater"]["configured"]);
    EXPECT_EQ(m["explicit_groundwater"]["mode"], "PER_SUBCATCH");
    EXPECT_EQ(m["explicit_groundwater"]["geometry_source"], "subcatchments");
    EXPECT_FALSE(m["legacy_groundwater"]["configured"]);
    EXPECT_FALSE(m["runnable"]);
    EXPECT_FALSE(fs::exists(directory / "wrapped"));
}
#endif
