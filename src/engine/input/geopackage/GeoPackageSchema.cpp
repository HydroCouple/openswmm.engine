/**
 * @file GeoPackageSchema.cpp
 * @brief DDL implementation for the OpenSWMM GeoPackage schema.
 * @ingroup engine_geopackage
 */

#include "GeoPackageSchema.hpp"
#include "GpkgUtils.hpp"

namespace openswmm::gpkg {

// ============================================================================
// GeoPackage metadata tables (OGC standard)
// ============================================================================

static const char* GPKG_METADATA_DDL = R"SQL(
-- GeoPackage required metadata tables (OGC 12-128r18)
CREATE TABLE IF NOT EXISTS gpkg_spatial_ref_sys (
    srs_name                 TEXT NOT NULL,
    srs_id                   INTEGER PRIMARY KEY,
    organization             TEXT NOT NULL,
    organization_coordsys_id INTEGER NOT NULL,
    definition               TEXT NOT NULL,
    description              TEXT
);

-- Default SRS entries required by the GeoPackage standard
INSERT OR IGNORE INTO gpkg_spatial_ref_sys VALUES
    ('Undefined cartesian SRS', -1, 'NONE', -1, 'undefined', 'undefined cartesian coordinate reference system'),
    ('Undefined geographic SRS', 0, 'NONE', 0, 'undefined', 'undefined geographic coordinate reference system'),
    ('WGS 84 geodetic', 4326, 'EPSG', 4326,
     'GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]]',
     'longitude/latitude coordinates in decimal degrees on the WGS 84 spheroid');

CREATE TABLE IF NOT EXISTS gpkg_contents (
    table_name  TEXT NOT NULL PRIMARY KEY,
    data_type   TEXT NOT NULL,
    identifier  TEXT UNIQUE,
    description TEXT DEFAULT '',
    last_change TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
    min_x       DOUBLE,
    min_y       DOUBLE,
    max_x       DOUBLE,
    max_y       DOUBLE,
    srs_id      INTEGER,
    CONSTRAINT fk_gc_r_srs_id FOREIGN KEY (srs_id) REFERENCES gpkg_spatial_ref_sys(srs_id)
);

CREATE TABLE IF NOT EXISTS gpkg_geometry_columns (
    table_name         TEXT NOT NULL,
    column_name        TEXT NOT NULL,
    geometry_type_name TEXT NOT NULL,
    srs_id             INTEGER NOT NULL,
    z                  TINYINT NOT NULL,
    m                  TINYINT NOT NULL,
    CONSTRAINT pk_gc PRIMARY KEY (table_name, column_name),
    CONSTRAINT fk_gc_tn FOREIGN KEY (table_name) REFERENCES gpkg_contents(table_name),
    CONSTRAINT fk_gc_srs FOREIGN KEY (srs_id) REFERENCES gpkg_spatial_ref_sys(srs_id)
);
)SQL";

// ============================================================================
// Part A: Model Input tables
// ============================================================================

static const char* PART_A_DDL = R"SQL(
-- Options (key-value)
CREATE TABLE IF NOT EXISTS options (
    simulation_id  TEXT NOT NULL,
    key            TEXT NOT NULL,
    value          TEXT NOT NULL,
    PRIMARY KEY (simulation_id, key)
);

-- Nodes (POINT feature table)
CREATE TABLE IF NOT EXISTS nodes (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    node_id         TEXT NOT NULL,
    node_type       TEXT NOT NULL,
    geom            BLOB,
    invert_elev     REAL,
    max_depth       REAL,
    init_depth      REAL,
    surcharge_depth REAL,
    ponded_area     REAL,
    outfall_type    TEXT,
    outfall_stage   REAL,
    outfall_has_flap_gate INTEGER DEFAULT 0,
    divider_type    TEXT,
    divider_cutoff  REAL,
    divider_curve   TEXT,
    storage_curve   TEXT,
    storage_a       REAL,
    storage_b       REAL,
    storage_c       REAL,
    tag             TEXT,
    UNIQUE(simulation_id, node_id)
);

