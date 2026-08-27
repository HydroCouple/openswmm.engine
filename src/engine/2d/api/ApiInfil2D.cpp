/**
 * @file ApiInfil2D.cpp
 * @brief C API implementation for 2D per-cell infiltration (plan §5.5.6, I6).
 *
 * @details Implements all functions declared in openswmm_infil2d.h. Follows
 *          Api2D.cpp exactly: the SWMMEngine is extracted from the opaque
 *          handle, the SurfaceRouter2D is reached through it, and the call is
 *          delegated — here to the `twoD::Infil2D` the router owns.
 *
 *          Two conventions are worth stating once, because both are load-
 *          bearing and neither is obvious from the signatures:
 *
 *          1. **Staleness.** `Infil2D::resolve()` is the only re-entry point
 *             that rebuilds kernel state, and it rebuilds it for the WHOLE
 *             mesh while zeroing `cumulative()`. Calling it mid-run would
 *             discard the Horton/Green-Ampt integration history of every cell
 *             the caller never touched and desynchronize `cum_depth_` from
 *             `MassBalance2D::infil_out`. There is no per-cell re-init entry
 *             point and Infil2D.hpp is a frozen contract, so every parameter
 *             setter here is *rejected* once the engine has left OPENED —
 *             `SWMM_ERR_LIFECYCLE`, mirroring the guard the LID/inflow C API
 *             uses (openswmm_infrastructure_impl.cpp:581).
 *
 *          2. **Empty-when-inactive.** `resolvedRows()`, `provenance()` and
 *             `cumulative()` are EMPTY — not sized `n_triangles` — both before
 *             resolve() runs and after a resolve() that found no model
 *             (Infil2D.cpp:312-316, the I7 fast path). Every reader here
 *             therefore checks the container's own size before indexing and
 *             degrades to "no infiltration" / zero-fill rather than reading
 *             out of bounds.
 *
 * @ingroup engine_infil2d
 */

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_infil2d.h>
#include "../../core/SWMMEngine.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

// Helper macros — same shape as Api2D.cpp.
#define GET_ENGINE(engine) \
    auto* eng = reinterpret_cast<openswmm::SWMMEngine*>(engine); \
    if (!eng) return SWMM_ERR_BADHANDLE

// Configuration lives on the parsed mesh, not on an initialized solver, so the
// infiltration API uses the mesh guard (the state the GUI edits in), not
// CHECK_2D_ACTIVE.
#define CHECK_2D_MESH(eng) \
    auto& router2d = eng->surfaceRouter2D(); \
    if (router2d.mesh().n_vertices() <= 0) return SWMM_ERR_BADPARAM

#define CHECK_TRI_IDX(idx, router2d) \
    if ((idx) < 0 || (idx) >= router2d.mesh().n_triangles()) \
        return SWMM_ERR_BADINDEX

// See note 1 in the file header.
#define CHECK_EDITABLE(eng) \
    if (eng->context().state != openswmm::EngineState::OPENED && \
        eng->context().state != openswmm::EngineState::BUILDING) \
        return SWMM_ERR_LIFECYCLE

