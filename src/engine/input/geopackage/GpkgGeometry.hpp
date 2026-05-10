/**
 * @file GpkgGeometry.hpp
 * @brief GeoPackage Binary geometry encoding and decoding (ISO WKB with GP header).
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_GPKG_GEOMETRY_HPP
#define OPENSWMM_GPKG_GEOMETRY_HPP

#include <vector>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace openswmm::gpkg {

// WKB geometry type codes (ISO 13249)
enum WkbType : uint32_t {
    WKB_POINT        = 1,
    WKB_LINESTRING   = 2,
    WKB_POLYGON      = 3,
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

inline void append_u8(std::vector<uint8_t>& buf, uint8_t v) {
    buf.push_back(v);
}

inline void append_u32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(&v),
               reinterpret_cast<const uint8_t*>(&v) + 4);
}

inline void append_f64(std::vector<uint8_t>& buf, double v) {
    buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(&v),
               reinterpret_cast<const uint8_t*>(&v) + 8);
}

// GP header with envelope type 1 (min/max XY = 32 bytes envelope)
inline void write_gp_header(std::vector<uint8_t>& buf, int32_t srs_id,
                            double min_x, double min_y, double max_x, double max_y) {
    append_u8(buf, GP_MAGIC_1);
    append_u8(buf, GP_MAGIC_2);
    append_u8(buf, 0x00);        // version 0
    // flags: byte_order=1 (LE), envelope_type=1 (XY), empty=0
    // bits: [empty:1][envelope:3][byte_order:1] -> 0b0_001_1 = 0x03 (LE) with envelope type 1
    uint8_t flags = 0x00;
    flags |= WKB_LITTLE_ENDIAN;  // bit 0: byte order (1=LE)
    flags |= (1 << 1);           // bits 1-3: envelope type 1 (XY only)
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
 * @brief Encode a POINT geometry in GeoPackage Binary format.
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
 * @brief Encode a LINESTRING geometry in GeoPackage Binary format.
 * @param xs  X coordinates of vertices.
 * @param ys  Y coordinates of vertices.
 */
inline std::vector<uint8_t> encode_linestring(const std::vector<double>& xs,
                                               const std::vector<double>& ys,
                                               int32_t srs_id) {
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
 * @brief Encode a MULTIPOLYGON geometry (single polygon, single ring) in GeoPackage Binary format.
 * @param xs  X coordinates of polygon ring vertices.
 * @param ys  Y coordinates of polygon ring vertices.
 */
inline std::vector<uint8_t> encode_multipolygon(const std::vector<double>& xs,
                                                 const std::vector<double>& ys,
                                                 int32_t srs_id) {
    if (xs.empty()) {
        std::vector<uint8_t> buf;
        detail::write_gp_header_empty(buf, srs_id);
        detail::append_u8(buf, WKB_LITTLE_ENDIAN);
        detail::append_u32(buf, WKB_MULTIPOLYGON);
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
    buf.reserve(8 + 32 + 50 + n_pts * 16);
    detail::write_gp_header(buf, srs_id, min_x, min_y, max_x, max_y);

    // WKB MULTIPOLYGON
    detail::append_u8(buf, WKB_LITTLE_ENDIAN);
    detail::append_u32(buf, WKB_MULTIPOLYGON);
    detail::append_u32(buf, 1); // 1 polygon

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
    // Outer ring of the first polygon only (sufficient for SWMM subcatchments)
    std::vector<double> xs, ys;
    int32_t srs_id = 0;
};

// ============================================================================
// Decode functions
// ============================================================================

namespace detail {

inline size_t parse_gp_header(const uint8_t* data, size_t size, int32_t& srs_id) {
    if (size < 8) throw std::runtime_error("GeoPackage binary too short");
    if (data[0] != GP_MAGIC_1 || data[1] != GP_MAGIC_2)
        throw std::runtime_error("Invalid GeoPackage binary magic");
    uint8_t flags = data[3];
    std::memcpy(&srs_id, data + 4, 4);
    // Determine envelope size from flags bits 1-3
    int envelope_type = (flags >> 1) & 0x07;
    size_t envelope_size = 0;
    switch (envelope_type) {
        case 0: envelope_size = 0; break;
        case 1: envelope_size = 32; break;  // XY
        case 2: envelope_size = 48; break;  // XYZ
        case 3: envelope_size = 48; break;  // XYM
        case 4: envelope_size = 64; break;  // XYZM
        default: throw std::runtime_error("Unknown envelope type");
    }
    return 8 + envelope_size; // offset to WKB payload
}

template<typename T>
inline T read_val(const uint8_t* data, size_t& offset) {
    T val;
    std::memcpy(&val, data + offset, sizeof(T));
    offset += sizeof(T);
    return val;
}

} // namespace detail

inline DecodedPoint decode_point(const std::vector<uint8_t>& blob) {
    DecodedPoint pt;
    size_t offset = detail::parse_gp_header(blob.data(), blob.size(), pt.srs_id);
    offset += 1; // byte order
    uint32_t type = detail::read_val<uint32_t>(blob.data(), offset);
    if (type != WKB_POINT) throw std::runtime_error("Expected WKB POINT");
    pt.x = detail::read_val<double>(blob.data(), offset);
    pt.y = detail::read_val<double>(blob.data(), offset);
    return pt;
}

inline DecodedLinestring decode_linestring(const std::vector<uint8_t>& blob) {
    DecodedLinestring ls;
    size_t offset = detail::parse_gp_header(blob.data(), blob.size(), ls.srs_id);
    offset += 1; // byte order
    uint32_t type = detail::read_val<uint32_t>(blob.data(), offset);
    if (type != WKB_LINESTRING) throw std::runtime_error("Expected WKB LINESTRING");
    uint32_t n = detail::read_val<uint32_t>(blob.data(), offset);
    ls.xs.resize(n);
    ls.ys.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        ls.xs[i] = detail::read_val<double>(blob.data(), offset);
        ls.ys[i] = detail::read_val<double>(blob.data(), offset);
    }
    return ls;
}

inline DecodedMultipolygon decode_multipolygon(const std::vector<uint8_t>& blob) {
    DecodedMultipolygon mp;
    size_t offset = detail::parse_gp_header(blob.data(), blob.size(), mp.srs_id);
    offset += 1; // byte order
    uint32_t type = detail::read_val<uint32_t>(blob.data(), offset);
    if (type != WKB_MULTIPOLYGON) throw std::runtime_error("Expected WKB MULTIPOLYGON");
    uint32_t n_polys = detail::read_val<uint32_t>(blob.data(), offset);
    if (n_polys == 0) return mp;
    // Read first polygon
    offset += 1; // polygon byte order
    uint32_t poly_type = detail::read_val<uint32_t>(blob.data(), offset);
    (void)poly_type;
    uint32_t n_rings = detail::read_val<uint32_t>(blob.data(), offset);
    if (n_rings == 0) return mp;
    // Read first ring (outer ring)
    uint32_t n_pts = detail::read_val<uint32_t>(blob.data(), offset);
    mp.xs.resize(n_pts);
    mp.ys.resize(n_pts);
    for (uint32_t i = 0; i < n_pts; ++i) {
        mp.xs[i] = detail::read_val<double>(blob.data(), offset);
        mp.ys[i] = detail::read_val<double>(blob.data(), offset);
    }
    return mp;
}

} // namespace openswmm::gpkg

#endif // OPENSWMM_GPKG_GEOMETRY_HPP