-- Links (LINESTRING feature table)
CREATE TABLE IF NOT EXISTS links (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    link_id         TEXT NOT NULL,
    link_type       TEXT NOT NULL,
    geom            BLOB,
    from_node       TEXT NOT NULL,
    to_node         TEXT NOT NULL,
    offset1         REAL,
    offset2         REAL,
    xsect_shape     TEXT,
    xsect_geom1     REAL,
    xsect_geom2     REAL,
    xsect_geom3     REAL,
    xsect_geom4     REAL,
    xsect_barrels   INTEGER,
    xsect_culvert   INTEGER,
    xsect_curve     TEXT,
    roughness       REAL,
    length          REAL,
    loss_inlet      REAL,
    loss_outlet     REAL,
    loss_avg        REAL,
    has_flap_gate   INTEGER DEFAULT 0,
    seep_rate       REAL,
    q0              REAL,
    q_limit         REAL,
    pump_curve      TEXT,
    pump_init_state REAL,
    pump_startup    REAL,
    pump_shutoff    REAL,
    crest_height    REAL,
    discharge_coeff REAL,
    end_contractions INTEGER,
    can_surcharge   INTEGER DEFAULT 0,
    tag             TEXT,
    UNIQUE(simulation_id, link_id)
);

-- Subcatchments (MULTIPOLYGON feature table)
CREATE TABLE IF NOT EXISTS subcatchments (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    subcatch_id     TEXT NOT NULL,
    geom            BLOB,
    outlet_node     TEXT,
    outlet_subcatch TEXT,
    rain_gage       TEXT,
    area            REAL,
    width           REAL,
    slope           REAL,
    curb_length     REAL,
    frac_imperv     REAL,
    n_imperv        REAL,
    n_perv          REAL,
    ds_imperv       REAL,
    ds_perv         REAL,
    pct_zero_imperv REAL,
    subarea_routing TEXT,
    pct_routed      REAL,
    infil_model     TEXT,
    infil_p1        REAL,
    infil_p2        REAL,
    infil_p3        REAL,
    infil_p4        REAL,
    infil_p5        REAL,
    tag             TEXT,
    UNIQUE(simulation_id, subcatch_id)
);

-- Rain gages (POINT feature table)
CREATE TABLE IF NOT EXISTS rain_gages (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    gage_id         TEXT NOT NULL,
    geom            BLOB,
    rain_type       TEXT,
    rain_interval   TEXT,
    snow_catch      REAL,
    data_source     TEXT,
    source_name     TEXT,
    station_id      TEXT,
    rain_units      TEXT,
    UNIQUE(simulation_id, gage_id)
);

-- Network topology: node-link connectivity
CREATE TABLE IF NOT EXISTS node_links (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    link_id         TEXT NOT NULL,
    from_node       TEXT NOT NULL,
    to_node         TEXT NOT NULL,
    link_type       TEXT NOT NULL,
    direction       INTEGER NOT NULL DEFAULT 1,
    UNIQUE(simulation_id, link_id)
);
CREATE INDEX IF NOT EXISTS idx_node_links_from ON node_links(simulation_id, from_node);
CREATE INDEX IF NOT EXISTS idx_node_links_to   ON node_links(simulation_id, to_node);

-- Network topology: subcatchment routing
CREATE TABLE IF NOT EXISTS subcatch_routing (
    fid              INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id    TEXT NOT NULL,
    subcatch_id      TEXT NOT NULL,
    outlet_type      TEXT NOT NULL,
    outlet_node      TEXT,
    outlet_subcatch  TEXT,
    UNIQUE(simulation_id, subcatch_id)
);

