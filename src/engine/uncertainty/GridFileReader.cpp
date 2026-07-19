/**
 * @file GridFileReader.cpp
 * @brief Implementation of the HDF5 grid-file reader (SR-2a).
 *
 * @ingroup engine_uncertainty
 */

#include "GridFileReader.hpp"

#include <hdf5.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace openswmm {

// ============================================================================
// Anonymous helpers
// ============================================================================

namespace {

// Read a string attribute from an HDF5 object. Returns false on failure.
bool read_string_attr(hid_t loc, const char* name, std::string& out) {
    if (H5Aexists(loc, name) <= 0)
        return false;

    hid_t attr = H5Aopen(loc, name, H5P_DEFAULT);
    if (attr < 0) return false;

    hid_t atype = H5Aget_type(attr);
    if (atype < 0) { H5Aclose(attr); return false; }

    // Handle both fixed-size and variable-length strings
    bool is_vlen = (H5Tget_class(atype) == H5T_VLEN);
    size_t len = 0;
    hid_t space = H5Aget_space(attr);
    hsize_t nelem = 1;
    if (space >= 0) {
        H5Sget_simple_extent_dims(space, &nelem, nullptr);
        H5Sclose(space);
    }

    bool ok = false;
    if (is_vlen) {
        char* buf = nullptr;
        if (H5Aread(attr, atype, &buf) >= 0 && buf) {
            out = buf;
            free(buf);
            ok = true;
        }
    } else {
        len = H5Tget_size(atype);
        if (len > 0) {
            std::vector<char> buf(len + 1, '\0');
            if (H5Aread(attr, atype, buf.data()) >= 0) {
                out = buf.data();
                ok = true;
            }
        }
    }

    H5Tclose(atype);
    H5Aclose(attr);
    return ok;
}

// Check if a dataset exists in the file.
bool dataset_exists(hid_t file, const char* name) {
    return (H5Lexists(file, name, H5P_DEFAULT) > 0);
}

// Get the dimensionality and size of a dataset.
bool get_dataset_dims(hid_t file, const char* name, std::vector<hsize_t>& dims) {
    hid_t ds = H5Dopen(file, name, H5P_DEFAULT);
    if (ds < 0) return false;

    hid_t space = H5Dget_space(ds);
    if (space < 0) { H5Dclose(ds); return false; }

    int ndims = H5Sget_simple_extent_ndims(space);
    if (ndims < 0) { H5Sclose(space); H5Dclose(ds); return false; }

    dims.resize(ndims);
    H5Sget_simple_extent_dims(space, dims.data(), nullptr);
    H5Sclose(space);
    H5Dclose(ds);
    return true;
}

// Parse family string to enum.
GridFamily parse_family(const std::string& s) {
    std::string upper(s.size(), '\0');
    std::transform(s.begin(), s.end(), upper.begin(),
                   [](unsigned char c){ return static_cast<char>(std::toupper(c)); });
    if (upper == "NORMAL")    return GridFamily::NORMAL;
    if (upper == "LOGNORMAL") return GridFamily::LOGNORMAL;
    if (upper == "UNIFORM")   return GridFamily::UNIFORM;
    if (upper == "MIXED")     return GridFamily::MIXED;
    return GridFamily::UNKNOWN;
}

// Parse spread_kind string to enum.
GridSpreadKind parse_spread_kind(const std::string& s) {
    std::string upper(s.size(), '\0');
    std::transform(s.begin(), s.end(), upper.begin(),
                   [](unsigned char c){ return static_cast<char>(std::toupper(c)); });
    if (upper == "SD")        return GridSpreadKind::SD;
    if (upper == "CV")       return GridSpreadKind::CV;
    if (upper == "HALFRANGE") return GridSpreadKind::HALFRANGE;
    return GridSpreadKind::UNKNOWN;
}

} // anonymous namespace

// ============================================================================
// Lifecycle
// ============================================================================

GridFileReader::GridFileReader()
    : file_id_(nullptr)
{}

GridFileReader::~GridFileReader() {
    close();
}

