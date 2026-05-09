/**
 * @file Api2D.cpp
 * @brief C API implementation for the 2D surface routing module.
 *
 * @details Implements all functions declared in openswmm_2d.h. Each function
 *          extracts the SWMMEngine from the opaque handle, accesses the
 *          SurfaceRouter2D, and delegates the operation.
 *
 * @ingroup engine_2d
 */

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>
#include "../../core/SWMMEngine.hpp"

#include <cstring>
#include <algorithm>

// Helper macros for common validation patterns
#define GET_ENGINE(engine) \
    auto* eng = reinterpret_cast<openswmm::SWMMEngine*>(engine); \
    if (!eng) return SWMM_ERR_BADHANDLE

#define CHECK_2D_ACTIVE(eng) \
    auto& router2d = eng->surfaceRouter2D(); \
    if (!router2d.isActive()) return SWMM_ERR_BADPARAM

#define CHECK_TRI_IDX(idx, router2d) \
    if ((idx) < 0 || (idx) >= router2d.mesh().n_triangles()) \
        return SWMM_ERR_BADINDEX

#define CHECK_VERT_IDX(idx, router2d) \
    if ((idx) < 0 || (idx) >= router2d.mesh().n_vertices()) \
        return SWMM_ERR_BADINDEX

extern "C" {

// ============================================================================
// 2D Module Status
// ============================================================================

int swmm_2d_is_active(SWMM_Engine engine, int* active) {
    GET_ENGINE(engine);
    if (!active) return SWMM_ERR_BADPARAM;
    *active = eng->surfaceRouter2D().isActive() ? 1 : 0;
    return SWMM_OK;
}

// ============================================================================
// Mesh Geometry
// ============================================================================

int swmm_2d_vertex_count(SWMM_Engine engine, int* count) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    if (!count) return SWMM_ERR_BADPARAM;
    *count = router2d.mesh().n_vertices();
    return SWMM_OK;
}

int swmm_2d_triangle_count(SWMM_Engine engine, int* count) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    if (!count) return SWMM_ERR_BADPARAM;
    *count = router2d.mesh().n_triangles();
    return SWMM_OK;
}

int swmm_2d_vertex_get_xyz(SWMM_Engine engine, int idx,
                             double* x, double* y, double* z) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    CHECK_VERT_IDX(idx, router2d);
    if (!x || !y || !z) return SWMM_ERR_BADPARAM;

    auto& m = router2d.mesh();
    *x = m.vx[idx]; *y = m.vy[idx]; *z = m.vz[idx];
    return SWMM_OK;
}

int swmm_2d_vertex_get_xyz_bulk(SWMM_Engine engine,
                                  double* x, double* y, double* z) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    if (!x || !y || !z) return SWMM_ERR_BADPARAM;

    auto& m = router2d.mesh();
    int nv = m.n_vertices();
    std::memcpy(x, m.vx.data(), nv * sizeof(double));
    std::memcpy(y, m.vy.data(), nv * sizeof(double));
    std::memcpy(z, m.vz.data(), nv * sizeof(double));
    return SWMM_OK;
}

int swmm_2d_triangle_get_vertices(SWMM_Engine engine, int idx,
                                    int* v0, int* v1, int* v2) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    CHECK_TRI_IDX(idx, router2d);
    if (!v0 || !v1 || !v2) return SWMM_ERR_BADPARAM;

    auto& m = router2d.mesh();
    *v0 = m.tri_v0[idx]; *v1 = m.tri_v1[idx]; *v2 = m.tri_v2[idx];
    return SWMM_OK;
}

int swmm_2d_triangle_get_area(SWMM_Engine engine, int idx, double* area) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    CHECK_TRI_IDX(idx, router2d);
    if (!area) return SWMM_ERR_BADPARAM;

    *area = router2d.mesh().tri_area[idx];
    return SWMM_OK;
}

int swmm_2d_triangle_get_centroid(SWMM_Engine engine, int idx,
                                    double* cx, double* cy, double* cz) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    CHECK_TRI_IDX(idx, router2d);
    if (!cx || !cy || !cz) return SWMM_ERR_BADPARAM;

    auto& m = router2d.mesh();
    *cx = m.tri_cx[idx]; *cy = m.tri_cy[idx]; *cz = m.tri_cz[idx];
    return SWMM_OK;
}

int swmm_2d_triangle_get_mannings(SWMM_Engine engine, int idx, double* n) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    CHECK_TRI_IDX(idx, router2d);
    if (!n) return SWMM_ERR_BADPARAM;

    *n = router2d.mesh().mannings_n[idx];
    return SWMM_OK;
}

int swmm_2d_triangle_get_neighbours(SWMM_Engine engine, int idx,
                                      int* n0, int* n1, int* n2) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    CHECK_TRI_IDX(idx, router2d);
    if (!n0 || !n1 || !n2) return SWMM_ERR_BADPARAM;

    auto& m = router2d.mesh();
    *n0 = m.tri_nbr0[idx]; *n1 = m.tri_nbr1[idx]; *n2 = m.tri_nbr2[idx];
    return SWMM_OK;
}

