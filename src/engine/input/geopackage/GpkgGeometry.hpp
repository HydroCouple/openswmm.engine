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
 * @file      GpkgGeometry.hpp
 * @brief     GeoPackage Binary geometry encoding and decoding (ISO WKB with GP header).
 *
 * @details Encodes/decodes POINT, LINESTRING, and MULTIPOLYGON geometries
 *          in the GeoPackage Standard Binary format (Annex F of the spec).
 *
 *          GP Binary layout:
 *            [Magic: 2B "GP"] [Version: 1B] [Flags: 1B] [SRS ID: 4B]
 *            [Envelope: 0-64B depending on flags] [ISO WKB payload]
 *
 * @ingroup engine_geopackage
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_GPKG_GEOMETRY_HPP
#define OPENSWMM_GPKG_GEOMETRY_HPP

#include "core/PolygonRings.hpp"

#include <algorithm>
#include <vector>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace openswmm::gpkg {

// WKB geometry type codes (ISO 13249)
enum WkbType : uint32_t {
    WKB_POINT = 1,
    WKB_LINESTRING = 2,
    WKB_POLYGON = 3,
    WKB_MULTIPOLYGON = 6,
};

// Byte order
constexpr uint8_t WKB_LITTLE_ENDIAN = 1;

// GeoPackage header magic
constexpr uint8_t GP_MAGIC_1 = 0x47; // 'G'
constexpr uint8_t GP_MAGIC_2 = 0x50; // 'P'

// ============================================================================
// Encoding helpers
// ============================================================================

namespace detail {

/**
 * @brief Append 1 native representation byte.
 *
 * No endian conversion or size/range validation; vector allocation failures propagate.
 *
 * @param[in,out] buf Owned byte-vector sink; appends without clearing and can reallocate,
 *     invalidating old byte views.
 * @param[in] v Value to encode.
 */
inline void append_u8(std::vector<uint8_t>& buf, uint8_t v) {
    buf.push_back(v);
}

/**
 * @brief Append 4 native representation bytes.
 *
 * No endian conversion or size/range validation; vector allocation failures propagate.
 *
 * @param[in,out] buf Owned byte-vector sink; appends without clearing and can reallocate,
 *     invalidating old byte views.
 * @param[in] v Value to encode; requires little-endian host representation for the advertised WKB
 *     byte order.
 */
inline void append_u32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(&v),
               reinterpret_cast<const uint8_t*>(&v) + 4);
}

/**
 * @brief Append 8 native representation bytes.
 *
 * No endian conversion or size/range validation; vector allocation failures propagate.
 *
 * @param[in,out] buf Owned byte-vector sink; appends without clearing and can reallocate,
 *     invalidating old byte views.
 * @param[in] v Value to encode; requires little-endian host representation for the advertised WKB
 *     byte order.
 */
inline void append_f64(std::vector<uint8_t>& buf, double v) {
    buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(&v),
               reinterpret_cast<const uint8_t*>(&v) + 8);
}

// GP header with envelope type 1 (min/max XY = 32 bytes envelope)
/**
 * @brief Append a version-zero nonempty GP header with XY envelope.
 *
 * Advertises little endian and envelope kind XY; caller uses a compatible host and supplies ordered
 * finite envelope values. Does not append a WKB payload.
 *
 * @param[in,out] buf Owned byte-vector sink; appends without clearing and can reallocate,
 *     invalidating old byte views.
 * @param[in] srs_id Signed spatial-reference identifier written in native 32-bit representation.
 * @param[in] min_x Envelope minimum X in the selected CRS units.
 * @param[in] min_y Envelope minimum Y in CRS units.
 * @param[in] max_x Envelope maximum X in CRS units.
 * @param[in] max_y Envelope maximum Y in CRS units.
 */
