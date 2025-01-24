/*!
 * \file enums.h
 * \brief Header file for enumerated constants.
 * \author L. Rossman
 * \date Created: 2022-06-01
 * \date Last updated: 2024-12-30
 * \version 5.3.0
 * \details
 * Enumerated constants used in the SWMM model.
 *
 * Update history
 * ==============
 * Build 5.1.004:
 * - IGNORE_RDII for the ignore RDII option added.
 * Build 5.1.007:
 * - s_GWF for [GWF] input file section added.
 * - s_ADJUST for [ADJUSTMENTS] input file section added.
 * Build 5.1.008:
 * - Enumerations for fullness state of a conduit added.
 * - NUM_THREADS added for number of parallel threads option.
 * - Runoff flow categories added to represent mass balance components.
 * Build 5.1.010:
 * - New ROADWAY_WEIR type of weir added.
 * - Potential evapotranspiration (PET) added as a system output variable.
 * Build 5.1.011:
 * - s_EVENT added to InputSectionType enumeration.
 * Build 5.1.013:
 * - SURCHARGE_METHOD and RULE_STEP options added.
 * - WEIR_CURVE added as a curve type.
 * Build 5.2.0:
 * - Support added for Streets and Inlets.
 * - Support added for variable speed pumps.
 * - Support added for analytical storage shapes.
 * Build 5.2.1:
 * - Adds a NEITHER option to the NormalFlowType enumeration.
 */

#ifndef ENUMS_H
#define ENUMS_H

/*!
* \addtogroup SWMM_Enumerations SWMM Enumerations
* \brief Enumerated constants used in the SWMM model.
* \ingroup SWMM_Constants
* \{
*/

/*!
 * \enum ObjectType
 * \brief Enumeration of object types used in SWMM5
 */
enum ObjectType
{
     /*! \brief Rain gage */
     GAGE, // rain gage
     /*! \brief Subcatchment */
     SUBCATCH,
     /*! \brief Node */
     NODE,
     /*! \brief Link */
     LINK,
     /*! \brief Pollutant */
     POLLUT,
     /*! \brief Land use category */
     LANDUSE,
     /*! \brief Dry weather flow time pattern */
     TIMEPATTERN,
     /*! \brief Generic table of values */
     CURVE,
     /*! \brief Time series of values */
     TSERIES,
     /*! \brief Convyance system control rules */
     CONTROL,
     /*! \brief Irregular channel cross-section */
     TRANSECT,
     /*! \brief Groundwater aquifer */
     AQUIFER,
     /*! \brief RDII unit hydrograph */
     UNITHYD,
     /*! \brief Snowmelt parameter set */
     SNOWMELT,
     /*! \brief Custom conduit shape */
     SHAPE,
     /*! \brief LID treatment units */
     LID,
     /*! \brief Street */
     STREET,
     /*! \brief Inlet */
     INLET,
     /*! \brief Maximum number of object types */
     MAX_OBJ_TYPES
};

/*!
 * \def MAX_NODE_TYPES
 * \brief Maximum number of node types
 */
#define MAX_NODE_TYPES 5

/*!
 * \enum NodeType
 * \brief Enumeration of node sub-types used in SWMM5
 */
enum NodeType
{    
     /*! \brief Junction node */
     JUNCTION,
     /*! \brief Outfall node */
     OUTFALL,
     /*! \brief Storage node */
     STORAGE,
     /*! \brief Divider node */
     DIVIDER
};

/*!
 * \def MAX_LINK_TYPES
 * \brief Maximum number of link types
 */
#define MAX_LINK_TYPES 5

/*!
 * \enum LinkType
 * \brief Enumeration of link sub-types used in SWMM5
 */
enum LinkType
{    
     /*! \brief Conduit link */
     CONDUIT,
     /*! \brief Pump link */
     PUMP,
     /*! \brief Orifice link */
     ORIFICE,
     /*! \brief Weir link */
     WEIR,
     /*! \brief Outlet link */
     OUTLET
};

/*!
 * \enum FileType
 * \brief Enumeration of file types used in SWMM5
 */
enum FileType
{
     /*! \brief Rainfall file */
     RAINFALL_FILE,
     /*! \brief Runoff file */
     RUNOFF_FILE,
     /*! \brief Hotstart file */
     HOTSTART_FILE,
     /*! \brief RDII file */
     RDII_FILE,
     /*! \brief Infows interface file */
     INFLOWS_FILE,
     /*! \brief Outflows interface file */
     OUTFLOWS_FILE
}; 


