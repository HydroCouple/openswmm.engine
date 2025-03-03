/*!
 *  \file swmm_output_enums.h
 *  \author Michael Tryby (US EPA - ORD/CESER)
 *  \brief Header file for SWMM output API enumeration types.
 *  \date Created: 2019-10-18
 *  \date Last edited: 2024-10-17
 */

 #ifndef SWMM_OUTPUT_ENUMS_H_
 #define SWMM_OUTPUT_ENUMS_H_

/*!
 * \enum SMO_unitSystem
 * \brief Enumeration of unit systems for used in output file
 */
typedef enum {
     /*! \brief US Customary units (ft, acre, etc.) */
     SMO_US,
     /*! \brief International System of Units (m, ha, etc.) */
     SMO_SI
} SMO_unitSystem;

/*!
 * \enum SMO_massUnits
 * \brief Flow units used in the simulation
 */
typedef enum {
     /*! \brief Cubic feet per second*/
     SMO_CFS,
     /*! \brief Gallons per minute*/
     SMO_GPM,
	 /*! \brief Million gallons per day*/
     SMO_MGD,
	 /*! \brief Cubic meters per second*/
     SMO_CMS,
	 /*! \brief Liters per second*/
     SMO_LPS,
	 /*! \brief Million liters per day*/
     SMO_MLD
} SMO_flowUnits;

/*!
 * \enum SMO_concUnits
 * \brief Concentration units used in the simulation
 */
typedef enum {
	/*! \brief Milligrams per liter*/
    SMO_MG,
	/*! \brief Micrograms per liter*/
    SMO_UG,
	/*! \brief Counts per liter*/
    SMO_COUNT,
	/*! \brief No units*/
    SMO_NONE
} SMO_concUnits;

/*!
 * \enum SMO_elementType
 * \brief SWMM element types
 */
typedef enum {
	/*! \brief Subcatchment */
    SMO_subcatch,
	/*! \brief Node */
    SMO_node,
	/*! \brief Link */
    SMO_link,
	/*! \brief System */
    SMO_sys,
	/*! \brief Pollutant */
    SMO_pollut
} SMO_elementType;

/*!
 * \enum SMO_time
 * \brief Report time related attributes
 */
typedef enum {
	/*! \brief Report step size (seconds) */
    SMO_reportStep,
	/*! \brief Number of reporting periods */
    SMO_numPeriods
} SMO_time;

/*!
 * \enum SMO_subcatchAttribute
 * \brief Subcatchment attributes
 */
typedef enum {
	/*! \brief Subcatchment rainfall (in/hr or mm/hr) */
    SMO_rainfall_subcatch,
	/*! \brief Subcatchment snow depth (in or mm) */
    SMO_snow_depth_subcatch,
	/*! \brief Subcatchment evaporation loss (in/hr or mm/hr) */
    SMO_evap_loss,
	/*! \brief Subcatchment infiltration loss (in/hr or mm/hr) */
    SMO_infil_loss,
	/*! \brief Subcatchment runoff flow (flow units) */
    SMO_runoff_rate,
	/*! \brief Subcatchment groundwater flow (flow units) */
    SMO_gwoutflow_rate,
	/*! \brief Subcatchment groundwater elevation (ft or m) */
    SMO_gwtable_elev,
	/*! \brief Subcatchment soil moisture content (-) */
    SMO_soil_moisture,
	/*! \brief Subcatchment pollutant concentration (-) */
    SMO_pollutant_conc_subcatch	// first pollutant
} SMO_subcatchAttribute;

/*!
 * \enum SMO_nodeAttribute
 * \brief Node attributes
 */
typedef enum {
	/*! \brief Node depth above invert (ft or m) */
    SMO_invert_depth,
	/*! \brief Node hydraulic head (ft or m) */
    SMO_hydraulic_head,
	/*! \brief Node volume stored (ft3 or m3) */
    SMO_stored_ponded_volume,
	/*! \brief Node lateral inflow (flow units) */
    SMO_lateral_inflow,
	/*! \brief Node total inflow (flow units) */
    SMO_total_inflow,
	/*! \brief Node flooding losses (flow units) */
    SMO_flooding_losses,
	/*! \brief Node pollutant concentration (-) */
    SMO_pollutant_conc_node,
} SMO_nodeAttribute;

/*!
 * \enum SMO_linkAttribute
 * \brief Link attributes
 */
typedef enum {
	/*! \brief Link flow rate (flow units) */
    SMO_flow_rate_link,
	/*! \brief Link flow depth (ft or m) */
    SMO_flow_depth,
	/*! \brief Link flow velocity (ft/s or m/s) */
    SMO_flow_velocity,
	/*! \brief Link flow volume (ft3 or m3) */
    SMO_flow_volume,
	/*! \brief Link capacity (fraction of conduit filled) */
    SMO_capacity,
	/*! \brief Link pollutant concentration (-) */
    SMO_pollutant_conc_link
} SMO_linkAttribute;

/*!
 * \enum SMO_systemAttribute
 * \brief System attributes
 */
typedef enum {
	/*! \brief Air temperature (deg. F or deg. C) */
    SMO_air_temp,
	/*! \brief Rainfall intensity (in/hr or mm/hr) */
    SMO_rainfall_system,
	/*! \brief Snow depth (in or mm) */
    SMO_snow_depth_system,
	/*! \brief Evaporation and infiltration loss rate (in/day or mm/day) */
    SMO_evap_infil_loss,
	/*! \brief Runoff flow (flow units) */
    SMO_runoff_flow,
	/*! \brief Dry weather inflow (flow units) */
    SMO_dry_weather_inflow,
	/*! \brief Groundwater inflow (flow units) */
    SMO_groundwater_inflow,
	/*! \brief Rainfall Derived Infiltration and Inflow (RDII) (flow units) */
    SMO_RDII_inflow,
	/*! \brief Direct inflow (flow units) */
    SMO_direct_inflow,
	/*! \brief Total lateral inflow; sum of variables 4 to 8 (flow units) */
    SMO_total_lateral_inflow,
	/*! \brief Flooding losses (flow units) */
    SMO_flood_losses,
	/*! \brief Outfall flow (flow units) */
    SMO_outfall_flows,
	/*! \brief Volume stored in storage nodes (ft3 or m3) */
    SMO_volume_stored,
	/*! \brief Evaporation rate (in/day or mm/day) */
    SMO_evap_rate
} SMO_systemAttribute;


#endif /* SWMM_OUTPUT_ENUMS_H_ */
