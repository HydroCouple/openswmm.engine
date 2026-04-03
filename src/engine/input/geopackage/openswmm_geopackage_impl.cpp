/**
 * @file openswmm_geopackage_impl.cpp
 * @brief C API implementation for GeoPackage I/O.
 *
 * @details Model definitions and simulation results are read-only.
 *          Only observed/sensor timeseries data is writable through this API.
 *
 * @ingroup engine_geopackage
 */

#include <openswmm/engine/openswmm_geopackage.h>

#include "GpkgUtils.hpp"
#include "GeoPackageSchema.hpp"
#include "GeoPackagePluginInfo.hpp"

#include <string>
#include <cstring>

using namespace openswmm;
using namespace openswmm::gpkg;

// ============================================================================
// Internal handle
// ============================================================================

struct GpkgHandle {
    DbPtr db;
    std::string error_msg;
    bool in_transaction = false;
};

static GpkgHandle* as_handle(SWMM_Gpkg h) {
    return static_cast<GpkgHandle*>(h);
}

static const char* safe_str(const char* s) { return s ? s : ""; }

// Variable ID lookup helper (cached per query, not per handle — simple enough)
static int lookup_variable_id(sqlite3* db, const char* name, const char* obj_type) {
    auto stmt = prepare(db,
        "SELECT variable_id FROM variables WHERE name = ? AND object_type = ?");
    bind_text(stmt.get(), 1, name);
    bind_text(stmt.get(), 2, obj_type);
    if (sqlite3_step(stmt.get()) == SQLITE_ROW)
        return column_int(stmt.get(), 0);
    return -1;
}

static int count_query(sqlite3* db, const std::string& sql, const char* param = nullptr) {
    auto stmt = prepare(db, sql);
    if (param) bind_text(stmt.get(), 1, param);
    if (sqlite3_step(stmt.get()) == SQLITE_ROW)
        return column_int(stmt.get(), 0);
    return 0;
}

// ============================================================================
// C API
// ============================================================================