GridFileReader::GridFileReader(GridFileReader&& other) noexcept
    : file_id_(other.file_id_)
    , n_time_(other.n_time_)
    , nx_(other.nx_)
    , ny_(other.ny_)
    , family_(other.family_)
    , spread_kind_(other.spread_kind_)
    , units_(std::move(other.units_))
    , crs_(std::move(other.crs_))
    , has_location_(other.has_location_)
    , has_family_code_(other.has_family_code_)
    , x_coords_(std::move(other.x_coords_))
    , y_coords_(std::move(other.y_coords_))
    , time_axis_(std::move(other.time_axis_))
    , cur_index_(other.cur_index_)
    , next_index_(other.next_index_)
    , loc_cur_(std::move(other.loc_cur_))
    , loc_next_(std::move(other.loc_next_))
    , sp_cur_(std::move(other.sp_cur_))
    , sp_next_(std::move(other.sp_next_))
    , family_code_(std::move(other.family_code_))
    , last_error_(std::move(other.last_error_))
{
    other.file_id_ = nullptr;
    other.cur_index_ = -1;
    other.next_index_ = -1;
}

GridFileReader& GridFileReader::operator=(GridFileReader&& other) noexcept {
    if (this != &other) {
        close();
        file_id_       = other.file_id_;
        n_time_        = other.n_time_;
        nx_            = other.nx_;
        ny_            = other.ny_;
        family_        = other.family_;
        spread_kind_   = other.spread_kind_;
        units_         = std::move(other.units_);
        crs_           = std::move(other.crs_);
        has_location_  = other.has_location_;
        has_family_code_ = other.has_family_code_;
        x_coords_      = std::move(other.x_coords_);
        y_coords_      = std::move(other.y_coords_);
        time_axis_     = std::move(other.time_axis_);
        cur_index_     = other.cur_index_;
        next_index_    = other.next_index_;
        loc_cur_       = std::move(other.loc_cur_);
        loc_next_      = std::move(other.loc_next_);
        sp_cur_        = std::move(other.sp_cur_);
        sp_next_       = std::move(other.sp_next_);
        family_code_   = std::move(other.family_code_);
        last_error_    = std::move(other.last_error_);
        other.file_id_ = nullptr;
        other.cur_index_ = -1;
        other.next_index_ = -1;
    }
    return *this;
}

// ============================================================================
// open()
// ============================================================================

bool GridFileReader::open(const std::string& path) {
    close();  // reset state

    hid_t file = H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    if (file < 0) {
        last_error_ = "GridFileReader: cannot open file '" + path + "'";
        return false;
    }
    file_id_ = reinterpret_cast<void*>(file);

    if (!validate_schema_()) {
        // last_error_ already set by validate_schema_
        H5Fclose(file);
        file_id_ = nullptr;
        return false;
    }

    if (!read_metadata_()) {
        H5Fclose(file);
        file_id_ = nullptr;
        return false;
    }

    if (!read_axes_()) {
        H5Fclose(file);
        file_id_ = nullptr;
        return false;
    }

    if (!read_time_axis_()) {
        H5Fclose(file);
        file_id_ = nullptr;
        return false;
    }

    return true;
}

// ============================================================================
// close()
// ============================================================================

void GridFileReader::close() {
    if (file_id_) {
        H5Fclose(reinterpret_cast<hid_t>(file_id_));
        file_id_ = nullptr;
    }
    n_time_ = 0;
    nx_ = 0;
    ny_ = 0;
    family_ = GridFamily::UNKNOWN;
    spread_kind_ = GridSpreadKind::UNKNOWN;
    units_.clear();
    crs_.clear();
    has_location_ = false;
    has_family_code_ = false;
    x_coords_.clear();
    y_coords_.clear();
    time_axis_.clear();
    cur_index_ = -1;
    next_index_ = -1;
    loc_cur_.clear();
    loc_next_.clear();
    sp_cur_.clear();
    sp_next_.clear();
    family_code_.clear();
    last_error_.clear();
}

// ============================================================================
// validate_schema_()
// ============================================================================

