
/*!
* \file errormanager.h
* \brief Header file for SWMM output API error handling.
* \author Michael E. Tryby (US EPA - ORD/NRMRL)
* \date Created: 2017-08-25
* \date Last edited: 2024-10-17
*/
#ifndef ERRORMANAGER_H_
#define ERRORMANAGER_H_

/*! \brief Maximum length of error message */
#define ERR_MAXMSG 256

/*!
* \typedef p_msg_lookup
* \brief Function pointer for error message lookup
* \param[in] errorcode Error code
* \param[out] message Error message
* \param[in] length Length of error message
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


/*!
* \brief Constructs a new error handle.
* \param[in] message_lookup Function pointer for error message lookup
* \return Pointer to error manager object
*/
error_handle_t* new_error_manager(p_msg_lookup message_lookup);

/*!
* \brief Destroy error manager object
* \param[in] error_handle Pointer to error manager object
*/
void dst_error_manager(error_handle_t* error_handle);

/*!
* \brief Set error code in error manager object
* \param[in] error_handle Pointer to error manager object
* \param[in] errorcode Error code
* \return Error code
*/
int set_error(error_handle_t* error_handle, int errorcode);

/*!
* \brief Check error status and return error message
* \param[in] error_handle Pointer to error manager object
* \return Error message
*/
char* check_error(error_handle_t* error_handle);

/*!
* \brief Clear error status
* \param[in,out] error_handle Pointer to error manager object
*/
void clear_error(error_handle_t* error_handle);


#endif /* ERRORMANAGER_H_ */