extern "C" {

// ---------- Lifecycle -------------------------------------------------------

SWMM_ENGINE_API SWMM_Gpkg swmm_gpkg_open(const char* path) {
    if (!path) return nullptr;
    try {
        auto* h = new GpkgHandle();
        h->db = open_database(path, SQLITE_OPEN_READWRITE);
        return h;
    } catch (const std::exception&) {
        return nullptr;
    }
}

SWMM_ENGINE_API void swmm_gpkg_close(SWMM_Gpkg gpkg) {
    if (!gpkg) return;
    auto* h = as_handle(gpkg);
    if (h->in_transaction) {
        sqlite3_exec(h->db.get(), "ROLLBACK", nullptr, nullptr, nullptr);
    }
    h->db.reset();
    delete h;
}

SWMM_ENGINE_API const char* swmm_gpkg_last_error(SWMM_Gpkg gpkg) {
    if (!gpkg) return "";
    return as_handle(gpkg)->error_msg.c_str();
}

// ---------- Transactions ----------------------------------------------------

SWMM_ENGINE_API int swmm_gpkg_begin(SWMM_Gpkg gpkg) {
    if (!gpkg) return SWMM_GPKG_ERR;
    auto* h = as_handle(gpkg);
    if (h->in_transaction) { h->error_msg = "Transaction already active"; return SWMM_GPKG_ERR; }
    try {
        exec(h->db.get(), "BEGIN IMMEDIATE");
        h->in_transaction = true;
        return SWMM_GPKG_OK;
    } catch (const std::exception& e) { h->error_msg = e.what(); return SWMM_GPKG_ERR; }
}

SWMM_ENGINE_API int swmm_gpkg_commit(SWMM_Gpkg gpkg) {
    if (!gpkg) return SWMM_GPKG_ERR;
    auto* h = as_handle(gpkg);
    if (!h->in_transaction) { h->error_msg = "No active transaction"; return SWMM_GPKG_ERR; }
    try {
        exec(h->db.get(), "COMMIT");
        h->in_transaction = false;
        return SWMM_GPKG_OK;
    } catch (const std::exception& e) { h->error_msg = e.what(); return SWMM_GPKG_ERR; }
}

SWMM_ENGINE_API int swmm_gpkg_rollback(SWMM_Gpkg gpkg) {
    if (!gpkg) return SWMM_GPKG_ERR;
    auto* h = as_handle(gpkg);
    if (!h->in_transaction) return SWMM_GPKG_OK; // no-op
    try {
        exec(h->db.get(), "ROLLBACK");
        h->in_transaction = false;
        return SWMM_GPKG_OK;
    } catch (const std::exception& e) { h->error_msg = e.what(); return SWMM_GPKG_ERR; }
}

// ---------- Registration ----------------------------------------------------

SWMM_ENGINE_API int swmm_gpkg_register(const char* license_key,
                                         const char* organization,
                                         const char* contact_email,
                                         const char* deployment_id) {
    RegistrationInfo info;
    info.license_key = safe_str(license_key);
    info.organization = safe_str(organization);
    info.contact_email = safe_str(contact_email);
    info.deployment_id = safe_str(deployment_id);
    return GeoPackagePluginInfo::instance().register_plugin(info) ? 1 : 0;
}

SWMM_ENGINE_API int swmm_gpkg_is_registered(void) {
    return GeoPackagePluginInfo::instance().registered() ? 1 : 0;
}

// ---------- Read: Simulation metadata ---------------------------------------

SWMM_ENGINE_API int swmm_gpkg_simulation_count(SWMM_Gpkg gpkg) {
    if (!gpkg) return -1;
    try { return count_query(as_handle(gpkg)->db.get(), "SELECT count(*) FROM simulations"); }
    catch (...) { return -1; }
}

SWMM_ENGINE_API int swmm_gpkg_simulation_id(SWMM_Gpkg gpkg, int index,
                                              char* buf, int bufsz) {
    if (!gpkg || !buf || bufsz <= 0) return SWMM_GPKG_ERR;
    auto* h = as_handle(gpkg);
    try {
        auto stmt = prepare(h->db.get(),
            "SELECT simulation_id FROM simulations ORDER BY created_at LIMIT 1 OFFSET ?");
        bind_int(stmt.get(), 1, index);
        if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            std::string id = column_text(stmt.get(), 0);
            std::strncpy(buf, id.c_str(), static_cast<size_t>(bufsz - 1));
            buf[bufsz - 1] = '\0';
            return SWMM_GPKG_OK;
        }
        return SWMM_GPKG_ERR;
    } catch (const std::exception& e) { h->error_msg = e.what(); return SWMM_GPKG_ERR; }
}

// ---------- Read: Model object counts ---------------------------------------

SWMM_ENGINE_API int swmm_gpkg_node_count(SWMM_Gpkg gpkg, const char* sim_id) {
    if (!gpkg || !sim_id) return -1;
    try { return count_query(as_handle(gpkg)->db.get(),
        "SELECT count(*) FROM nodes WHERE simulation_id = ?", sim_id); }
    catch (...) { return -1; }
}

SWMM_ENGINE_API int swmm_gpkg_link_count(SWMM_Gpkg gpkg, const char* sim_id) {
    if (!gpkg || !sim_id) return -1;
    try { return count_query(as_handle(gpkg)->db.get(),
        "SELECT count(*) FROM links WHERE simulation_id = ?", sim_id); }
    catch (...) { return -1; }
}

SWMM_ENGINE_API int swmm_gpkg_subcatch_count(SWMM_Gpkg gpkg, const char* sim_id) {
    if (!gpkg || !sim_id) return -1;
    try { return count_query(as_handle(gpkg)->db.get(),
        "SELECT count(*) FROM subcatchments WHERE simulation_id = ?", sim_id); }
    catch (...) { return -1; }
}

SWMM_ENGINE_API int swmm_gpkg_gage_count(SWMM_Gpkg gpkg, const char* sim_id) {
    if (!gpkg || !sim_id) return -1;
    try { return count_query(as_handle(gpkg)->db.get(),
        "SELECT count(*) FROM rain_gages WHERE simulation_id = ?", sim_id); }
    catch (...) { return -1; }
}

SWMM_ENGINE_API int swmm_gpkg_topology_edge_count(SWMM_Gpkg gpkg, const char* sim_id) {
    if (!gpkg || !sim_id) return -1;
    try { return count_query(as_handle(gpkg)->db.get(),
        "SELECT count(*) FROM node_links WHERE simulation_id = ?", sim_id); }
    catch (...) { return -1; }
}

