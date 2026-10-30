/**
 * @file      errormanager.h
 * @brief     Header file for SWMM output API error handling.
 * @author    Michael E. Tryby (US EPA - ORD/NRMRL)
 * @date      Created: 2017-08-25
 * @date      Last edited: 2024-10-17
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com> (OpenSWMM maintenance and extensions)
 * @copyright Copyright (c) 2026 Caleb Buahin. OpenSWMM contributions only; original EPA SWMM
 *            credits are retained.
 * @license   Apache-2.0 for OpenSWMM contributions; original EPA SWMM provenance is retained.
 */

#ifndef ERRORMANAGER_H_
#define ERRORMANAGER_H_

/*! \brief Maximum length of error message */
#define ERR_MAXMSG 256

/*!
* \enum OutputErrorTypes
* \brief Error codes
*/
enum OutputErrorTypes {
    /*! \brief Error 411: memory allocation failure */
    ERR411 = 411,
    /*! \brief Error 421: invalid parameter code */
    ERR421 = 421,
    /*! \brief Error 422: reporting period index out of range */
    ERR422 = 422,
    /*! \brief Error 423: element index out of range */
    ERR423 = 423,
    /*! \brief Error 424: no memory allocated for results */
    ERR424 = 424,
    /*! \brief Error 434: unable to open binary output file */
    ERR434 = 434,
    /*! \brief Error 435: invalid file - not created by SWMM */
    ERR435 = 435,
    /*! \brief Error 436: invalid file - contains no results */
    ERR436 = 436,
    /*! \brief rror 440: an unspecified error has occurred */
    ERR440 = 440,
};

/*!
* \enum OutputWarningTypes
* \brief Warning codes
*/
enum OutputWarningTypes {
    /*! \brief Model run issued warnings */
    WARN10 = 10,
};

/**
 * @brief Describe a borrowed lookup callback used to format one error code.
 *
 * Invoked synchronously by check_error. The manager retains the callback pointer until destruction,
 * so its code must remain loaded for that lifetime.
 *
 * @param[in] errorcode Stored nonzero warning/error code to translate.
 * @param[out] message Writable caller-owned buffer of length bytes; write a bounded null-terminated
 *     message.
 * @param[in] length Positive writable buffer capacity in bytes, including its terminator.
 */
typedef void (*p_msg_lookup)(int errorcode, char* message, int length);

/*!
* \struct error_s
* \brief Structure for managing errors
*
* This structure holds the error status and a function pointer for looking up error messages.
*
* \var error_s::error_status
* Error status code.
*
* \var error_s::message_lookup
* Function pointer for error message lookup.
*/
typedef struct error_s {
    /*! \brief Error status code */
    int error_status;

    /*! \brief Function pointer for error message lookup */
    p_msg_lookup message_lookup;

} error_handle_t;

/**
 * @brief Allocate a zero-status error manager and retain its lookup callback.
 *
 * @param[in] message_lookup Callback kept in the manager; must remain callable for its lifetime and
 *     be non-null before checking a nonzero error.
 * @return Owned calloc-initialized manager, released with dst_error_manager. Allocation failure is
 *     not safely handled: the implementation dereferences the allocation immediately.
 */
error_handle_t* new_error_manager(p_msg_lookup message_lookup);

/**
 * @brief Release an error manager allocation.
 *
 * No message allocations are owned by the manager. Caller remains responsible for strings returned
 * by check_error; the argument itself is not cleared.
 *
 * @param[in] error_handle Owned manager to destroy, or null (free accepts null); all aliases become
 *     invalid.
 */
void dst_error_manager(error_handle_t* error_handle);

/**
 * @brief Record a nonzero error without clearing a previous status on success.
 *
 * @param[in,out] error_handle Live manager; non-null is required only when errorcode is nonzero.
 * @param[in] errorcode Nonzero status replaces the stored code; zero leaves the previous code
 *     intact.
 * @return The supplied errorcode unchanged, including zero.
 */
int set_error(error_handle_t* error_handle, int errorcode);

/**
 * @brief Allocate a copy of the current stored error message.
 *
 * Invokes lookup synchronously only when allocation succeeds. Does not clear status or retain the
 * message; each call creates a separate allocation.
 *
 * @param[in] error_handle Live non-null manager with a callable lookup for nonzero status; manager
 *     is read only.
 * @return Owned zero-initialized ERR_MAXMSG-byte message allocation, or null for zero status or
 *     allocation failure; free the returned allocation.
 */
char* check_error(error_handle_t* error_handle);

/**
 * @brief Explicitly clear the sticky error status.
 *
 * @param[in,out] error_handle Live non-null manager; error_status becomes zero, callback remains
 *     unchanged.
 */
void clear_error(error_handle_t* error_handle);

#endif /* ERRORMANAGER_H_ */