/*!
 * \enum FileUsageType
 * \brief Enumeration of file usage types used in SWMM5
 */
enum FileUsageType
{
     /*! \brief No file usage */
     NO_FILE,
     /*! \brief Temporary scratch file */
     SCRATCH_FILE,
     /*! \brief Use previously saved file */
     USE_FILE,
     /*! \brief Save file currently in use */
     SAVE_FILE
};


/*!
 * \enum GageDataType
 * \brief Enumeration of rain gage data types used in SWMM5
 */
enum GageDataType
{    
     /*! \brief Rainfall from user-supplied time series */
     RAIN_TSERIES,
     /*! \brief Rainfall from external file */
     RAIN_FILE
};

/*!
 * \enum XsectType
 * \brief Enumeration of cross section shape types used in SWMM5
 */
enum XsectType
{    
     /*! \brief Dummy cross section */
     DUMMY,           // 0
     /*! \brief Circular cross section */
     CIRCULAR,        // 1      closed
     /*! \brief Filled circular cross section */
     FILLED_CIRCULAR, // 2      closed
     /*! \brief Rectangular cross section */
     RECT_CLOSED,     // 3      closed
     /*! \brief Rectangular open cross section */
     RECT_OPEN,       // 4
     /*! \brief Trapezoidal cross section */
     TRAPEZOIDAL,     // 5
     /*! \brief Triangular cross section */
     TRIANGULAR,      // 6
     /*! \brief Parabolic cross section */
     PARABOLIC,       // 7
     /*! \brief Power function cross section */
     POWERFUNC,       // 8
     /*! \brief Rectangular triangular cross section */
     RECT_TRIANG,     // 
     /*! \brief Rectangular round cross section */
     RECT_ROUND,      // 10
     /*! \brief Modified Baskethandle cross section */
     MOD_BASKET,      // 11
     /*! \brief Horizontal elliptical cross section */
     HORIZ_ELLIPSE,   // 12     closed
     /*! \brief Vertical elliptical cross section */
     VERT_ELLIPSE,    // 13     closed
     /*! \brief Arch cross section */
     ARCH,            // 14     closed
     /*! \brief Egg-shaped cross section */
     EGGSHAPED,       // 15     closed
     /*! \brief Horseshoe cross section */
     HORSESHOE,       // 16     closed
     /*! \brief Gothic cross section */
     GOTHIC,          // 17     closed
     /*! \brief Catenary cross section */
     CATENARY,        // 18     closed
     /*! \brief Semi-elliptical cross section */
     SEMIELLIPTICAL,  // 19     closed
     /*! \brief Baskethandle cross section */
     BASKETHANDLE,    // 20     closed
     /*! \brief Semi-circular cross section */
     SEMICIRCULAR,    // 21     closed
     /*! \brief Irregular cross section */
     IRREGULAR,       // 22
     /*! \brief Custom cross section */
     CUSTOM,          // 23     closed
     /*! \brief Force main cross section */
     FORCE_MAIN,      // 24     closed
     /*! \brief Street cross section */
     STREET_XSECT // 25
}; 

/*!
 * \enum UnitsType
 * \brief Enumeration of measurement units types used in SWMM5
 */
enum UnitsType
{    
     /*! \brief US units */
     US, // US units
     /*! \brief SI units */
     SI, // SI (metric) units
}; 

/*!
 * \enum FlowUnitsType
 * \brief Enumeration of flow units types used in SWMM5
 */
enum FlowUnitsType
{    
     /*! \brief Cubic feet per second */
     CFS,
     /*! \brief Gallons per minute */
     GPM,
     /*! \brief Million gallons per day */
     MGD,
     /*! \brief Cubic meters per second */
     CMS,
     /*! \brief Liters per second */
     LPS,
     /*! \brief Million liters per day */
     MLD,
}; 

enum ConcUnitsType
{
     /*! \brief Milligrams per liter */
     MG,
     /*! \brief Micrograms per liter */
     UG,
     /*! \brief Counts per liter */
     COUNT,
};

/*!
* \enum ConversionType
* \brief Quanties requiring unit conversions in SWMM5
*/
enum ConversionType
{    
     /*! \brief Rainfall */
     RAINFALL,
     /*! \brief Rain depth */
     RAINDEPTH,
     /*! \brief Evaporation rate */
     EVAPRATE,
     /*! \brief Length */
     LENGTH,
     /*! \brief Land area */
     LANDAREA,
     /*! \brief Volume */
     VOLUME,
     /*! \brief Wind speed */
     WINDSPEED,
     /*! \brief Temperature */
     TEMPERATURE,
     /*! \brief Mass */
     MASS,
     /*! \brief Groundwater flow */
     GWFLOW,
     /*! \brief Flow rate */
     FLOW
};

