/**
 * @file StorageGeometry.hpp
 * @brief Storage-shape raw parameters ⇄ area-relation coefficients, and the
 *        INP/GeoPackage keywords for each shape.
 *
 * @ingroup new_engine
 *
 * @details Single source of truth for the shape math. Three call sites must never
 *          disagree about it — the INP parser (NodesHandler), the C API
 *          (openswmm_nodes_impl) and the GeoPackage reader — so the conversion lives
 *          here rather than being open-coded at each.
 *
 *          Users supply three raw parameters (p1, p2, p3 — lengths/slope); the solver
 *          evaluates the quadratic area relation `A(d) = c + a*d + b*d²`. Legacy
 *          `storage_readParams()` (src/legacy/engine/node.c:713-752) derives a/b/c at
 *          parse time and then **discards** p1/p2/p3, which is why a legacy model
 *          cannot round-trip its own shape parameters. We keep the raw values in
 *          StorageData so `.inp`/`.gpkg` writes are lossless, and derive a/b/c here
 *          with the identical formulas.
 *
 *          Coefficient slots map to legacy names as: a ≡ a1, b ≡ a2, c ≡ a0.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_DATA_STORAGEGEOMETRY_HPP
#define OPENSWMM_ENGINE_DATA_STORAGEGEOMETRY_HPP

#include "NodeData.hpp"

#include <cmath>
#include <string>
#include <string_view>

namespace openswmm {

/** @brief π to the precision legacy uses (src/legacy/engine/consts.h). */
inline constexpr double kStoragePi = 3.141592653589793;

/**
 * @brief True when @p s is one of the four geometric shapes (i.e. its a/b/c are
 *        the quadratic coefficients, not TABULAR/FUNCTIONAL).
 */
inline constexpr bool storage_shape_is_geometric(StorageShape s) noexcept {
    return s == StorageShape::CYLINDRICAL || s == StorageShape::CONICAL ||
           s == StorageShape::PARABOLOID  || s == StorageShape::PYRAMIDAL;
}

/**
 * @brief Validate raw shape parameters.
 * @details Mirrors legacy node.c:700-708 — length and width must be > 0, slope/height
 *          must be >= 0, and a PARABOLOID's height may not be 0 (it divides).
 *          TABULAR/FUNCTIONAL always validate (their params are unused here).
 */
inline bool storage_shape_params_valid(StorageShape s,
                                       double p1, double p2, double p3) noexcept {
    if (!storage_shape_is_geometric(s)) return true;
    if (p1 <= 0.0 || p2 <= 0.0 || p3 < 0.0) return false;
    if (s == StorageShape::PARABOLOID && p3 == 0.0) return false;
    return true;
}

/**
 * @brief Derive the area-relation coefficients for a geometric shape.
 *
 * @details Formulas are legacy node.c:713-752, verbatim:
 *
 *          | shape       | a (=a1)          | b (=a2)            | c (=a0)   |
 *          |-------------|------------------|--------------------|-----------|
 *          | CYLINDRICAL | 0                | 0                  | π·A·B     |
 *          | CONICAL     | 2π·B·Z           | π·(B/A)·Z²         | π·A·B     |
 *          | PARABOLOID  | π·A·B / Z        | 0                  | 0         |
 *          | PYRAMIDAL   | 2·(L + W)·Z      | 4·Z²               | L·W       |
 *
 *          where A = p1/2, B = p2/2 (semi-axes), Z = p3, L = p1, W = p2.
 *
 * @param[out] a,b,c  Untouched when the shape is not geometric or params are invalid.
 * @return false when @p s is TABULAR/FUNCTIONAL (nothing to derive) or the params are
 *         invalid — callers treat the latter as an input error.
 */
inline bool storage_shape_coeffs(StorageShape s, double p1, double p2, double p3,
                                 double& a, double& b, double& c) noexcept {
    if (!storage_shape_is_geometric(s)) return false;
    if (!storage_shape_params_valid(s, p1, p2, p3)) return false;

    const double A = p1 / 2.0;   // base/top semi-axis length
    const double B = p2 / 2.0;   // base/top semi-axis width
    const double Z = p3;         // side slope (run/rise), or height for PARABOLOID

    switch (s) {
        case StorageShape::CYLINDRICAL:
            a = 0.0;
            b = 0.0;
            c = kStoragePi * A * B;
            return true;
        case StorageShape::CONICAL:
            a = 2.0 * kStoragePi * B * Z;
            b = kStoragePi * B / A * Z * Z;
            c = kStoragePi * A * B;
            return true;
        case StorageShape::PARABOLOID:
            a = kStoragePi * A * B / Z;   // Z != 0 guaranteed by params_valid
            b = 0.0;
            c = 0.0;
            return true;
        case StorageShape::PYRAMIDAL:
            a = 2.0 * (p1 + p2) * Z;      // 2(L + W)Z — full base dims, not semi-axes
            b = 4.0 * Z * Z;
            c = p1 * p2;                  // L·W
            return true;
        default:
            return false;
    }
}

/**
 * @brief Canonical INP/GeoPackage keyword for a shape.
 * @note PARABOLOID's keyword is **PARABOLIC** — that is what legacy writes and reads
 *       (src/legacy/engine/text.h:195). The enumerator keeps the geometric name.
 */
inline const char* storage_shape_keyword(StorageShape s) noexcept {
    switch (s) {
        case StorageShape::TABULAR:     return "TABULAR";
        case StorageShape::FUNCTIONAL:  return "FUNCTIONAL";
        case StorageShape::CYLINDRICAL: return "CYLINDRICAL";
        case StorageShape::CONICAL:     return "CONICAL";
        case StorageShape::PARABOLOID:  return "PARABOLIC";
        case StorageShape::PYRAMIDAL:   return "PYRAMIDAL";
    }
    return "FUNCTIONAL";
}

/**
 * @brief Parse a shape keyword (caller upper-cases first).
 * @details Accepts `PARABOLOID` as an alias for the canonical `PARABOLIC` so a file
 *          written against the enumerator name still reads back.
 * @return false when the keyword is not a storage shape.
 */
inline bool storage_shape_from_keyword(std::string_view kw, StorageShape& out) noexcept {
    if (kw == "TABULAR")     { out = StorageShape::TABULAR;     return true; }
    if (kw == "FUNCTIONAL")  { out = StorageShape::FUNCTIONAL;  return true; }
    if (kw == "CYLINDRICAL") { out = StorageShape::CYLINDRICAL; return true; }
    if (kw == "CONICAL")     { out = StorageShape::CONICAL;     return true; }
    if (kw == "PARABOLIC" || kw == "PARABOLOID")
                             { out = StorageShape::PARABOLOID;  return true; }
    if (kw == "PYRAMIDAL")   { out = StorageShape::PYRAMIDAL;   return true; }
    return false;
}

/** @brief True when @p v is a valid StorageShape ordinal (C API boundary check). */
inline constexpr bool storage_shape_is_valid_code(int v) noexcept {
    return v >= static_cast<int>(StorageShape::TABULAR) &&
           v <= static_cast<int>(StorageShape::PYRAMIDAL);
}

}  // namespace openswmm

#endif  // OPENSWMM_ENGINE_DATA_STORAGEGEOMETRY_HPP
