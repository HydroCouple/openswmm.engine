/*!
* \file findroot.h
* \brief Header file for root finding method.
* \author L. Rossman
* \date Created: 2013-11-19
* \date Last updated: 2025-01-02
*/

#ifndef FINDROOT_H
#define FINDROOT_H

/*!
* \typedef newton_callback
* \brief Callback function for Newton-Raphson root finder.
* \param x Value of the independent variable.
* \param f Value of the function at x.
* \param df Value of the derivative of the function at x.
* \param p Pointer to additional data.
*/
typedef void (*newton_callback) (double x, double* f, double* df, void* p);

/*!
* \typedef ridder_callback
* \brief Callback function for Ridder's root finder.
* \param x Value of the independent variable.
* \param p Pointer to additional data.
*/
typedef double (*ridder_callback) (double x, void* p);

/*!
* \brief Finds the root of a function using Newton's method.
* \param x1 Lower bound of the bracketing interval.
* \param x2 Upper bound of the bracketing interval.
* \param rts Pointer to the root.
* \param xacc Desired accuracy of the root.
* \param func Callback function for the function and its derivative.
* \param p Pointer to additional data.
* \return Number of function evaluations used or 0 if the maximum allowed iterations were exceeded.
*/
int findroot_Newton(double x1, double x2, double* rts, double xacc, newton_callback func, void* p);

/*!
* \brief Finds the root of a function using Ridder's method.
* \param x1 Lower bound of the bracketing interval.
* \param x2 Upper bound of the bracketing interval.
* \param xacc Desired accuracy of the root.
* \param func Callback function for the function.
* \param p Pointer to additional data.
* \return The root of the function.
*/
double findroot_Ridder(double x1, double x2, double xacc, ridder_callback func, void* p);

#endif //FINDROOT_H
