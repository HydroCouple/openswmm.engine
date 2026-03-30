/**
 * @file SimulationOptions.hpp
 * @brief Simulation options parsed from the [OPTIONS] section.
 *
 * @details Contains all standard SWMM options plus an extension map for
 *          unknown keys (R05) and a CRS field (R06).
 *
 * Standard SWMM 5.x options are documented in:
 * @see Legacy reference: src/solver/input.c — readOption()
 * @see Legacy reference: src/solver/globals.h — global option variables
 *
 * New in 6.0.0:
 * - `crs`: Coordinate reference system (EPSG code or PROJ string). R06.
 * - `ext_options`: Key-value map for unknown/extension option keys. R05.
 *
 * @defgroup engine_core Core Engine The core engine module, including lifecycle management, simulation options, and shared utilities.
 * @ingroup  new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_SIMULATION_OPTIONS_HPP
#define OPENSWMM_ENGINE_SIMULATION_OPTIONS_HPP

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace openswmm {

// ============================================================================
// Enumerators matching legacy SWMM enums.h
// ============================================================================

/**
 * @brief Flow unit systems.
 * @see Legacy: FlowUnitsType in enums.h
 */
enum class FlowUnits : int {
    CFS  = 0,  ///< Cubic feet per second
    GPM  = 1,  ///< Gallons per minute
    MGD  = 2,  ///< Million gallons per day
    CMS  = 3,  ///< Cubic meters per second
    LPS  = 4,  ///< Liters per second
    MLD  = 5   ///< Million liters per day
};

/**
 * @brief Flow routing methods.
 * @see Legacy: RouteModelType in enums.h
 */
enum class RoutingModel : int {
    STEADY   = 0,  ///< Steady-state (no routing)
    KINWAVE  = 1,  ///< Kinematic wave approximation
    DYNWAVE  = 2   ///< Dynamic wave (full Saint-Venant)
};

/**
 * @brief Infiltration methods.
 * @see Legacy: InfilModelType in enums.h
 */
enum class InfiltrationModel : int {
    HORTON        = 0,
    MOD_HORTON    = 1,
    GREEN_AMPT    = 2,
    MOD_GREEN_AMPT = 3,
    CURVE_NUMBER  = 4
};

/**
 * @brief Runoff routing methods.
 * @see Legacy: RunoffModelType in enums.h
 */
enum class RunoffModel : int {
    NONE       = 0,
    NL_POND    = 1  ///< Non-linear reservoir (default)
};

// ============================================================================
// SimulationOptions struct
// ============================================================================

/**
 * @brief All SWMM simulation options parsed from [OPTIONS] section.
 *
 * @details This struct consolidates all options that were previously global
 *          variables in src/solver/globals.h (e.g., FlowUnits, RouteModel,
 *          RouteStep, etc.) into a single value type that lives inside
 *          SimulationContext.
 *
 * The `ext_options` map stores any key in [OPTIONS] that is not recognized
 * as a standard SWMM option. A SWMM_WARN_UNKNOWN_OPTION warning is issued.
 * Extension options are available to plugins via the C API.
 *
 * @ingroup engine_core
 */
struct SimulationOptions {
    // -----------------------------------------------------------------------
    // Time control
    // -----------------------------------------------------------------------

    /** @brief Simulation start date/time (decimal days, Julian date).
     *  @details Legacy default: Jan 1, 2004 = datetime_encodeDate(2004,1,1). */
    double start_date = 2453006.0;  // Jan 1, 2004 Julian date (legacy default)

    /** @brief Simulation end date/time (decimal days, Julian date). */
    double end_date = 0.0;

    /** @brief Report start date/time. */
    double report_start = 0.0;

    /** @brief Hydraulic routing timestep in seconds. Legacy default: 20. */
    double routing_step = 20.0;

    /** @brief Minimum routing timestep in seconds (CFL floor). */
    double min_routing_step = 0.5;

    /** @brief Dry-weather runoff timestep in seconds. */
    double dry_step = 3600.0;

    /** @brief Wet-weather runoff timestep in seconds. */
    double wet_step = 300.0;

    /** @brief Reporting output interval in seconds. */
    double report_step = 900.0;

    /** @brief Antecedent dry days. */
    double dry_days = 0.0;

    /** @brief Courant factor for variable time stepping (0 = fixed step).
     *  @details Legacy default: 0.75 (enabled). */
    double variable_step = 0.75;

    /**
     * @brief Conduit lengthening timestep (seconds, 0 = use routing_step).
     * @details Used to compute modified conduit lengths for CFL stability.
     * @see Legacy: LengtheningStep
     */
    double lengthening_step = 0.0;

    /** @brief Street sweeping start day-of-year (1-365). 0 = not set. */
    int sweep_start = 1;