namespace {

using openswmm::InfilModel;
using openswmm::twoD::Infil2D;
using openswmm::twoD::Infil2DDest;
using openswmm::twoD::Infil2DProvenance;
using openswmm::twoD::Infil2DRow;
using openswmm::twoD::kInfil2DMaxParams;

/// Map a `SWMM_INFIL2D_*` method code onto `InfilModel`.
/// @returns False when the code names no method.
bool toMethod(int code, InfilModel& out) {
    switch (code) {
        case SWMM_INFIL2D_HORTON:         out = InfilModel::HORTON;         return true;
        case SWMM_INFIL2D_MOD_HORTON:     out = InfilModel::MOD_HORTON;     return true;
        case SWMM_INFIL2D_GREEN_AMPT:     out = InfilModel::GREEN_AMPT;     return true;
        case SWMM_INFIL2D_MOD_GREEN_AMPT: out = InfilModel::MOD_GREEN_AMPT; return true;
        case SWMM_INFIL2D_CURVE_NUMBER:   out = InfilModel::CURVE_NUM;      return true;
        case SWMM_INFIL2D_CONSTANT:       out = InfilModel::CONSTANT;       return true;
        default:                                                            return false;
    }
}

/// Inverse of toMethod.
int fromMethod(InfilModel m) {
    switch (m) {
        case InfilModel::HORTON:         return SWMM_INFIL2D_HORTON;
        case InfilModel::MOD_HORTON:     return SWMM_INFIL2D_MOD_HORTON;
        case InfilModel::GREEN_AMPT:     return SWMM_INFIL2D_GREEN_AMPT;
        case InfilModel::MOD_GREEN_AMPT: return SWMM_INFIL2D_MOD_GREEN_AMPT;
        case InfilModel::CURVE_NUM:      return SWMM_INFIL2D_CURVE_NUMBER;
        case InfilModel::CONSTANT:       return SWMM_INFIL2D_CONSTANT;
    }
    return SWMM_INFIL2D_HORTON;
}

/// Convert a caller row into the engine type, validating as we go.
///
/// The parameter bounds mirror `validateRow` in Infil2D.cpp (which is file-
/// local, so it cannot be shared) — rejecting here means the caller learns
/// immediately instead of at initialize() with a parse-style message. D-I4:
/// LOST is the only destination this release routes.
///
/// @returns SWMM_OK, or SWMM_ERR_BADPARAM naming nothing (the C API has no
///          message channel; the same row is re-validated by resolve()).
int toRow(const SWMM_Infil2DRow& in, Infil2DRow& out) {
    out = Infil2DRow{};
    if (in.has_method == 0) return SWMM_OK;   // NONE: every other field is moot

    if (!toMethod(in.method, out.method)) return SWMM_ERR_BADPARAM;
    out.has_method = true;

    for (int k = 0; k < kInfil2DMaxParams; ++k) {
        if (!std::isfinite(in.p[k])) return SWMM_ERR_BADPARAM;
        out.p[k] = in.p[k];
    }

    // D-I4: parse the other destinations, accept only LOST.
    switch (in.dest) {
        case SWMM_INFIL2D_DEST_LOST: out.dest = Infil2DDest::LOST; break;
        case SWMM_INFIL2D_DEST_SUBCATCH_AQUIFER:
        case SWMM_INFIL2D_DEST_AQUIFER_2D:
        default:
            return SWMM_ERR_BADPARAM;
    }

    switch (out.method) {
        case InfilModel::HORTON:
        case InfilModel::MOD_HORTON:
            if (out.p[0] < 0.0 || out.p[1] < 0.0) return SWMM_ERR_BADPARAM;
            break;
        case InfilModel::GREEN_AMPT:
        case InfilModel::MOD_GREEN_AMPT:
            if (out.p[1] < 0.0) return SWMM_ERR_BADPARAM;
            if (out.p[2] < 0.0 || out.p[2] > 1.0) return SWMM_ERR_BADPARAM;
            break;
        case InfilModel::CURVE_NUM:
            if (out.p[0] < 1.0 || out.p[0] > 100.0) return SWMM_ERR_BADPARAM;
            break;
        case InfilModel::CONSTANT:
            if (out.p[0] < 0.0) return SWMM_ERR_BADPARAM;
            break;
    }
    return SWMM_OK;
}

/// Convert an engine row into the caller's POD.
void fromRow(const Infil2DRow& in, SWMM_Infil2DRow& out) {
    out.has_method = in.has_method ? 1 : 0;
    out.method     = fromMethod(in.method);
    for (int k = 0; k < kInfil2DMaxParams; ++k) out.p[k] = in.p[k];
    switch (in.dest) {
        case Infil2DDest::SUBCATCH_AQUIFER: out.dest = SWMM_INFIL2D_DEST_SUBCATCH_AQUIFER; break;
        case Infil2DDest::AQUIFER_2D:       out.dest = SWMM_INFIL2D_DEST_AQUIFER_2D;       break;
        case Infil2DDest::LOST:
        default:                            out.dest = SWMM_INFIL2D_DEST_LOST;             break;
    }
}

/// Drop every per-cell override row for @p tri. Used by both the clear path
/// and the upsert path so a cell never accumulates two override rows.
void eraseOverrides(Infil2D& infil, int tri) {
    auto& v = infil.overrides();
    std::size_t w = 0;
    for (std::size_t r = 0; r < v.size(); ++r) {
        if (v[r].tri == tri) continue;
        if (w != r) v[w] = v[r];
        ++w;
    }
    v.resize(w);
}

/// Copy `min(n, count)` doubles out of @p src, zero-filling when @p src is the
/// empty "no model resolved" container (see note 2 in the file header).
void fillBulk(double* dst, int n, int count, const std::vector<double>& src) {
    const int m = (n < count) ? n : count;
    for (int i = 0; i < m; ++i)
        dst[i] = (static_cast<std::size_t>(i) < src.size())
               ? src[static_cast<std::size_t>(i)]
               : 0.0;
}

}  // namespace