inline void write_gp_header(std::vector<uint8_t>& buf, int32_t srs_id, double min_x, double min_y,
                            double max_x, double max_y) {
    append_u8(buf, GP_MAGIC_1);
    append_u8(buf, GP_MAGIC_2);
    append_u8(buf, 0x00); // version 0
    // flags: byte_order=1 (LE), envelope_type=1 (XY), empty=0
    // bits: [empty:1][envelope:3][byte_order:1] -> 0b0_001_1 = 0x03 (LE) with envelope type 1
    uint8_t flags = 0x00;
    flags |= WKB_LITTLE_ENDIAN; // bit 0: byte order (1=LE)
    flags |= (1 << 1);          // bits 1-3: envelope type 1 (XY only)
    append_u8(buf, flags);
    // SRS ID
    buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(&srs_id),
               reinterpret_cast<const uint8_t*>(&srs_id) + 4);
    // Envelope (32 bytes for type 1: min_x, max_x, min_y, max_y)
    append_f64(buf, min_x);
    append_f64(buf, max_x);
    append_f64(buf, min_y);
    append_f64(buf, max_y);
}

// GP header with no envelope (empty geometry)
/**
 * @brief Append an empty GP header without an envelope.
 *
 * Sets empty bit and little-endian flag; WKB geometry bytes are appended separately by the caller.
 *
 * @param[in,out] buf Owned byte-vector sink; appends without clearing and can reallocate,
 *     invalidating old byte views.
 * @param[in] srs_id Native 32-bit spatial-reference ID; little-endian host required for advertised
 *     encoding.
 */
inline void write_gp_header_empty(std::vector<uint8_t>& buf, int32_t srs_id) {
    append_u8(buf, GP_MAGIC_1);
    append_u8(buf, GP_MAGIC_2);
    append_u8(buf, 0x00);
    uint8_t flags = WKB_LITTLE_ENDIAN | (1 << 4); // empty flag set
    append_u8(buf, flags);
    buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(&srs_id),
               reinterpret_cast<const uint8_t*>(&srs_id) + 4);
}

} // namespace detail

// ============================================================================
// Encode functions
// ============================================================================

/**
 * @brief Encode an XY point with a matching XY envelope.
 *
 * No CRS transform, unit conversion or geometry validity check.
 *
 * @param[in] x X coordinate in the selected CRS units; no finite validation.
 * @param[in] y Y coordinate in CRS units; no finite validation.
 * @param[in] srs_id Spatial-reference identifier copied into the GP header.
 * @return Owned GP+WKB POINT bytes; caller must run on a little-endian IEEE-double-compatible host.
 */
inline std::vector<uint8_t> encode_point(double x, double y, int32_t srs_id) {
    std::vector<uint8_t> buf;
    buf.reserve(8 + 32 + 21); // header + envelope + WKB point
    detail::write_gp_header(buf, srs_id, x, y, x, y);
    // WKB POINT
    detail::append_u8(buf, WKB_LITTLE_ENDIAN);
    detail::append_u32(buf, WKB_POINT);
    detail::append_f64(buf, x);
    detail::append_f64(buf, y);
    return buf;
}

/**
 * @brief Encode an XY LINESTRING.
 *
 * Little-endian native 32-bit/IEEE-double representation required. No coordinate finiteness,
 * minimum point count, topology or ring orientation validation. Allocation failures propagate.
 *
 * @param[in] xs Borrowed X coordinates; count must fit the encoded uint32 length.
 * @param[in] ys Borrowed Y coordinates with at least xs.size() entries; extra entries are ignored.
 * @param[in] srs_id Copied spatial-reference identifier; coordinates remain in their input CRS
 *     units.
 * @return Owned GP header/envelope and WKB geometry; empty xs produces an empty geometry.
 */
