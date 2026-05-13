/**
 * @file Inlet.hpp
 * @brief Street inlet capture efficiency — FHWA HEC-22.
 *
 * @details Inlet types: GRATE, CURB, COMBO, SLOTTED, DROP_GRATE, DROP_CURB, CUSTOM.
 *          On-grade capture uses splash-over velocity and frontal/longitudinal
 *          flow fractions. On-sag capture uses weir/orifice transition.
 *
 *          Batch: group inlets by type, compute capture in vectorised batches.
 *
 * @note Legacy reference: src/legacy/engine/inlet.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_INLET_HPP
#define OPENSWMM_INLET_HPP

#include <vector>

namespace openswmm {

struct SimulationContext;
struct InletUsageStore;

namespace inlet {

enum class InletType : int {
    GRATE       = 0,
    CURB        = 1,
    COMBO       = 2,
    SLOTTED     = 3,
    DROP_GRATE  = 4,
    DROP_CURB   = 5,
    CUSTOM      = 6
};

enum class GrateType : int {
    P_BAR_50      = 0,
    P_BAR_50x100  = 1,
    P_BAR_30      = 2,
    CURVED_VANE   = 3,
    TILT_BAR_45   = 4,
    TILT_BAR_30   = 5,
    RETICULINE    = 6,
    GENERIC       = 7
};

struct InletSoA {
    int count = 0;
    std::vector<int>    link_idx;       ///< Street conduit link index
    std::vector<int>    node_idx;       ///< Receiving node index
    std::vector<int>    bypass_node;    ///< Bypass node (downstream of street conduit)
    std::vector<int>    inlet_type;
    std::vector<int>    grate_type;
    std::vector<double> grate_length;   ///< Grate inlet length (ft)
    std::vector<double> grate_width;    ///< Grate inlet width (ft)
    std::vector<double> curb_length;    ///< Curb opening length (ft)
    std::vector<double> curb_height;    ///< Curb opening height (ft)
    std::vector<int>    curb_throat;    ///< Throat angle type (0=horiz, 1=inclined, 2=vert)
    std::vector<double> slotted_length; ///< Slotted drain length (ft)
    std::vector<double> slotted_width;  ///< Slotted drain width (ft)
    std::vector<double> clog_factor;    ///< Clogging reduction (0-1)
    std::vector<double> opening_ratio;  ///< Grate opening ratio
    std::vector<int>    num_inlets;     ///< Number of inlets per side
    std::vector<double> flow_limit;     ///< Max capture flow per inlet (cfs)
    std::vector<double> local_depress;  ///< Local gutter depression (ft)
    std::vector<double> local_width;    ///< Local depression width (ft)
    std::vector<int>    n_sides;        ///< 1 or 2 sided street

    // Street geometry (resolved from StreetStore)
    std::vector<double> sx;             ///< Street cross slope (fraction)
    std::vector<double> gutter_depression; ///< Street gutter depression (ft)
    std::vector<double> gutter_width;   ///< Street gutter width (ft)
    std::vector<double> road_roughness; ///< Street Manning's n
    std::vector<double> t_crown;        ///< Distance curb to crown (ft)

    // Working arrays (per-inlet results)
    std::vector<double> flow_capture;   ///< Captured flow rate (cfs)

    // Gap #56: backflow — fraction of capture node overflow that returns to bypass
    std::vector<double> backflow_ratio; ///< Pre-computed fraction of node overflow → backflow
    std::vector<double> backflow;       ///< Current backflow rate (cfs), updated each step

    // Gap #68: statistics (accumulated each timestep)
    std::vector<double> stat_capture_vol;   ///< Total captured volume (ft³)
    std::vector<double> stat_bypass_vol;    ///< Total bypassed volume (ft³)
    std::vector<double> stat_backflow_vol;  ///< Total backflow volume (ft³)
    std::vector<double> stat_peak_flow;     ///< Peak captured flow rate (cfs)

    void resize(int n);
};

class InletSolver {
public:
    void init(SimulationContext& ctx);

    /**
     * @brief Batch compute inlet capture for all inlets.
     *
     * @param ctx  Simulation context.
     * @param dt   Timestep (seconds).
     */
    void computeAll(SimulationContext& ctx, double dt);

    /**
     * @brief Adjust quality inflows at bypass and capture nodes for inlet transfers.
     *
     * @details Gap #55: called after hydraulic routing + inlet capture, before
     *          quality routing. For each inlet, transfers pollutant mass based on
     *          net flow direction (capture → capture node, backflow → bypass node).
     *          Matches legacy `inlet_adjustQualInflows()`.
     *
     * @param ctx  Simulation context.
     * @param dt   Routing timestep (seconds).
     */
    void adjustQualInflows(SimulationContext& ctx, double dt);

    /**
     * @brief Gap #68: Copy accumulated stats from internal SoA into InletUsageStore.
     *        Called by SWMMEngine::report() before writing summary reports.
     */
    void gatherStats(InletUsageStore& usages) const;

private:
    InletSoA soa_;

    /// On-grade grate capture efficiency (HEC-22 Eq 4-19/4-21).
    static double grateCapture(double flow, double velocity, double length,
                                double sx, int grate_type, double open_ratio);

    /// On-grade curb opening capture (HEC-22 Eq 4-22a/4-23).
    static double curbCapture(double flow, double curb_length,
                               double sx, double sl, double n,
                               double gutter_depress, double gutter_width,
                               double spread);

    /// Splash-over velocity for grate type.
    static double splashOverVelocity(double length, int grate_type);

    /// Compute flow spread from Izzard's Manning equation for triangular gutter.
    static double computeFlowSpread(double flow, double qfactor,
                                     double sx, double sw, double a_gutter,
                                     double w_gutter, double t_crown);

    /// Compute Eo (gutter flow ratio) from HEC-22 Eq 4-4.
    static double computeEo(double sr, double ts, double w);

    /// Compute on-grade capture for a single inlet (grate, curb, combo, slotted).
    double computeOnGradeCapture(int idx, double flow, double depth) const;

    /// Gap #56: Total unclogged open area of inlet ii (ft²). Returns 0 for CUSTOM type.
    double getInletArea(int ii) const noexcept;

    /// Gap #56: Pre-compute backflow_ratio[] for all inlets from capture-node topology.
    void computeBackflowRatios();
};

} // namespace inlet
} // namespace openswmm

#endif // OPENSWMM_INLET_HPP