bool GridFileReader::validate_schema_() {
    hid_t file = reinterpret_cast<hid_t>(file_id_);

    // Required: /spread
    if (!dataset_exists(file, "/spread")) {
        last_error_ = "GridFileReader: required dataset '/spread' is missing";
        return false;
    }

    // Required: /time
    if (!dataset_exists(file, "/time")) {
        last_error_ = "GridFileReader: required dataset '/time' is missing";
        return false;
    }

    // Required: /x, /y
    if (!dataset_exists(file, "/x")) {
        last_error_ = "GridFileReader: required dataset '/x' is missing";
        return false;
    }
    if (!dataset_exists(file, "/y")) {
        last_error_ = "GridFileReader: required dataset '/y' is missing";
        return false;
    }

    // Validate /spread dimensions: (T, ny, nx)
    std::vector<hsize_t> spread_dims;
    if (!get_dataset_dims(file, "/spread", spread_dims)) {
        last_error_ = "GridFileReader: cannot read '/spread' dimensions";
        return false;
    }
    if (spread_dims.size() != 3) {
        last_error_ = "GridFileReader: '/spread' must be 3-D (T, ny, nx), got "
                      + std::to_string(spread_dims.size()) + "-D";
        return false;
    }
    n_time_ = static_cast<int>(spread_dims[0]);
    ny_     = static_cast<int>(spread_dims[1]);
    nx_     = static_cast<int>(spread_dims[2]);

    if (n_time_ <= 0 || ny_ <= 0 || nx_ <= 0) {
        last_error_ = "GridFileReader: '/spread' has zero-size dimension"
                      " (T=" + std::to_string(n_time_)
                      + ", ny=" + std::to_string(ny_)
                      + ", nx=" + std::to_string(nx_) + ")";
        return false;
    }

    // Validate /time dimensions: (T,)
    std::vector<hsize_t> time_dims;
    if (!get_dataset_dims(file, "/time", time_dims)) {
        last_error_ = "GridFileReader: cannot read '/time' dimensions";
        return false;
    }
    if (time_dims.size() != 1 || static_cast<int>(time_dims[0]) != n_time_) {
        last_error_ = "GridFileReader: '/time' must be 1-D with length "
                      + std::to_string(n_time_) + ", got "
                      + std::to_string(time_dims.size()) + "-D";
        return false;
    }

    // Validate /x, /y dimensions
    std::vector<hsize_t> x_dims, y_dims;
    if (!get_dataset_dims(file, "/x", x_dims)) {
        last_error_ = "GridFileReader: cannot read '/x' dimensions";
        return false;
    }
    if (x_dims.size() != 1 || static_cast<int>(x_dims[0]) != nx_) {
        last_error_ = "GridFileReader: '/x' must be 1-D with length "
                      + std::to_string(nx_) + ", got "
                      + std::to_string(x_dims.size()) + "-D";
        return false;
    }
    if (!get_dataset_dims(file, "/y", y_dims)) {
        last_error_ = "GridFileReader: cannot read '/y' dimensions";
        return false;
    }
    if (y_dims.size() != 1 || static_cast<int>(y_dims[0]) != ny_) {
        last_error_ = "GridFileReader: '/y' must be 1-D with length "
                      + std::to_string(ny_) + ", got "
                      + std::to_string(y_dims.size()) + "-D";
        return false;
    }

    // Optional: /location — if present, must match /spread dims
    has_location_ = dataset_exists(file, "/location");
    if (has_location_) {
        std::vector<hsize_t> loc_dims;
        if (!get_dataset_dims(file, "/location", loc_dims)) {
            last_error_ = "GridFileReader: cannot read '/location' dimensions";
            return false;
        }
        if (loc_dims != spread_dims) {
            last_error_ = "GridFileReader: '/location' dimensions must match '/spread'";
            return false;
        }
    }

    // Optional: /family_code — required when root family == MIXED (SR-4b).
    // Must be 2-D (ny, nx) uint8, one code per cell (0=NORMAL, 1=LOGNORMAL,
    // 2=UNIFORM). Validated here (existence + dimensions); codes are read and
    // checked at read_family_code_plane_() time.
    has_family_code_ = dataset_exists(file, "/family_code");
    if (has_family_code_) {
        std::vector<hsize_t> fc_dims;
        if (!get_dataset_dims(file, "/family_code", fc_dims)) {
            last_error_ = "GridFileReader: cannot read '/family_code' dimensions";
            return false;
        }
        if (fc_dims.size() != 2
            || static_cast<int>(fc_dims[0]) != ny_
            || static_cast<int>(fc_dims[1]) != nx_) {
            last_error_ = "GridFileReader: '/family_code' must be 2-D (ny, nx)";
            return false;
        }
    }

    return true;
}

// ============================================================================
// read_metadata_()
// ============================================================================