inline std::vector<uint8_t> encode_linestring(const std::vector<double>& xs,
                                              const std::vector<double>& ys, int32_t srs_id) {
    if (xs.empty()) {
        std::vector<uint8_t> buf;
        detail::write_gp_header_empty(buf, srs_id);
        detail::append_u8(buf, WKB_LITTLE_ENDIAN);
        detail::append_u32(buf, WKB_LINESTRING);
        detail::append_u32(buf, 0);
        return buf;
    }
    double min_x = xs[0], max_x = xs[0], min_y = ys[0], max_y = ys[0];
    for (size_t i = 1; i < xs.size(); ++i) {
        min_x = std::min(min_x, xs[i]);
        max_x = std::max(max_x, xs[i]);
        min_y = std::min(min_y, ys[i]);
        max_y = std::max(max_y, ys[i]);
    }
    std::vector<uint8_t> buf;
    buf.reserve(8 + 32 + 9 + xs.size() * 16);
    detail::write_gp_header(buf, srs_id, min_x, min_y, max_x, max_y);
    detail::append_u8(buf, WKB_LITTLE_ENDIAN);
    detail::append_u32(buf, WKB_LINESTRING);
    detail::append_u32(buf, static_cast<uint32_t>(xs.size()));
    for (size_t i = 0; i < xs.size(); ++i) {
        detail::append_f64(buf, xs[i]);
        detail::append_f64(buf, ys[i]);
    }
    return buf;
}

/**
 * @brief Encode closed-ring spatial coordinates in MULTIPOLYGON form.
 *
 * Little-endian native 32-bit/IEEE-double representation required. Appends the first point when
 * last XY is not exactly equal to first. Closed rings delimit parts and holes; containment determines
 * their nesting. No coordinate finiteness,
 * minimum point count, topology or ring orientation validation. Allocation failures propagate.
 *
 * @param[in] xs Borrowed X coordinates; count must fit the encoded uint32 length including any
 *     automatically appended closure point.
 * @param[in] ys Borrowed Y coordinates with at least xs.size() entries; extra entries are ignored.
 * @param[in] srs_id Copied spatial-reference identifier; coordinates remain in their input CRS
 *     units.
 * @return Owned GP header/envelope and WKB geometry; empty xs produces an empty geometry.
 */
inline std::vector<uint8_t> encode_multipolygon(const std::vector<double>& xs,
                                                const std::vector<double>& ys, int32_t srs_id) {
    if (xs.empty()) {
        std::vector<uint8_t> buf;
        detail::write_gp_header_empty(buf, srs_id);
        detail::append_u8(buf, WKB_LITTLE_ENDIAN);
        detail::append_u32(buf, WKB_MULTIPOLYGON);
        detail::append_u32(buf, 0);
        return buf;
    }
    const auto rings = polygon::rings(xs, ys);
    // Containment depth identifies exteriors (even) and holes (odd), without
    // relying on ring winding. Valid polygon rings do not cross each other.
    std::vector<int> depth(rings.size(), 0), parent(rings.size(), -1);
    for (size_t i = 0; i < rings.size(); ++i)
        for (size_t j = 0; j < rings.size(); ++j)
            if (i != j && polygon::contains(xs, ys, rings[j], xs[rings[i].first],
                                            ys[rings[i].first]))
                ++depth[i];
    for (size_t i = 0; i < rings.size(); ++i)
        if (depth[i] % 2)
            for (size_t j = 0; j < rings.size(); ++j)
                if (depth[j] == depth[i] - 1 &&
                    polygon::contains(xs, ys, rings[j], xs[rings[i].first], ys[rings[i].first])) {
                    parent[i] = static_cast<int>(j);
                    break;
                }

    const auto minmax_x = std::minmax_element(xs.begin(), xs.end());
    const auto minmax_y = std::minmax_element(ys.begin(), ys.end());
    std::vector<uint8_t> buf;
    detail::write_gp_header(buf, srs_id, *minmax_x.first, *minmax_y.first,
                            *minmax_x.second, *minmax_y.second);
    detail::append_u8(buf, WKB_LITTLE_ENDIAN);
    detail::append_u32(buf, WKB_MULTIPOLYGON);
    const auto polygon_count = std::count_if(depth.begin(), depth.end(),
                                             [](int d) { return d % 2 == 0; });
    detail::append_u32(buf, static_cast<uint32_t>(polygon_count));
    const auto write_ring = [&](polygon::Ring ring) {
        detail::append_u32(buf, static_cast<uint32_t>(ring.count + 1));
        for (size_t k = 0; k <= ring.count; ++k) {
            const auto v = ring.first + k % ring.count;
            detail::append_f64(buf, xs[v]);
            detail::append_f64(buf, ys[v]);
        }
    };
    for (size_t i = 0; i < rings.size(); ++i) {
        if (depth[i] % 2)
            continue;
        detail::append_u8(buf, WKB_LITTLE_ENDIAN);
        detail::append_u32(buf, WKB_POLYGON);
        detail::append_u32(buf, 1 + static_cast<uint32_t>(
            std::count(parent.begin(), parent.end(), static_cast<int>(i))));
        write_ring(rings[i]);
        for (size_t j = 0; j < rings.size(); ++j)
            if (parent[j] == static_cast<int>(i))
                write_ring(rings[j]);
    }
    return buf;
}