/*!
* \def MAX_SUBCATCH_RESULTS
* \brief Maximum number of subcatchment results
*/
#define MAX_SUBCATCH_RESULTS 9

/*!
* \enum SubcatchResultType
* \brief Enumeration of computed subcatchment quantities
*/
enum SubcatchResultType
{
     /*! \brief Rainfall intensity */
     SUBCATCH_RAINFALL,
     /*! \brief Snow depth */
     SUBCATCH_SNOWDEPTH,
     /*! \brief Evaporation loss */
     SUBCATCH_EVAP,
     /*! \brief Infiltration loss */
     SUBCATCH_INFIL,
     /*! \brief Runoff flow rate */
     SUBCATCH_RUNOFF,
     /*! \brief Groundwater flow rate */
     SUBCATCH_GW_FLOW,
     /*! \brief Groundwater elevation of saturated table */
     SUBCATCH_GW_ELEV,
     /*! \brief Soil moisture content */
     SUBCATCH_SOIL_MOIST,
     /*! \brief Pollutant washoff concentration */
     SUBCATCH_WASHOFF,
}; 

/*!
* \def MAX_NODE_RESULTS
* \brief Maximum number of node results
*/
#define MAX_NODE_RESULTS 7

/*!
* \enum NodeResultType
* \brief Enumeration of computed node quantities
*/
enum NodeResultType
{    
     /*! \brief Depth of water above node invert */
     NODE_DEPTH,
     /*! \brief Hydraulic head at node */
     NODE_HEAD,
     /*! \brief Volume of stored & ponded water */
     NODE_VOLUME,
     /*! \brief Lateral inflow rate */
     NODE_LATFLOW,
     /*! \brief Total inflow rate */
     NODE_INFLOW,
     /*! \brief Overflow rate */
     NODE_OVERFLOW,
     /*! \brief Concentration of each pollutant */
     NODE_QUAL,
};

/*!
* \def MAX_LINK_RESULTS
* \brief Maximum number of link results
*/
#define MAX_LINK_RESULTS 6

/*!
* \enum LinkResultType
* \brief Enumeration of computed link quantities
*/
enum LinkResultType
{
     /*! \brief Flow rate */
     LINK_FLOW,
     /*! \brief Flow depth */
     LINK_DEPTH,
     /*! \brief Flow velocity */
     LINK_VELOCITY,
     /*! \brief Flow volume */
     LINK_VOLUME,
     /*! \brief Ratio of area to full area */
     LINK_CAPACITY,
     /*! \brief Concentration of each pollutant */
     LINK_QUAL,
}; 

/*!
* \def MAX_SYS_RESULTS
* \brief Maximum number of system results
*/
#define MAX_SYS_RESULTS 15

/*!
* \enum SysResultType
* \brief Enumeration of computed system quantities
*/
enum SysFlowType
{    
     /*! \brief Air temperature */
     SYS_TEMPERATURE, 
     /*! \brief Rainfall intensity */
     SYS_RAINFALL,
     /*! \brief Snow depth */
     SYS_SNOWDEPTH,   
     /*! \brief Infiltration loss */
     SYS_INFIL,
     /*! \brief Runoff flow */
     SYS_RUNOFF,
     /*! \brief Dry weather inflow */
     SYS_DWFLOW,
     /*! \brief Groundwater inflow */
     SYS_GWFLOW,
     /*! \brief RDII inflow */
     SYS_IIFLOW,
     /*! \brief External inflow */
     SYS_EXFLOW,
     /*! \brief Total lateral inflow */
     SYS_INFLOW,
     /*! \brief Flooding outflow */
     SYS_FLOODING,
     /*! \brief Outfall flow */
     SYS_OUTFLOW,
     /*! \brief Storage volume */
     SYS_STORAGE,
     /*! \brief Evaporation */
     SYS_EVAP,
     /*! \brief Potential evapotranspiration */
     SYS_PET,
};

