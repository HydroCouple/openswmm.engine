// SPDX-License-Identifier: Apache-2.0
#pragma once
namespace openswmm::surface::ueb_reference {
// Reference units: hours, metres, kJ/m2, degrees Celsius.
// Callers supply one leading padding element for each vector and eight for input.
// This preserves the reference's one-based addressing without out-of-object pointers.
int equilibriumStep(double dt,double* input,double* site,double* state,double* parameters,double* output,double dust,double fresh,double visible_age=.2,double infrared_age=.5,double snow_fraction=-1);
int forceRestoreStep(double dt,double* input,double* site,double* state,double* temperatures,
    double* averages,int count,double* parameters,double* output,double dust,double surface_mean,double bulk_mean,double visible_age=.2,double infrared_age=.5,double snow_fraction=-1);
}