SWMM_ENGINE_API int swmm_gpkg_variable_count(SWMM_Gpkg gpkg) {
    if (!gpkg) return -1;
    try { return count_query(as_handle(gpkg)->db.get(), "SELECT count(*) FROM variables"); }
    catch (...) { return -1; }
}

// ---------- Read: Result timeseries -----------------------------------------

SWMM_ENGINE_API int swmm_gpkg_result_ts_count(SWMM_Gpkg gpkg,
                                                const char* simulation_id,
                                                const char* object_type,
                                                const char* object_id,
                                                const char* variable_name) {
    if (!gpkg || !simulation_id || !object_type || !object_id || !variable_name)
        return SWMM_GPKG_ERR;
    auto* h = as_handle(gpkg);
    try {
        int vid = lookup_variable_id(h->db.get(), variable_name, object_type);
        if (vid < 0) return 0;
        auto stmt = prepare(h->db.get(),
            "SELECT count(*) FROM result_timeseries "
            "WHERE simulation_id = ? AND object_type = ? AND object_id = ? AND variable_id = ?");
        bind_text(stmt.get(), 1, simulation_id);
        bind_text(stmt.get(), 2, object_type);
        bind_text(stmt.get(), 3, object_id);
        bind_int(stmt.get(), 4, vid);
        if (sqlite3_step(stmt.get()) == SQLITE_ROW)
            return column_int(stmt.get(), 0);
        return 0;
    } catch (const std::exception& e) { h->error_msg = e.what(); return SWMM_GPKG_ERR; }
}

SWMM_ENGINE_API int swmm_gpkg_read_result_ts(SWMM_Gpkg gpkg,
                                               const char* simulation_id,
                                               const char* object_type,
                                               const char* object_id,
                                               const char* variable_name,
                                               double* times,
                                               double* values,
                                               int max_count) {
    if (!gpkg || !simulation_id || !object_type || !object_id || !variable_name
        || !times || !values || max_count <= 0)
        return SWMM_GPKG_ERR;
    auto* h = as_handle(gpkg);
    try {
        int vid = lookup_variable_id(h->db.get(), variable_name, object_type);
        if (vid < 0) { h->error_msg = "Unknown variable"; return 0; }

        auto stmt = prepare(h->db.get(),
            "SELECT elapsed_time, value FROM result_timeseries "
            "WHERE simulation_id = ? AND object_type = ? AND object_id = ? AND variable_id = ? "
            "ORDER BY elapsed_time LIMIT ?");
        bind_text(stmt.get(), 1, simulation_id);
        bind_text(stmt.get(), 2, object_type);
        bind_text(stmt.get(), 3, object_id);
        bind_int(stmt.get(), 4, vid);
        bind_int(stmt.get(), 5, max_count);

        int n = 0;
        while (sqlite3_step(stmt.get()) == SQLITE_ROW && n < max_count) {
            times[n] = column_double(stmt.get(), 0);
            values[n] = column_double(stmt.get(), 1);
            ++n;
        }
        return n;
    } catch (const std::exception& e) { h->error_msg = e.what(); return SWMM_GPKG_ERR; }
}

// ---------- Read: Summary statistics ----------------------------------------

SWMM_ENGINE_API int swmm_gpkg_read_summary(SWMM_Gpkg gpkg,
                                             const char* simulation_id,
                                             const char* object_type,
                                             const char* object_id,
                                             const char* variable_name,
                                             double* value) {
    if (!gpkg || !simulation_id || !object_type || !object_id || !variable_name || !value)
        return SWMM_GPKG_ERR;
    auto* h = as_handle(gpkg);
    try {
        int vid = lookup_variable_id(h->db.get(), variable_name, object_type);
        if (vid < 0) return SWMM_GPKG_ERR;

        auto stmt = prepare(h->db.get(),
            "SELECT value FROM result_summary "
            "WHERE simulation_id = ? AND object_type = ? AND object_id = ? AND variable_id = ?");
        bind_text(stmt.get(), 1, simulation_id);
        bind_text(stmt.get(), 2, object_type);
        bind_text(stmt.get(), 3, object_id);
        bind_int(stmt.get(), 4, vid);
        if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            *value = column_double(stmt.get(), 0);
            return SWMM_GPKG_OK;
        }
        h->error_msg = "Summary value not found";
        return SWMM_GPKG_ERR;
    } catch (const std::exception& e) { h->error_msg = e.what(); return SWMM_GPKG_ERR; }
}