-- Curves
CREATE TABLE IF NOT EXISTS curves (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    curve_id        TEXT NOT NULL,
    curve_type      TEXT NOT NULL,
    x_value         REAL NOT NULL,
    y_value         REAL NOT NULL,
    ordinal         INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_curves_lookup ON curves(simulation_id, curve_id, ordinal);

-- Input timeseries
CREATE TABLE IF NOT EXISTS input_timeseries (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    series_id       TEXT NOT NULL,
    timestamp       TEXT NOT NULL,
    value           REAL NOT NULL,
    ordinal         INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_input_ts_lookup ON input_timeseries(simulation_id, series_id, ordinal);

-- Patterns
CREATE TABLE IF NOT EXISTS patterns (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    pattern_id      TEXT NOT NULL,
    pattern_type    TEXT NOT NULL,
    ordinal         INTEGER NOT NULL,
    factor          REAL NOT NULL
);

-- Evaporation settings
CREATE TABLE IF NOT EXISTS evaporation (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    evap_type       TEXT NOT NULL,
    evap_values     TEXT,
    ts_name         TEXT,
    pan_coeff       TEXT,
    recovery_pat    TEXT,
    dry_only        INTEGER DEFAULT 0,
    UNIQUE(simulation_id)
);

-- Temperature and climate settings
CREATE TABLE IF NOT EXISTS climate_settings (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    temp_source     TEXT NOT NULL DEFAULT 'NONE',
    temp_ts_name    TEXT,
    temp_file       TEXT,
    temp_file_start REAL,
    wind_type       TEXT NOT NULL DEFAULT 'MONTHLY',
    wind_speed      TEXT,
    snow_divt       REAL DEFAULT 34.0,
    snow_ati_wt     REAL DEFAULT 0.5,
    snow_nrg_ratio  REAL DEFAULT 0.6,
    snow_lat        REAL DEFAULT 0.0,
    snow_min_melt   REAL DEFAULT 0.0,
    snow_max_melt   REAL DEFAULT 0.0,
    adc_imperv      TEXT,
    adc_perv        TEXT,
    UNIQUE(simulation_id)
);

-- Snowpack definitions (one row per snowpack-surface combination)
CREATE TABLE IF NOT EXISTS snowpacks (
    fid              INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id    TEXT NOT NULL,
    snowpack_id      TEXT NOT NULL,
    surface_type     TEXT NOT NULL,
    p1 REAL, p2 REAL, p3 REAL, p4 REAL,
    p5 REAL, p6 REAL, p7 REAL,
    removal_subcatch TEXT,
    UNIQUE(simulation_id, snowpack_id, surface_type)
);

-- Monthly climate adjustments
CREATE TABLE IF NOT EXISTS adjustments (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    adjust_type     TEXT NOT NULL,
    adj_values      TEXT NOT NULL,
    UNIQUE(simulation_id, adjust_type)
);

-- Subcatchment pattern adjustments
CREATE TABLE IF NOT EXISTS subcatch_adjustments (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    subcatch_id     TEXT NOT NULL,
    adjust_type     TEXT NOT NULL,
    pattern_id      TEXT NOT NULL,
    UNIQUE(simulation_id, subcatch_id, adjust_type)
);

-- Pollutants
CREATE TABLE IF NOT EXISTS pollutants (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    pollutant_id    TEXT NOT NULL,
    units           TEXT NOT NULL,
    rain_conc       REAL,
    gw_conc         REAL,
    ii_conc         REAL,
    decay_coeff     REAL,
    snow_only       INTEGER,
    co_pollutant    TEXT,
    co_fraction     REAL,
    UNIQUE(simulation_id, pollutant_id)
);

-- LID control definitions (one row per lid-layer combination)
CREATE TABLE IF NOT EXISTS lid_controls (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    lid_id          TEXT NOT NULL,
    layer_type      TEXT NOT NULL,
    p1 REAL, p2 REAL, p3 REAL, p4 REAL,
    p5 REAL, p6 REAL, p7 REAL,
    UNIQUE(simulation_id, lid_id, layer_type)
);

-- LID usage assignments (one row per subcatchment-LID pair)
CREATE TABLE IF NOT EXISTS lid_usage (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    subcatch_id     TEXT NOT NULL,
    lid_id          TEXT NOT NULL,
    number          INTEGER,
    area            REAL,
    width           REAL,
    init_sat        REAL,
    from_imperv     REAL,
    to_perv         INTEGER DEFAULT 0,
    rpt_file        TEXT,
    drain_to        TEXT,
    from_perv       REAL DEFAULT 0.0,
    UNIQUE(simulation_id, subcatch_id, lid_id)
);

-- RDII assignments (one row per node-UH pair)
CREATE TABLE IF NOT EXISTS rdii_assignments (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    node_name       TEXT NOT NULL,
    uh_name         TEXT NOT NULL,
    sewer_area      REAL NOT NULL,
    UNIQUE(simulation_id, node_name, uh_name)
);

-- Unit hydrograph definitions (gage lines + parameter lines)
CREATE TABLE IF NOT EXISTS unit_hydrographs (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    uh_name         TEXT NOT NULL,
    gage_name       TEXT,
    month           TEXT,
    response        TEXT,
    r               REAL,
    t               REAL,
    k               REAL,
    dmax            REAL DEFAULT 0,
    drecov          REAL DEFAULT 0,
    dinit           REAL DEFAULT 0
);

-- Treatment expressions (one row per node-pollutant pair)
CREATE TABLE IF NOT EXISTS treatment (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    node_id         TEXT NOT NULL,
    pollutant_id    TEXT NOT NULL,
    expression      TEXT NOT NULL,
    UNIQUE(simulation_id, node_id, pollutant_id)
);

-- Transects
CREATE TABLE IF NOT EXISTS transects (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    transect_id     TEXT NOT NULL,
    station         REAL NOT NULL,
    elevation       REAL NOT NULL,
    ordinal         INTEGER NOT NULL,
    n_left          REAL,
    n_right         REAL,
    n_channel       REAL,
    left_overbank   REAL,
    right_overbank  REAL
);
)SQL";

// ============================================================================
// Part B: Simulation Results & Reports
// ============================================================================

static const char* PART_B_DDL = R"SQL(
-- Simulation run registry
CREATE TABLE IF NOT EXISTS simulations (
    simulation_id              TEXT PRIMARY KEY,
    name                       TEXT NOT NULL,
    description                TEXT,
    created_at                 TEXT NOT NULL,
    engine_version             TEXT NOT NULL,
    engine_build               TEXT,
    start_date                 TEXT,
    end_date                   TEXT,
    report_step                REAL,
    wet_step                   REAL,
    dry_step                   REAL,
    routing_step               REAL,
    routing_model              TEXT,
    inp_hash                   TEXT,
    status                     TEXT DEFAULT 'created',
    elapsed_wall_time          REAL,
    continuity_error_runoff    REAL,
    continuity_error_flow      REAL,
    continuity_error_quality   REAL
);

-- Variable catalog
CREATE TABLE IF NOT EXISTS variables (
    variable_id    INTEGER PRIMARY KEY AUTOINCREMENT,
    name           TEXT NOT NULL,
    object_type    TEXT NOT NULL,
    category       TEXT NOT NULL,
    units          TEXT,
    description    TEXT,
    UNIQUE(name, object_type)
);

-- Result timeseries (per-timestep)
CREATE TABLE IF NOT EXISTS result_timeseries (
    simulation_id  TEXT NOT NULL,
    object_type    TEXT NOT NULL,
    object_id      TEXT NOT NULL,
    variable_id    INTEGER NOT NULL,
    elapsed_time   REAL NOT NULL,
    value          REAL NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_result_ts_obj
    ON result_timeseries(simulation_id, object_type, object_id, variable_id, elapsed_time);

-- Summary statistics (per-object, written once at end)
CREATE TABLE IF NOT EXISTS result_summary (
    simulation_id  TEXT NOT NULL,
    object_type    TEXT NOT NULL,
    object_id      TEXT NOT NULL,
    variable_id    INTEGER NOT NULL,
    value          REAL NOT NULL,
    PRIMARY KEY (simulation_id, object_type, object_id, variable_id)
);
)SQL";

// ============================================================================
// Part C: Observed / Sensor Data
// ============================================================================

static const char* PART_C_DDL = R"SQL(
CREATE TABLE IF NOT EXISTS observed_series (
    series_id         INTEGER PRIMARY KEY AUTOINCREMENT,
    name              TEXT NOT NULL UNIQUE,
    description       TEXT,
    source            TEXT,
    source_id         TEXT,
    variable_id       INTEGER NOT NULL,
    object_type       TEXT,
    object_id         TEXT,
    units             TEXT,
    time_zone         TEXT,
    collection_method TEXT,
    start_date        TEXT,
    end_date          TEXT,
    record_count      INTEGER
);

CREATE TABLE IF NOT EXISTS observed_values (
    series_id      INTEGER NOT NULL,
    timestamp      TEXT NOT NULL,
    value          REAL NOT NULL,
    quality_flag   TEXT,
    qualifier      TEXT
);
CREATE INDEX IF NOT EXISTS idx_obs_values_lookup
    ON observed_values(series_id, timestamp);
)SQL";

