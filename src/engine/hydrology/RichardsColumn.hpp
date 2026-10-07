// SPDX-License-Identifier: Apache-2.0
#ifndef OPENSWMM_RICHARDS_COLUMN_HPP
#define OPENSWMM_RICHARDS_COLUMN_HPP
#include <string>
#include <vector>

namespace openswmm::richards {
// All kernel quantities are SI. Storage includes explicitly authored elastic
// water storage; theta is pore water content and remains <= theta_s.
struct Material {
    double theta_r = 0.0, alpha = 0.0, n = 0.0, l = 0.5, specific_storage = 0.0;
};
struct Options {
    bool enabled = false;
    int cells_per_layer = 8;
    double atol = 1.e-7, rtol = 1.e-5, max_step = 30.0;
};
struct Cell {
    double volume = 0.0, area = 0.0, z = 0.0, dz = 0.0;
    // Surface theta_s is the open storage fraction; area is always the full
    // contact footprint. Porous theta_s is saturated pore water content.
    double theta_s = 1.0, Ks = 0.0, wilting = 0.0, potential_et = 0.0;
    Material material;
};
enum class Bottom { Sealed, FreeDrainage, Head };
struct Column {
    // First cell is a surface reservoir at z=soil-surface elevation; the
    // remaining cells are porous control volumes ordered top to bottom.
    std::vector<Cell> cells;
    std::vector<double> water;
    Options options;
    Bottom bottom = Bottom::Sealed;
    double bottom_K = 0.0, bottom_head = 0.0, bottom_distance = 0.0;
};
// Negative indices: -3 evaporation, -4 bottom outflow, -5 bottom inflow.
struct Transfer { int from = -1, to = -1; double volume = 0.0; };
struct Report {
    bool ok = true;
    std::string error;
    int accepted = 0, rejected = 0, rhs = 0, newton = 0;
    double min_step = 0.0, bottom_volume = 0.0, evaporation = 0.0, balance = 0.0;
    std::vector<Transfer> transfers;
};
bool valid(const Material& p, double theta_s);
bool valid(const Options& o);
double theta(const Material& p, double theta_s, double pressure);
double storage(const Material& p, double theta_s, double pressure);
double pressure(const Material& p, double theta_s, double storage_fraction);
double conductivity(const Material& p, double Ks, double pressure);

// Adaptive stiff BDF1 with step doubling. Compatible column sizes are packed
// cell-major, node-lane minor; each lane has its own clock and error test.
// Failed calls leave all supplied column states unchanged.
std::vector<Report> advance(std::vector<Column*>& columns, double dt);
}
#endif
