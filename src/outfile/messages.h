 /*! 
 * \file messages.h
 * \brief Header file for SWMM output API error and warning messages.
 * \author Michael Tryby (US EPA - ORD/NRMRL)
 * \date Created: 2017-10-20
 * \date Last edited: 2024-10-23
 */
#ifndef SRC_MESSAGES_H_
#define SRC_MESSAGES_H_


 /*! 
 * \def MAXMSG
 * \brief Maximum length of error message 
 */
#define MAXMSG 56

/*!
* \defgroup Output_Error_Warning_Local_Functions Output API Error, Warning, and Functions
* \brief Output API error, warning, and local functions
* \{
*/

/*!
* \addtogroup Output_API_Error_Messages Output API Error and Warning Messages
* \brief Error messages for the output API
* \ingroup Output_Error_Warning_Local_Functions
* \{
*/

/*!
* \def WARN10
* \brief Warning: model run issued warnings
*/
#define WARN10 "Warning: model run issued warnings"

/*!
* \def ERR101
* \brief Error 101: memory allocation failure
*/
#define ERR411 "Error 411: memory allocation failure"

/*!
* \def ERR421
* \brief Error 421: invalid parameter code
*/
#define ERR421 "Input Error 421: invalid parameter code"

/*!
* \def ERR422
* \brief Error 422: reporting period index out of range
*/
#define ERR422 "Input Error 422: reporting period index out of range"

/*!
* \def ERR423
* \brief Error 423: element index out of range
*/
#define ERR423 "Input Error 423: element index out of range"

/*!
* \def ERR424
* \brief Error 424: no memory allocated for results
*/
#define ERR424 "Input Error 424: no memory allocated for results"

/*!
* \def ERR434
* \brief Error 434: unable to open binary output file
*/
#define ERR434 "File Error 434: unable to open binary output file"

/*!
* \def ERR435
* \brief Error 435: invalid file - not created by SWMM
*/
#define ERR435 "File Error 435: invalid file - not created by SWMM"

/*!
* \def ERR436
* \brief Error 436: invalid file - contains no results
*/
#define ERR436 "File Error 436: invalid file - contains no results"

/*!
* \def ERR440
* \brief Error 440: an unspecified error has occurred
*/
#define ERR440 "ERROR 440: an unspecified error has occurred"

/*!
 * \}
 */

/*!
 * \}
 */

#endif /* SRC_MESSAGES_H_ */