/*!
* \enum FlowClassType
* \brief Enumeration of conduit flow classifications
*/
enum FlowClassType
{
     /*! \brief Dry conduit */
     DRY,
     /*! \brief Upstream end is dry */
     UP_DRY,
     /*! \brief Downstream end is dry */
     DN_DRY, 
     /*! \brief Subcritical flow */
     SUBCRITICAL,
     /*! \brief Supercritical flow */
     SUPCRITICAL,
     /*! \brief Free-fall at the upstream end */
     UP_CRITICAL,
     /*! \brief Free-fall at the downstream end */
     DN_CRITICAL,      
     /*! \brief Number of distinct flow classes */
     MAX_FLOW_CLASSES, 
     /*! \brief Upstream end is full */
     UP_FULL,
     /*! \brief Downstream end is full */
     DN_FULL,
     /*! \brief Completely full conduit */
     ALL_FULL,
};


/*!
* \enum RunoffFlowType
* \brief Enumeration of runoff flow categories
*/
enum RunoffFlowType
{
     /*! \brief Rainfall */
     RUNOFF_RAINFALL,
     /*! \brief Evaporation */
     RUNOFF_EVAP,
     /*! \brief Infiltration */
     RUNOFF_INFIL,
     /*! \brief Runoff */
     RUNOFF_RUNOFF,
     /*! \brief LID drain flow */
     RUNOFF_DRAINS,
     /*! \brief Runon from outfalls */
     RUNOFF_RUNON,
}; 

/*!
* \enum LoadingType
* \brief Enumeration of surface pollutant loading categories
*/
enum LoadingType
{
     /*! \brief Pollutant buildup load */
     BUILDUP_LOAD,
     /*! \brief Pollutant deposition load */
     DEPOSITION_LOAD,
     /*! \brief Pollutant load removed by sweeping */
     SWEEPING_LOAD,  
     /*! \brief Pollutant load removed by BMPs */
     BMP_REMOVAL_LOAD,
     /*! \brief Runon load removed by infiltration */
     INFIL_LOAD,
     /*! \brief Pollutant load removed by runoff */
     RUNOFF_LOAD,
     /*! \brief Pollutant load remaining on surface */
     FINAL_LOAD,     
}; 

/*!
* \enum RainfallType
* \brief Enumeration of rainfall input data type options
*/
enum RainfallType
{    
     /*! \brief Rainfall expressed as intensity */
     RAINFALL_INTENSITY,
     /*! \brief Rainfall expressed as volume */
     RAINFALL_VOLUME,
     /*! \brief Rainfall expressed as cumulative volume */
     CUMULATIVE_RAINFALL,
};

/*!
* \enum TempType
* \brief Enumeration of temperature data type options
*/
enum TempType
{    
     /*! \brief No temperature data supplied */
     NO_TEMP,
     /*! \brief Temperature data from time series */
     TSERIES_TEMP,
     /*! \brief Temperature data from climate file */
     FILE_TEMP,
}; 

/*!
* \enum WindType
* \brief Enumeration of wind speed data type options
*/
enum WindType
{    
     /*! \brief Wind speed varies by month */
     MONTHLY_WIND,
     /*! \brief Wind speed from climate file */
     FILE_WIND,
};

/*!
* \enum EvapType
* \brief Enumeration of evaporation data type options
*/
enum EvapType
{    
     /*! \brief Constant evaporation rate */
     CONSTANT_EVAP,
     /*! \brief Evaporation rate varies by month */
     MONTHLY_EVAP,
     /*! \brief Evaporation rate from time series */
     TIMESERIES_EVAP,
     /*! \brief Evaporation rate from daily temperature */
     TEMPERATURE_EVAP,
     /*! \brief Evaporation rate from climate file */
     FILE_EVAP,
     /*! \brief Soil moisture recovery pattern */
     RECOVERY,
     /*! \brief Evaporation allowed only in dry periods */
     DRYONLY,
};

/*!
* \enum NormalizerType
* \brief Enumeration of buildup normalization options
*/
enum NormalizerType
{    
     /*! \brief Normalized per unit of area */
     PER_AREA,
     /*! \brief Normalized per unit of curb length */
     PER_CURB,
};

/*!
* \enum BuildupType
* \brief Enumeration of pollutant buildup function types
*/
enum BuildupType
{
     /*! \brief No buildup */
     NO_BUILDUP,
     /*! \brief Power function buildup */
     POWER_BUILDUP,
     /*! \brief Exponential function buildup */
     EXPON_BUILDUP,
     /*! \brief Saturation function buildup */
     SATUR_BUILDUP,
     /*! \brief External time series buildup */
     EXTERNAL_BUILDUP,
};

