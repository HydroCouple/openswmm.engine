/*!
 * \file project.h
 * \brief Header file for the SWMM project class.
 * \author Caleb Buahin (EPA\ORD)
 * \date 2023-10-01
 */

#ifndef SWMM_H
#define SWMM_H

#include <string>
#include <vector>

#include "swmm5_export.h"

using namespace std;

/*!
 * \class SWMM
 * \brief Class for managing a SWMM project.
 *
 *   This module provides project-related services such as:
 *   - opening a new project and reading its input data
 *   - allocating and freeing memory for project objects
 *   - setting default values for object properties and options
 *   - initializing the internal state of all objects
 *   - managing hash tables for identifying objects by ID name
 *
 *   Update History
 *   ==============
 *   Build 5.1.004:
 *   - Ignore RDII option added.
 *   Build 5.1.007:
 *   - Default monthly adjustments for climate variables included.
 *   - User-supplied GW flow equations initialized to NULL.
 *   - Storage node exfiltration object initialized to NULL.
 *   - Freeing of memory used for storage node exfiltration included.
 *   Build 5.1.008:
 *   - Constants used for dynamic wave routing moved to dynwave.c.
 *   - Input processing of minimum time step & number of
 *     parallel threads for dynamic wave routing added.
 *   - Default values of hyd. conductivity adjustments added.
 *   - Freeing of memory used for outfall pollutant load added.
 *   Build 5.1.009:
 *   - Fixed bug in computing total duration introduced in 5.1.008.
 *   Build 5.1.011:
 *   - Memory management of hydraulic event dates array added.
 *   Build 5.1.012:
 *   - Minimum conduit slope option initialized to 0 (none).
 *   - NO/YES no longer accepted as options for NORMAL_FLOW_LIMITED.
 *   Build 5.1.013:
 *   - omp_get_num_threads function protected against lack of compiler
 *     support for OpenMP.
 *   - Rain gage validation now performed after subcatchment validation.
 *   - More robust parsing of MinSurfarea option provided.
 *   - Support added for new RuleStep analysis option.
 *   Build 5.1.015:
 *   - Support added for multiple infiltration methods within a project.
 *   Build 5.2.0:
 *   - Support added for Streets and Inlets.
 *   - Support added for RptFlags.disabled option.
 *   - Object's rptFlag changed to record its index in output file.
 *   Build 5.2.2:
 *   - Default number of threads changed from OMP max. number to 1
 *     to be consistent with User Manual.
 *   Build 5.2.4:
 *   - Default Inertial Damping changed from SOME to PARTIAL_DAMPING.
 *   - Default CourantFactor changed from 0 (fixed routing time step)
 *   - to 0.75 (variable time step)
 *   Build 5.3.0:
 *   - Fixed potential precision loss when calculating TotalDuration.
 *   - Memory allocation and reading options for saving multiple hotstart files
 *   - Added support for api provided pollutant fluxes
 */

class SWMMProject;

/*!
 * \brief ISlot interface class must be implemented by classes that want to listen to signals.
 * \details ISlot is a template class that can be used to listen to signals with any number of arguments.
 * \tparam Args are the arguments that will be passed by the signal.
 * \sa ISignal
 */
template <typename... Args>
class ISlot
{
public:
    /*!
     * \brief ISlot::~ISlot is a virtual destructor.
     */
    virtual ~ISlot() = default;

    /*!
     * \brief operator() is the function call operator that is called when a signal is emitted.
     * \param[in] sender is the object that emitted the signal.
     * \param[in] args are the arguments passed by the signal.
     */
    virtual void operator()(const ISignal<Args...> &sender, Args... args) = 0;
};

/*!
 * \brief ISignal interface class is used to emit signals/events to listeners.
 * \tparam Args are the arguments that will be passed by the signal.
 * \sa ISlot
 * \sa IPropertyChanged
 */
template <typename... Args>
class ISignal
{
public:
    /*!
     * \brief ISignal::~ISignal is a virtual destructor.
     */
    virtual ~ISignal() = default;

    /*!
     * \brief connect is used to connect a slot to the signal.
     * \param[in] slot is the slot that will listen to the signal.
     */
    virtual void connect(const shared_ptr<ISlot<Args...>> &slot) = 0;

    /*!
     * \brief disconnect is used to disconnect a slot from the signal.
     * \param[in] slot is the slot that will be disconnected from the signal.
     */
    virtual void disconnect(const shared_ptr<ISlot<Args...>> &slot) = 0;

    /*!
     * \brief blockSignals is used to block signals from being emitted.
     * \param[in] block is a boolean value that is used to specify if signals should be blocked or not.
     */
    virtual void blockSignals(bool block) = 0;

protected:
    /*!
     * \brief emit is used to emit the signal.
     * \param[in] args are the arguments that will be passed by the signal.
     */
    virtual void emit(Args... args) = 0;
};

/*!
 * \brief The ModelStatusChangeEventArgs contains the information that will
 * be passed when the SWMMProject fires a signal.
 * \details Sending exchange item events is optional,
 * so it should not be used as a mechanism to build critical functionality upon.
 */
class ModelStatusChangeEventArgs
{
public:
    /*!
     * \brief ~ModelStatusChangeEventArgs destructor
     */
    virtual ~ModelStatusChangeEventArgs() = default;

    /*!
     * \brief Gets the IModelComponent that fired the event.
     * \returns The IModelComponent that threw the event.
     */
    virtual SWMMProject *component() const = 0;

    /*!
     * \brief Gets the IModelComponent's status before the status change.
     * \returns The previous ComponentStatus of the component that threw the event.
     */
    virtual SWMMProject::ModelStatus previousStatus() const = 0;

    /*!
     * \brief Gets the IModelComponent's status after the status change.
     * \returns The new ComponentStatus of the component that threw the event.
     */
    virtual SWMMProject::ModelStatus status() const = 0;

    /*!
     * \brief Gets additional information about the status change.
     */
    virtual string message() const = 0;

    /*!
     * \brief A bool indicating whether this event has a progresss monitor.
     * \returns True if status has a percent progress otherwise false and the progress bar shows busy.
     */
    virtual bool hasProgressMonitor() const = 0;

    /*!
     * \brief Number between 0 and 100 indicating the progress made by a component in its simulation.
     * \returns A number between 0 and 100 indicating the progress made by a component.
     */
    virtual float percentProgress() const = 0;
};

class EXPORT_SWMM_SOLVER_API SWMMProject
{

public:
    /*!
     * \brief ModelStatus is an enumeration of the different statuses of the model.
     */
    enum ModelStatus
    {
        /*!
         * \brief The model instance is in the CREATED state. instance has just been created.
         * This status must and will be followed by HydroCouple::Initializing.
         */
        CREATED = 0,
        INITIALIZING,
        INITIALIZED,
        VALIDATING,
        VALIDATED,
        INVALID,
        STARTING,
        RUNNING,
        COMPLETED,
        FINISHING,
        FINISHED,
        FAILED
    };

    /*!
     * \brief Constructor for the SWMMProject class.
     * \param inputFile Path to the SWMM input file.
     * \param reportFile Path to the SWMM report file (optional).
     * \param outputFile Path to the SWMM output file (optional).
     */
    SWMMProject(const string &inputFile, const string &reportFile = "", const string &outputFile = "");
    
    ~SWMMProject();

    void initialize();

    vector<string> validate() const;

    void start();

    ModelStatus status();

    double currentTime() const;

private:
    string m_inputFile, m_reportFile, m_outputFile;
    ModelStatus m_status;
    vector<string> m_validationErrors;
};

#endif