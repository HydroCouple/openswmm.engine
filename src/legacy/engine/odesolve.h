//-----------------------------------------------------------------------------
//  odesolve.h
//
//  Header file for ODE solver contained in odesolve.c
//
//-----------------------------------------------------------------------------

/**
 * @file      odesolve.h
 * @brief     Adaptive integration of ordinary differential equations.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com> (OpenSWMM maintenance and extensions)
 * @copyright Copyright (c) 2026 Caleb Buahin. OpenSWMM contributions only; original EPA SWMM
 *            credits are retained.
 * @license   Apache-2.0 for OpenSWMM contributions; original EPA SWMM provenance is retained.
 */

#ifndef ODESOLVE_H
#define ODESOLVE_H

// functions that open, close, and use the ODE solver
int odesolve_open(int n);
void odesolve_close(void);
int odesolve_integrate(double ystart[], int n, double x1, double x2, double eps, double h1,
                       void (*derivs)(double, double*, double*));

#endif //ODESOLVE_H
