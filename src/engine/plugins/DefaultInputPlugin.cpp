/**
 * @file DefaultInputPlugin.cpp
 * @brief DefaultInputPlugin — reads/writes SWMM .inp files.
 *
 * @see DefaultInputPlugin.hpp
 * @ingroup engine_plugins
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "DefaultInputPlugin.hpp"

#include "../input/InputReader.hpp"
#include "../input/PostParseResolver.hpp"
#include "../core/InpWriter.hpp"
#include "../core/SimulationContext.hpp"

// Section handler includes
#include "../input/handlers/TitleHandler.hpp"
#include "../input/handlers/OptionsHandler.hpp"
#include "../input/handlers/NodesHandler.hpp"
#include "../input/handlers/LinksHandler.hpp"
#include "../input/handlers/CatchmentHandler.hpp"
#include "../input/handlers/TablesHandler.hpp"
#include "../input/handlers/UserFlagsHandler.hpp"
#include "../input/handlers/UserFlagValuesHandler.hpp"
#include "../input/handlers/PluginsHandler.hpp"
#include "../input/handlers/InflowsHandler.hpp"
#include "../input/handlers/QualityHandler.hpp"
#include "../input/handlers/HydrologyHandler.hpp"
#include "../input/handlers/ControlsHandler.hpp"
#include "../input/handlers/SpatialHandler.hpp"
#include "../input/handlers/InfraHandler.hpp"

namespace openswmm {

// ============================================================================
// Constructor
// ============================================================================

DefaultInputPlugin::DefaultInputPlugin() {
    register_builtin_handlers();
}

// ============================================================================
// register_builtin_handlers()
// ============================================================================

void DefaultInputPlugin::register_builtin_handlers() {
    auto noop = [](SimulationContext&, const std::vector<std::string>&) {};

    // Title / options
    registry_.register_builtin("TITLE",   input::handle_title);
    registry_.register_builtin("OPTIONS", input::handle_options);

    // Node sections
    registry_.register_builtin("JUNCTIONS",    input::handle_junctions);
    registry_.register_builtin("OUTFALLS",     input::handle_outfalls);
    registry_.register_builtin("DIVIDERS",     input::handle_dividers);
    registry_.register_builtin("STORAGE",      input::handle_storage);
    registry_.register_builtin("COORDINATES",  input::handle_coordinates);

    // Link sections
    registry_.register_builtin("CONDUITS",     input::handle_conduits);
    registry_.register_builtin("PUMPS",        input::handle_pumps);
    registry_.register_builtin("ORIFICES",     input::handle_orifices);
    registry_.register_builtin("WEIRS",        input::handle_weirs);
    registry_.register_builtin("OUTLETS",      input::handle_outlets);
    registry_.register_builtin("XSECTIONS",    input::handle_xsections);
    registry_.register_builtin("TRANSECTS",    input::handle_transects);
    registry_.register_builtin("LOSSES",       input::handle_losses);

    // Catchment sections
    registry_.register_builtin("SUBCATCHMENTS", input::handle_subcatchments);
    registry_.register_builtin("SUBAREAS",      input::handle_subareas);
    registry_.register_builtin("INFILTRATION",  input::handle_infiltration);
    registry_.register_builtin("LID_CONTROLS",  input::handle_lid_controls);
    registry_.register_builtin("LID_USAGE",     input::handle_lid_usage);
    registry_.register_builtin("AQUIFERS",      input::handle_aquifers);
    registry_.register_builtin("GROUNDWATER",   input::handle_groundwater);
    registry_.register_builtin("GWF",           input::handle_gwf);

    // Rain / climate
    registry_.register_builtin("RAINGAGES",     input::handle_raingages);
    registry_.register_builtin("EVAPORATION",   input::handle_evaporation);
    registry_.register_builtin("TEMPERATURE",   input::handle_temperature);
    registry_.register_builtin("SNOWPACKS",     input::handle_snowpacks);

    // Inflows / loading
    registry_.register_builtin("INFLOWS",       input::handle_inflows);
    registry_.register_builtin("DWF",           input::handle_dwf);
    registry_.register_builtin("RDII",          input::handle_rdii);
    registry_.register_builtin("HYDROGRAPHS",  input::handle_hydrographs);
    registry_.register_builtin("LOADINGS",      input::handle_loadings);
    registry_.register_builtin("PATTERNS",      input::handle_patterns);

    // Tables
    registry_.register_builtin("CURVES",        input::handle_curves);
    registry_.register_builtin("TIMESERIES",    input::handle_timeseries);

    // Water quality
    registry_.register_builtin("POLLUTANTS",    input::handle_pollutants);
    registry_.register_builtin("LANDUSES",      input::handle_landuses);
    registry_.register_builtin("COVERAGES",     input::handle_coverages);
    registry_.register_builtin("BUILDUP",       input::handle_buildup);
    registry_.register_builtin("WASHOFF",       input::handle_washoff);
    registry_.register_builtin("TREATMENT",     input::handle_treatment);

    // Control / map / reporting
    registry_.register_builtin("CONTROLS",      input::handle_controls);
    registry_.register_builtin("VERTICES",      input::handle_vertices);
    registry_.register_builtin("POLYGONS",      input::handle_polygons);
    registry_.register_builtin("SYMBOLS",       input::handle_symbols);
    registry_.register_builtin("LABELS",        noop);
    registry_.register_builtin("BACKDROP",      noop);
    registry_.register_builtin("MAP",           input::handle_map);
    registry_.register_builtin("TAGS",          input::handle_tags);
    registry_.register_builtin("PROFILE",       noop);
    registry_.register_builtin("REPORT",        input::handle_report);
    registry_.register_builtin("FILES",         noop);
    registry_.register_builtin("ADJUSTMENTS",   input::handle_adjustments);
    registry_.register_builtin("EVENTS",        input::handle_events);

    // Streets / inlets (legacy s_STREET, s_INLET, s_INLET_USAGE)
    registry_.register_builtin("STREETS",       input::handle_streets);
    registry_.register_builtin("INLETS",        input::handle_inlets);
    registry_.register_builtin("INLET_USAGE",   input::handle_inlet_usage);

    // New in 6.0.0
    registry_.register_builtin("USER_FLAGS",       input::handle_user_flags);
    registry_.register_builtin("USER_FLAG_VALUES", input::handle_user_flag_values);
    registry_.register_builtin("PLUGINS",          input::handle_plugins);
}

// ============================================================================
// Plugin lifecycle
// ============================================================================

int DefaultInputPlugin::initialize(
    const std::vector<std::string>& /*init_args*/,
    const IPluginComponentInfo* /*info*/)
{
    state_ = PluginState::INITIALIZED;
    return 0;
}

int DefaultInputPlugin::validate(const SimulationContext& /*ctx*/) {
    state_ = PluginState::VALIDATED;
    return 0;
}

int DefaultInputPlugin::read(const std::string& path, SimulationContext& ctx) {
    input::InputReader reader(registry_);
    bool ok = reader.read(path, ctx);

    skipped_sections_ = reader.skipped_sections();

    if (!ok) {
        last_error_ = ctx.error_message;
        return ctx.error_code != 0 ? ctx.error_code : -1;
    }
    return 0;
}

int DefaultInputPlugin::write(const std::string& path, const SimulationContext& ctx) {
    int err = inp_writer::writeInpFile(ctx, path);
    if (err != 0) {
        last_error_ = "Failed to write .inp file: " + path;
    }
    return err;
}

int DefaultInputPlugin::finalize(const SimulationContext& /*ctx*/) {
    state_ = PluginState::FINALIZED;
    return 0;
}

} /* namespace openswmm */
