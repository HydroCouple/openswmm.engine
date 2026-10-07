// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NodeCoupling.hpp"

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
               const std::vector<double>* node_conc = nullptr);
private:
    struct Point {
        int index, node;
        std::vector<int> cells;
        std::vector<double> fraction, reserved;
        double total = 0, area = 0;
    };
    std::vector<Point> points_;
    std::vector<int> degree_;
    std::vector<double> node_area_, node_reserved_;
};
}