/*!
* \enum WashoffType
* \brief Enumeration of pollutant washoff function types
*/
enum WashoffType
{
     /*! \brief No washoff */
     NO_WASHOFF,
     /*! \brief Exponential washoff equation */
     EXPON_WASHOFF,
     /*! \brief Rating curve washoff equation */
     RATING_WASHOFF,
     /*! \brief Event mean concentration washoff */
     EMC_WASHOFF,
};

/*!
* \enum SubAreaType
* \brief Enumeration of subcatchment area types
*/
enum SubAreaType
{    
     /*! \brief Impervious area with no depression storage */
     IMPERV0,
     /*! \brief Impervious area with depression storage */
     IMPERV1,
     /*! \brief Pervious area */
     PERV,
};

/*!
* \enum RunoffRoutingType
* \brief Enumeration of runoff routing types
*/
enum RunoffRoutingType
{
     /*! \brief Pervious and impervious runoff goes to outlet */
     TO_OUTLET,
     /*! \brief Pervious runoff goes to impervious area */
     TO_IMPERV,
     /*! \brief Impervious runoff goes to pervious area */
     TO_PERV,
};

/*!
* \enum RouteModelType
* \brief Enumeration of routing model types
*/
enum RouteModelType
{    
     /*! \brief No routing */
     NO_ROUTING,
     /*! \brief Steady flow routing */
     SF,
     /*! \brief Kinematic wave routing */
     KW,
     /*! \brief Extended kinematic wave routing */
     EKW,
     /*! \brief Dynamic wave routing */
     DW,
};

/*!
* \enum ForceMainType
* \brief Enumeration of force main friction loss models
*/
enum ForceMainType
{
     /*! \brief Hazen-Williams equation */
     H_W,
     /*! \brief Darcy-Weisbach equation */
     D_W,
};

/*!
* \enum OffsetType
* \brief Enumeration of node offset types
*/
enum OffsetType
{
     /*! \brief Offset measured as depth */
     DEPTH_OFFSET,
     /*! \brief Offset measured as elevation */
     ELEV_OFFSET,
};

/*!
* \enum KinWaveMethodType
* \brief Enumeration of kinematic wave routing methods
*/
enum KinWaveMethodType
{
     /*! \brief Normal method */
     NORMAL,
     /*! \brief Modified method */
     MODIFIED,
};

/*!
* \enum CompatibilityType
* \brief Enumeration of SWMM5 compatibility types
*/
enum CompatibilityType
{
     /*! \brief SWMM 5.0 weighting for area & hyd. radius */
     SWMM5,
     /*! \brief SWMM 3 weighting */
     SWMM3,
     /*! \brief SWMM 4 weighting */
     SWMM4,
};

/*!
* \enum NormalFlowType
* \brief Enumeration of normal flow computation methods
*/
enum NormalFlowType
{    
     /*! \brief Based on slope only */
     SLOPE,
     /*! \brief Based on Froude number only */
     FROUDE,
     /*! \brief Based on both slope & Froude number */
     BOTH,
     /*! \brief Neither slope nor Froude number */
     NEITHER, 
};

/*!
* \enum InertialDampingType
* \brief Enumeration of inertial damping options
*/
enum InertialDampingType
{
     /*! \brief No inertial damping */
     NO_DAMPING,
     /*! \brief Partial inertial damping */
     PARTIAL_DAMPING,
     /*! \brief Full inertial damping */
     FULL_DAMPING,
}; 

/*!
* \enum SurchargeMethodType
* \brief Enumeration of surcharge method types
*/
enum SurchargeMethodType
{    
     /*! \brief Original EXTAN method */
     EXTRAN,
     /*! \brief Preissmann slot method */
     SLOT,
};

/*!
* \enum InflowType
* \brief Enumeration of inflow method types
*/
enum InflowType
{
     /*! \brief User supplied external inflow */
     EXTERNAL_INFLOW, 
     /*! \brief User supplied dry weather inflow */
     DRY_WEATHER_INFLOW, 
     /*! \brief Computed runoff inflow */ 
     WET_WEATHER_INFLOW,
     /*! \brief Computed groundwater inflow */
     GROUNDWATER_INFLOW,
     /*! \brief Computed I&I inflow */
     RDII_INFLOW,
     /*! \brief Inflow parameter is flow */
     FLOW_INFLOW,
     /*! \brief Inflow parameter is pollutant concen. */
     CONCEN_INFLOW,
     /*! \brief Inflow parameter is pollutant mass */
     MASS_INFLOW,
};