bool GridFileReader::read_metadata_() {
    hid_t file = reinterpret_cast<hid_t>(file_id_);

    // Required root attributes: family, spread_kind
    std::string family_str;
    if (!read_string_attr(file, "family", family_str)) {
        last_error_ = "GridFileReader: required root attribute 'family' is missing";
        return false;
    }
    family_ = parse_family(family_str);
    if (family_ == GridFamily::UNKNOWN) {
        last_error_ = "GridFileReader: unknown family '" + family_str
                      + "' (supported: NORMAL, LOGNORMAL, UNIFORM, MIXED)";
        return false;
    }

    std::string spread_kind_str;
    if (!read_string_attr(file, "spread_kind", spread_kind_str)) {
        last_error_ = "GridFileReader: required root attribute 'spread_kind' is missing";
        return false;
    }
    spread_kind_ = parse_spread_kind(spread_kind_str);
    if (spread_kind_ == GridSpreadKind::UNKNOWN) {
        last_error_ = "GridFileReader: unknown spread_kind '" + spread_kind_str
                      + "' (supported: SD, CV, HALFRANGE)";
        return false;
    }

    // Optional attributes: units, crs
    read_string_attr(file, "units", units_);
    read_string_attr(file, "crs", crs_);

    // When family == MIXED, /family_code must be present (validated in
    // validate_schema_). Conversely, /family_code without family=MIXED is
    // accepted but ignored — it does not change the single-family evaluation.
    if (family_ == GridFamily::MIXED && !has_family_code_) {
        last_error_ = "GridFileReader: root family='MIXED' requires a "
                      "/family_code dataset (ny, nx uint8)";
        return false;
    }

    // Read the static /family_code plane once (it does not vary with time).
    if (has_family_code_) {
        if (!read_family_code_plane_())
            return false;
    }

    return true;
}

// ============================================================================
// read_axes_()
// ============================================================================

bool GridFileReader::read_axes_() {
    hid_t file = reinterpret_cast<hid_t>(file_id_);

    // Read /x
    {
        hid_t ds = H5Dopen(file, "/x", H5P_DEFAULT);
        if (ds < 0) { last_error_ = "GridFileReader: cannot open '/x'"; return false; }
        x_coords_.resize(static_cast<std::size_t>(nx_));
        if (H5Dread(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    x_coords_.data()) < 0) {
            last_error_ = "GridFileReader: cannot read '/x' data";
            H5Dclose(ds);
            return false;
        }
        H5Dclose(ds);
    }

    // Read /y
    {
        hid_t ds = H5Dopen(file, "/y", H5P_DEFAULT);
        if (ds < 0) { last_error_ = "GridFileReader: cannot open '/y'"; return false; }
        y_coords_.resize(static_cast<std::size_t>(ny_));
        if (H5Dread(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    y_coords_.data()) < 0) {
            last_error_ = "GridFileReader: cannot read '/y' data";
            H5Dclose(ds);
            return false;
        }
        H5Dclose(ds);
    }

    return true;
}

// ============================================================================
// read_time_axis_()
// ============================================================================

bool GridFileReader::read_time_axis_() {
    hid_t file = reinterpret_cast<hid_t>(file_id_);

    hid_t ds = H5Dopen(file, "/time", H5P_DEFAULT);
    if (ds < 0) { last_error_ = "GridFileReader: cannot open '/time'"; return false; }

    time_axis_.resize(static_cast<std::size_t>(n_time_));
    if (H5Dread(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                time_axis_.data()) < 0) {
        last_error_ = "GridFileReader: cannot read '/time' data";
        H5Dclose(ds);
        return false;
    }
    H5Dclose(ds);

    // Validate monotonicity
    for (int t = 1; t < n_time_; ++t) {
        if (time_axis_[t] <= time_axis_[t - 1]) {
            last_error_ = "GridFileReader: '/time' is not strictly increasing"
                          " at index " + std::to_string(t);
            return false;
        }
    }

    return true;
}

// ============================================================================
// read_plane_()
// ============================================================================

bool GridFileReader::read_plane_(int t, std::vector<float>& loc_buf,
                                            std::vector<float>& sp_buf) const {
    hid_t file = reinterpret_cast<hid_t>(file_id_);
    const auto plane_size = static_cast<std::size_t>(ny_ * nx_);

    // Helper to read one 2-D hyperslab from a 3-D dataset
    auto read_slab = [&](const char* name, std::vector<float>& buf) -> bool {
        hid_t ds = H5Dopen(file, name, H5P_DEFAULT);
        if (ds < 0) return false;

        hid_t filespace = H5Dget_space(ds);
        hsize_t offset[3] = { static_cast<hsize_t>(t), 0, 0 };
        hsize_t count[3]  = { 1, static_cast<hsize_t>(ny_),
                                  static_cast<hsize_t>(nx_) };
        H5Sselect_hyperslab(filespace, H5S_SELECT_SET, offset, nullptr,
                            count, nullptr);

        hid_t memspace = H5Screate_simple(3, count, nullptr);
        herr_t status = H5Dread(ds, H5T_NATIVE_FLOAT, memspace, filespace,
                                  H5P_DEFAULT, buf.data());
        H5Sclose(memspace);
        H5Sclose(filespace);
        H5Dclose(ds);
        return status >= 0;
    };

    sp_buf.resize(plane_size);
    if (!read_slab("/spread", sp_buf)) {
        last_error_ = "GridFileReader: cannot read '/spread' plane at t="
                      + std::to_string(t);
        return false;
    }

    if (has_location_) {
        loc_buf.resize(plane_size);
        if (!read_slab("/location", loc_buf)) {
            last_error_ = "GridFileReader: cannot read '/location' plane at t="
                          + std::to_string(t);
            return false;
        }
    } else {
        loc_buf.clear();
    }

    return true;
}

