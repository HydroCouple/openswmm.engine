//-----------------------------------------------------------------------------
//   inlet.h
//
//   Project: EPA SWMM5
//   Version: 5.2
//   Date:    11/01/21 (Build 5.2.0)
//   Author:  L. Rossman
//
//   Street/Channel Inlet Functions
//
//-----------------------------------------------------------------------------

/**
 * @file      inlet.h
 * @brief     Street inlet configuration, capture, and bypass calculations.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com> (OpenSWMM maintenance and extensions)
 * @copyright Copyright (c) 2026 Caleb Buahin. OpenSWMM contributions only; original EPA SWMM
 *            credits are retained.
 * @license   Apache-2.0 for OpenSWMM contributions; original EPA SWMM provenance is retained.
 */

#ifndef INLET_H
#define INLET_H

typedef struct TInlet TInlet;

int inlet_create(int nInlets);
void inlet_delete();
int inlet_readDesignParams(char* tok[], int ntoks);
int inlet_readUsageParams(char* tok[], int ntoks);
void inlet_validate();

void inlet_findCapturedFlows(double tStep);
void inlet_adjustQualInflows();
void inlet_adjustQualOutflows();

void inlet_writeStatsReport();
double inlet_capturedFlow(int link);

#endif