// ---------- Write: Observed data (the only writable section) ----------------

SWMM_ENGINE_API int swmm_gpkg_create_observed_series(SWMM_Gpkg gpkg,
                                                       const char* name,
                                                       const char* variable_name,
                                                       const char* object_type,
                                                       const char* object_id,
                                                       const char* source,
                                                       const char* units) {
    if (!gpkg || !name || !variable_name) return SWMM_GPKG_ERR;
    auto* h = as_handle(gpkg);
    try {
        std::string obj_type = object_type ? object_type : "NODE";
        int vid = lookup_variable_id(h->db.get(), variable_name, obj_type.c_str());
        if (vid < 0) {
            // Create a new variable entry for this observed measurement
            auto ins = prepare(h->db.get(),
                "INSERT INTO variables (name, object_type, category, units) VALUES (?, ?, 'STATE', ?)");
            bind_text(ins.get(), 1, variable_name);
            bind_text(ins.get(), 2, obj_type);
            bind_text(ins.get(), 3, safe_str(units));
            sqlite3_step(ins.get());
            vid = static_cast<int>(sqlite3_last_insert_rowid(h->db.get()));
        }

        auto stmt = prepare(h->db.get(),
            "INSERT INTO observed_series "
            "(name, variable_id, object_type, object_id, source, units) "
            "VALUES (?, ?, ?, ?, ?, ?)");
        bind_text(stmt.get(), 1, name);
        bind_int(stmt.get(), 2, vid);
        if (object_type) bind_text(stmt.get(), 3, object_type);
        else bind_null(stmt.get(), 3);
        if (object_id) bind_text(stmt.get(), 4, object_id);
        else bind_null(stmt.get(), 4);
        if (source) bind_text(stmt.get(), 5, source);
        else bind_null(stmt.get(), 5);
        if (units) bind_text(stmt.get(), 6, units);
        else bind_null(stmt.get(), 6);
        sqlite3_step(stmt.get());

        return static_cast<int>(sqlite3_last_insert_rowid(h->db.get()));
    } catch (const std::exception& e) { h->error_msg = e.what(); return SWMM_GPKG_ERR; }
}

SWMM_ENGINE_API int swmm_gpkg_write_observed_value(SWMM_Gpkg gpkg,
                                                     int series_id,
                                                     const char* timestamp,
                                                     double value,
                                                     const char* quality_flag) {
    if (!gpkg || !timestamp) return SWMM_GPKG_ERR;
    auto* h = as_handle(gpkg);
    try {
        auto stmt = prepare(h->db.get(),
            "INSERT INTO observed_values (series_id, timestamp, value, quality_flag) "
            "VALUES (?, ?, ?, ?)");
        bind_int(stmt.get(), 1, series_id);
        bind_text(stmt.get(), 2, timestamp);
        bind_double(stmt.get(), 3, value);
        if (quality_flag) bind_text(stmt.get(), 4, quality_flag);
        else bind_null(stmt.get(), 4);
        sqlite3_step(stmt.get());
        return SWMM_GPKG_OK;
    } catch (const std::exception& e) { h->error_msg = e.what(); return SWMM_GPKG_ERR; }
}

SWMM_ENGINE_API int swmm_gpkg_write_observed_values(SWMM_Gpkg gpkg,
                                                      int series_id,
                                                      const char** timestamps,
                                                      const double* values,
                                                      const char** quality_flags,
                                                      int count) {
    if (!gpkg || !timestamps || !values || count <= 0) return SWMM_GPKG_ERR;
    auto* h = as_handle(gpkg);
    try {
        auto stmt = prepare(h->db.get(),
            "INSERT INTO observed_values (series_id, timestamp, value, quality_flag) "
            "VALUES (?, ?, ?, ?)");

        for (int i = 0; i < count; ++i) {
            sqlite3_reset(stmt.get());
            sqlite3_clear_bindings(stmt.get());
            bind_int(stmt.get(), 1, series_id);
            bind_text(stmt.get(), 2, timestamps[i]);
            bind_double(stmt.get(), 3, values[i]);
            if (quality_flags && quality_flags[i])
                bind_text(stmt.get(), 4, quality_flags[i]);
            else
                bind_null(stmt.get(), 4);
            int rc = sqlite3_step(stmt.get());
            if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
                h->error_msg = sqlite3_errmsg(h->db.get());
                return SWMM_GPKG_ERR;
            }
        }
        return SWMM_GPKG_OK;
    } catch (const std::exception& e) { h->error_msg = e.what(); return SWMM_GPKG_ERR; }
}