/*!
* \enum PatternType
* \brief Enumeration of time pattern types
*/
enum PatternType
{
     /*! \brief Dry weather flow multipliers for each month */
     MONTHLY_PATTERN,
     /*! \brief Dry weather flow multipliers for each day of week */
     DAILY_PATTERN,
     /*! \brief Dry weather flow multipliers for each hour of day */
     HOURLY_PATTERN,
     /*! \brief Dry weather flow multipliers for weekend days */
     WEEKEND_PATTERN,
}; 

/*!
* \enum OutfallType
* \brief Enumeration of outfall types
*/
enum OutfallType
{
     /*! \brief Critical depth outfall condition */
     FREE_OUTFALL,
     /*! \brief Normal flow depth outfall condition */
     NORMAL_OUTFALL,
     /*! \brief Fixed depth outfall condition */
     FIXED_OUTFALL,
     /*! \brief Tidal stage outfall condition */
     TIDAL_OUTFALL,
     /*! \brief Variable time series outfall depth */
     TIMESERIES_OUTFALL,
}; 

/*!
* \enum StorageType
* \brief Enumeration of storage node types
*/
enum StorageType
{
     /*! \brief Area versus depth storage from table */
     TABULAR,
     /*! \brief Area versus depth storage from function */
     FUNCTIONAL,
     /*! \brief Area versus depth storage from elliptical cylinder */
     CYLINDRICAL,
     /*! \brief Area versus depth storage from elliptical cone */
     CONICAL,
     /*! \brief Area versus depth storage from elliptical paraboloid */
     PARABOLOID,
     /*! \brief Area versus depth storage from rectangular pyramid */
     PYRAMIDAL,
};

/*!
* \enum ReactorType
* \brief Enumeration of reactor types
*/
enum ReactorType
{
     /*! \brief Completely mixed reactor */
     CSTR,
     /*! \brief Plug flow reactor */
     PLUG,
}; 

/*!
* \enum TreatmentType
* \brief Enumeration of pollutant treatment types
*/
enum TreatmentType
{
     /*! \brief Treatment stated as a removal */
     REMOVAL,
     /*! \brief Treatment stated as effluent concen. */
     CONCEN,
};

/*!
* \enum DividerType
* \brief Enumeration of flow divider types
*/
enum DividerType
{
     /*! \brief Diverted flow is excess of cutoff flow */
     CUTOFF_DIVIDER,
     /*! \brief Table of diverted flow versus inflow */
     TABULAR_DIVIDER,
     /*! \brief Diverted flow proportional to excess flow */
     WEIR_DIVIDER,
     /*! \brief Diverted flow if flow > full conduit flow */
     OVERFLOW_DIVIDER,
};

/*!
* \enum PumpCurveType
* \brief Enumeration of pump curve types
*/
enum PumpCurveType
{
     /*! \brief Flow varies stepwise with wet well volume */
     TYPE1_PUMP,
     /*! \brief Flow varies stepwise with inlet depth */
     TYPE2_PUMP,
     /*! \brief Flow varies with head delivered */
     TYPE3_PUMP,
     /*! \brief Flow varies with inlet depth */
     TYPE4_PUMP,
     /*! \brief Variable speed version of TYPE3 pump */
     TYPE5_PUMP,
     /*! \brief Outflow varies with inflow */
     IDEAL_PUMP,
};

/*!
* \enum OrificeType
* \brief Enumeration of orifice types
*/
enum OrificeType
{    
     /*! \brief Side orifice */
     SIDE_ORIFICE,
     /*! \brief Bottom orifice */
     BOTTOM_ORIFICE,     
}; 

/*!
* \enum WeirType
* \brief Enumeration of weir types
*/
enum WeirType
{
     /*! \brief Transverse weir */
     TRANSVERSE_WEIR,
     /*! \brief Side flow weir */
     SIDEFLOW_WEIR,
     /*! \brief V-notch (triangular) weir */
     VNOTCH_WEIR,
     /*! \brief Trapezoidal weir */
     TRAPEZOIDAL_WEIR,
     /*! \brief FHWA HDS-5 roadway weir */
     ROADWAY_WEIR,
}; 