// ============================================================================
// read_family_code_plane_() — read the static /family_code dataset (SR-4b)
// ============================================================================

bool GridFileReader::read_family_code_plane_() {
    hid_t file = reinterpret_cast<hid_t>(file_id_);
    const auto plane_size = static_cast<std::size_t>(ny_ * nx_);

    hid_t ds = H5Dopen(file, "/family_code", H5P_DEFAULT);
    if (ds < 0) {
        last_error_ = "GridFileReader: cannot open '/family_code'";
        return false;
    }

    family_code_.resize(plane_size);
    if (H5Dread(ds, H5T_NATIVE_UINT8, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                family_code_.data()) < 0) {
        last_error_ = "GridFileReader: cannot read '/family_code' data";
        H5Dclose(ds);
        return false;
    }
    H5Dclose(ds);

    // Validate codes: each must be 0 (NORMAL), 1 (LOGNORMAL), or 2 (UNIFORM).
    for (std::size_t i = 0; i < plane_size; ++i) {
        if (family_code_[i] > 2) {
            last_error_ = "GridFileReader: /family_code has invalid value "
                          + std::to_string(family_code_[i])
                          + " at index " + std::to_string(i)
                          + " (supported: 0=NORMAL, 1=LOGNORMAL, 2=UNIFORM)";
            return false;
        }
    }

    return true;
}

// ============================================================================
// advance()
// ============================================================================

bool GridFileReader::advance() {
    if (!file_id_ || n_time_ == 0) return false;

    // First call: load t=0 as current, t=1 as next
    if (cur_index_ < 0) {
        cur_index_ = 0;
        if (!read_plane_(0, loc_cur_, sp_cur_)) return false;

        if (n_time_ > 1) {
            next_index_ = 1;
            if (!read_plane_(1, loc_next_, sp_next_)) return false;
        } else {
            next_index_ = -1;
            loc_next_.clear();
            sp_next_.clear();
        }
        return true;
    }

    // Subsequent calls: shift next→current, load new next
    if (next_index_ < 0) {
        // Already at the last plane
        cur_index_ = n_time_;  // has_current() will return false
        return false;
    }

    cur_index_ = next_index_;
    loc_cur_ = std::move(loc_next_);
    sp_cur_  = std::move(sp_next_);

    int new_next = cur_index_ + 1;
    if (new_next < n_time_) {
        next_index_ = new_next;
        if (!read_plane_(new_next, loc_next_, sp_next_)) return false;
    } else {
        next_index_ = -1;
        loc_next_.clear();
        sp_next_.clear();
    }

    return true;
}

// ============================================================================
// Plane accessors
// ============================================================================

const float* GridFileReader::location_now() const noexcept {
    if (!has_current() || !has_location_ || loc_cur_.empty()) return nullptr;
    return loc_cur_.data();
}

const float* GridFileReader::spread_now() const noexcept {
    if (!has_current()) return nullptr;
    return sp_cur_.data();
}

const float* GridFileReader::location_next() const noexcept {
    if (next_index_ < 0 || !has_location_ || loc_next_.empty()) return nullptr;
    return loc_next_.data();
}

const float* GridFileReader::spread_next() const noexcept {
    if (next_index_ < 0) return nullptr;
    return sp_next_.data();
}

const uint8_t* GridFileReader::family_code_now() const noexcept {
    if (!has_family_code_ || family_code_.empty()) return nullptr;
    return family_code_.data();
}

double GridFileReader::time_now() const noexcept {
    if (!has_current()) return 0.0;
    return time_axis_[static_cast<std::size_t>(cur_index_)];
}

double GridFileReader::time_next() const noexcept {
    if (next_index_ < 0) return time_now();
    return time_axis_[static_cast<std::size_t>(next_index_)];
}

} // namespace openswmm