// ============================================================================
// Implementation
// ============================================================================

void create_schema(sqlite3* db) {
    exec(db, "PRAGMA journal_mode=WAL");
    exec(db, "PRAGMA foreign_keys=ON");
    exec(db, "PRAGMA application_id=0x47504B47"); // 'GPKG'

    exec(db, GPKG_METADATA_DDL);
    exec(db, PART_A_DDL);
    exec(db, PART_B_DDL);
    exec(db, PART_C_DDL);
}

void register_crs(sqlite3* db, int srs_id, const std::string& org,
                  int org_id, const std::string& srs_name, const std::string& wkt) {
    auto stmt = prepare(db,
        "INSERT OR IGNORE INTO gpkg_spatial_ref_sys "
        "(srs_name, srs_id, organization, organization_coordsys_id, definition) "
        "VALUES (?, ?, ?, ?, ?)");
    bind_text(stmt.get(), 1, srs_name);
    bind_int(stmt.get(), 2, srs_id);
    bind_text(stmt.get(), 3, org);
    bind_int(stmt.get(), 4, org_id);
    bind_text(stmt.get(), 5, wkt);
    sqlite3_step(stmt.get());
}

void register_feature_table(sqlite3* db, const std::string& table_name,
                            const std::string& geom_type, int srs_id,
                            const std::string& identifier, const std::string& description,
                            double min_x, double min_y, double max_x, double max_y) {
    // gpkg_contents
    auto stmt = prepare(db,
        "INSERT OR REPLACE INTO gpkg_contents "
        "(table_name, data_type, identifier, description, min_x, min_y, max_x, max_y, srs_id) "
        "VALUES (?, 'features', ?, ?, ?, ?, ?, ?, ?)");
    bind_text(stmt.get(), 1, table_name);
    bind_text(stmt.get(), 2, identifier);
    bind_text(stmt.get(), 3, description);
    bind_double(stmt.get(), 4, min_x);
    bind_double(stmt.get(), 5, min_y);
    bind_double(stmt.get(), 6, max_x);
    bind_double(stmt.get(), 7, max_y);
    bind_int(stmt.get(), 8, srs_id);
    sqlite3_step(stmt.get());

    // gpkg_geometry_columns
    auto stmt2 = prepare(db,
        "INSERT OR REPLACE INTO gpkg_geometry_columns "
        "(table_name, column_name, geometry_type_name, srs_id, z, m) "
        "VALUES (?, 'geom', ?, ?, 0, 0)");
    bind_text(stmt2.get(), 1, table_name);
    bind_text(stmt2.get(), 2, geom_type);
    bind_int(stmt2.get(), 3, srs_id);
    sqlite3_step(stmt2.get());
}

