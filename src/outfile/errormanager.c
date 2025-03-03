/*!
* \file errormanager.c
* \brief Source file providing a simple interface for managing SWMM output API runtime error messages.
* \author Michael E. Tryby (US EPA - ORD/NRMRL)
* \date Created: 2017-08-25
* \date Last edited: 2024-10-17
*/
#include <string.h>
#include <stdlib.h>
#include "errormanager.h"


/*!
* \copydoc new_error_manager
*/
error_handle_t* new_error_manager(p_msg_lookup message_lookup)
{	
	error_handle_t *error_handle = NULL;
	error_handle = (error_handle_t*)calloc(1, sizeof(error_handle_t));

	error_handle->message_lookup = message_lookup;

	return error_handle;
}

/*!
* \copydoc dst_error_manager
*/
void dst_error_manager(error_handle_t* error_handle)
{
	free(error_handle);
}

/*!
* \copydoc set_error
*/
int set_error(error_handle_t* error_handle, int errorcode)
{
	// If the error code is 0 no action is taken and 0 is returned.
	// This is a feature not a bug.
	if (errorcode)
		error_handle->error_status = errorcode;

	return errorcode;
}

/*!
* \copydoc check_error
*/
char* check_error(error_handle_t* error_handle)
{
	char* temp = NULL;

	if (error_handle->error_status != 0) {
		temp = (char*) calloc(ERR_MAXMSG, sizeof(char));

		if (temp)
			error_handle->message_lookup(error_handle->error_status, temp, ERR_MAXMSG);
	}
	return temp;
}

/*!
* \copydoc clear_error
*/
void clear_error(error_handle_t* error_handle)
{
	error_handle->error_status = 0;
}
