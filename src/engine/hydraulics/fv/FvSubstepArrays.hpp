// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file      FvSubstepArrays.hpp
 * @brief     The arrays one explicit FV substep reads and writes, as raw pointers.
 *
 * @details Declarations only, so the host solver, the plugin interface and the
 *          device kernels can all name the same struct without pulling the
 *          kernel bodies (and their host/device function attributes) into
 *          translation units that never run them.
 *
 * @see FvSubstepKernels.hpp, FvDeviceMarcher.hpp
 * @ingroup engine_fv
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_FV_SUBSTEP_ARRAYS_HPP
#define OPENSWMM_ENGINE_FV_SUBSTEP_ARRAYS_HPP

#include <cstdint>

namespace openswmm::hydkernels {
struct CulvertCurve;
}

namespace openswmm::fv {
struct FvGeometry;
namespace kernels {
struct FaceState;
}
} // namespace openswmm::fv

namespace openswmm::fv::substep {

/// Every array one substep reads or writes. Plain pointers so the same struct
/// describes host vectors (ExplicitFvSolver) and device views (the marcher).
struct Arrays {
    // ---- sizes ----------------------------------------------------------
    int n_cells = 0;
    int n_faces = 0;
    int n_nodes = 0;
    int n_structs = 0;

    // ---- mesh (static) --------------------------------------------------
    const int* cell_geom = nullptr;
    const int* cell_conduit = nullptr;
    const int* cell_face0 = nullptr;
    const int* cell_face1 = nullptr;
    const std::int8_t* cell_side0 = nullptr;
    const std::int8_t* cell_side1 = nullptr;
    const double* cell_dx = nullptr;
    const double* cell_zb = nullptr;

    const int* face_cl = nullptr;
    const int* face_cr = nullptr;
    const int* face_node = nullptr;
    const int* face_geom = nullptr;
    const int* face_culvert = nullptr;
    const int* face_npos = nullptr; ///< position of the face in its node's row
    const std::uint8_t* face_gate = nullptr;
    const double* face_zb = nullptr;
    const std::int8_t* face_dir_l = nullptr;
    const std::int8_t* face_dir_r = nullptr;
    const double* face_a_rim = nullptr;

    const int* conduit_section = nullptr;
    const double* conduit_roughness = nullptr;
    const double* conduit_rough_factor = nullptr;
    const double* conduit_loss_inlet = nullptr;
    const double* conduit_loss_outlet = nullptr;
    const double* conduit_slope = nullptr;
    const hydkernels::CulvertCurve* conduit_culvert_curve = nullptr;
    const std::uint8_t* conduit_culvert_mitered = nullptr;

    const int* node_face_ptr = nullptr;
    const int* node_face_idx = nullptr;
    const double* node_face_sign = nullptr;
    const int* node_face_sorted = nullptr; ///< each row's faces, ascending
    const int* node_struct_ptr = nullptr;
    const int* node_struct_idx = nullptr; ///< structures touching the node, ascending
    const double* node_invert = nullptr;
    const double* node_full_depth = nullptr;
    const double* node_ponded_area = nullptr;
    const double* node_sur_depth = nullptr;
    const double* node_surf_area = nullptr;
    const double* node_vfull = nullptr;
    const double* node_vol_dmax = nullptr;
    const double* node_vol_atop = nullptr;
    const double* node_vol_tbl = nullptr;
    const int* node_vol_off = nullptr;
    const int* node_vj_face = nullptr;
    const std::uint8_t* node_can_pond = nullptr;
    const std::uint8_t* node_kind = nullptr;
    const std::uint8_t* node_alg = nullptr;
    const std::uint8_t* node_pass_static = nullptr;
    const std::uint8_t* node_dummy_drain = nullptr;

    const int* struct_n1 = nullptr;
    const int* struct_n2 = nullptr;
    const std::uint8_t* struct_is_dummy = nullptr;

    const FvGeometry* geom = nullptr;

    // ---- options (static) -----------------------------------------------
    double cfl = 0.5;
    bool mixed_wave = true;
    bool mw_rh = true;
    bool deg1_guard_slot = true;
    bool census_pass_far = true;
    bool have_dummy = false; ///< mesh.struct_is_dummy is not empty

    // ---- forcing (one routing step) -------------------------------------
    bool has_lateral = false;
    bool has_source_max = false;
    bool has_structure_flow = false;
    bool has_q_cap = false;
    bool has_conduit_loss = false;
    bool has_withdrawal = false;
    const double* node_lateral = nullptr;    ///< zeros when absent
    const double* node_fixed_head = nullptr; ///< NaN when not prescribed
    const double* node_source_max = nullptr;
    const double* node_withdrawal = nullptr;
    const double* struct_q = nullptr;     ///< forcing.structure_flow, per structure
    const double* struct_q_cap = nullptr; ///< forcing.link_q_cap, per structure
    const double* conduit_loss = nullptr;
    const double* cell_qlat = nullptr;
    const std::uint8_t* node_pass = nullptr;
    const std::uint8_t* node_lat_div = nullptr;

    // ---- state ------------------------------------------------------------
    double* cell_a = nullptr;
    double* cell_q = nullptr;
    double* cell_h = nullptr;
    double* cell_eta = nullptr;
    double* cell_u = nullptr;
    double* cell_ah = nullptr;
    double* cell_t = nullptr;
    double* cell_i1 = nullptr;
    double* cell_q_int = nullptr;

    double* node_volume = nullptr;
    double* node_head = nullptr;
    double* node_carry = nullptr;
    double* node_exch = nullptr;
    double* node_in = nullptr;
    double* node_out = nullptr;
    double* flood_vol = nullptr;
    double* unmet = nullptr;
    double* node_qstruct = nullptr;
    double* node_qdummy = nullptr;

    double* f_mass = nullptr;
    double* f_mom = nullptr;
    double* f_corr_l = nullptr;
    double* f_corr_r = nullptr;
    double* relax_beta = nullptr;
    double* face_lat_spill = nullptr;
    kernels::FaceState* f_state_l = nullptr;
    kernels::FaceState* f_state_r = nullptr;

    double* struct_applied_vol = nullptr;
    double* dummy_vol = nullptr;
    std::uint8_t* inlet_control = nullptr;

    // ---- scratch ----------------------------------------------------------
    double* out_cell = nullptr;
    double* out_node = nullptr;
    double* node_struct_scale = nullptr;
    double* dummy_q = nullptr;
    double* settle_carry = nullptr;
    double* absorb = nullptr; ///< per face: volume a cell took from its node
    std::uint8_t* dep = nullptr;
};

} // namespace openswmm::fv::substep

#endif // OPENSWMM_ENGINE_FV_SUBSTEP_ARRAYS_HPP
