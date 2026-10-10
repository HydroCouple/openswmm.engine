// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NodeCoupling.hpp"
#include <algorithm>
#include <array>

namespace openswmm::detail { class CompletedSourceArchive; }
namespace openswmm::twoD {
// A routing-step reservation, consumed before the next surface advance.
// Overlapping outfalls divide each donor cell; incident 1D links divide each
// outfall's rate. Unused reservations expire rather than being spent twice.
class OutfallExchange {
public:
    void initialize(const MeshData&, const std::vector<CouplingPoint>&,
                    SimulationContext&);
    void prepare(const MeshData&, const SurfaceStateData&, const SolverOptions2D&, SimulationContext&, double dt);
    bool apply(const MeshData&, SurfaceStateData&, const SolverOptions2D&,
               SimulationContext&, double dt, std::vector<double>& accumulated,
               const std::vector<double>* node_conc = nullptr,
               std::vector<double>* cell_accumulated = nullptr);
private:
    friend class SurfaceRouter2D;
    void checkpoint(detail::CompletedSourceArchive&);
    void validateCheckpoint(const SolverOptions2D&, const std::vector<double>& limits,
                            const std::vector<double>& accumulated, bool changed,
                            const std::array<double,2>& ledger) const;
    void resetWindow() { std::fill(pending_.begin(), pending_.end(), 0.0); }
    struct Point {
        int index, node;
        std::vector<int> cells;
        std::vector<double> fraction, reserved;
        double total = 0, area = 0;
    };
    std::vector<Point> points_;
    std::vector<int> degree_;
    std::vector<double> node_area_, node_reserved_;
    std::vector<double> pending_; // accepted per-cell receipts since the last surface batch
    double prepared_dt_ = 0;
    bool prepared_ = false; // false after consumption; unused reservations expire
};
}
