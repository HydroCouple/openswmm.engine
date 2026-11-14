// SPDX-License-Identifier: Apache-2.0

/**
 * @file      LayerTransport.hpp
 * @brief     Conservative species transfers between physical layers.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_LAYER_TRANSPORT_HPP
#define OPENSWMM_LAYER_TRANSPORT_HPP
#include <string>
#include <vector>
namespace openswmm::twoD {
/**
 * stores share one directed coefficient; to=-1 denotes an external sink. Water coefficients
 * advect aqueous concentration. Grid/soil coefficients
 */
struct LayerTransportMove {
    int from = -1, to = -1;
    double capacity = 0;
};
struct LayerTransportResult {
    std::vector<double> mass, concentration, transferred;
    std::string error;
};
// Reusable private column workspace. Inputs to the overloads below must not
// alias this storage. Results remain valid until the next use of this scratch;
// the caller must provide exclusive access for the complete call.
struct LayerTransportScratch {
    LayerTransportResult result;
    std::vector<double> diagonal, upper, lower, b, dry, requested, remaining, scale;
};
const LayerTransportResult& solveLayerTransport(
    const std::vector<double>& capacity, const std::vector<double>& rhs,
    const std::vector<LayerTransportMove>& moves, LayerTransportScratch& scratch,
    bool signed_species = false);
const LayerTransportResult& advanceLayerTransportEuler(
    const std::vector<double>& capacity, const std::vector<double>& initial,
    const std::vector<double>& rhs, const std::vector<LayerTransportMove>& moves,
    LayerTransportScratch& scratch, bool signed_species = false,
    const std::vector<double>* donor_available = nullptr);
/**
 * @brief Conservative, positivity-preserving implicit donor transport on a column chain. rhs
 * holds mass after already-paired incoming/outgoing receipts; capacity is the final
 * aqueous+sorption storage coefficient. Dry isolated mass is retained, not divided by an
 * artificial water-volume floor.
 *
 * @param[in] capacity Borrowed nonempty per-layer aqueous-plus-sorption capacities, finite
 * nonnegative m3-equivalent coefficients.
 * @param[in] rhs Borrowed equally sized post-receipt inventories in native concentration*m3;
 * finite and nonnegative unless signed_species is true.
 * @param[in] moves Borrowed integrated nonnegative m3 transfer coefficients with valid distinct
 * donor/recipient indices; to == -1 means an external sink.
 * @param[in] signed_species True allows signed concentration/inventory, used for
 * temperature-volume; false enforces nonnegative inventory.
 * @return Value-owned result with final mass, final dissolved concentration, and transfers in
 * move order. Internal moves must join adjacent layers; isolated zero-capacity inventories are
 * retained separately rather than divided by a volume floor.
 * @note Always inspect result.error before using arrays: invalid dimensions/data, singular
 * systems or nonfinite results return a message and may leave partially filled result vectors.
 * Inputs remain unchanged. Allocations may throw; no input storage is retained or engine state
 * published.
 */
LayerTransportResult solveLayerTransport(const std::vector<double>& capacity,
                                         const std::vector<double>& rhs,
                                         const std::vector<LayerTransportMove>& moves,
                                         bool signed_species = false);
/**
 * @brief Forward-Euler stage evaluated from beginning inventories/storage. All outgoing moves
 * from a donor share one finite beginning-mass budget; incoming receipts never finance another
 * export in the same stage.
 *
 * @param[in] capacity Borrowed nonempty per-layer aqueous-plus-sorption capacities, finite
 * nonnegative m3-equivalent coefficients.
 * @param[in] rhs Borrowed equally sized post-receipt inventories in native concentration*m3;
 * finite and nonnegative unless signed_species is true.
 * @param[in] moves Borrowed integrated nonnegative m3 transfer coefficients with valid distinct
 * donor/recipient indices; to == -1 means an external sink.
 * @param[in] signed_species True allows signed concentration/inventory, used for
 * temperature-volume; false enforces nonnegative inventory.
 * @param[in] initial Borrowed equally sized beginning-stage inventories, native
 * concentration*m3; concentrations are evaluated from these values and capacity.
 * @param[in] donor_available Optional equally sized finite per-donor inventory reservations;
 * limits export alongside min(initial,rhs) for unsigned species. NULL uses no additional
 * reservation.
 * @return Value-owned result with final mass, BEGINNING concentration, and signed transfer
 * amounts in move order. Nonadjacent internal moves are allowed. Unsigned outgoing budgets are
 * shared; signed species skip positivity scaling.
 * @note Always inspect result.error before using arrays: invalid dimensions/data, singular
 * systems or nonfinite results return a message and may leave partially filled result vectors.
 * Inputs remain unchanged. Allocations may throw; no input storage is retained or engine state
 * published.
 */
LayerTransportResult advanceLayerTransportEuler(
    const std::vector<double>& capacity, const std::vector<double>& initial,
    const std::vector<double>& rhs, const std::vector<LayerTransportMove>& moves,
    bool signed_species = false, const std::vector<double>* donor_available = nullptr);
}
#endif