extern "C" {

// ============================================================================
// Options — [2D_INFILTRATION_OPTIONS]
// ============================================================================

int swmm_infil2d_get_options(SWMM_Engine engine, SWMM_Infil2DOptions* options) {
    GET_ENGINE(engine);
    CHECK_2D_MESH(eng);
    if (!options) return SWMM_ERR_BADPARAM;

    options->infil_step = router2d.infil().options().infil_step;
    return SWMM_OK;
}

int swmm_infil2d_set_options(SWMM_Engine engine,
                             const SWMM_Infil2DOptions* options) {
    GET_ENGINE(engine);
    CHECK_2D_MESH(eng);
    if (!options) return SWMM_ERR_BADPARAM;
    if (!std::isfinite(options->infil_step)) return SWMM_ERR_BADPARAM;
    CHECK_EDITABLE(eng);

    // <= 0 is the documented "use the project WET_STEP" sentinel, so it is
    // stored verbatim rather than clamped.
    router2d.infil().options().infil_step = options->infil_step;
    return SWMM_OK;
}

// ============================================================================
// Tag defaults — [2D_INFILTRATION_DEFAULTS]
// ============================================================================

int swmm_infil2d_defaults_count(SWMM_Engine engine, int* count) {
    GET_ENGINE(engine);
    CHECK_2D_MESH(eng);
    if (!count) return SWMM_ERR_BADPARAM;

    *count = static_cast<int>(router2d.infil().defaults().size());
    return SWMM_OK;
}

int swmm_infil2d_get_default(SWMM_Engine engine, int idx,
                             SWMM_Infil2DRow* row) {
    GET_ENGINE(engine);
    CHECK_2D_MESH(eng);
    if (!row) return SWMM_ERR_BADPARAM;

    const auto& defs = router2d.infil().defaults();
    if (idx < 0 || idx >= static_cast<int>(defs.size())) return SWMM_ERR_BADINDEX;

    fromRow(defs[static_cast<std::size_t>(idx)].row, *row);
    return SWMM_OK;
}

int swmm_infil2d_get_default_tag(SWMM_Engine engine, int idx,
                                 char* buf, int buflen) {
    GET_ENGINE(engine);
    CHECK_2D_MESH(eng);
    if (!buf || buflen <= 0) return SWMM_ERR_BADPARAM;

    const auto& defs = router2d.infil().defaults();
    if (idx < 0 || idx >= static_cast<int>(defs.size())) return SWMM_ERR_BADINDEX;

    const std::string& s = defs[static_cast<std::size_t>(idx)].tag;
    const std::size_t n  = (s.size() < static_cast<std::size_t>(buflen) - 1)
                         ? s.size()
                         : static_cast<std::size_t>(buflen) - 1;
    std::memcpy(buf, s.data(), n);
    buf[n] = '\0';
    return SWMM_OK;
}

int swmm_infil2d_set_default(SWMM_Engine engine, const char* tag,
                             const SWMM_Infil2DRow* row) {
    GET_ENGINE(engine);
    CHECK_2D_MESH(eng);
    if (!tag || tag[0] == '\0' || !row) return SWMM_ERR_BADPARAM;
    CHECK_EDITABLE(eng);

    openswmm::twoD::Infil2DDefault entry;
    entry.tag = tag;
    const int rc = toRow(*row, entry.row);
    if (rc != SWMM_OK) return rc;

    // Upsert: a tag never carries two definitions.
    auto& defs = router2d.infil().defaults();
    std::size_t w = 0;
    for (std::size_t r = 0; r < defs.size(); ++r) {
        if (defs[r].tag == entry.tag) continue;
        if (w != r) defs[w] = std::move(defs[r]);
        ++w;
    }
    defs.resize(w);
    defs.push_back(std::move(entry));
    return SWMM_OK;
}

int swmm_infil2d_remove_default(SWMM_Engine engine, const char* tag) {
    GET_ENGINE(engine);
    CHECK_2D_MESH(eng);
    if (!tag || tag[0] == '\0') return SWMM_ERR_BADPARAM;
    CHECK_EDITABLE(eng);

    const std::string want(tag);
    auto& defs = router2d.infil().defaults();
    std::size_t w = 0;
    for (std::size_t r = 0; r < defs.size(); ++r) {
        if (defs[r].tag == want) continue;
        if (w != r) defs[w] = std::move(defs[r]);
        ++w;
    }
    defs.resize(w);
    return SWMM_OK;
}

// ============================================================================
// Per-cell overrides — [2D_INFILTRATION]
// ============================================================================

int swmm_infil2d_get_cell(SWMM_Engine engine, int tri,
                          SWMM_Infil2DRow* row, int* is_override) {
    GET_ENGINE(engine);
    CHECK_2D_MESH(eng);
    CHECK_TRI_IDX(tri, router2d);
    if (!row) return SWMM_ERR_BADPARAM;

    const Infil2D& infil = router2d.infil();
    const auto  ui       = static_cast<std::size_t>(tri);
    const auto& resolved = infil.resolvedRows();
    const auto& prov     = infil.provenance();

    // Resolved path — resolvedRows()/provenance() are sized n_triangles only
    // once resolve() found at least one model.
    if (ui < resolved.size() && ui < prov.size()) {
        fromRow(resolved[ui], *row);
        if (is_override)
            *is_override = (prov[ui] == Infil2DProvenance::OVERRIDE) ? 1 : 0;
        return SWMM_OK;
    }

    // Unresolved path — only the authored per-cell layer is visible. Last row
    // wins, matching the order resolve() applies overrides in.
    fromRow(Infil2DRow{}, *row);
    if (is_override) *is_override = 0;
    for (const auto& o : infil.overrides()) {
        if (o.tri != tri) continue;
        fromRow(o.row, *row);
        if (is_override) *is_override = 1;
    }
    return SWMM_OK;
}

int swmm_infil2d_set_cell(SWMM_Engine engine, int tri,
                          const SWMM_Infil2DRow* row) {
    GET_ENGINE(engine);
    CHECK_2D_MESH(eng);
    CHECK_TRI_IDX(tri, router2d);
    CHECK_EDITABLE(eng);

    Infil2D& infil = router2d.infil();

    if (!row) {                       // NULL = clear the override entirely
        eraseOverrides(infil, tri);
        return SWMM_OK;
    }

    openswmm::twoD::Infil2DOverride entry;
    entry.tri    = tri;
    const int rc = toRow(*row, entry.row);
    if (rc != SWMM_OK) return rc;     // validated BEFORE anything is erased

    eraseOverrides(infil, tri);
    infil.overrides().push_back(std::move(entry));
    return SWMM_OK;
}

int swmm_infil2d_set_cells(SWMM_Engine engine, const int* tris, int n,
                           const SWMM_Infil2DRow* row) {
    GET_ENGINE(engine);
    CHECK_2D_MESH(eng);
    if (!tris || n <= 0) return SWMM_ERR_BADPARAM;
    CHECK_EDITABLE(eng);

    // --- one validation pass; nothing is written until it passes ------------
    Infil2DRow parsed;
    if (row) {
        const int rc = toRow(*row, parsed);
        if (rc != SWMM_OK) return rc;
    }

    const int nt = router2d.mesh().n_triangles();
    for (int i = 0; i < n; ++i)
        if (tris[i] < 0 || tris[i] >= nt) return SWMM_ERR_BADINDEX;

    // --- apply --------------------------------------------------------------
    Infil2D& infil = router2d.infil();
    for (int i = 0; i < n; ++i) {
        eraseOverrides(infil, tris[i]);
        if (!row) continue;           // NULL row = clear on every listed cell
        openswmm::twoD::Infil2DOverride entry;
        entry.tri = tris[i];
        entry.row = parsed;
        infil.overrides().push_back(entry);
    }
    return SWMM_OK;
}

// ============================================================================
// State readback (SI)
// ============================================================================

int swmm_infil2d_get_rate_bulk(SWMM_Engine engine, double* f, int n) {
    GET_ENGINE(engine);
    CHECK_2D_MESH(eng);
    if (!f || n <= 0) return SWMM_ERR_BADPARAM;

    // state().infil_rate is allocated with the rest of the surface state, but
    // is all-zero (and may be empty before initialize()) when no model
    // resolved — fillBulk covers both.
    fillBulk(f, n, router2d.mesh().n_triangles(), router2d.state().infil_rate);
    return SWMM_OK;
}

int swmm_infil2d_get_cum_bulk(SWMM_Engine engine, double* F, int n) {
    GET_ENGINE(engine);
    CHECK_2D_MESH(eng);
    if (!F || n <= 0) return SWMM_ERR_BADPARAM;

    // The ROUTER's cumulative, not Infil2D::cumulative(): it accumulates the
    // depth-ramped infilSink() value the solver actually removed, so this
    // reader agrees with swmm_infil2d_get_total_volume / the ledger. It is
    // EMPTY, not zero-sized-per-triangle, when no model resolved — fillBulk
    // zero-fills rather than index it.
    fillBulk(F, n, router2d.mesh().n_triangles(), router2d.infilCumulative());
    return SWMM_OK;
}

int swmm_infil2d_get_total_volume(SWMM_Engine engine, double* volume) {
    GET_ENGINE(engine);
    if (!volume) return SWMM_ERR_BADPARAM;

    // Same guard as swmm_2d_get_mass_balance — this reads the same ledger.
    const auto& mb = eng->context().mass_balance_2d;
    if (!mb.active) return SWMM_ERR_BADPARAM;

    *volume = mb.infil_out;
    return SWMM_OK;
}

}  // extern "C"
