/*!
* \file exfil.h
* \brief Exfiltration object header file.
* \author L. Rossman
* \date Created: 2021-11-01
* \date Last updated: 2024-12-30
* \version 5.3.0
* \details Include file for exfiltration functions.
*/
#ifndef EXFIL_H
#define EXFIL_H

/*!
 * \defgroup Exfil_Enums_Structs_Variables_Functions Exfil File Constants, Enumerations, Structs, Shared Variables, and Functions
 * \brief Exfile file constants, enumerations, structs, shared variables, and functions
 * \{
 */

/*!
 * \addtogroup Exfil_Structs Exfil File Structures
 * \brief Exfil structures.
 * \ingroup Exfil_Enums_Structs_Variables_Functions
 * \{
 */

/*!
* \struct TExfil
* \brief Exfiltration object.
*/
typedef struct
{
    /*! \brief Green-Ampt infiltration parameters for bottom of exfiltration layer */   
    TGrnAmpt*  btmExfil;
    /*! \brief Green-Ampt infiltration parameters for bank of exfiltration layer */
    TGrnAmpt*  bankExfil;
    /*! \brief Area of exfiltration layer */
    double     btmArea;
    /*! \brief Bank minimum depth of exfiltration layer */
    double     bankMinDepth;
    /*! \brief Bank maximum depth of exfiltration layer */
    double     bankMaxDepth;
    /*! \brief Bank maximum area of exfiltration layer */
    double     bankMaxArea;
}   TExfil;

/*!
 * \}
 */

/*!
 * \addtogroup Exfil_Functions Exfil File Functions
 * \brief Exfil file functions.
 * \ingroup Exfil_Enums_Structs_Variables_Functions
 * \{
 */

/*!
* \brief Reads exfiltration parameters from a line of input data.
* \param k Index of line's keyword in project's keyword list
* \param tok Array of string tokens
* \param ntoks Number of tokens
* \param n Number of exfiltration parameters
* \return Error code
*/
int exfil_readStorageParams(int k, char* tok[], int ntoks, int n);

/*!
* \brief Initializes exfiltration parameters.
* \param k Index of exfiltration object in project's object list
*/
void exfil_initState(int k);

/*!
* \brief Gets exfiltration loss rate.
* \param exfil Pointer to exfiltration object
* \param tStep Time step
* \param depth Depth of water above exfiltration layer
* \param area Area of exfiltration layer
* \return Exfiltration loss rate
*/
double exfil_getLoss(TExfil* exfil, double tStep, double depth, double area);
/*!
 * \}
 */

/*!
 * \}
 */

#endif