// ============================================================================
// Coupling Map
// ============================================================================

int swmm_2d_vertex_coupling_count(SWMM_Engine engine, int* count) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    if (!count) return SWMM_ERR_BADPARAM;

    int n = 0;
    auto& m = router2d.mesh();
    for (int i = 0; i < m.n_vertices(); ++i) {
        if (m.vert_coupled_node[i] >= 0) ++n;
    }
    *count = n;
    return SWMM_OK;
}

int swmm_2d_triangle_coupling_count(SWMM_Engine engine, int* count) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    if (!count) return SWMM_ERR_BADPARAM;

    int n = 0;
    auto& m = router2d.mesh();
    for (int i = 0; i < m.n_triangles(); ++i) {
        if (m.tri_coupled_node[i] >= 0) ++n;
    }
    *count = n;
    return SWMM_OK;
}

int swmm_2d_vertex_get_coupled_node(SWMM_Engine engine, int vertex_idx,
                                      int* node_idx) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    CHECK_VERT_IDX(vertex_idx, router2d);
    if (!node_idx) return SWMM_ERR_BADPARAM;

    *node_idx = router2d.mesh().vert_coupled_node[vertex_idx];
    return SWMM_OK;
}

int swmm_2d_triangle_get_coupled_node(SWMM_Engine engine, int tri_idx,
                                        int* node_idx) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    CHECK_TRI_IDX(tri_idx, router2d);
    if (!node_idx) return SWMM_ERR_BADPARAM;

    *node_idx = router2d.mesh().tri_coupled_node[tri_idx];
    return SWMM_OK;
}

// ============================================================================
// 2D State — Per-Triangle
// ============================================================================

int swmm_2d_get_depth(SWMM_Engine engine, int idx, double* depth) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    CHECK_TRI_IDX(idx, router2d);
    if (!depth) return SWMM_ERR_BADPARAM;

    *depth = router2d.state().depth[idx];
    return SWMM_OK;
}

int swmm_2d_get_head(SWMM_Engine engine, int idx, double* head) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    CHECK_TRI_IDX(idx, router2d);
    if (!head) return SWMM_ERR_BADPARAM;

    *head = router2d.state().head[idx];
    return SWMM_OK;
}

int swmm_2d_get_coupling_flux(SWMM_Engine engine, int idx, double* flux) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    CHECK_TRI_IDX(idx, router2d);
    if (!flux) return SWMM_ERR_BADPARAM;

    *flux = router2d.state().coupling_flux[idx];
    return SWMM_OK;
}

int swmm_2d_get_rainfall(SWMM_Engine engine, int idx, double* rainfall) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    CHECK_TRI_IDX(idx, router2d);
    if (!rainfall) return SWMM_ERR_BADPARAM;

    *rainfall = router2d.state().rainfall[idx];
    return SWMM_OK;
}

int swmm_2d_get_net_source(SWMM_Engine engine, int idx, double* net_source) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    CHECK_TRI_IDX(idx, router2d);
    if (!net_source) return SWMM_ERR_BADPARAM;

    *net_source = router2d.state().net_source[idx];
    return SWMM_OK;
}

int swmm_2d_get_depths_bulk(SWMM_Engine engine, double* depths) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    if (!depths) return SWMM_ERR_BADPARAM;

    auto& s = router2d.state();
    std::memcpy(depths, s.depth.data(), s.depth.size() * sizeof(double));
    return SWMM_OK;
}

int swmm_2d_get_heads_bulk(SWMM_Engine engine, double* heads) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    if (!heads) return SWMM_ERR_BADPARAM;

    auto& s = router2d.state();
    std::memcpy(heads, s.head.data(), s.head.size() * sizeof(double));
    return SWMM_OK;
}

int swmm_2d_get_coupling_fluxes_bulk(SWMM_Engine engine, double* fluxes) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    if (!fluxes) return SWMM_ERR_BADPARAM;

    auto& s = router2d.state();
    std::memcpy(fluxes, s.coupling_flux.data(),
                s.coupling_flux.size() * sizeof(double));
    return SWMM_OK;
}

// ============================================================================
// 2D State — Per-Vertex
// ============================================================================

int swmm_2d_vertex_get_head(SWMM_Engine engine, int idx, double* head) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    CHECK_VERT_IDX(idx, router2d);
    if (!head) return SWMM_ERR_BADPARAM;

    *head = router2d.state().vert_head[idx];
    return SWMM_OK;
}

int swmm_2d_vertex_get_heads_bulk(SWMM_Engine engine, double* heads) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    if (!heads) return SWMM_ERR_BADPARAM;

    auto& s = router2d.state();
    std::memcpy(heads, s.vert_head.data(), s.vert_head.size() * sizeof(double));
    return SWMM_OK;
}

// ============================================================================
// 2D Solver Statistics
// ============================================================================

int swmm_2d_get_max_depth(SWMM_Engine engine, double* max_depth) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    if (!max_depth) return SWMM_ERR_BADPARAM;

    auto& s = router2d.state();
    *max_depth = *std::max_element(s.depth.begin(), s.depth.end());
    return SWMM_OK;
}