/*!
* \enum CurveType
* \brief Enumeration of curve types
*/
enum CurveType
{
     /*! \brief Surface area v. depth for storage node */
     STORAGE_CURVE,
     /*! \brief Diverted flow v. inflow for divider node */
     DIVERSION_CURVE,
     /*! \brief Water elev. v. hour of day for outfall */
     TIDAL_CURVE,
     /*! \brief Flow rate v. head for outlet link */
     RATING_CURVE,
     /*! \brief Control setting v. controller variable */
     CONTROL_CURVE,
     /*! \brief Width v. depth for custom x-section */
     SHAPE_CURVE,
     /*! \brief Discharge coeff. v. head for weir */
     WEIR_CURVE,
     /*! \brief Flow v. wet well volume for pump */
     PUMP1_CURVE,
     /*! \brief Flow v. depth for pump (discrete) */
     PUMP2_CURVE,
     /*! \brief Flow v. head for pump (continuous) */
     PUMP3_CURVE,
     /*! \brief Flow v. depth for pump (continuous) */
     PUMP4_CURVE,
     /*! \brief Variable speed version of TYPE3 pump */
     PUMP5_CURVE,
}; 

/*!
* \enum NodeInletType
* \brief Enumeration of node inlet types
*/
enum NodeInletType
{    
     /*! \brief No inlet */
     NO_INLET,
     /*! \brief Bypass flow inlet */
     BYPASS,
     /*! \brief Capture flow inlet */
     CAPTURE,
};

/*!
* \enum InputSectionType
* \brief Enumeration of input file section types
*/
enum InputSectionType
{
     /*! \brief Title section */
     s_TITLE,
     /*! \brief Options section */
     s_OPTION,
     /*! \brief Input files section */
     s_FILE,
     /*! \brief Raingage section */
     s_RAINGAGE,
     /*! \brief Temperature data section */
     s_TEMP,
     /*! \brief Evaporation data section */
     s_EVAP,
     /*! \brief Subcatchments data section */
     s_SUBCATCH,
     /*! \brief Subareas data section */
     s_SUBAREA,
     /*! \brief Infiltration data section */
     s_INFIL,
     /*! \brief Aquifers data section */
     s_AQUIFER,
     /*! \brief Groundwater units data section */
     s_GROUNDWATER,
     /*! \brief Snowmelt data section */
     s_SNOWMELT,
     /*! \brief Junctions data section */
     s_JUNCTION,
     /*! \brief Outfalls data section */
     s_OUTFALL,
     /*! \brief Storage units data section */
     s_STORAGE,
     /*! \brief Dividers unit data section */
     s_DIVIDER,
     /*! \brief Conduits data section */
     s_CONDUIT,
     /*! \brief Pumps data section */
     s_PUMP,
     /*! \brief Orifices data section */
     s_ORIFICE,
     /*! \brief Weirs data section */
     s_WEIR,
     /*! \brief Outlets data section */
     s_OUTLET,
     /*! \brief Xsections data section */
     s_XSECTION,
     /*! \brief Transects data section */
     s_TRANSECT,
     /*! \brief Loss methods data section */
     s_LOSSES,
     /*! \brief Control rules data section */
     s_CONTROL,
     /*! \brief Pollutants data section */
     s_POLLUTANT,
     /*! \brief Land uses data section */
     s_LANDUSE,
     /*! \brief Pollutant buildup data section */
     s_BUILDUP,
     /*! \brief Pollutant washoff data section */
     s_WASHOFF,
     /*! \brief Coverages data section */
     s_COVERAGE,
     /*! \brief Inflows data section */
     s_INFLOW,
     /*! \brief DWF patterns data section */
     s_DWF,
     /*! \brief Time patterns data section */
     s_PATTERN,
     /*! \brief RDII data section */
     s_RDII,
     /*! \brief Unit hydrographs data section */
     s_UNITHYD,
     /*! \brief Pollutant loading data section */
     s_LOADING,
     /*! \brief Treatment functions data section */
     s_TREATMENT,
     /*! \brief Curve data section */
     s_CURVE,
     /*! \brief Time series data section */
     s_TIMESERIES,
     /*! \brief Reporting options data section */
     s_REPORT,
     /*! \brief Coordinates data section */
     s_COORDINATE,
     /*! \brief Vertices data section */
     s_VERTICES,
     /*! \brief Polygons data section */
     s_POLYGON,
     /*! \brief Labels data section */
     s_LABEL,
     /*! \brief Symbols data section */
     s_SYMBOL,
     /*! \brief Backdrop data section */
     s_BACKDROP,
     /*! \brief Tags data section */
     s_TAG,
     /*! \brief Profiles data section */
     s_PROFILE,
     /*! \brief Map data section */
     s_MAP,
     /*! \brief LID control data section */
     s_LID_CONTROL,
     /*! \brief LID usage data section */
     s_LID_USAGE,
     /*! \brief GWF data section */
     s_GWF,
     /*! \brief Adjustments data section */
     s_ADJUST,
     /*! \brief Event data section */
     s_EVENT,
     /*! \brief Streets data section */
     s_STREET,
     /*! \brief Inlets usage data section */
     s_INLET_USAGE,
     /*! \brief Inlets data section */
     s_INLET,
};

