/*!
* \file error.h
* \brief Header file for error codes.
* \author L. Rossman
* \date Created: 2021-11-01
* \date Last updated: 2024-12-30
* \version 5.3.0
* \details
* Error codes used in the SWMM model.
*
* Update history
* ==============
* Build 5.3.0
* - Moved API error codes to swmm.h so that they can be accessed for interpretation
*/

#ifndef ERROR_H
#define ERROR_H

/*!
* \addtogroup SWMM_Error_Constants SWMM Error Constants
* \brief General error constants used in the SWMM model.
* \ingroup SWMM_Constants
* \{
*/


/*!
* \enum ErrorType
* \brief Enumeration of error codes used in SWMM5
*/
enum ErrorType {

// ... Runtime Errors
      /*! \brief No error */
      ERR_NONE                 = 0,
      /*! \brief Memory allocation error */
      ERR_MEMORY               = 101,
      /*! \brief Kinematic wave routing error */
      ERR_KINWAVE              = 103,
      /*! \brief ODE solver error */
      ERR_ODE_SOLVER           = 105,
      /*! \brief Time step error */
      ERR_TIMESTEP             = 107,

// ... Subcatchment/Aquifer Errors
      /*! \brief Subcatchment has no outlet */
      ERR_SUBCATCH_OUTLET      = 108,
      /*! \brief Aquifer parameter error */
      ERR_AQUIFER_PARAMS       = 109,
      /*! \brief Ground elevation error */
      ERR_GROUND_ELEV          = 110,

// ... Conduit/Pump Errors
      /*! \brief Conduit length error */
      ERR_LENGTH               = 111,
      /*! \brief Conduit elevation drop error */
      ERR_ELEV_DROP            = 112,
      /*! \brief Conduit roughness error */
      ERR_ROUGHNESS            = 113,
      /*! \brief Conduit barrel error */
      ERR_BARRELS              = 114,
      /*! \brief Conduit slope error */
      ERR_SLOPE                = 115,
      /*! \brief Conduit no cross section */
      ERR_NO_XSECT             = 117,
      /*! \brief Conduit xsection error */
      ERR_XSECT                = 119,
      /*! \brief No curve specified error */
      ERR_NO_CURVE             = 121,
      /*! \brief Pump limits error */
      ERR_PUMP_LIMITS          = 122,

// ... Topology Errors
      /*! \brief Network loop error */
      ERR_LOOP                 = 131,
      /*! \brief Network multiple outlet error */
      ERR_MULTI_OUTLET         = 133,
      /*! \brief Network dummy links error */
      ERR_DUMMY_LINK           = 134,

// ... Node Errors
      /*! \brief Error divider error */
      ERR_DIVIDER              = 135,
      /*! \brief Divider link error */
      ERR_DIVIDER_LINK         = 136,
      /*! \brief Weir divider error */
      ERR_WEIR_DIVIDER         = 137,
      /*! \brief Node depth error */
      ERR_NODE_DEPTH           = 138,
      /*! \brief Regulator error */
      ERR_REGULATOR            = 139,
      /*! \brief Storage volume error */
      ERR_STORAGE_VOLUME       = 140, 
      /*! \brief Outfall error */
      ERR_OUTFALL              = 141,
      /*! \brief Regulator shape error */
      ERR_REGULATOR_SHAPE      = 143,
      /*! \brief No outlets error */
      ERR_NO_OUTLETS           = 145,

// ... RDII Errors
      /*! \brief RDII unit hydrograph time error */
      ERR_UNITHYD_TIMES        = 151,
      /*! \brief RDII unit hydrograph ratios error */
      ERR_UNITHYD_RATIOS       = 153,
      /*! \brief RDII area error */
      ERR_RDII_AREA            = 155,

// ... Rain Gage Errors
      /*! \brief Rain file conflict error */
      ERR_RAIN_FILE_CONFLICT   = 156,
      /*! \brief Rain gage format error */
      ERR_RAIN_GAGE_FORMAT     = 157,
      /*! \brief Rain gage time series error */
      ERR_RAIN_GAGE_TSERIES    = 158,
      /*! \brief Rain gage interval error */
      ERR_RAIN_GAGE_INTERVAL   = 159,

// ... Treatment Function Error
      /*! \brief Cyclic treatment error */
      ERR_CYCLIC_TREATMENT     = 161,

// ... Curve/Time Series Errors
      /*! \brief Curve sequence error */
      ERR_CURVE_SEQUENCE       = 171,
      /*! \brief Time series sequence error */
      ERR_TIMESERIES_SEQUENCE  = 173,

// ... Snowmelt Errors
      /*! \brief Snowmelt parameters error */
      ERR_SNOWMELT_PARAMS      = 181,
      /*! \brief Snowpack parameters error */
      ERR_SNOWPACK_PARAMS      = 182,

// ... LID Errors
      /*! \brief LID type error */
      ERR_LID_TYPE             = 183,
      /*! \brief LID layer error */
      ERR_LID_LAYER            = 184,
      /*! \brief LID parameters error */
      ERR_LID_PARAMS           = 185,
      /*! \brief LID areas error */
      ERR_LID_AREAS            = 187,
      /*! \brief LID capture area error */
      ERR_LID_CAPTURE_AREA     = 188,

// ... Simulation Date/Time Errors
      /*! \brief Start date error */
      ERR_START_DATE           = 191,
      /*! \brief Report start date error */
      ERR_REPORT_DATE          = 193,
      /*! \brief Report time step error */
      ERR_REPORT_STEP          = 195,

// ... Input Parser Errors
      /*! \brief Input error */
      ERR_INPUT                = 200,
      /*! \brief Line length error */
      ERR_LINE_LENGTH          = 201,
      /*! \brief Error items error */
      ERR_ITEMS                = 203,
      /*! \brief Keyword error */
      ERR_KEYWORD              = 205,
      /*! \brief Duplicate ID error */
      ERR_DUP_NAME             = 207,
      /*! \brief Name error */
      ERR_NAME                 = 209,
      /*! \brief Data value error */
      ERR_NUMBER               = 211,
      /*! \brief Datetime error */
      ERR_DATETIME             = 213,
      /*! \brief Rule error */
      ERR_RULE                 = 217,
      /*! \brief Transect unknown error */
      ERR_TRANSECT_UNKNOWN     = 219,
      /*! \brief Transect sequence error */
      ERR_TRANSECT_SEQUENCE    = 221,
      /*! \brief Transect too few error */
      ERR_TRANSECT_TOO_FEW     = 223,
      /*! \brief Transect too many error */
      ERR_TRANSECT_TOO_MANY    = 225,
      /*! \brief Transect Manning error */
      ERR_TRANSECT_MANNING     = 227,
      /*! \brief Transect overbank error */
      ERR_TRANSECT_OVERBANK    = 229,
      /*! \brief Transect no depth error */
      ERR_TRANSECT_NO_DEPTH    = 231,
      /*! \brief Math expression error */
      ERR_MATH_EXPR            = 233,
      /*! \brief Infiltration parameters error */
      ERR_INFIL_PARAMS         = 235,

// ... File Name/Opening Errors
      /*! \brief File name error */
      ERR_FILE_NAME            = 301,
      /*! \brief Input file error */
      ERR_INP_FILE             = 303,
      /*! \brief Report file error */
      ERR_RPT_FILE             = 305,
      /*! \brief Output file error */
      ERR_OUT_FILE             = 307,
      /*! \brief Output size error */
      ERR_OUT_SIZE             = 308,
      /*! \brief Output write error */
      ERR_OUT_WRITE            = 309,
      /*! \brief Output read error */
      ERR_OUT_READ             = 311,

// ... Rain File Errors
      /*! \brief Rain file scratch error */
      ERR_RAIN_FILE_SCRATCH    = 313,
      /*! \brief Rain file open error */
      ERR_RAIN_FILE_OPEN       = 315,
      /*! \brief Rain file data error */
      ERR_RAIN_FILE_DATA       = 317,
      /*! \brief Rain file sequence error */
      ERR_RAIN_FILE_SEQUENCE   = 318, 
      /*! \brief Rain file format error */
      ERR_RAIN_FILE_FORMAT     = 319,
      /*! \brief Rain interface format error */
      ERR_RAIN_IFACE_FORMAT    = 320,
      /*! \brief Rain file gage error */
      ERR_RAIN_FILE_GAGE       = 321,

// ... Runoff File Errors
      /*! \brief Runoff file scratch error */
      ERR_RUNOFF_FILE_OPEN     = 323,
      /*! \brief Runoff file format error */
      ERR_RUNOFF_FILE_FORMAT   = 325,
      /*! \brief Runoff file end error */
      ERR_RUNOFF_FILE_END      = 327,
      /*! \brief Runoff file read error */
      ERR_RUNOFF_FILE_READ     = 329,

// ... Hotstart File Errors
      /*! \brief Hotstart file scratch error */
      ERR_HOTSTART_FILE_OPEN   = 331,
      /*! \brief Hotstart file format error */
      ERR_HOTSTART_FILE_FORMAT = 333,
      /*! \brief Hotstart file read error */
      ERR_HOTSTART_FILE_READ   = 335,

// ... Climate File Errors
      /*! \brief Climate file error */
      ERR_NO_CLIMATE_FILE      = 336,
      /*! \brief Climate file open error */
      ERR_CLIMATE_FILE_OPEN    = 337,
      /*! \brief Climate file read error */
      ERR_CLIMATE_FILE_READ    = 338,
      /*! \brief Climate file end error */
      ERR_CLIMATE_END_OF_FILE  = 339,

// ... RDII File Errors
      /*! \brief RDII file scratch error */
      ERR_RDII_FILE_SCRATCH    = 341,
      /*! \brief RDII file open error */
      ERR_RDII_FILE_OPEN       = 343,
      /*! \brief RDII file format error */
      ERR_RDII_FILE_FORMAT     = 345,
      
// ... Routing File Errors
      /*! \brief Routing file open error */
      ERR_ROUTING_FILE_OPEN    = 351,
      /*! \brief Routing file format error */
      ERR_ROUTING_FILE_FORMAT  = 353,
      /*! \brief Routing file no match error */
      ERR_ROUTING_FILE_NOMATCH = 355,
      /*! \brief Routing file names error */
      ERR_ROUTING_FILE_NAMES   = 357,

// ... Time Series File Errors
      /*! \brief Table file open error */
      ERR_TABLE_FILE_OPEN      = 361,
      /*! \brief Table file read error */
      ERR_TABLE_FILE_READ      = 363,

// ... Runtime Errors
      /*! \brief System error */
      ERR_SYSTEM               = 500,

// ... Additional Errors
      /*! \brief Maximum error message length */
      MAXERRMSG = 1000,
};
/*!
* \}
*/

/*!
* \brief Get the error message for a given error code.
* \param[in] i Error code
* \param[out] msg Error message
* \return Error message
*/
char* error_getMsg(int i, char* msg);

/*!
* \brief Set an error code in the error manager.
* \param[in] errcode Error code
* \param[in] s Error message
* \return Error code
*/
int   error_setInpError(int errcode, char* s);

#endif //ERROR_H
