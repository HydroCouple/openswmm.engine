//-----------------------------------------------------------------------------
//   street.h
//
//   Project: EPA SWMM5
//   Version: 5.2
//   Date:    11/01/21  (Build 5.2.0)
//   Author:  L. Rossman
//
//   Definition of the Street Cross-Section Object
//
//-----------------------------------------------------------------------------

/**
 * @file      street.h
 * @brief     Street cross-section geometry and hydraulic properties.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com> (OpenSWMM maintenance and extensions)
 * @copyright Copyright (c) 2026 Caleb Buahin. OpenSWMM contributions only; original EPA SWMM
 *            credits are retained.
 * @license   Apache-2.0 for OpenSWMM contributions; original EPA SWMM provenance is retained.
 */

#ifndef STREET_H
#define STREET_H

// Shared street functions
int street_create(int nStreets);
void street_delete();
int street_readParams(char* tok[], int ntoks);
double street_getExtentFilled(int link);

#endif