    /** @brief Street sweeping end day-of-year (1-365). 0 = not set. */
    int sweep_end = 365;

    // -----------------------------------------------------------------------
    // Flow and routing
    // -----------------------------------------------------------------------

    /** @brief Flow units system. */
    FlowUnits flow_units = FlowUnits::CFS;

    /** @brief Routing method. Legacy default: DYNWAVE. */
    RoutingModel routing_model = RoutingModel::DYNWAVE;

    /** @brief Infiltration method for subcatchments. */
    InfiltrationModel infiltration = InfiltrationModel::HORTON;

    /** @brief Runoff routing method. */
    RunoffModel runoff_model = RunoffModel::NL_POND;

    // -----------------------------------------------------------------------
    // Solver settings
    // -----------------------------------------------------------------------

    /** @brief Maximum number of flow routing iterations (dynamic wave). */
    int max_trials = 8;

    /** @brief Surcharge method: 0=EXTRAN, 1=SLOT. @see Legacy: SurchargeMethod */
    int surcharge_method = 0;

    /** @brief Normal flow limitation: 0=SLOPE, 1=FROUDE, 2=BOTH, 3=NEITHER.
     *  @see Legacy: NormalFlowLtd */
    int normal_flow_ltd = 2;  // BOTH (legacy default)

    /** @brief Force main equation: 0=Hazen-Williams, 1=Darcy-Weisbach.
     *  @see Legacy: ForceMainEqn */
    int force_main_eqn = 0;

    /** @brief Inertial damping: 0=NONE, 1=PARTIAL, 2=FULL.
     *  @see Legacy: InertDamping */
    int inertial_damping = 1;  // PARTIAL (legacy default)

    /** @brief Link offset mode: 0=DEPTH_OFFSET, 1=ELEV_OFFSET.
     *  @see Legacy: LinkOffsets */
    int link_offsets = 0;  // DEPTH_OFFSET (legacy default)

    /** @brief Minimum conduit slope (ft/ft). @see Legacy: MinSlope */
    double min_slope = 0.0;

    /** @brief Minimum node surface area (ft²). @see Legacy: MinSurfArea */
    double min_surf_area = 0.0;  // 0 = use MIN_SURFAREA constant

    /** @brief Convergence head tolerance in project length units.
     *  @details Legacy default: 0.0 (sentinel → runtime default 0.005 ft). */
    double head_tol = 0.005;

    /** @brief System flow tolerance (fraction, e.g., 0.05 = 5%).
     *  @details Legacy default: 0.05. Input is in percent, divided by 100. */
    double sys_flow_tol = 0.05;

    /** @brief Lateral inflow tolerance (fraction, e.g., 0.05 = 5%).
     *  @details Legacy default: 0.05. Input is in percent, divided by 100. */
    double lat_flow_tol = 0.05;

    // -----------------------------------------------------------------------
    // System settings
    // -----------------------------------------------------------------------

    /** @brief True if quality routing is enabled. */
    bool quality_routing = false;

    /** @brief True if evaporation is included. */
    bool allow_ponding = false;

    /** @brief Ignore rainfall (for hot start runs). */
    bool ignore_rainfall = false;

    /** @brief Ignore snowmelt. */
    bool ignore_snow_melt = false;

    /** @brief Ignore groundwater. */
    bool ignore_groundwater = false;

    /** @brief Ignore RDII. */
    bool ignore_rdii = false;

    /** @brief Ignore routing. */
    bool ignore_routing = false;

    /** @brief Ignore water quality. */
    bool ignore_quality = false;

    // -----------------------------------------------------------------------
    // Threading
    // -----------------------------------------------------------------------

    /**
     * @brief Number of OpenMP threads for parallel solver loops.
     *
     * @details Parsed from the THREADS keyword in [OPTIONS].
     *   - 0 → use all available threads (omp_get_max_threads()).
     *   - 1 → single-threaded (default, no OpenMP overhead).
     *   - N → use min(N, omp_get_max_threads()) threads.
     *
     * A performance threshold is applied at startup: if the number of
     * conduit links is less than 4 × num_threads, threading is disabled
     * to avoid overhead dominating on small networks.
     *
     * @see Legacy reference: globals.h NumThreads, project.c
     */
    int num_threads = 1;

    // -----------------------------------------------------------------------
    // New in 6.0.0 — CRS (R06)
    // -----------------------------------------------------------------------

    /**
     * @brief Coordinate reference system string.
     *
     * @details Set via the CRS key in [OPTIONS]:
     * @code
     * [OPTIONS]
     * CRS  EPSG:4326
     * @endcode
     * or using a PROJ string:
     * @code
     * [OPTIONS]
     * CRS  "+proj=utm +zone=33 +datum=WGS84"
     * @endcode
     *
     * Empty string if not specified. Also stored in SpatialFrame::crs.
     */
    std::string crs;