/**
 * @brief Encode a single exterior ring in POLYGON form.
 *
 * Little-endian native 32-bit/IEEE-double representation required. Appends the first point when
 * last XY is not exactly equal to first; one polygon/ring only, no holes. No coordinate finiteness,
 * minimum point count, topology or ring orientation validation. Allocation failures propagate.
 *
 * @param[in] xs Borrowed X coordinates; count must fit the encoded uint32 length including any
 *     automatically appended closure point.
 * @param[in] ys Borrowed Y coordinates with at least xs.size() entries; extra entries are ignored.
 * @param[in] srs_id Copied spatial-reference identifier; coordinates remain in their input CRS
 *     units.
 * @return Owned GP header/envelope and WKB geometry; empty xs produces an empty geometry.
 */
inline std::vector<uint8_t> encode_polygon(const std::vector<double>& xs,
                                           const std::vector<double>& ys, int32_t srs_id) {
    if (xs.empty()) {
        std::vector<uint8_t> buf;
        detail::write_gp_header_empty(buf, srs_id);
        detail::append_u8(buf, WKB_LITTLE_ENDIAN);
        detail::append_u32(buf, WKB_POLYGON);
        detail::append_u32(buf, 0);
        return buf;
    }
    // Ensure ring closure
    bool closed = (xs.front() == xs.back() && ys.front() == ys.back());
    uint32_t n_pts = static_cast<uint32_t>(xs.size()) + (closed ? 0 : 1);

    double min_x = xs[0], max_x = xs[0], min_y = ys[0], max_y = ys[0];
    for (size_t i = 1; i < xs.size(); ++i) {
        min_x = std::min(min_x, xs[i]);
        max_x = std::max(max_x, xs[i]);
        min_y = std::min(min_y, ys[i]);
        max_y = std::max(max_y, ys[i]);
    }

    std::vector<uint8_t> buf;
    buf.reserve(8 + 32 + 30 + n_pts * 16);
    detail::write_gp_header(buf, srs_id, min_x, min_y, max_x, max_y);

    // WKB POLYGON
    detail::append_u8(buf, WKB_LITTLE_ENDIAN);
    detail::append_u32(buf, WKB_POLYGON);
    detail::append_u32(buf, 1); // 1 ring
    detail::append_u32(buf, n_pts);
    for (size_t i = 0; i < xs.size(); ++i) {
        detail::append_f64(buf, xs[i]);
        detail::append_f64(buf, ys[i]);
    }
    if (!closed) {
        detail::append_f64(buf, xs.front());
        detail::append_f64(buf, ys.front());
    }
    return buf;
}

// ============================================================================
// Decode structures
// ============================================================================

struct DecodedPoint {
    double x = 0.0, y = 0.0;
    int32_t srs_id = 0;
};

struct DecodedLinestring {
    std::vector<double> xs, ys;
    int32_t srs_id = 0;
};

struct DecodedMultipolygon {
    // All rings, explicitly closed and concatenated (exterior, holes, next polygon).
    std::vector<double> xs, ys;
    int32_t srs_id = 0;
};

// ============================================================================
// Decode functions
// ============================================================================