void populate_default_variables(sqlite3* db) {
    auto stmt = prepare(db,
        "INSERT OR IGNORE INTO variables (name, object_type, category, units, description) "
        "VALUES (?, ?, ?, ?, ?)");

    struct VarDef { const char *name, *obj, *cat, *units, *desc; };
    static const VarDef defs[] = {
        // --- NODE ---
        {"depth",           "NODE", "STATE", "m",    "Water depth above invert"},
        {"head",            "NODE", "STATE", "m",    "Hydraulic head"},
        {"volume",          "NODE", "STATE", "m3",   "Stored water volume"},
        {"lateral_inflow",  "NODE", "STATE", "CMS",  "Lateral inflow rate"},
        {"total_inflow",    "NODE", "STATE", "CMS",  "Total inflow rate"},
        {"overflow",        "NODE", "STATE", "CMS",  "Overflow rate"},
        {"max_depth",       "NODE", "STAT",  "m",    "Maximum water depth"},
        {"max_overflow",    "NODE", "STAT",  "CMS",  "Maximum overflow rate"},
        {"time_flooded",    "NODE", "STAT",  "hours","Duration of flooding"},
        // --- LINK ---
        {"flow",            "LINK", "STATE", "CMS",  "Flow rate"},
        {"depth",           "LINK", "STATE", "m",    "Flow depth"},
        {"velocity",        "LINK", "STATE", "m/s",  "Flow velocity"},
        {"volume",          "LINK", "STATE", "m3",   "Stored volume"},
        {"capacity",        "LINK", "STATE", "-",    "Full-flow capacity fraction"},
        {"froude",          "LINK", "STATE", "-",    "Froude number"},
        {"max_flow",        "LINK", "STAT",  "CMS",  "Maximum flow rate"},
        {"max_velocity",    "LINK", "STAT",  "m/s",  "Maximum flow velocity"},
        {"max_filling",     "LINK", "STAT",  "frac", "Maximum depth/full depth"},
        {"time_surcharged", "LINK", "STAT",  "hours","Duration of surcharge"},
        // --- SUBCATCH ---
        {"rainfall",        "SUBCATCH", "STATE", "mm/hr", "Rainfall intensity"},
        {"snow_depth",      "SUBCATCH", "STATE", "mm",    "Snow depth"},
        {"evap_loss",       "SUBCATCH", "STATE", "mm",    "Evaporation loss"},
        {"infil_loss",      "SUBCATCH", "STATE", "mm",    "Infiltration loss"},
        {"runoff",          "SUBCATCH", "STATE", "CMS",   "Runoff flow rate"},
        {"gw_flow",         "SUBCATCH", "STATE", "CMS",   "Groundwater outflow"},
        {"gw_elev",         "SUBCATCH", "STATE", "m",     "Groundwater elevation"},
        {"soil_moist",      "SUBCATCH", "STATE", "-",     "Soil moisture"},
        {"precip_volume",   "SUBCATCH", "STAT",  "m3",    "Total precipitation volume"},
        {"runoff_volume",   "SUBCATCH", "STAT",  "m3",    "Total runoff volume"},
        // --- SYSTEM ---
        {"air_temp",        "SYSTEM", "CLIMATE", "C",     "Air temperature"},
        {"rainfall",        "SYSTEM", "CLIMATE", "mm/hr", "System rainfall"},
        {"snow_depth",      "SYSTEM", "CLIMATE", "mm",    "System snow depth"},
        {"infil",           "SYSTEM", "STATE",   "mm/hr", "Total infiltration rate"},
        {"runoff",          "SYSTEM", "STATE",   "CMS",   "Total runoff flow"},
        {"dw_inflow",       "SYSTEM", "STATE",   "CMS",   "Total dry weather inflow"},
        {"gw_inflow",       "SYSTEM", "STATE",   "CMS",   "Total groundwater inflow"},
        {"ii_inflow",       "SYSTEM", "STATE",   "CMS",   "Total RDII inflow"},
        {"ext_inflow",      "SYSTEM", "STATE",   "CMS",   "Total external inflow"},
        {"total_inflow",    "SYSTEM", "STATE",   "CMS",   "Total lateral inflow"},
        {"flooding",        "SYSTEM", "STATE",   "CMS",   "Total flooding"},
        {"outflow",         "SYSTEM", "STATE",   "CMS",   "Total outflow"},
        {"storage",         "SYSTEM", "STATE",   "m3",    "Total storage volume"},
        {"evap",            "SYSTEM", "STATE",   "mm/day","Total evaporation rate"},
        {"pet",             "SYSTEM", "STATE",   "mm/day","Potential evapotranspiration"},
    };

    for (const auto& v : defs) {
        sqlite3_reset(stmt.get());
        sqlite3_clear_bindings(stmt.get());
        bind_text(stmt.get(), 1, v.name);
        bind_text(stmt.get(), 2, v.obj);
        bind_text(stmt.get(), 3, v.cat);
        bind_text(stmt.get(), 4, v.units);
        bind_text(stmt.get(), 5, v.desc);
        sqlite3_step(stmt.get());
    }
}

} // namespace openswmm::gpkg