    // -----------------------------------------------------------------------
    // New in 6.0.0 — Extension options map (R05)
    // -----------------------------------------------------------------------

    /**
     * @brief Key-value map for unknown [OPTIONS] keys.
     *
     * @details Any key in [OPTIONS] that is not a recognized SWMM keyword is
     *          stored here as a string pair. A SWMM_WARN_UNKNOWN_OPTION
     *          warning is issued for each unknown key.
     *
     *          Plugins can retrieve values via swmm_options_get_ext().
     *          Keys are stored uppercase-normalized.
     *
     * Example:
     * @code
     * [OPTIONS]
     * TURBULENCE_DAMP  0.85    ;; stored as ext_options["TURBULENCE_DAMP"] = "0.85"
     * PLUGIN_TIMEOUT   30      ;; stored as ext_options["PLUGIN_TIMEOUT"] = "30"
     * @endcode
     */
    std::unordered_map<std::string, std::string> ext_options;

    // -----------------------------------------------------------------------
    // Evaporation settings (from [EVAPORATION] section)
    // -----------------------------------------------------------------------

    /** @brief Evaporation type: 0=CONSTANT, 1=MONTHLY, 2=TIMESERIES, 3=TEMPERATURE, 4=FILE. */
    int evap_type = 0;

    /** @brief Monthly evaporation values (used when evap_type == 0 or 1). */
    double evap_values[12] = {0,0,0,0,0,0,0,0,0,0,0,0};

    /** @brief Timeseries name for evaporation (evap_type == 2). */
    std::string evap_ts_name;

    /** @brief Recovery pattern name for evaporation. */
    std::string evap_recovery_pat;

    /** @brief If true, evaporation only occurs on dry days. */
    bool evap_dry_only = false;

    // -----------------------------------------------------------------------
    // Temperature settings (from [TEMPERATURE] section)
    // -----------------------------------------------------------------------

    /** @brief Temperature source: 0=NONE, 1=TIMESERIES, 2=FILE. */
    int temp_source = 0;

    /** @brief Timeseries name for temperature data. */
    std::string temp_ts_name;

    /** @brief File path for temperature data. */
    std::string temp_file;

    /** @brief Temperature file start date (Julian). */
    double temp_file_start = 0.0;

    /** @brief Wind speed type: 0=MONTHLY, 1=FILE. */
    int wind_type = 0;

    /** @brief Monthly wind speed values (12 months). */
    double wind_speed[12] = {0,0,0,0,0,0,0,0,0,0,0,0};

    /** @brief Snowmelt: dividing temperature. */
    double snow_divt = 34.0;

    /** @brief Snowmelt: ATI weight (0–1). */
    double snow_ati_wt = 0.5;

    /** @brief Snowmelt: negative melt ratio. */
    double snow_nrg_ratio = 0.6;

    /** @brief Snowmelt: latitude (degrees). */
    double snow_lat = 0.0;

    /** @brief Snowmelt: minimum melt coefficient. */
    double snow_min_melt = 0.0;

    /** @brief Snowmelt: maximum melt coefficient. */
    double snow_max_melt = 0.0;

    /** @brief Areal depletion curve for impervious surfaces (10 fractions). */
    double adc_imperv[10] = {1,1,1,1,1,1,1,1,1,1};

    /** @brief Areal depletion curve for pervious surfaces (10 fractions). */
    double adc_perv[10] = {1,1,1,1,1,1,1,1,1,1};

    // -----------------------------------------------------------------------
    // Report settings (from [REPORT] section)
    // -----------------------------------------------------------------------

    /** @brief Subcatchment reporting: 0=NONE, 1=ALL, 2=SOME. */
    int rpt_subcatchments = 0;

    /** @brief Node reporting: 0=NONE, 1=ALL, 2=SOME. */
    int rpt_nodes = 0;

    /** @brief Link reporting: 0=NONE, 1=ALL, 2=SOME. */
    int rpt_links = 0;

    /** @brief Report input summary. */
    bool rpt_input = false;

    /** @brief Report continuity errors (default true). */
    bool rpt_continuity = true;

    /** @brief Report flow statistics (default true). */
    bool rpt_flowstats = true;

    /** @brief Report control actions (default false). */
    bool rpt_controls = false;

    /** @brief Report time-averaged results (default false). */
    bool rpt_averages = false;

    /** @brief Named subcatchments to report (used when rpt_subcatchments == 2). */
    std::vector<std::string> rpt_subcatch_names;

    /** @brief Named nodes to report (used when rpt_nodes == 2). */
    std::vector<std::string> rpt_node_names;

    /** @brief Named links to report (used when rpt_links == 2). */
    std::vector<std::string> rpt_link_names;
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_SIMULATION_OPTIONS_HPP */
