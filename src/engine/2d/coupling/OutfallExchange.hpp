// SPDX-License-Identifier: Apache-2.0

/**
 * @file      OutfallExchange.hpp
 * @brief     Routing-step reservation and application of outfall exchanges.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "NodeCoupling.hpp"
#include <algorithm>
#include <array>

namespace openswmm::detail {
class CompletedSourceArchive;
}
namespace openswmm::twoD {
// A routing-step reservation, consumed before the next surface advance.
// Overlapping outfalls divide each donor cell; incident 1D links divide each
// outfall's rate. Unused reservations expire rather than being spent twice.
class OutfallExchange {
public:
    void initialize(const MeshData&, const std::vector<CouplingPoint>&, SimulationContext&);
    void prepare(const MeshData&, const SurfaceStateData&, const SolverOptions2D&,
                 SimulationContext&, double dt);
    bool apply(const MeshData&, SurfaceStateData&, const SolverOptions2D&, SimulationContext&,
               double dt, std::vector<double>& accumulated,
               const std::vector<double>* node_conc = nullptr,
               std::vector<double>* cell_accumulated = nullptr);
    /// Book one routing step of each coupled outfall's tailwater (after
    /// updateOutfallBoundaries cached it): time the override is engaged,
    /// time the outlet crown is drowned, and the highest engaged stage.
    void recordTailwater(const SimulationContext&, double dt);
    /**
     * @brief A coupled outfall's tailwater record for the report: seconds with the 2D tailwater
     * engaged / above the outlet crown, and the highest engaged stage (1D length; -1e30 if never
     * engaged). False for an uncoupled node.
     *
     * @param[in] node Zero-based original 1D node ID; unmapped returns false.
     * @param[out] engaged_s Cumulative engaged duration s on true.
     * @param[out] submerged_s Cumulative crown-submerged duration s on true.
     * @param[out] max_stage Maximum engaged native 1D stage, -1e30 sentinel if never engaged.
     * @return True after copying all three records for a mapped node. False leaves outputs
     * unchanged. Read-only and allocation-free; caller preserves matching initialized arrays and
     * serializes updates.
     */
    bool tailwater(int node, double& engaged_s, double& submerged_s, double& max_stage) const;

private:
    friend class SurfaceRouter2D;
    void checkpoint(detail::CompletedSourceArchive&);
    void validateCheckpoint(const SolverOptions2D&, const std::vector<double>& limits,
                            const std::vector<double>& accumulated, bool changed,
                            const std::array<double, 2>& ledger) const;
    /**
     * @brief Clear accepted per-cell outfall receipts after the surface window consumes them.
     * @note Zeros pending_ only, retaining reservations, preparation flags and cumulative
     * tailwater records. Does not alter surface water, species or mass balance. Requires
     * exclusive owner access; no allocation occurs.
     */
    void resetWindow() { std::fill(pending_.begin(), pending_.end(), 0.0); }
    struct Point {
        int index, node;
        std::vector<int> cells;
        std::vector<double> fraction, reserved;
        double total = 0, area = 0;
        /// SPREAD: the receiving cells within the radius (own cells first),
        /// and the highest own-cell bed (m) — a farther cell receives while
        /// it is wet or no higher than that.
        std::vector<int> spread;
        double bed_ref = 0;
    };
    std::vector<Point> points_;
    std::vector<int> recv_; ///< Reused SPREAD scratch; empty outside apply(), not checkpointed.
    std::vector<int> nodes_; ///< distinct outfall nodes of points_
    std::vector<int> degree_;
    std::vector<double> node_area_, node_reserved_;
    /// Per node: tailwater record (s, s, 1D length); see recordTailwater().
    std::vector<double> tw_engaged_s_, tw_submerged_s_, tw_max_;
    std::vector<double> pending_; // accepted per-cell receipts since the last surface batch
    double prepared_dt_ = 0;
    bool prepared_ = false; // false after consumption; unused reservations expire
};
}