namespace detail {

/**
 * @brief Read GP magic and envelope kind to locate the WKB payload.
 *
 * Does not check version, byte-order/empty bits or whether the envelope/payload bytes exist. Caller
 * validates the complete geometry before using this offset.
 * @throws std::runtime_error For
 * size<8, invalid magic or unknown envelope kind.
 *
 * @param[in] data Borrowed readable block of at least size bytes; non-null whenever size>=8.
 * @param[in] size Block byte count; only the fixed eight-byte prefix is bounds-checked.
 * @param[out] srs_id Assigned from native 32-bit bytes after magic validation; may be assigned
 *     before an envelope-kind refusal.
 * @return Computed payload offset 8+envelope bytes, which can exceed size.
 */
inline size_t parse_gp_header(const uint8_t* data, size_t size, int32_t& srs_id) {
    if (size < 8)
        throw std::runtime_error("GeoPackage binary too short");
    if (data[0] != GP_MAGIC_1 || data[1] != GP_MAGIC_2)
        throw std::runtime_error("Invalid GeoPackage binary magic");
    uint8_t flags = data[3];
    std::memcpy(&srs_id, data + 4, 4);
    // Determine envelope size from flags bits 1-3
    int envelope_type = (flags >> 1) & 0x07;
    size_t envelope_size = 0;
    switch (envelope_type) {
    case 0:
        envelope_size = 0;
        break;
    case 1:
        envelope_size = 32;
        break; // XY
    case 2:
        envelope_size = 48;
        break; // XYZ
    case 3:
        envelope_size = 48;
        break; // XYM
    case 4:
        envelope_size = 64;
        break; // XYZM
    default:
        throw std::runtime_error("Unknown envelope type");
    }
    return 8 + envelope_size; // offset to WKB payload
}

/**
 * @brief Read an unchecked native scalar and advance a byte offset.
 *
 * Caller guarantees enough bytes and a valid object representation; this helper is not safe
 * validation for arbitrary malformed geometry.
 *
 * @param[in] data Borrowed bytes containing a complete trivially copyable T at the current offset.
 * @param[in,out] offset Current byte offset; advanced by sizeof(T) after memcpy, with no
 *     bounds/overflow check.
 * @return Native T representation copied from data; no endian conversion.
 */
template <typename T> inline T read_val(const uint8_t* data, size_t& offset) {
    T val;
    std::memcpy(&val, data + offset, sizeof(T));
    offset += sizeof(T);
    return val;
}

} // namespace detail

/**
 * @brief Decode POINT coordinates from a prevalidated native-endian GP blob.
 *
 * Reads native 32-bit/double values and ignores WKB byte-order markers; compatible little-endian
 * encoding is required. No payload bounds/count-budget check is implemented; do not pass
 * unvalidated truncated or excessive-count blobs.
 * @throws std::runtime_error For checked
 * GP-header or top-level WKB-type mismatch; allocations may also throw.
 *
 * @param[in] blob Borrowed complete GP+WKB geometry. Caller verifies all advertised counts and byte
 *     ranges before entry; header length alone is insufficient.
 * @return Owned decoded coordinates and header SRS ID, without CRS/unit transformation.
 */
inline DecodedPoint decode_point(const std::vector<uint8_t>& blob) {
    DecodedPoint pt;
    size_t offset = detail::parse_gp_header(blob.data(), blob.size(), pt.srs_id);
    offset += 1; // byte order
    uint32_t type = detail::read_val<uint32_t>(blob.data(), offset);
    if (type != WKB_POINT)
        throw std::runtime_error("Expected WKB POINT");
    pt.x = detail::read_val<double>(blob.data(), offset);
    pt.y = detail::read_val<double>(blob.data(), offset);
    return pt;
}

/**
 * @brief Decode LINESTRING coordinates from a prevalidated native-endian GP blob.
 *
 * Reads native 32-bit/double values and ignores WKB byte-order markers; compatible little-endian
 * encoding is required. No payload bounds/count-budget check is implemented; do not pass
 * unvalidated truncated or excessive-count blobs.
 * @throws std::runtime_error For checked
 * GP-header or top-level WKB-type mismatch; allocations may also throw.
 *
 * @param[in] blob Borrowed complete GP+WKB geometry. Caller verifies all advertised counts and byte
 *     ranges before entry; header length alone is insufficient.
 * @return Owned decoded coordinates and header SRS ID, without CRS/unit transformation.
 */
