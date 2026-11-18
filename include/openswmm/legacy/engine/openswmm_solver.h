/**
 * @file      openswmm_solver.h
 * @brief     Prototypes for SWMM5 API functions.
 * @author    L. Rossman
 * @date      Created on: 2021-11-01
 * @date      Last edited on: 2024-12-23 (Build 5.3.0)
 * @details This file contains the prototypes for SWMM5 API functions.
 *
 *  Update History
 *  ==============
 *  - Build 5.3.0:
 *      - Added new functions to support saving hotstart files at specific times.
 *      - Expansions to the SWMM API to include attributes of more objects and water quality.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com> (OpenSWMM maintenance and extensions)
 * @copyright Copyright (c) 2026 Caleb Buahin. OpenSWMM contributions only; original EPA SWMM
 *            credits are retained.
 * @license   Apache-2.0 for OpenSWMM contributions; original EPA SWMM provenance is retained.
 */

#ifndef OPENSWMM_LEGACY_SOLVER_H_
#define OPENSWMM_LEGACY_SOLVER_H_

#include "openswmm_legacy_solver_export.h"

// --- use "C" linkage for C++ programs
#ifdef __cplusplus
extern "C" {
#endif

/*!
 * \enum swmm_Object
 * \brief Enumeration of object types used in SWMM5
 */
typedef enum {
    /*! \brief Rain gages */
    swmm_GAGE,
    /*! \brief Subcatchments */
    swmm_SUBCATCH,
    /*! \brief Nodes */
    swmm_NODE,
    /*! \brief Links */
    swmm_LINK,
    /*! \brief Pollutants */
    swmm_POLLUTANT,
    /*! \brief Land uses */
    swmm_LANDUSE,
    /*! \brief Time patterns */
    swmm_TIME_PATTERN,
    /*! \brief Curve functions */
    swmm_CURVE,
    /*! \brief Time series */
    swmm_TIMESERIES,
    /*! \brief Control rules */
    swmm_CONTROL_RULE,
    /*! \brief Transects */
    swmm_TRANSECT,
    /*! \brief Aquifers */
    swmm_AQUIFER,
    /*! \brief Unit hydrographs */
    swmm_UNIT_HYDROGRAPH,
    /*! \brief Snow packs */
    swmm_SNOWPACK,
    /*! \brief Cross section shape */
    smmm_XSECTION_SHAPE,
    /*! \brief Low impact development units */
    swmm_LID,
    /*! \brief Street*/
    swmm_STREET,
    /*! \brief Inlet */
    swmm_INLET,
    /*! \brief System */
    swmm_SYSTEM = 100
} swmm_Object;

/*!
 * \enum swmm_NodeType
 * \brief Enumeration of node types used in SWMM5
 */
typedef enum {
    /*! \brief Junction node */
    swmm_JUNCTION = 0,
    /*! \brief Outfall node */
    swmm_OUTFALL = 1,
    /*! \brief Storage node */
    swmm_STORAGE = 2,
    /*! \brief Divider node */
    swmm_DIVIDER = 3
} swmm_NodeType;

/*!
 * \enum swmm_LinkType
 * \brief Enumeration of link types used in SWMM5
 */
typedef enum {
    /*! \brief Conduit link */
    swmm_CONDUIT = 0,
    /*! \brief Pump link */
    swmm_PUMP = 1,
    /*! \brief Orifice link */
    swmm_ORIFICE = 2,
    /*! \brief Weir link */
    swmm_WEIR = 3,
    /*! \brief Outlet link */
    swmm_OUTLET = 4
} swmm_LinkType;

/*!
 * \enum swmm_GageProperty
 * \brief Enumeration of gage properties used in SWMM5
 */
typedef enum {
    /*! \brief Total precipitation */
    swmm_GAGE_TOTAL_PRECIPITATION = 100,
    /*! \brief Rainfall */
    swmm_GAGE_RAINFALL,
    /*! \brief Snowfall */
    swmm_GAGE_SNOWFALL,
    /*! \brief Rainfall scaling factor (dimensionless, > 0). */
    swmm_GAGE_SCALEFACTOR,
} swmm_GageProperty;

/*!
 * \enum swmm_SubcatchProperty
 * \brief Enumeration of subcatchment properties used in SWMM5
 * @todo Add LID properties to this enum
 */
typedef enum {
    /*! \brief Area */
    swmm_SUBCATCH_AREA = 200,
    /*! \brief Rain gage */
    swmm_SUBCATCH_RAINGAGE,
    /*! \brief Rainfall */
    swmm_SUBCATCH_RAINFALL,
    /*! \brief Evaporation */
    swmm_SUBCATCH_EVAP,
    /*! \brief Infiltration */
    swmm_SUBCATCH_INFIL,
    /*! \brief Runoff */
    swmm_SUBCATCH_RUNOFF,
    /*! \brief Report flag */
    swmm_SUBCATCH_RPTFLAG,
    /*! \brief Width */
    swmm_SUBCATCH_WIDTH,
    /*! \brief Slope */
    swmm_SUBCATCH_SLOPE,
    /*! \brief Outlet type */
    swmm_SUBCATCH_OUTLET_TYPE,
    /*! \brief Outlet index */
    swmm_SUBCATCH_OUTLET_INDEX,
    /*! \brief Infiltration model */
    swmm_SUBCATCH_INFILTRATION_MODEL,
    /*! \brief Fraction impervious */
    swmm_SUBCATCH_FRACTION_IMPERVIOUS,
    /*! \brief Subarea routing to */
    swmm_SUBCATCH_SUB_AREA_ROUTE_TO,
    /*! \brief Fraction subarea routed to outlet */
    swmm_SUBCATCH_SUB_AREA_FRACTION_OUTLET,
    /*! \brief Subarea's Manning's n */
    swmm_SUBCATCH_SUB_AREA_MANNINGS_N,
    /*! \brief Subarea's depression storage */
    swmm_SUBCATCH_SUB_AREA_FRACTION_AREA,
    /*! \brief Subarea's depression storage */
    swmm_SUBCATCH_SUB_AREA_DEPRESSION_STORAGE,
    /*! \brief Subarea's inflow */
    swmm_SUBCATCH_SUB_AREA_INFLOW,
    /*! \brief Subarea's runoff */
    swmm_SUBCATCH_SUB_AREA_RUNOFF,
    /*! \brief Subarea's depth */
    swmm_SUBCATCH_SUB_AREA_DEPTH,
    /*! \brief Subcatch's LID unit count */
    swmm_SUBCATCH_LID_UNITS_COUNT,
    /*! \brief Subcatch's LID pervious area */
    swmm_SUBCATCH_LID_UNITS_PERV_AREA,
    /*! \brief Subcatch's LID flow to pervious area */
    swmm_SUBCATCH_LID_UNITS_FLOW_TO_PERV_AREA,
    /*! \brief Subcatch's LID drain flow */
    swmm_SUBCATCH_LID_UNITS_DRAIN_FLOW,
    /*! \brief Number of Subcatch's LID replicates*/
    swmm_SUBCATCH_LID_UNIT_REPLICATES,
    /*! \brief Subcatch's LID unit area */
    swmm_SUBCATCH_LID_UNIT_AREA,
    /*! \brief Subcatch's LID unit full top width */
    swmm_SUBCATCH_LID_UNIT_FULL_WIDTH,
    /*! \brief Subcatch's LID unit bottom width */
    swmm_SUBCATCH_LID_UNIT_BOTTOM_WIDTH,
    /*! \brief Subcatch's LID unit initial saturation of soil and storage layers*/
    swmm_SUBCATCH_LID_UNIT_INIT_SATURATION,
    /*! \brief Subcatch's LID unit fraction of impervious area runoff treated */
    swmm_SUBCATCH_LID_UNIT_FROM_IMPERVIOUS,
    /*! \brief Subcatch's LID unit fraction of pervious area runoff treated */
    swmm_SUBCATCH_LID_UNIT_FROM_PERVIOUS,
    /*! \brief Whether Subcatch's LID unit's flow is sent to the pervious area */
    swmm_SUBCATCH_LID_UNIT_TO_PERVIOUS,
    /*! \brief The object type of the receiving drain flow */
    swmm_SUBCATCH_LID_UNIT_RECEIVING_OUTLET_TYPE,
    /*! \brief The index of the object receiving drain flow */
    swmm_SUBCATCH_LID_UNIT_RECEIVING_OUTLET_INDEX,
    /*! \brief Subcatch's LID unit surface depth */
    swmm_SUBCATCH_LID_UNIT_SURFACE_DEPTH,
    /*! \brief Subcatch's LID unit soil moisture content of biocell soil layer */
    swmm_SUBCATCH_LID_UNIT_SOIL_MOISTURE,
    /*! \brief Subcatch's LID unit green and ampt soil capillary suction */
    swmm_SUBCATCH_LID_UNIT_GREEN_AMPT_CAPILLARY_SUCTION,
    /*! \brief Subcatch's LID unit green and ampt saturated conductivity */
    swmm_SUBCATCH_LID_UNIT_GREEN_AMPT_SATURATED_CONDUCTIVITY,
    /*! \brief Subcatch's LID unit green and ampt maximum soil moisture deficit */
    swmm_SUBCATCH_LID_UNIT_GREEN_AMPT_MAXIMUM_SOIL_MOISTURE_DEFICIT,
    /*! \brief Curb length */
    swmm_SUBCATCH_CURB_LENGTH,
    /*! \brief API provided rainfall */
    swmm_SUBCATCH_API_RAINFALL,
    /*! \brief API provided snowfall */
    swmm_SUBCATCH_API_SNOWFALL,
    /*! \brief Pollutant buildup */
    swmm_SUBCATCH_POLLUTANT_BUILDUP,
    /*! \brief External pollutant buildup */
    swmm_SUBCATCH_EXTERNAL_POLLUTANT_BUILDUP,
    /*! \brief Runoff concentration */
    swmm_SUBCATCH_POLLUTANT_RUNOFF_CONCENTRATION,
    /*! \brief Ponded concentration */
    swmm_SUBCATCH_POLLUTANT_PONDED_CONCENTRATION,
    /*! \brief Total pollutant load */
    swmm_SUBCATCH_POLLUTANT_TOTAL_LOAD,
    /*! \brief API prescribed potential evapotranspiration rate (in/day or mm/day).
     *         Settable while the simulation is running; overrides the
     *         climate-derived evaporation rate for the subcatchment's surface,
     *         LID, and groundwater evaporation. Set a negative value to clear
     *         and revert to climate-derived evaporation. */
    swmm_SUBCATCH_API_PET,
    /*! \brief Groundwater upper zone moisture content (volume fraction,
     *         0..porosity). Settable while the simulation is running
     *         (state injection / data assimilation). */
    swmm_SUBCATCH_GW_MOISTURE,
    /*! \brief Groundwater saturated (lower) zone depth above the aquifer
     *         bottom (ft or m). Settable while the simulation is running. */
    swmm_SUBCATCH_GW_LOWER_DEPTH,
    /*! \brief Snow pack snow water equivalent on a snow subarea
     *         (in or mm; sub_index = 0 plowable, 1 impervious, 2 pervious).
     *         Settable while the simulation is running. */
    swmm_SUBCATCH_SNOW_SWE,
    /*! \brief Snow pack free water on a snow subarea (in or mm;
     *         sub_index as for swmm_SUBCATCH_SNOW_SWE). Settable while
     *         the simulation is running. */
    swmm_SUBCATCH_SNOW_FW,
    /*! \brief Snow pack antecedent temperature index on a snow subarea
     *         (deg F or deg C). Settable while the simulation is running. */
    swmm_SUBCATCH_SNOW_ATI,
    /*! \brief Snow pack cold content on a snow subarea (in or mm of melt
     *         equivalent). Settable while the simulation is running. */
    swmm_SUBCATCH_SNOW_COLDC,
    /*! \brief Subcatchment rainfall scale factor (optional token 9 of
     *         [SUBCATCHMENTS]; default 1.0 = no scaling). Multiplies the
     *         gage-derived rainfall for this subcatchment only, composing
     *         multiplicatively with the gage's own scale factor. API-prescribed
     *         rainfall is NOT scaled by it. Must be > 0. Settable while the
     *         simulation is running. */
    swmm_SUBCATCH_RAIN_SCALE_FACTOR,
    /*! \brief Subcatchment snowfall scale factor (optional token 10 of
     *         [SUBCATCHMENTS]; default 1.0 = no scaling). Composes
     *         multiplicatively with the gage snow catch factor (SCF): SCF
     *         corrects the physical gage's snow-catch deficiency, while this
     *         represents spatial variation across the catchment. Must be > 0.
     *         Settable while the simulation is running. */
    swmm_SUBCATCH_SNOW_SCALE_FACTOR,
} swmm_SubcatchProperty;

/*!
 * \enum swmm_NodeProperty
 * \brief Enumeration of node properties used in SWMM5
 */
#define OPENSWMM_JUNCTION_STORAGE 1

typedef enum {
    /*! \brief Node type */
    swmm_NODE_TYPE = 300,
    /*! \brief Elevation */
    swmm_NODE_ELEV,
    /*! \brief Maximum depth */
    swmm_NODE_MAXDEPTH,
    /*! \brief Depth */
    swmm_NODE_DEPTH,
    /*! \brief Hydraulic head */
    swmm_NODE_HEAD,
    /*! \brief Volume */
    swmm_NODE_VOLUME,
    /*! \brief Lateral inflow */
    swmm_NODE_LATFLOW,
    /*! \brief Inflow */
    swmm_NODE_INFLOW,
    /*! \brief Overflow */
    swmm_NODE_OVERFLOW,
    /*! \brief Report flag */
    swmm_NODE_RPTFLAG,
    /*! \brief Surcharge depth */
    swmm_NODE_SURCHARGE_DEPTH,
    /*! \brief Ponded area */
    swmm_NODE_PONDED_AREA,
    /*! \brief Initial depth */
    swmm_NODE_INITIAL_DEPTH,
    /*! \brief Pollutant concentration */
    swmm_NODE_POLLUTANT_CONCENTRATION,
    /*! \brief Pollutant lateral mass flux inflow */
    swmm_NODE_POLLUTANT_LATMASS_FLUX,
    /*! \brief Total outflow */
    swmm_NODE_OUTFLOW,
    swmm_NODE_STORAGE_AREA, /*!< Physical junction chamber area in project ft2/m2 */
} swmm_NodeProperty;

/*!
 * \enum swmm_LinkProperty
 * \brief Enumeration of link properties used in SWMM5
 */
typedef enum {
    /*! \brief Link type */
    swmm_LINK_TYPE = 400,
    /*! \brief Upstream node */
    swmm_LINK_NODE1 = 401,
    /*! \brief Downstream node */
    swmm_LINK_NODE2 = 402,
    /*! \brief Length */
    swmm_LINK_LENGTH = 403,
    /*! \brief Slope */
    swmm_LINK_SLOPE = 404,
    /*! \brief Full depth */
    swmm_LINK_FULLDEPTH = 405,
    /*! \brief Full flow */
    swmm_LINK_FULLFLOW = 406,
    /*! \brief Setting */
    swmm_LINK_SETTING = 407,
    /*! \brief Time open */
    swmm_LINK_TIMEOPEN = 408,
    /*! \brief Time closed */
    swmm_LINK_TIMECLOSED = 409,
    /*! \brief Flow */
    swmm_LINK_FLOW = 410,
    /*! \brief Depth */
    swmm_LINK_DEPTH = 411,
    /*! \brief Velocity */
    swmm_LINK_VELOCITY = 412,
    /*! \brief Top width */
    swmm_LINK_TOPWIDTH = 413,
    /*! \brief Volume */
    swmm_LINK_VOLUME = 414,
    /*! \brief Capacity */
    swmm_LINK_CAPACITY = 415,
    /*! \brief Report flag */
    swmm_LINK_RPTFLAG = 416,
    /*! \brief Upstream invert offset */
    swmm_LINK_OFFSET1 = 417,
    /*! \brief Downstream invert offset */
    swmm_LINK_OFFSET2 = 418,
    /*! \brief Initial flow */
    swmm_LINK_INITIAL_FLOW = 419,
    /*! \brief Flow limit */
    swmm_LINK_FLOW_LIMIT = 420,
    /*! \brief Inlet loss */
    swmm_LINK_INLET_LOSS = 421,
    /*! \brief Outlet loss */
    swmm_LINK_OUTLET_LOSS = 422,
    /*! \brief Average loss */
    swmm_LINK_AVERAGE_LOSS = 423,
    /*! \brief Seepage rate */
    swmm_LINK_SEEPAGE_RATE = 424,
    /*! \brief Flap gate */
    swmm_LINK_HAS_FLAPGATE = 425,
    /*! \brief Pollutant concentration */
    swmm_LINK_POLLUTANT_CONCENTRATION = 426,
    /*! \brief Pollutant load */
    swmm_LINK_POLLUTANT_LOAD = 427,
    /*! \brief Pollutant lateral mass flux */
    swmm_LINK_POLLUTANT_LATMASS_FLUX = 428,
} swmm_LinkProperty;

/*!
 * \enum swmm_PollutProperty
 * \brief Enumeration of pollutant properties used in SWMM5.
 * \details The source concentrations are settable while the simulation is
 * running, enabling dynamic wet-deposition, groundwater, RDII, and dry
 * weather quality forcing. Values are in the pollutant's concentration
 * units (e.g. mg/L) as defined in the input file.
 */
typedef enum {
    /*! \brief Rain (wet deposition) concentration */
    swmm_POLLUT_RAIN_CONCEN = 500,
    /*! \brief Groundwater inflow concentration */
    swmm_POLLUT_GW_CONCEN,
    /*! \brief RDII inflow concentration */
    swmm_POLLUT_RDII_CONCEN,
    /*! \brief Dry weather sanitary flow concentration */
    swmm_POLLUT_DWF_CONCEN,
    /*! \brief First-order decay constant (1/day); read live each step */
    swmm_POLLUT_KDECAY,
    /*! \brief Co-pollutant index (-1 = none) */
    swmm_POLLUT_CO_POLLUTANT,
    /*! \brief Co-pollutant fraction (0-1) */
    swmm_POLLUT_CO_FRACTION,
    /*! \brief Buildup-only-under-snow flag (0/1) */
    swmm_POLLUT_SNOW_ONLY,
    /*! \brief Initial conveyance-network concentration (pre-start only) */
    swmm_POLLUT_INIT_CONCEN,
} swmm_PollutProperty;

/*!
 * \enum swmm_PatternProperty
 * \brief Enumeration of time-pattern properties used in SWMM5.
 * \details Pattern multiplier factors are settable both before the
 * simulation starts and while it is running. They are looked up afresh on
 * every step (DWF/GW/inflow scaling), so a mid-run edit takes effect on the
 * next step. The factor index is passed as the \c subIndex argument of
 * \c swmm_setValueExpanded / \c swmm_getValueExpanded (0-based position
 * within the pattern: 0-11 monthly, 0-6 daily, 0-23 hourly/weekend).
 */
typedef enum {
    /*! \brief One multiplier factor (subIndex = factor position) */
    swmm_PATTERN_FACTOR = 600,
    /*! \brief Number of factors in the pattern (read-only) */
    swmm_PATTERN_COUNT,
    /*! \brief Pattern type code (read-only): 0 monthly, 1 daily, 2 hourly, 3 weekend */
    swmm_PATTERN_TYPE,
} swmm_PatternProperty;

/*!
 * \enum swmm_LanduseProperty
 * \brief Enumeration of land-use properties used in SWMM5.
 * \details Street-sweeping parameters are settable both before the
 * simulation starts and while it is running; they are read per step when
 * sweeping is evaluated, so a mid-run edit takes effect on the next step.
 */
typedef enum {
    /*! \brief Street-sweeping interval (days) */
    swmm_LANDUSE_SWEEP_INTERVAL = 700,
    /*! \brief Fraction of buildup available for sweeping (0-1) */
    swmm_LANDUSE_SWEEP_REMOVAL,
    /*! \brief Buildup function type code (subIndex = pollutant index) */
    swmm_LANDUSE_BUILDUP_FUNC,
    /*! \brief Buildup coefficient c0 / max buildup (subIndex = pollutant) */
    swmm_LANDUSE_BUILDUP_COEFF1,
    /*! \brief Buildup coefficient c1 / rate (subIndex = pollutant) */
    swmm_LANDUSE_BUILDUP_COEFF2,
    /*! \brief Buildup coefficient c2 / exponent (subIndex = pollutant) */
    swmm_LANDUSE_BUILDUP_COEFF3,
    /*! \brief Buildup normalizer code: 0 area, 1 curb length (subIndex = pollutant) */
    swmm_LANDUSE_BUILDUP_NORMALIZER,
    /*! \brief Washoff function type code (subIndex = pollutant) */
    swmm_LANDUSE_WASHOFF_FUNC,
    /*! \brief Washoff coefficient (subIndex = pollutant) */
    swmm_LANDUSE_WASHOFF_COEFF,
    /*! \brief Washoff exponent (subIndex = pollutant) */
    swmm_LANDUSE_WASHOFF_EXPON,
    /*! \brief Washoff street-sweeping removal efficiency 0-1 (subIndex = pollutant) */
    swmm_LANDUSE_WASHOFF_SWEEP_EFFIC,
    /*! \brief Washoff BMP removal efficiency 0-1 (subIndex = pollutant) */
    swmm_LANDUSE_WASHOFF_BMP_EFFIC,
} swmm_LanduseProperty;

/*!
 * \enum swmm_AquiferProperty
 * \brief Enumeration of aquifer properties used in SWMM5.
 * \details Values use input-file units (the [AQUIFERS] line columns). The
 * flux-coefficient properties (conductivity, slopes, evap/loss coefficients)
 * are read live each step, so they are settable both before the simulation
 * starts and while it is running; the structural / initial-condition
 * properties (porosity, wilting point, field capacity, bottom elevation,
 * water table elevation, upper moisture) bound or seed the groundwater state
 * and return \c ERR_API_IS_RUNNING while the simulation is running.
 */
typedef enum {
    /*! \brief Porosity (volumetric fraction, pre-start-only) */
    swmm_AQUIFER_POROSITY = 800,
    /*! \brief Wilting point (volumetric fraction, pre-start-only) */
    swmm_AQUIFER_WILTING_POINT,
    /*! \brief Field capacity (volumetric fraction, pre-start-only) */
    swmm_AQUIFER_FIELD_CAPACITY,
    /*! \brief Saturated hydraulic conductivity (in/hr or mm/hr) */
    swmm_AQUIFER_CONDUCTIVITY,
    /*! \brief Conductivity slope (unitless) */
    swmm_AQUIFER_CONDUCT_SLOPE,
    /*! \brief Tension slope (ft or m) */
    swmm_AQUIFER_TENSION_SLOPE,
    /*! \brief Upper-zone evaporation fraction (0-1) */
    swmm_AQUIFER_UPPER_EVAP_FRAC,
    /*! \brief Lower-zone evaporation depth (ft or m) */
    swmm_AQUIFER_LOWER_EVAP_DEPTH,
    /*! \brief Lower-zone seepage-loss coefficient (in/hr or mm/hr) */
    swmm_AQUIFER_LOWER_LOSS_COEFF,
    /*! \brief Aquifer bottom elevation (ft or m, pre-start-only) */
    swmm_AQUIFER_BOTTOM_ELEV,
    /*! \brief Initial water table elevation (ft or m, pre-start-only) */
    swmm_AQUIFER_WATER_TABLE_ELEV,
    /*! \brief Initial upper-zone moisture (volumetric fraction, pre-start-only) */
    swmm_AQUIFER_UPPER_MOISTURE,
} swmm_AquiferProperty;

/*!
 * \enum swmm_SystemProperty
 * \brief Enumeration of system properties used in SWMM5
 */
typedef enum {
    /*! \brief Start date */
    swmm_STARTDATE = 0,
    /*! \brief Current date */
    swmm_CURRENTDATE = 1,
    /*! \brief Elapsed time */
    swmm_ELAPSEDTIME = 2,
    /*! \brief Routing step */
    swmm_ROUTESTEP = 3,
    /*! \brief Maximum routing step */
    swmm_MAXROUTESTEP = 4,
    /*! \brief Reporting step */
    swmm_REPORTSTEP = 5,
    /*! \brief Total steps */
    swmm_TOTALSTEPS = 6,
    /*! \brief No report flag */
    swmm_NOREPORT = 7,
    /*! \brief Flow units */
    swmm_FLOWUNITS = 8,
    /*! \brief End date */
    swmm_ENDDATE = 9,
    /*! \brief Report start */
    swmm_REPORTSTART = 10,
    /*! \brief Unit system */
    swmm_UNITSYSTEM = 11,
    /*! \brief Surcharge method */
    swmm_SURCHARGEMETHOD = 12,
    /*! \brief Allow ponding */
    swmm_ALLOWPONDING = 13,
    /*! \brief Inertia damping */
    swmm_INERTIADAMPING = 14,
    /*! \brief Normal flow limited */
    swmm_NORMALFLOWLTD = 15,
    /*! \brief Skip steady state */
    swmm_SKIPSTEADYSTATE = 16,
    /*! \brief Ignore rainfall */
    swmm_IGNORERAINFALL = 17,
    /*! \brief Ignore RDII */
    swmm_IGNORERDII = 18,
    /*! \brief Ignore snowmelt */
    swmm_IGNORESNOWMELT = 19,
    /*! \brief Ignore groundwater */
    swmm_IGNOREGROUNDWATER = 20,
    /*! \brief Ignore routing */
    swmm_IGNOREROUTING = 21,
    /*! \brief Ignore quality */
    swmm_IGNOREQUALITY = 22,
    /*! \brief Error code */
    swmm_ERROR_CODE = 23,
    /*! \brief Rule step */
    swmm_RULESTEP = 24,
    /*! \brief Sweep start */
    swmm_SWEEPSTART = 25,
    /*! \brief Sweep end */
    swmm_SWEEPEND = 26,
    /*! \brief Maximum trials */
    swmm_MAXTRIALS = 27,
    /*! \brief Number of threads */
    swmm_NUMTHREADS = 28,
    /*! \brief Minimum routing step */
    swmm_MINROUTESTEP = 29,
    /*! \brief Lengthening step */
    swmm_LENGTHENINGSTEP = 30,
    /*! \brief Start dry days */
    swmm_STARTDRYDAYS = 31,
    /*! \brief Courant factor */
    swmm_COURANTFACTOR = 32,
    /*! \brief Minimum surface area */
    swmm_MINSURFAREA = 33,
    /*! \brief Minimum slope */
    swmm_MINSLOPE = 34,
    /*! \brief Runoff error */
    swmm_RUNOFFERROR = 35,
    /*! \brief Flow error */
    swmm_FLOWERROR = 36,
    /*! \brief Quality error */
    swmm_QUALERROR = 37,
    /*! \brief Head tolerance */
    swmm_HEADTOL = 38,
    /*! \brief System flow tolerance */
    swmm_SYSFLOWTOL = 39,
    /*! \brief Lateral flow tolerance */
    swmm_LATFLOWTOL = 40,
    /*! \brief Current climate-derived evaporation rate (in/day or mm/day),
     *         including any monthly adjustments (read-only) */
    swmm_EVAPRATE = 41,
    /*! \brief Current air temperature (deg F or deg C; read-only) */
    swmm_TEMPERATURE = 42,
    /*! \brief API prescribed air temperature (deg F or deg C). Settable
     *         while the simulation is running; overrides the climate
     *         data-source value (and bypasses monthly adjustments) for
     *         snowmelt and evaporation. Set a value <= -999 to clear and
     *         revert to climate-derived temperature. Getter returns -999
     *         when no prescription is active. */
    swmm_API_TEMPERATURE = 43,
    /*! \brief Current wind speed (mph or km/hr; read-only) */
    swmm_WINDSPEED = 44,
    /*! \brief API prescribed wind speed (mph or km/hr). Settable while the
     *         simulation is running; overrides the monthly/file value used
     *         in snowmelt. Set a negative value to clear. Getter returns
     *         -1 when no prescription is active. */
    swmm_API_WINDSPEED = 45,
    /*! \brief API prescribed system-wide evaporation rate (in/day or
     *         mm/day). Settable while the simulation is running; replaces
     *         the climate-derived Evap.rate (after monthly adjustments)
     *         for every consumer — subcatchments, LID units, groundwater,
     *         conduits and storage nodes. Per-subcatchment
     *         swmm_SUBCATCH_API_PET still takes precedence. Set a negative
     *         value to clear. Getter returns -1 when not prescribed. */
    swmm_API_EVAP = 46,
    /*! \brief Evaporation DRY_ONLY option (0/1). Settable while the
     *         simulation is running. */
    swmm_EVAP_DRY_ONLY = 47,
} swmm_SystemProperty;

/*!
 * \enum swmm_FlowUnitsProperty
 * \brief Enumeration of flow units used in SWMM5
 */
typedef enum {
    /*! \brief Cubic feet per second */
    swmm_CFS = 0,
    /*! \brief Gallons per minute */
    swmm_GPM = 1,
    /*! \brief Million gallons per day */
    swmm_MGD = 2,
    /*! \brief Cubic meters per second */
    swmm_CMS = 3,
    /*! \brief Liters per second */
    swmm_LPS = 4,
    /*! \brief Million liters per day */
    swmm_MLD = 5
} swmm_FlowUnitsProperty;

/*!
 * \enum swmm_API_Errors
 * \brief Enumeration of API errors used in SWMM5
 */
typedef enum {
    /*! \brief API error for file not opened */
    ERR_API_NOT_OPEN = -999901,
    /*! \brief API error for API not started */
    ERR_API_NOT_STARTED = -999902,
    /*! \brief API error for API not ended */
    ERR_API_NOT_ENDED = -999903,
    /*! \brief API error for errorneous object type */
    ERR_API_OBJECT_TYPE = -999904,
    /*! \brief API error for errorneous object index */
    ERR_API_OBJECT_INDEX = -999905,
    /*! \brief API error for errorneous object name */
    ERR_API_OBJECT_NAME = -999906,
    /*! \brief API error for errorneous property type */
    ERR_API_PROPERTY_TYPE = -999907,
    /*! \brief API error for errorneous property value */
    ERR_API_PROPERTY_VALUE = -999908,
    /*! \brief API error for errorneous time period */
    ERR_API_TIME_PERIOD = -999909,
    /*! \brief API error for errorneous hotstart file open */
    ERR_API_HOTSTART_FILE_OPEN = -999910,
    /*! \brief API error for errorneous hotstart file format */
    ERR_API_HOTSTART_FILE_FORMAT = -999911,
    /*! \brief API error for API already running */
    ERR_API_IS_RUNNING = -999912,
} swmm_API_Errors;

/* ====================================================================
 *  Statistics and mass balance structures
 * ==================================================================== */

/*!
 * \brief Maximum number of flow classification classes for link statistics.
 */
#define SWMM_MAX_FLOW_CLASSES 7

/*!
 * \struct swmm_SubcatchStats
 * \brief Cumulative subcatchment statistics.
 */
typedef struct {
    double precip;       /*!< Total precipitation volume (ft3) */
    double runon;        /*!< Runon volume (ft3) */
    double evap;         /*!< Evaporation volume (ft3) */
    double infil;        /*!< Infiltration volume (ft3) */
    double runoff;       /*!< Runoff volume (ft3) */
    double maxFlow;      /*!< Maximum runoff rate (cfs) */
    double impervRunoff; /*!< Impervious runoff volume (ft3) */
    double pervRunoff;   /*!< Pervious runoff volume (ft3) */
} swmm_SubcatchStats;

/*!
 * \struct swmm_NodeStats
 * \brief Cumulative node statistics.
 */
typedef struct {
    double avgDepth;            /*!< Raw sum of sampled depths (ft), not a normalized average */
    double maxDepth;            /*!< Maximum node depth (ft) */
    double maxDepthDate;        /*!< Absolute SWMM day of maximum depth */
    double maxRptDepth;         /*!< Maximum report-step depth (ft) */
    double volFlooded;          /*!< Flooded volume (ft3) */
    double timeFlooded;         /*!< Total time flooded (seconds) */
    double timeSurcharged;      /*!< Total time surcharged (seconds) */
    double timeCourantCritical; /*!< Number of Courant-critical events, not duration */
    double totLatFlow;          /*!< Lateral inflow volume (ft3) */
    double maxLatFlow;          /*!< Maximum lateral inflow (cfs) */
    double maxInflow;           /*!< Maximum total inflow (cfs) */
    double maxOverflow;         /*!< Maximum flooding rate (cfs) */
    double maxPondedVol;        /*!< Maximum ponded volume (ft3) */
    double maxInflowDate;       /*!< Absolute SWMM day of maximum inflow */
    double maxOverflowDate;     /*!< Absolute SWMM day of maximum overflow */
} swmm_NodeStats;

/*!
 * \struct swmm_StorageStats
 * \brief Cumulative storage node statistics.
 */
typedef struct {
    double initVol;     /*!< Initial volume (ft3) */
    double avgVol;      /*!< Raw sum of sampled volumes (ft3), not a normalized average */
    double maxVol;      /*!< Maximum volume (ft3) */
    double maxFlow;     /*!< Maximum total inflow (cfs) */
    double evapLosses;  /*!< Evaporation volume (ft3) */
    double exfilLosses; /*!< Exfiltration volume (ft3) */
    double maxVolDate;  /*!< Absolute SWMM day of maximum volume */
} swmm_StorageStats;

/*!
 * \struct swmm_OutfallStats
 * \brief Cumulative outfall node statistics.
 */
typedef struct {
    double
        avgFlow; /*!< Raw sum of sampled outflow rates (cfs); divide by totalPeriods for the sample mean */
    double maxFlow; /*!< Maximum outflow rate (cfs) */
    double*
        totalLoad; /*!< Optional caller-owned Npollut array in native concentration-volume accounting units */
    int totalPeriods; /*!< Number of included outflow samples */
} swmm_OutfallStats;

/*!
 * \struct swmm_LinkStats
 * \brief Cumulative link statistics.
 */
typedef struct {
    double maxFlow;             /*!< Maximum flow rate (cfs) */
    double maxFlowDate;         /*!< Absolute SWMM day of maximum flow */
    double maxVeloc;            /*!< Maximum velocity (ft/s) */
    double maxDepth;            /*!< Maximum depth (ft) */
    double timeNormalFlow;      /*!< Time in normal flow (seconds) */
    double timeInletControl;    /*!< Time under inlet control (seconds) */
    double timeSurcharged;      /*!< Time surcharged (seconds) */
    double timeFullUpstream;    /*!< Time full at upstream (seconds) */
    double timeFullDnstream;    /*!< Time full at downstream (seconds) */
    double timeFullFlow;        /*!< Time at full flow (seconds) */
    double timeCapacityLimited; /*!< Time capacity-limited (seconds) */
    double timeInFlowClass
        [SWMM_MAX_FLOW_CLASSES]; /*!< Seconds accumulated in each fixed flow-class slot */
    double timeCourantCritical;  /*!< Number of Courant-critical events, not duration */
    long flowTurns;              /*!< Number of flow reversals */
    int flowTurnSign;            /*!< Current flow direction sign */
} swmm_LinkStats;

/*!
 * \struct swmm_PumpStats
 * \brief Cumulative pump statistics.
 */
typedef struct {
    double utilized;     /*!< Accumulated utilization time (seconds) */
    double minFlow;      /*!< Minimum sampled pumping rate (cfs) */
    double avgFlow;      /*!< Raw sum of sampled pumping rates (cfs), not a normalized average */
    double maxFlow;      /*!< Maximum pumping rate (cfs) */
    double volume;       /*!< Pumped volume (ft3) */
    double energy;       /*!< Accumulated pumping energy (kWh) */
    double offCurveLow;  /*!< Time below the pump curve (seconds) */
    double offCurveHigh; /*!< Time above the pump curve (seconds) */
    int startUps;        /*!< Number of pump start-ups */
    int totalPeriods;    /*!< Number of included pumping samples */
} swmm_PumpStats;

/*!
 * \struct swmm_RoutingTotals
 * \brief System-level flow routing mass balance totals (raw ft3; pctError is percent).
 */
typedef struct {
    double dwInflow;     /*!< Dry weather inflow volume */
    double wwInflow;     /*!< Wet weather inflow volume */
    double gwInflow;     /*!< Groundwater inflow volume */
    double iiInflow;     /*!< RDII inflow volume */
    double exInflow;     /*!< Direct/external inflow volume */
    double flooding;     /*!< Internal flooding volume */
    double outflow;      /*!< External outflow volume */
    double evapLoss;     /*!< Evaporation loss volume */
    double seepLoss;     /*!< Seepage loss volume */
    double reacted;      /*!< Reaction losses (quality) */
    double initStorage;  /*!< Initial storage volume */
    double finalStorage; /*!< Final storage volume */
    double pctError;     /*!< Continuity error (%) */
} swmm_RoutingTotals;

/*!
 * \struct swmm_RunoffTotals
 * \brief System-level surface runoff mass balance totals.
 */
typedef struct {
    double rainfall;       /*!< Total rainfall volume (ft3) */
    double evap;           /*!< Evaporation loss (volume) */
    double infil;          /*!< Infiltration loss (volume) */
    double runoff;         /*!< Runoff volume */
    double drains;         /*!< LID drain volume */
    double runon;          /*!< Runon from outfalls (volume) */
    double initStorage;    /*!< Initial surface storage (ft3) */
    double finalStorage;   /*!< Final surface storage (ft3) */
    double initSnowCover;  /*!< Initial snow cover (ft3) */
    double finalSnowCover; /*!< Final snow cover (ft3) */
    double snowRemoved;    /*!< Snow removal (ft3) */
    double pctError;       /*!< Continuity error (%) */
} swmm_RunoffTotals;

/**
 * @brief Receive synchronous legacy simulation progress.
 *
 * Borrowed function pointer used during swmm_run_with_callback; no return value/cancellation
 * channel. Do not reenter or concurrently use the process-global legacy engine.
 *
 * @param[in] progress NewRoutingTime/TotalDuration fraction supplied after an attempted step;
 *     caller should handle completion/error callbacks and does not own engine state.
 */
typedef void (*progress_callback)(double progress);

/**
 * @brief Run the global legacy engine through open/start/step/end/report/close.
 *
 * Resets global open/start flags and errors, saves binary results, writes console progress and
 * closes resources before returning. Writes the full text report only for scratch output when no
 * error occurred. Operates on one process-global project; do not overlap another legacy simulation.
 *
 * @param[in] inputFile Non-null null-terminated input-deck path, borrowed for the call.
 * @param[in] reportFile Non-null null-terminated report path; the engine opens/overwrites the
 *     report.
 * @param[in] outputFile Non-null null-terminated binary output path; an empty string selects a
 *     scratch output file.
 * @return Final process-global ErrorCode, zero for a successful run.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_run(const char* inputFile, const char* reportFile,
                                            const char* outputFile);

/**
 * @brief Run the global legacy engine and report synchronous step progress.
 *
 * Uses the same lifecycle and scratch-report behavior as swmm_run. Progress is
 * NewRoutingTime/TotalDuration; this entry point adds no clamp or zero-duration guard. Callback
 * execution occurs on the simulation thread and must not reenter or concurrently mutate the global
 * engine.
 *
 * @param[in] inputFile Non-null null-terminated input-deck path, borrowed for the call.
 * @param[in] reportFile Non-null null-terminated report path; the engine opens/overwrites the
 *     report.
 * @param[in] outputFile Non-null null-terminated binary output path; an empty string selects a
 *     scratch output file.
 * @param[in] callback Optional borrowed function pointer called after each attempted step,
 *     including completion/error; null disables progress. Return type is void, so it cannot cancel.
 * @return Final process-global ErrorCode.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_run_with_callback(const char* inputFile,
                                                          const char* reportFile,
                                                          const char* outputFile,
                                                          progress_callback callback);

/**
 * @brief Open, parse and validate a single process-global legacy model.
 *
 * Resets global error/warning/lifecycle state, opens files, reads the deck and validates it. On a
 * failure after files/project allocation, call swmm_close to release partial resources. Requires
 * the previous project to have been closed; this function resets flags rather than safely disposing
 * an already-open model.
 *
 * @param[in] inputFile Non-null null-terminated input-deck path, borrowed for the call.
 * @param[in] reportFile Non-null null-terminated report path; the engine opens/overwrites the
 *     report.
 * @param[in] outputFile Non-null null-terminated binary output path; an empty string selects a
 *     scratch output file.
 * @return Zero on success or the legacy engine error enum. Lifecycle errors can also set the
 *     process-global ErrorCode.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_open(const char* inputFile, const char* reportFile,
                                             const char* outputFile);

/**
 * @brief Initialize the opened model and its computing systems.
 *
 * Requires successful swmm_open and no active run. Resets clocks, initializes
 * runoff/routing/quality, opens output and rainfall/hotstart interfaces, mass balance and
 * statistics. A failed start may already have marked the model started and allocated systems;
 * finalize/close through the lifecycle before reuse.
 *
 * @param[in] saveFlag Zero suppresses periodic binary-result saving; nonzero enables it.
 * @return Zero on success or the legacy engine error enum. Lifecycle errors can also set the
 *     process-global ErrorCode.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_start(int saveFlag);

/**
 * @brief Advance one routing step and save any scheduled results.
 *
 * Requires a successfully started open model. Advances associated runoff/quality processors as
 * needed. The pointer is used only during the call; no wall-clock duration or callback scheduling
 * is implied.
 *
 * @param[out] elapsedTime Non-null writable double, set to elapsed days since simulation start, or
 *     zero at completion; initialized to zero before lifecycle validation.
 * @return Zero on success or the legacy engine error enum. Lifecycle errors can also set the
 *     process-global ErrorCode.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_step(double* elapsedTime);

/**
 * @brief Advance routing up to a requested interval while retaining the configured step afterward.
 *
 * Requires open/started state. Repeatedly steps until the temporary end, then restores original
 * RouteStep and full RoutingDuration, and exposes the actual elapsed time. Existing global errors
 * are returned without advancing.
 *
 * @param[in] strideStep Requested stride in seconds; <= 0 delegates to one swmm_step. A positive
 *     stride temporarily caps RouteStep if smaller and stops at model end.
 * @param[out] elapsedTime Non-null writable elapsed-days slot; positive when more model time
 *     remains, zero at final completion or before validation.
 * @return Zero on success or the legacy engine error enum. Lifecycle errors can also set the
 *     process-global ErrorCode.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_stride(int strideStep, double* elapsedTime);

/**
 * @brief Select a validated hotstart input for the next start.
 *
 * Requires an open model before swmm_start. Validation errors are returned locally without storing
 * them as a new terminating global ErrorCode; a preexisting global error still blocks the call.
 * Success stores the path/mode; actual state loading occurs at start.
 *
 * @param[in] hotStartFile Non-null null-terminated filename, copied up to MAXFNAME characters and
 *     resolved through the legacy input-directory path rules.
 * @return Zero on success or the legacy engine error enum. Lifecycle errors can also set the
 *     process-global ErrorCode.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_useHotStart(const char* hotStartFile);

/**
 * @brief Write current running state to a named hotstart file.
 *
 * Requires an open started model and no prior global error. The writer may create/overwrite the
 * destination; writer errors are returned directly.
 *
 * @param[in] hotStartFile Non-null null-terminated destination passed to the hotstart writer;
 *     borrowed only during the call.
 * @return Zero on success or the legacy engine error enum. Lifecycle errors can also set the
 *     process-global ErrorCode.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_saveHotStart(const char* hotStartFile);

/**
 * @brief Finalize output/continuity and close computing systems while retaining model/statistics.
 *
 * Requires an open project. If started, writes final binary records, finalizes
 * mass-balance/statistics reports when no error and reporting enabled, closes computing systems and
 * clears the started flag. Calling again on an open ended model performs no additional
 * finalization. Statistics remain allocated until swmm_close.
 * @return Zero on success or the legacy engine error enum. Lifecycle errors can also set the
 *     process-global ErrorCode.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_end(void);

/**
 * @brief Write the full results report from the current legacy project.
 *
 * Caller must provide an opened model with appropriate completed output/report state. This wrapper
 * checks only ErrorCode; it does not enforce a separate lifecycle condition. Normally call after
 * swmm_end and before swmm_close.
 * @return Current ErrorCode after report_writeReport, or the existing error without writing.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_report(void);

/**
 * @brief Release global statistics/model/output resources and close project files.
 *
 * Clears open/started flags and each closed file pointer; repeated explicit closes avoid double
 * fclose. Deletes scratch output where applicable. Does not itself run swmm_end, so finish an
 * active simulation first to produce final output and accounting. Previously borrowed model storage
 * becomes invalid.
 * @return Always zero.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_close(void);

/**
 * @brief Read finalized runoff, routing and quality continuity errors.
 *
 * Call after swmm_end and before swmm_close for final values. These are percentages (0.1 means 0.1
 * percent), unlike the worker JSON fractions. Outputs are copied, not retained.
 *
 * @param[out] runoffErr Non-null writable percent-error slot.
 * @param[out] flowErr Non-null writable routing percent-error slot.
 * @param[out] qualErr Non-null writable quality percent-error slot.
 * @return Always zero; outputs are zero unless the model is open and not started.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_getMassBalErr(float* runoffErr, float* flowErr,
                                                      float* qualErr);

/**
 * @brief Recompute current runoff and routing continuity errors during a run.
 *
 * Uses live water accumulators/current storage between steps and does not change the eventual
 * finalized errors. Values are percentages; caller storage is not retained.
 *
 * @param[out] runoffErr Non-null writable runoff percent-error slot.
 * @param[out] flowErr Non-null writable routing percent-error slot.
 * @return Always zero; both outputs are zero unless the model is open and started.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_getRunningMassBalErr(float* runoffErr, float* flowErr);

/**
 * @brief Return the compiled legacy engine version integer.
 * @return VERSION, without requiring an open model.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_getVersion(void);

/**
 * @brief Copy the current terminating error text into caller-owned storage.
 *
 * Lazily populates ErrorMsg from ErrorCode when necessary, then changes a leading newline in the
 * copy to a space. A negative length converts to an unsigned copy bound and is invalid. The buffer
 * is used synchronously only.
 *
 * @param[out] errMsg Non-null writable character buffer of at least msgLen + 1 bytes; sstrncpy can
 *     copy msgLen payload characters and append a terminator.
 * @param[in] msgLen Nonnegative maximum number of payload characters, not total buffer capacity;
 *     zero still writes a terminator.
 * @return Current global ErrorCode; does not clear it.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_getError(char* errMsg, int msgLen);

/**
 * @brief Copy the text for an explicit internal legacy error enum.
 *
 * The unusual declaration is an array of character pointers, not a character buffer. No
 * null/capacity checks are performed and no storage is retained; unknown-code behavior follows
 * error_getMsg.
 *
 * @param[in] errorCode Legacy internal error enum passed to error_getMsg, independent of global
 *     ErrorCode.
 * @param[out] outErrMsg Required pointer-array argument; only *outErrMsg is used and must reference
 *     writable storage for the bounded MAXMSG copy plus its terminator (reserve 1025 bytes).
 * @return Always zero after lookup/copy.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_getErrorFromCode(int errorCode, char* outErrMsg[1024]);

/**
 * @brief Return the current project warning counter.
 * @return Global number of warnings, reset at swmm_open; no lifecycle validation is performed.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_getWarnings(void);

/**
 * @brief Receive one process-global legacy engine warning.
 *
 * Runs on the engine thread. Avoid reentrant lifecycle/model edits and keep callback code/userData
 * alive until replacement or disabling.
 *
 * @param[in] message Borrowed null-terminated warning text valid only during this synchronous
 *     callback; copy it to retain.
 * @param[in] userData Opaque pointer supplied to swmm_setWarningCallback; ownership remains with
 *     the registering caller.
 */
typedef void (*swmm_LegacyWarningCallback)(const char* message, void* userData);

/**
 * @brief Retain a process-global callback for each emitted warning.
 *
 * Register before swmm_open to capture parse warnings. Invocations are synchronous on the legacy
 * engine thread; avoid reentrancy and concurrent access to global engine state.
 *
 * @param[in] cb Callback pointer to retain, or null to disable. Its code must remain valid until
 *     replaced/disabled.
 * @param[in] userData Opaque pointer retained and supplied unchanged; caller owns its lifetime
 *     until replacement/disable.
 * @return Always zero.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_setWarningCallback(swmm_LegacyWarningCallback cb,
                                                           void* userData);

/**
 * @brief Get the number of objects in a legacy category.
 *
 * @param[in] objType Legacy object category from swmm_GAGE up to but excluding swmm_SYSTEM.
 * @return Object count, zero when no project is open, or ERR_API_OBJECT_TYPE for an unsupported
 *     category. Errors share the integer result space with counts.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_getCount(int objType);

/**
 * @brief Copy the ID of one named legacy object.
 *
 * @param[in] objType Named category: gage, subcatchment, node, link, pollutant, landuse, time
 *     pattern, curve, time series, transect, aquifer or unit hydrograph/snowmelt where supported.
 * @param[in] index Zero-based object index strictly below the category count.
 * @param[out] name Non-null writable buffer, initially cleared; reserve at least size + 1 bytes for
 *     the legacy bounded copy.
 * @param[in] size Nonnegative maximum payload characters, excluding the final terminator.
 * @return Zero on success, or ERR_API_NOT_OPEN / ERR_API_OBJECT_TYPE / ERR_API_OBJECT_INDEX. Output
 *     remains empty on validation failure.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_getName(int objType, int index, char* name, int size);

/**
 * @brief Find a named object in the opened global model.
 *
 * @param[in] objType Supported category in [swmm_GAGE,swmm_SYSTEM).
 * @param[in] name Non-null null-terminated lookup name; borrowed only during the call.
 * @return Zero-based index or -1 when project_findObject cannot find the name; ERR_API_NOT_OPEN /
 *     ERR_API_OBJECT_TYPE on validation failure.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_getIndex(int objType, const char* name);

/**
 * @brief Read one legacy numeric-family property.
 *
 * Requires an open model. Dispatches by numeric property band: <100 system, <200 gage, <300
 * subcatchment, <400 node, <500 link; passes -1 for sub/pollutant indices. Prefer the Expanded
 * interface for explicit type/subobject selection.
 *
 * @param[in] property Property enum for the selected object family; system/date/step, gage,
 *     subcatchment, node and link values follow the public property enums.
 * @param[in] index Zero-based object index; ignored by system properties. Other families validate
 *     against their object count.
 * @return Property value in its declared/model units, or a legacy API error enum encoded as double.
 *     Numeric values and errors share the same return channel.
 */
double EXPORT_OPENSWMMCORE_SOLVER_API swmm_getValue(int property, int index);

/**
 * @brief Read one explicitly typed property.
 *
 * Requires an open model. Dispatches to the matching property implementation; no uniform validation
 * or transaction applies across properties. Buffers/strings are not involved.
 *
 * @param[in] objType swmm_SYSTEM, GAGE, SUBCATCH, NODE, LINK, POLLUTANT, TIME_PATTERN, LANDUSE or
 *     AQUIFER; selects the property implementation.
 * @param[in] property Property enum for the selected object family; system/date/step, gage,
 *     subcatchment, node and link values follow the public property enums.
 * @param[in] index Zero-based object index; ignored by system properties. Other families validate
 *     against their object count.
 * @param[in] subIndex Property-specific subobject (e.g. subarea 0..2, pattern factor or landuse
 *     entry); use the relevant property range, as it is not a universal optional index.
 * @param[in] pollutantIndex Zero-based pollutant index for pollutant-dependent
 *     node/link/subcatchment properties; unused by other properties.
 * @return Property value in its declared/model units, or a legacy API error enum encoded as double.
 *     Numeric values and errors share the same return channel.
 */
double EXPORT_OPENSWMMCORE_SOLVER_API swmm_getValueExpanded(int objType, int property, int index,
                                                            int subIndex, int pollutantIndex);

/**
 * @brief Set one legacy numeric-family property.
 *
 * Requires an open model. The compatibility setter supports rainfall/scale, reporting flags, node
 * lateral flow/outfall stage, link setting and route/report controls. Reporting edits generally
 * require not started. Prefer the Expanded interface for explicit type/subobject selection.
 *
 * @param[in] property Property enum for the selected object family; system/date/step, gage,
 *     subcatchment, node and link values follow the public property enums.
 * @param[in] index Zero-based object index; ignored by system properties. Other families validate
 *     against their object count.
 * @param[in] value New value in the selected property units; hydraulic lengths/flows and rainfall
 *     generally use model display units, dates absolute SWMM days, step sizes seconds, and
 *     fractions dimensionless. Property-specific range/lifecycle checks apply.
 * @return Zero or a legacy API object/property/value/state error. Some unsupported
 *     indices/lifecycle edits are silently ignored with zero, so zero does not guarantee mutation.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_setValue(int property, int index, double value);

/**
 * @brief Set one explicitly typed property.
 *
 * Requires an open model. Dispatches to the matching property implementation; no uniform validation
 * or transaction applies across properties. Buffers/strings are not involved.
 *
 * @param[in] objType swmm_SYSTEM, GAGE, SUBCATCH, NODE, LINK, POLLUTANT, TIME_PATTERN, LANDUSE or
 *     AQUIFER; selects the property implementation.
 * @param[in] property Property enum for the selected object family; system/date/step, gage,
 *     subcatchment, node and link values follow the public property enums.
 * @param[in] index Zero-based object index; ignored by system properties. Other families validate
 *     against their object count.
 * @param[in] subIndex Property-specific subobject (e.g. subarea 0..2, pattern factor or landuse
 *     entry); use the relevant property range, as it is not a universal optional index.
 * @param[in] pollutantIndex Zero-based pollutant index for pollutant-dependent
 *     node/link/subcatchment properties; unused by other properties.
 * @param[in] value New value in the selected property units; hydraulic lengths/flows and rainfall
 *     generally use model display units, dates absolute SWMM days, step sizes seconds, and
 *     fractions dimensionless. Property-specific range/lifecycle checks apply.
 * @return Zero or a legacy API object/property/value/state error. Some unsupported
 *     indices/lifecycle edits are silently ignored with zero, so zero does not guarantee mutation.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_setValueExpanded(int objType, int property, int index,
                                                         int subIndex, int pollutantIndex,
                                                         double value);

/**
 * @brief Replace a node/pollutant treatment expression for subsequent routing steps.
 *
 * May be called before or during a run. The old equation is freed before parsing its replacement;
 * parse failure leaves no equation for the pair. No rollback restores the old expression and the
 * caller string is not retained.
 *
 * @param[in] nodeIndex Zero-based node index within the opened model.
 * @param[in] pollutantIndex Zero-based pollutant index.
 * @param[in] expression Non-null nonempty null-terminated input-form equation, e.g. R = 0.5 or C =
 *     BOD * 0.2; copied into a bounded MAXLINE working buffer.
 * @return Zero on success; ERR_API_NOT_OPEN, ERR_API_OBJECT_INDEX or ERR_API_PROPERTY_VALUE on
 *     rejected lifecycle/index/expression.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_setTreatment(int nodeIndex, int pollutantIndex,
                                                     const char* expression);

/**
 * @brief Remove a node/pollutant treatment equation.
 *
 * Requires an open model, before or during a run. Frees the owned equation and leaves the pair
 * without treatment removal.
 *
 * @param[in] nodeIndex Zero-based node index.
 * @param[in] pollutantIndex Zero-based pollutant index.
 * @return Zero on success (including an already absent equation), or ERR_API_NOT_OPEN /
 *     ERR_API_OBJECT_INDEX.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_clearTreatment(int nodeIndex, int pollutantIndex);

/**
 * @brief Replace live underdrain parameters of a legacy LID process.
 *
 * Edits take effect on the next kernel evaluation. The validator tests negativity without an
 * explicit finite-value check; callers should supply finite physical parameters.
 *
 * @param[in] lidIndex Zero-based LID process index.
 * @param[in] coeff Nonnegative coefficient in authored rain-rate units (in/hr or mm/hr) for the
 *     configured head exponent.
 * @param[in] expon Nonnegative dimensionless head exponent.
 * @param[in] offset Nonnegative drain offset in in or mm, converted internally to ft.
 * @return Zero, ERR_API_NOT_OPEN, ERR_API_OBJECT_INDEX, or ERR_API_PROPERTY_VALUE for a negative
 *     parameter.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_setLidDrain(int lidIndex, double coeff, double expon,
                                                    double offset);

/**
 * @brief Read a saved output value after the run has ended.
 *
 * Requires an open ended model with readable saved output. Numeric zero is ambiguous with
 * unavailable data. Does not retain caller storage.
 *
 * @param[in] property swmm_CURRENTDATE or a supported subcatch/node/link result property.
 * @param[in] index Zero-based object index, ignored for the saved date.
 * @param[in] period One-based saved reporting period in [1,Nperiods], unlike the separate
 *     output-reader zero-based API.
 * @return Saved value in model display units (date in absolute SWMM days), or zero if closed, still
 *     started, invalid period/property, or an unsupported/missing value.
 */
double EXPORT_OPENSWMMCORE_SOLVER_API swmm_getSavedValue(int property, int index, int period);

/**
 * @brief Append a caller line to the open report.
 *
 * Does nothing if no project is open. Report-write failures are not separately returned by this
 * void wrapper.
 *
 * @param[in] line Non-null null-terminated report text; borrowed during the call.
 */
void EXPORT_OPENSWMMCORE_SOLVER_API swmm_writeLine(const char* line);

/**
 * @brief Decode an absolute legacy SWMM date/time into calendar fields.
 *
 * No model lifecycle is required; valid encodable dates and non-null outputs are caller
 * preconditions. Uses the legacy floor/whole-second conventions and does not retain pointers.
 *
 * @param[in] date Encoded date/time in decimal days relative to the legacy epoch 1899-12-30.
 * @param[out] year Non-null writable year.
 * @param[out] month Non-null writable month 1..12 for a valid date.
 * @param[out] day Non-null writable calendar day.
 * @param[out] hour Non-null writable hour.
 * @param[out] minute Non-null writable minute.
 * @param[out] second Non-null writable whole second.
 * @param[out] dayOfWeek Non-null writable weekday: Sunday=1 through Saturday=7, as returned by
 *     datetime_dayOfWeek.
 */
void EXPORT_OPENSWMMCORE_SOLVER_API swmm_decodeDate(double date, int* year, int* month, int* day,
                                                    int* hour, int* minute, int* second,
                                                    int* dayOfWeek);

/**
 * @brief Encode calendar date and clock fields using the legacy date utilities.
 *
 * @param[in] year Calendar year accepted by datetime_encodeDate.
 * @param[in] month Month 1..12.
 * @param[in] day Valid calendar day for the month/year.
 * @param[in] hour Clock hour 0..23.
 * @param[in] minute Minute 0..59.
 * @param[in] second Whole second 0..59.
 * @return datetime_encodeDate + datetime_encodeTime, in absolute SWMM decimal days. Invalid
 *     component sentinel behavior is inherited from those helpers; this wrapper does not validate
 *     the combined result.
 */
double EXPORT_OPENSWMMCORE_SOLVER_API swmm_encodeDate(int year, int month, int day, int hour,
                                                      int minute, int second);

/* ====================================================================
 *  Statistics and mass balance API functions
 *
 *  Call these after swmm_end() but before swmm_close().
 * ==================================================================== */

/**
 * @brief Copy raw internal subcatchment statistics.
 *
 * Call after swmm_end and before swmm_close for final data. Checks array presence, not explicit
 * lifecycle flags.  No display-unit conversion or averaging normalization is performed.
 *
 * @param[in] index Zero-based global subcatchment index; subtype must match subcatchment.
 * @param[out] stats Non-null caller-owned destination, borrowed only during the call. Volumes ft3,
 *     peak flow cfs.
 * @return Zero on success, ERR_API_NOT_ENDED if the statistics array is absent,
 *     ERR_API_OBJECT_INDEX for an invalid index.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_getSubcatchStats(int index, swmm_SubcatchStats* stats);

/**
 * @brief Copy raw internal node statistics.
 *
 * Call after swmm_end and before swmm_close for final data. Checks array presence, not explicit
 * lifecycle flags.  No display-unit conversion or averaging normalization is performed.
 *
 * @param[in] index Zero-based global node index; subtype must match node.
 * @param[out] stats Non-null caller-owned destination, borrowed only during the call. Depths ft,
 *     volumes ft3, flows cfs, duration fields seconds and max dates absolute SWMM days. avgDepth is
 *     an accumulated sample sum; timeCourantCritical counts events.
 * @return Zero on success, ERR_API_NOT_ENDED if the statistics array is absent,
 *     ERR_API_OBJECT_INDEX for an invalid index.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_getNodeStats(int index, swmm_NodeStats* stats);

/**
 * @brief Copy raw internal storage node statistics.
 *
 * Call after swmm_end and before swmm_close for final data. Checks array presence, not explicit
 * lifecycle flags.  No display-unit conversion or averaging normalization is performed.
 *
 * @param[in] index Zero-based global node index; subtype must match storage node.
 * @param[out] stats Non-null caller-owned destination, borrowed only during the call. Volumes ft3,
 *     maxFlow cfs and maxVolDate absolute SWMM days; avgVol is an accumulated sample sum.
 * @return Zero on success, ERR_API_NOT_ENDED if the statistics array is absent,
 *     ERR_API_OBJECT_INDEX for an invalid index, or ERR_API_OBJECT_TYPE for a wrong subtype.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_getStorageStats(int index, swmm_StorageStats* stats);

/**
 * @brief Copy raw internal outfall node statistics.
 *
 * Call after swmm_end and before swmm_close for final data. Checks array presence, not explicit
 * lifecycle flags. Flows cfs; avgFlow is a sample sum, totalPeriods its count. Loads retain
 * internal concentration-volume accounting units. No display-unit conversion or averaging
 * normalization is performed.
 *
 * @param[in] index Zero-based global node index; subtype must match outfall node.
 * @param[in,out] stats Non-null caller-owned destination, borrowed only during the call. Initialize
 *     totalLoad to null to skip loads or to writable storage for Npollut doubles; the API never
 *     allocates that storage.
 * @return Zero on success, ERR_API_NOT_ENDED if the statistics array is absent,
 *     ERR_API_OBJECT_INDEX for an invalid index, or ERR_API_OBJECT_TYPE for a wrong subtype.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_getOutfallStats(int index, swmm_OutfallStats* stats);

/**
 * @brief Copy raw internal link statistics.
 *
 * Call after swmm_end and before swmm_close for final data. Checks array presence, not explicit
 * lifecycle flags.  No display-unit conversion or averaging normalization is performed.
 *
 * @param[in] index Zero-based global link index; subtype must match link.
 * @param[out] stats Non-null caller-owned destination, borrowed only during the call. Depths ft,
 *     velocity ft/s, flows cfs, duration fields seconds, maxFlowDate absolute SWMM days;
 *     timeCourantCritical counts events.
 * @return Zero on success, ERR_API_NOT_ENDED if the statistics array is absent,
 *     ERR_API_OBJECT_INDEX for an invalid index.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_getLinkStats(int index, swmm_LinkStats* stats);

/**
 * @brief Copy raw internal pump link statistics.
 *
 * Call after swmm_end and before swmm_close for final data. Checks array presence, not explicit
 * lifecycle flags.  No display-unit conversion or averaging normalization is performed.
 *
 * @param[in] index Zero-based global link index; subtype must match pump link.
 * @param[out] stats Non-null caller-owned destination, borrowed only during the call. Flows cfs,
 *     volume ft3, energy kWh; utilized/off-curve fields are seconds and avgFlow remains a sample
 *     sum.
 * @return Zero on success, ERR_API_NOT_ENDED if the statistics array is absent,
 *     ERR_API_OBJECT_INDEX for an invalid index, or ERR_API_OBJECT_TYPE for a wrong subtype.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_getPumpStats(int index, swmm_PumpStats* stats);

/**
 * @brief Copy raw global routing continuity totals.
 *
 * Use after swmm_end and before swmm_close for finalized totals. Does not recompute accounting,
 * retain storage or normalize units.
 *
 * @param[out] totals Non-null caller-owned destination. routing volume categories are ft3; pctError
 *     is percent, with no model display conversion.
 * @return Always zero; no lifecycle or null checks.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_getSystemRoutingTotals(swmm_RoutingTotals* totals);

/**
 * @brief Copy raw global runoff continuity totals.
 *
 * Use after swmm_end and before swmm_close for finalized totals. Does not recompute accounting,
 * retain storage or normalize units.
 *
 * @param[out] totals Non-null caller-owned destination. runoff water categories including rainfall
 *     and snow stores are ft3; pctError is percent, with no model display conversion.
 * @return Always zero; no lifecycle or null checks.
 */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_getSystemRunoffTotals(swmm_RunoffTotals* totals);

#ifdef __cplusplus
} // matches the linkage specification from above */
#endif

#endif // OPENSWMM_LEGACY_SOLVER_H_