/*!
* \enum InputOptionType
* \brief Enumeration of input file option types
*/
enum InputOptionType
{
     /*! \brief Flow units */
     FLOW_UNITS,
     /*! \brief Infiltration modeling method */
     INFIL_MODEL,
     /*! \brief Flow routing method */
     ROUTE_MODEL,
     /*! \brief Starting date of simulation */
     START_DATE,
     /*! \brief Starting time of simulation */
     START_TIME,
     /*! \brief Ending date of simulation */
     END_DATE,
     /*! \brief Ending time of simulation */
     END_TIME,
     /*! \brief Reporting start date */
     REPORT_START_DATE,
     /*! \brief Reporting start time */
     REPORT_START_TIME,
     /*! \brief Sweep start date */
     SWEEP_START,
     /*! \brief Sweep end date */
     SWEEP_END,
     /*! \brief Start dry days */
     START_DRY_DAYS,
     /*! \brief Wet weather flow routing time step */
     WET_STEP,
     /*! \brief Dry weather flow routing time step */
     DRY_STEP,
     /*! \brief Flow routing time step */
     ROUTE_STEP,
     /*! \brief Control rule time step */
     RULE_STEP,
     /*! \brief Reporting time step */
     REPORT_STEP,
     /*! \brief Allow ponding */
     ALLOW_PONDING,
     /*! \brief Inertial dampening option */
     INERT_DAMPING,
     /*! \brief Slope weighting for dynamic wave routing */
     SLOPE_WEIGHTING,
     /*! \brief Variable step method */
     VARIABLE_STEP,
     /*! \brief Normal flow limitation */
     NORMAL_FLOW_LTD,
     /*! \brief Lengthening step for dynamic wave routing */
     LENGTHENING_STEP,
     /*! \brief Minimum surface area for dynamic wave routing */
     MIN_SURFAREA,
     /*! \brief Compatibility option */
     COMPATIBILITY,
     /*! \brief Skip steady state option */
     SKIP_STEADY_STATE,
     /*! \brief Temporary directory */
     TEMPDIR,
     /*! \brief Ignore rainfall */
     IGNORE_RAINFALL,
     /*! \brief Force main equation */
     FORCE_MAIN_EQN,
     /*! \brief Link offsets */
     LINK_OFFSETS,
     /*! \brief Minimum conduit slope */
     MIN_SLOPE,
     /*! \brief Ignore snowmelt */
     IGNORE_SNOWMELT,
     /*! \brief Ignore groundwater */
     IGNORE_GWATER,
     /*! \brief Ignore routing */
     IGNORE_ROUTING,
     /*! \brief Ignore quality */
     IGNORE_QUALITY,
     /*! \brief Maximum number of iterations trials for dynamic wave routing solver */
     MAX_TRIALS,
     /*! \brief Head convergence tolerance for dynamic wave routing solver */
     HEAD_TOL,
     /*! \brief System flow tolerance for dynamic wave routing solver */     
     SYS_FLOW_TOL,
     /*! \brief Lateral inflow tolerance for dynamic wave routing solver */
     LAT_FLOW_TOL,
     /*! \brief Ignore RDII */
     IGNORE_RDII,
     /*! \brief Minimum route step */
     MIN_ROUTE_STEP,
     /*! \brief Number of threads */
     NUM_THREADS,
     /*! \brief Surcharge method */
     SURCHARGE_METHOD,
};

/*!
* \enum NoYesType
* \brief Enumeration of no/yes options
*/
enum NoYesType
{
     /*! \brief No option */
     NO,
     /*! \brief Yes option */
     YES,
};

/*!
* \enum NoneAllType
* \brief Enumeration of none/all/some options
*/
enum NoneAllType
{
     /*! \brief None option */
     NONE,
     /*! \brief All option */
     ALL,
     /*! \brief Some option */
     SOME,
};

/*!
* \}
*/


#endif // ENUMS_H