// ---------- Read: Observed data ---------------------------------------------

SWMM_ENGINE_API int swmm_gpkg_observed_series_count(SWMM_Gpkg gpkg) {
    if (!gpkg) return SWMM_GPKG_ERR;
    try { return count_query(as_handle(gpkg)->db.get(), "SELECT count(*) FROM observed_series"); }
    catch (...) { return SWMM_GPKG_ERR; }
}

SWMM_ENGINE_API int swmm_gpkg_observed_value_count(SWMM_Gpkg gpkg, int series_id) {
    if (!gpkg) return SWMM_GPKG_ERR;
    auto* h = as_handle(gpkg);
    try {
        auto stmt = prepare(h->db.get(),
            "SELECT count(*) FROM observed_values WHERE series_id = ?");
        bind_int(stmt.get(), 1, series_id);
        if (sqlite3_step(stmt.get()) == SQLITE_ROW)
            return column_int(stmt.get(), 0);
        return 0;
    } catch (...) { return SWMM_GPKG_ERR; }
}

SWMM_ENGINE_API int swmm_gpkg_read_observed_values(SWMM_Gpkg gpkg,
                                                     int series_id,
                                                     char* timestamps,
                                                     int ts_buf_len,
                                                     double* values,
                                                     int max_count) {
    if (!gpkg || !values || max_count <= 0) return SWMM_GPKG_ERR;
    auto* h = as_handle(gpkg);
    try {
        auto stmt = prepare(h->db.get(),
            "SELECT timestamp, value FROM observed_values "
            "WHERE series_id = ? ORDER BY timestamp LIMIT ?");
        bind_int(stmt.get(), 1, series_id);
        bind_int(stmt.get(), 2, max_count);

        int n = 0;
        while (sqlite3_step(stmt.get()) == SQLITE_ROW && n < max_count) {
            if (timestamps && ts_buf_len > 0) {
                std::string ts = column_text(stmt.get(), 0);
                char* dst = timestamps + n * ts_buf_len;
                std::strncpy(dst, ts.c_str(), static_cast<size_t>(ts_buf_len - 1));
                dst[ts_buf_len - 1] = '\0';
            }
            values[n] = column_double(stmt.get(), 1);
            ++n;
        }
        return n;
    } catch (const std::exception& e) { h->error_msg = e.what(); return SWMM_GPKG_ERR; }
}

// ---------- Read: Ad-hoc queries --------------------------------------------

SWMM_ENGINE_API int swmm_gpkg_query_int(SWMM_Gpkg gpkg, const char* sql) {
    if (!gpkg || !sql) return SWMM_GPKG_ERR;
    auto* h = as_handle(gpkg);
    try {
        auto stmt = prepare(h->db.get(), sql);
        if (sqlite3_step(stmt.get()) == SQLITE_ROW)
            return column_int(stmt.get(), 0);
        return SWMM_GPKG_ERR;
    } catch (const std::exception& e) { h->error_msg = e.what(); return SWMM_GPKG_ERR; }
}

SWMM_ENGINE_API int swmm_gpkg_query_double(SWMM_Gpkg gpkg, const char* sql,
                                             double* result) {
    if (!gpkg || !sql || !result) return SWMM_GPKG_ERR;
    auto* h = as_handle(gpkg);
    try {
        auto stmt = prepare(h->db.get(), sql);
        if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            *result = column_double(stmt.get(), 0);
            return SWMM_GPKG_OK;
        }
        return SWMM_GPKG_ERR;
    } catch (const std::exception& e) { h->error_msg = e.what(); return SWMM_GPKG_ERR; }
}

} // extern "C"