int swmm_2d_get_total_volume(SWMM_Engine engine, double* volume) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    if (!volume) return SWMM_ERR_BADPARAM;

    *volume = router2d.totalVolume();
    return SWMM_OK;
}

int swmm_2d_get_total_exchange_flow(SWMM_Engine engine, double* flow) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    if (!flow) return SWMM_ERR_BADPARAM;

    *flow = router2d.totalExchangeFlow();
    return SWMM_OK;
}

int swmm_2d_get_cvode_steps(SWMM_Engine engine, long* steps) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    if (!steps) return SWMM_ERR_BADPARAM;

    *steps = router2d.lastCvodeSteps();
    return SWMM_OK;
}

int swmm_2d_get_cvode_last_step(SWMM_Engine engine, double* h_last) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    if (!h_last) return SWMM_ERR_BADPARAM;

    *h_last = router2d.lastCvodeStepSize();
    return SWMM_OK;
}

int swmm_2d_get_stat_max_depths(SWMM_Engine engine, double* max_depths) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    if (!max_depths) return SWMM_ERR_BADPARAM;

    auto& s = router2d.state();
    std::memcpy(max_depths, s.stat_max_depth.data(),
                s.stat_max_depth.size() * sizeof(double));
    return SWMM_OK;
}

// ============================================================================
// 2D Forcing
// ============================================================================

int swmm_2d_force_rainfall(SWMM_Engine engine, int idx,
                             double value, int mode, int persist) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    CHECK_TRI_IDX(idx, router2d);

    auto& s = const_cast<openswmm::twoD::SurfaceStateData&>(router2d.state());
    s.rainfall_forced[idx]    = static_cast<int8_t>(mode);
    s.rainfall_force_val[idx] = value;
    s.rainfall_persist[idx]   = static_cast<int8_t>(persist);
    return SWMM_OK;
}

int swmm_2d_force_rainfall_uniform(SWMM_Engine engine,
                                     double value, int mode, int persist) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);

    auto& s = const_cast<openswmm::twoD::SurfaceStateData&>(router2d.state());
    int nt = router2d.mesh().n_triangles();
    for (int i = 0; i < nt; ++i) {
        s.rainfall_forced[i]    = static_cast<int8_t>(mode);
        s.rainfall_force_val[i] = value;
        s.rainfall_persist[i]   = static_cast<int8_t>(persist);
    }
    return SWMM_OK;
}

int swmm_2d_force_coupling_flux(SWMM_Engine engine, int idx,
                                  double value, int mode, int persist) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    CHECK_TRI_IDX(idx, router2d);

    auto& s = const_cast<openswmm::twoD::SurfaceStateData&>(router2d.state());
    s.coupling_forced[idx]    = static_cast<int8_t>(mode);
    s.coupling_force_val[idx] = value;
    s.coupling_persist[idx]   = static_cast<int8_t>(persist);
    return SWMM_OK;
}

int swmm_2d_force_clear_all(SWMM_Engine engine) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);

    auto& s = const_cast<openswmm::twoD::SurfaceStateData&>(router2d.state());
    int nt = router2d.mesh().n_triangles();
    for (int i = 0; i < nt; ++i) {
        s.rainfall_forced[i] = 0;
        s.rainfall_force_val[i] = 0.0;
        s.rainfall_persist[i] = 0;
        s.coupling_forced[i] = 0;
        s.coupling_force_val[i] = 0.0;
        s.coupling_persist[i] = 0;
    }
    return SWMM_OK;
}

// ============================================================================
// 2D Solver Options
// ============================================================================

int swmm_2d_get_dry_depth(SWMM_Engine engine, double* dry_depth) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    if (!dry_depth) return SWMM_ERR_BADPARAM;

    *dry_depth = router2d.options().dry_depth;
    return SWMM_OK;
}

int swmm_2d_set_dry_depth(SWMM_Engine engine, double dry_depth) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);

    const_cast<openswmm::twoD::SolverOptions2D&>(router2d.options()).dry_depth
        = dry_depth;
    return SWMM_OK;
}

int swmm_2d_get_rel_tolerance(SWMM_Engine engine, double* rtol) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    if (!rtol) return SWMM_ERR_BADPARAM;

    *rtol = router2d.options().rel_tolerance;
    return SWMM_OK;
}

int swmm_2d_set_rel_tolerance(SWMM_Engine engine, double rtol) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);

    const_cast<openswmm::twoD::SolverOptions2D&>(router2d.options()).rel_tolerance
        = rtol;
    return SWMM_OK;
}

int swmm_2d_get_abs_tolerance(SWMM_Engine engine, double* atol) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);
    if (!atol) return SWMM_ERR_BADPARAM;

    *atol = router2d.options().abs_tolerance;
    return SWMM_OK;
}

int swmm_2d_set_abs_tolerance(SWMM_Engine engine, double atol) {
    GET_ENGINE(engine);
    CHECK_2D_ACTIVE(eng);

    const_cast<openswmm::twoD::SolverOptions2D&>(router2d.options()).abs_tolerance
        = atol;
    return SWMM_OK;
}

} // extern "C"
