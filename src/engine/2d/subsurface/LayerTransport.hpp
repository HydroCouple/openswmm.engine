// SPDX-License-Identifier: Apache-2.0
#ifndef OPENSWMM_LAYER_TRANSPORT_HPP
#define OPENSWMM_LAYER_TRANSPORT_HPP
#include <string>
#include <vector>
namespace openswmm::twoD {
/** A nonnegative integrated transport coefficient (m3). Adjacent internal
 * stores share one directed coefficient; to=-1 denotes an external sink.
 * Water coefficients advect aqueous concentration. Grid/soil coefficients
 * include Kd*rho_b and move equilibrium sorbed mass with the actual grains. */
struct LayerTransportMove {int from=-1,to=-1;double capacity=0;};
struct LayerTransportResult {
    std::vector<double> mass,concentration,transferred;
    std::string error;
};
/** Conservative, positivity-preserving implicit donor transport on a column
 * chain. rhs holds mass after already-paired incoming/outgoing receipts;
 * capacity is the final aqueous+sorption storage coefficient. Dry isolated
 * mass is retained, not divided by an artificial water-volume floor. */
LayerTransportResult solveLayerTransport(const std::vector<double>& capacity,
    const std::vector<double>& rhs,const std::vector<LayerTransportMove>& moves,
    bool signed_species=false);
/** Forward-Euler stage evaluated from beginning inventories/storage. All
 * outgoing moves from a donor share one finite beginning-mass budget;
 * incoming receipts never finance another export in the same stage. */
LayerTransportResult advanceLayerTransportEuler(const std::vector<double>& capacity,
    const std::vector<double>& initial,const std::vector<double>& rhs,
    const std::vector<LayerTransportMove>& moves,bool signed_species=false,
    const std::vector<double>* donor_available=nullptr);
}
#endif