inline DecodedLinestring decode_linestring(const std::vector<uint8_t>& blob) {
    DecodedLinestring ls;
    size_t offset = detail::parse_gp_header(blob.data(), blob.size(), ls.srs_id);
    offset += 1; // byte order
    uint32_t type = detail::read_val<uint32_t>(blob.data(), offset);
    if (type != WKB_LINESTRING)
        throw std::runtime_error("Expected WKB LINESTRING");
    uint32_t n = detail::read_val<uint32_t>(blob.data(), offset);
    ls.xs.resize(n);
    ls.ys.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        ls.xs[i] = detail::read_val<double>(blob.data(), offset);
        ls.ys[i] = detail::read_val<double>(blob.data(), offset);
    }
    return ls;
}

/**
 * @brief Decode MULTIPOLYGON coordinates from a prevalidated native-endian GP blob.
 *
 * Reads native 32-bit/double values and ignores WKB byte-order markers; compatible little-endian
 * encoding is required. All polygon rings are retained as concatenated, explicitly closed rings. Member
 * counts are checked against the available payload. The top-level header still requires a
 * prevalidated native-endian blob.
 * @throws
 * std::runtime_error For checked GP-header or top-level WKB-type mismatch; allocations may also
 * throw.
 *
 * @param[in] blob Borrowed complete GP+WKB geometry. Caller verifies all advertised counts and byte
 *     ranges before entry; header length alone is insufficient.
 * @return Owned decoded coordinates and header SRS ID, without CRS/unit transformation.
 */
inline DecodedMultipolygon decode_multipolygon(const std::vector<uint8_t>& blob) {
    DecodedMultipolygon mp;
    size_t offset = detail::parse_gp_header(blob.data(), blob.size(), mp.srs_id);
    offset += 1; // byte order
    uint32_t type = detail::read_val<uint32_t>(blob.data(), offset);
    if (type != WKB_MULTIPOLYGON)
        throw std::runtime_error("Expected WKB MULTIPOLYGON");
    uint32_t n_polys = detail::read_val<uint32_t>(blob.data(), offset);
    if (n_polys == 0)
        return mp;
    const auto require = [&](size_t bytes) {
        if (offset > blob.size() || bytes > blob.size() - offset)
            throw std::runtime_error("Truncated WKB MULTIPOLYGON");
    };
    for (uint32_t p = 0; p < n_polys; ++p) {
        require(9);
        offset += 1; // native little-endian polygon byte order
        const auto poly_type = detail::read_val<uint32_t>(blob.data(), offset);
        if (poly_type != WKB_POLYGON)
            throw std::runtime_error("Expected WKB POLYGON member");
        const auto n_rings = detail::read_val<uint32_t>(blob.data(), offset);
        for (uint32_t r = 0; r < n_rings; ++r) {
            require(4);
            const auto n_pts = detail::read_val<uint32_t>(blob.data(), offset);
            if (n_pts > (blob.size() - offset) / (2 * sizeof(double)))
                throw std::runtime_error("Truncated WKB polygon ring");
            const auto first = mp.xs.size();
            for (uint32_t i = 0; i < n_pts; ++i) {
                mp.xs.push_back(detail::read_val<double>(blob.data(), offset));
                mp.ys.push_back(detail::read_val<double>(blob.data(), offset));
            }
            if (n_pts && (mp.xs.back() != mp.xs[first] || mp.ys.back() != mp.ys[first])) {
                mp.xs.push_back(mp.xs[first]);
                mp.ys.push_back(mp.ys[first]);
            }
        }
    }
    return mp;
}

} // namespace openswmm::gpkg

#endif // OPENSWMM_GPKG_GEOMETRY_HPP
