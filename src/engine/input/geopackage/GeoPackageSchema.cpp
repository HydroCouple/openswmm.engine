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

-- Nodes (POINT feature table) — RELATIONAL: common columns + discriminator +
-- geometry only. Subtype properties live in the storages/outfalls/dividers
-- child tables below (1:1 specialization, FK on (simulation_id, node_id)).
-- nodes keeps the GeoPackage-required INTEGER PRIMARY KEY (fid); the child FK
-- targets the UNIQUE(simulation_id, node_id) key.
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
    tag             TEXT,
    UNIQUE(simulation_id, node_id)
);

-- Storage units (1:1 with a STORAGE node). curve_name set => tabulated;
-- otherwise functional A·d^B + C. Values are canonical internal units (the
-- whole .gpkg is internal-unit; see read path). Lossless side-table mirror.
CREATE TABLE IF NOT EXISTS storages (
    simulation_id   TEXT NOT NULL,
    node_id         TEXT NOT NULL,
    curve_name      TEXT,
    a               REAL,
    b               REAL,
    c               REAL,
    seep_rate       REAL,
    evap_frac       REAL,
    exfil_suction   REAL,
    exfil_ksat      REAL,
    exfil_imd       REAL,
    PRIMARY KEY (simulation_id, node_id),
    FOREIGN KEY (simulation_id, node_id)
        REFERENCES nodes(simulation_id, node_id)
        ON DELETE CASCADE ON UPDATE CASCADE
);

-- Outfalls (1:1 with an OUTFALL node). outfall_type = FREE/NORMAL/FIXED/TIDAL/
-- TIMESERIES; param = fixed stage or tidal/timeseries reference; route_to =
-- subcatchment name (deferred-resolved on read, like the .inp path).
CREATE TABLE IF NOT EXISTS outfalls (
    simulation_id   TEXT NOT NULL,
    node_id         TEXT NOT NULL,
    outfall_type    TEXT,
    param           REAL,
    has_flap_gate   INTEGER DEFAULT 0,
    route_to        TEXT,
    PRIMARY KEY (simulation_id, node_id),
    FOREIGN KEY (simulation_id, node_id)
        REFERENCES nodes(simulation_id, node_id)
        ON DELETE CASCADE ON UPDATE CASCADE
);

-- Dividers (1:1 with a DIVIDER node). divider_type = CUTOFF/OVERFLOW/TABULAR/
-- WEIR; curve_name (TABULAR) and divider_link are names (deferred-resolved).
CREATE TABLE IF NOT EXISTS dividers (
    simulation_id   TEXT NOT NULL,
    node_id         TEXT NOT NULL,
    divider_type    TEXT,
    cutoff          REAL,
    cd              REAL,
    max_depth       REAL,
    curve_name      TEXT,
    divider_link    TEXT,
    PRIMARY KEY (simulation_id, node_id),
    FOREIGN KEY (simulation_id, node_id)
        REFERENCES nodes(simulation_id, node_id)
        ON DELETE CASCADE ON UPDATE CASCADE
);

-- Links (LINESTRING feature table). Phase 7: slim relational base — common
-- fields + discriminator + geom. Subtype properties live in the
-- conduits/pumps/orifices/weirs/outlets child tables (1:1 specialization,
-- FK on (simulation_id, link_id), the link analogue of the node child tables).
-- xsect_*/has_flap_gate stay on the base table because they are SHARED by
-- conduit/orifice/weir (mirroring LinkData, where they stay on the base SoA
-- rather than any one LinkSubtypes side-table); pumps/outlets leave them NULL.
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
    q0              REAL,
    q_limit         REAL,
    direction       INTEGER DEFAULT 1,  -- +1 node1->node2, -1 reversed (adverse-slope DW reverse)
    xsect_shape     TEXT,
    xsect_geom1     REAL,
    xsect_geom2     REAL,
    xsect_geom3     REAL,
    xsect_geom4     REAL,
    xsect_curve     TEXT,    -- IRREGULAR/STREET/CUSTOM transect/street/shape-curve NAME
    has_flap_gate   INTEGER DEFAULT 0,
    tag             TEXT,
    UNIQUE(simulation_id, link_id)
);

-- Conduits (1:1 with a CONDUIT link). Mirrors ConduitData input fields; the
-- init-derived geometry (beta/q_full/q_max/slope/mod_length) is recomputed on
-- read by Router::init and intentionally NOT persisted.
CREATE TABLE IF NOT EXISTS conduits (
    simulation_id   TEXT NOT NULL,
    link_id         TEXT NOT NULL,
    roughness       REAL,
    length          REAL,
    xsect_barrels   INTEGER,
    xsect_culvert   INTEGER,
    loss_inlet      REAL,
    loss_outlet     REAL,
    loss_avg        REAL,
    seep_rate       REAL,
    PRIMARY KEY (simulation_id, link_id),
    FOREIGN KEY (simulation_id, link_id)
        REFERENCES links(simulation_id, link_id)
        ON DELETE CASCADE ON UPDATE CASCADE
);

-- Pumps (1:1 with a PUMP link). pump_curve is the curve NAME (deferred-resolved
-- on read); init_state/startup/shutoff are ctx-native. curve_type is derived at
-- init from the curve's table type and not persisted.
CREATE TABLE IF NOT EXISTS pumps (
    simulation_id   TEXT NOT NULL,
    link_id         TEXT NOT NULL,
    pump_curve      TEXT,
    init_state      REAL,
    startup_depth   REAL,
    shutoff_depth   REAL,
    PRIMARY KEY (simulation_id, link_id),
    FOREIGN KEY (simulation_id, link_id)
        REFERENCES links(simulation_id, link_id)
        ON DELETE CASCADE ON UPDATE CASCADE
);

-- Orifices (1:1 with an ORIFICE link). orientation = SIDE|BOTTOM (was param1);
-- the sill height is the base offset1.
CREATE TABLE IF NOT EXISTS orifices (
    simulation_id   TEXT NOT NULL,
    link_id         TEXT NOT NULL,
    orientation     TEXT,                -- SIDE | BOTTOM
    discharge_coeff REAL,
    orate           REAL,                -- open/close rate (s)
    PRIMARY KEY (simulation_id, link_id),
    FOREIGN KEY (simulation_id, link_id)
        REFERENCES links(simulation_id, link_id)
        ON DELETE CASCADE ON UPDATE CASCADE
);

-- Weirs (1:1 with a WEIR link). weir_type = TRANSVERSE|SIDEFLOW|V-NOTCH|
-- TRAPEZOIDAL|ROADWAY (was param1); end_contractions was param2.
CREATE TABLE IF NOT EXISTS weirs (
    simulation_id    TEXT NOT NULL,
    link_id          TEXT NOT NULL,
    weir_type        TEXT,
    discharge_coeff  REAL,
    crest_height     REAL,
    end_contractions INTEGER,
    PRIMARY KEY (simulation_id, link_id),
    FOREIGN KEY (simulation_id, link_id)
        REFERENCES links(simulation_id, link_id)
        ON DELETE CASCADE ON UPDATE CASCADE
);

-- Outlets (1:1 with an OUTLET link). rating_type = FUNCTIONAL/HEAD,
-- FUNCTIONAL/DEPTH, TABULAR/HEAD, TABULAR/DEPTH (was param1). rating_curve is the
-- TABULAR curve NAME; q_coeff/q_expon are the FUNCTIONAL C1/C2.
CREATE TABLE IF NOT EXISTS outlets (
    simulation_id   TEXT NOT NULL,
    link_id         TEXT NOT NULL,
    rating_type     TEXT,
    rating_curve    TEXT,
    q_coeff         REAL,
    q_expon         REAL,
    crest_height    REAL,
    PRIMARY KEY (simulation_id, link_id),
    FOREIGN KEY (simulation_id, link_id)
        REFERENCES links(simulation_id, link_id)
        ON DELETE CASCADE ON UPDATE CASCADE
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

-- Input timeseries (Slice IO-5: provenance columns track whether each
-- row was authored inline in [TIMESERIES] or imported from a FILE
-- reference; the GUI uses the column trio to label rows as such).
CREATE TABLE IF NOT EXISTS input_timeseries (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    series_id       TEXT NOT NULL,
    timestamp       TEXT NOT NULL,
    value           REAL NOT NULL,
    ordinal         INTEGER NOT NULL,
    source          TEXT NOT NULL DEFAULT 'inline',  -- 'inline' | 'imported_from_file'
    source_filename TEXT,
    source_column   TEXT
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

-- External inflows ([INFLOWS]) — one row per (node, constituent). constituent
-- is 'FLOW' or a pollutant name; timeseries/pattern hold names (resolved to
-- indices at solver init, like the .inp path). Hard FK to nodes for cascade.
CREATE TABLE IF NOT EXISTS inflows (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    node_id         TEXT NOT NULL,
    constituent     TEXT NOT NULL,
    timeseries      TEXT,
    inflow_type     TEXT,
    m_factor        REAL DEFAULT 1.0,
    s_factor        REAL DEFAULT 1.0,
    baseline        REAL DEFAULT 0.0,
    pattern         TEXT,
    UNIQUE(simulation_id, node_id, constituent),
    FOREIGN KEY (simulation_id, node_id)
        REFERENCES nodes(simulation_id, node_id)
        ON DELETE CASCADE ON UPDATE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_inflows_node ON inflows(simulation_id, node_id);

-- Dry-weather flow ([DWF]) — one row per (node, constituent); up to 4 pattern
-- names (monthly/daily/hourly/weekend). avg_value is ctx-native (not in the
-- unit-convert set). Hard FK to nodes for cascade.
CREATE TABLE IF NOT EXISTS dwf_inflows (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    node_id         TEXT NOT NULL,
    constituent     TEXT NOT NULL,
    avg_value       REAL DEFAULT 0.0,
    pat1            TEXT,
    pat2            TEXT,
    pat3            TEXT,
    pat4            TEXT,
    UNIQUE(simulation_id, node_id, constituent),
    FOREIGN KEY (simulation_id, node_id)
        REFERENCES nodes(simulation_id, node_id)
        ON DELETE CASCADE ON UPDATE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_dwf_node ON dwf_inflows(simulation_id, node_id);

-- Control rules ([CONTROLS]) — one row per rule; rule_text is the full
-- multi-line "RULE/IF/THEN/ELSE/PRIORITY" block, stored verbatim (names +
-- setpoints resolved by the control engine, identically to the .inp path).
-- ordinal preserves rule order.
CREATE TABLE IF NOT EXISTS control_rules (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    ordinal         INTEGER NOT NULL,
    rule_text       TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_control_rules ON control_rules(simulation_id, ordinal);

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
    temp_units      INTEGER DEFAULT -1,
    wind_type       TEXT NOT NULL DEFAULT 'MONTHLY',
    wind_speed      TEXT,
    snow_divt       REAL DEFAULT 34.0,
    snow_ati_wt     REAL DEFAULT 0.5,
    snow_nrg_ratio  REAL DEFAULT 0.6,
    snow_lat        REAL DEFAULT 0.0,
    snow_min_melt   REAL DEFAULT 0.0,
    snow_max_melt   REAL DEFAULT 0.0,
    snow_elev       REAL DEFAULT 0.0,
    snow_dtlong     REAL DEFAULT 0.0,
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

-- RDII exponential-decay IA parameters (one row per UH group x response).
-- @see docs/RDII_ExpDecay_Implementation.md
CREATE TABLE IF NOT EXISTS rdii_decay (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    uh_name         TEXT NOT NULL,
    response        TEXT NOT NULL,   -- 'SHORT' | 'MEDIUM' | 'LONG'
    k_dep           REAL NOT NULL,
    k_0             REAL NOT NULL,
    k_T             REAL NOT NULL,
    T_ref           REAL NOT NULL,
    theta_rec       REAL NOT NULL,
    T_freeze        REAL NOT NULL,
    UNIQUE(simulation_id, uh_name, response)
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

-- Transects ([TRANSECTS]) — one row per station/elevation point (ordinal); the
-- per-transect scalars (Manning n, bank stations, encroachments, X/Y/length
-- factors, comment) repeat on every row of a transect. Station/elevation are in
-- display units (ctx-native; not in the unit-convert set). The reader rebuilds
-- ctx.transects and resolve_cross_references derives the geometry tables.
CREATE TABLE IF NOT EXISTS transects (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    transect_id     TEXT NOT NULL,
    ordinal         INTEGER NOT NULL,
    station         REAL NOT NULL,
    elevation       REAL NOT NULL,
    n_left          REAL,
    n_right         REAL,
    n_channel       REAL,
    x_left_bank     REAL,
    x_right_bank    REAL,
    x_left_encroach REAL,
    x_right_encroach REAL,
    x_factor        REAL,
    y_factor        REAL,
    length_factor   REAL,
    comment         TEXT
);
CREATE INDEX IF NOT EXISTS idx_transects_lookup ON transects(simulation_id, transect_id, ordinal);
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
// Part D: External-File Content (Slice IO-5)
//
// Replaces opaque BLOB-style storage with structured editable rows for
// every legacy SWMM external file type (hotstart, raingage data, climate
// observations, routing-interface flows). Every row carries composite
// foreign keys into the model objects already in the schema so:
//   • Deleting a `simulation_id` cascades all related content.
//   • Renaming or deleting a node/link/subcatchment/gage/pollutant
//     propagates through every dependent row — no orphan rows survive
//     the rename.
//   • Inserting a row that references a non-existent parent is rejected
//     when `PRAGMA foreign_keys=ON` (set by both `create_schema` and
//     `GpkgUtils::open_database`).
//
// See openswmm.gui/docs/IO_PORTABILITY_PLAN.md §3.4 for the full
// rationale (in particular §3.4.1 covers the three-way pollutant-state
// split that lets each pollutant row carry its own owning-object FK).
// ============================================================================

static const char* PART_D_DDL = R"SQL(
-- ----------------------------------------------------------------------------
-- Hot-start state (replaces opaque .hsf snapshots).
-- ----------------------------------------------------------------------------

-- One row per slot. Carries the legacy HSF header metadata and acts as the
-- parent every per-object state row FKs into.
CREATE TABLE IF NOT EXISTS hotstart_slots (
    simulation_id   TEXT NOT NULL,
    slot_name       TEXT NOT NULL,                       -- 'use' or 'save_<index>'
    direction       TEXT NOT NULL,                       -- 'USE' | 'SAVE'
    save_datetime   REAL,                                -- nullable; 0 = end-of-run
    format_version  INTEGER NOT NULL,
    flow_units      TEXT,
    num_pollutants  INTEGER NOT NULL,
    captured_at     TEXT,                                -- ISO8601; NULL until populated
    status          TEXT NOT NULL DEFAULT 'pending',     -- 'pending' | 'populated'
    PRIMARY KEY (simulation_id, slot_name),
    FOREIGN KEY (simulation_id)
        REFERENCES simulations(simulation_id)
        ON DELETE CASCADE ON UPDATE CASCADE
);

-- Per-node routing state.
CREATE TABLE IF NOT EXISTS hotstart_node_state (
    simulation_id   TEXT NOT NULL,
    slot_name       TEXT NOT NULL,
    node_id         TEXT NOT NULL,
    depth           REAL NOT NULL,
    lateral_inflow  REAL,
    overflow        REAL,
    PRIMARY KEY (simulation_id, slot_name, node_id),
    FOREIGN KEY (simulation_id, slot_name)
        REFERENCES hotstart_slots(simulation_id, slot_name)
        ON DELETE CASCADE ON UPDATE CASCADE,
    FOREIGN KEY (simulation_id, node_id)
        REFERENCES nodes(simulation_id, node_id)
        ON DELETE CASCADE ON UPDATE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_hotstart_node_state_lookup
    ON hotstart_node_state(simulation_id, slot_name, node_id);

-- Per-link routing state.
CREATE TABLE IF NOT EXISTS hotstart_link_state (
    simulation_id   TEXT NOT NULL,
    slot_name       TEXT NOT NULL,
    link_id         TEXT NOT NULL,
    flow            REAL NOT NULL,
    depth           REAL,
    volume          REAL,
    setting         REAL,
    target_setting  REAL,
    time_open       REAL,
    time_closed     REAL,
    PRIMARY KEY (simulation_id, slot_name, link_id),
    FOREIGN KEY (simulation_id, slot_name)
        REFERENCES hotstart_slots(simulation_id, slot_name)
        ON DELETE CASCADE ON UPDATE CASCADE,
    FOREIGN KEY (simulation_id, link_id)
        REFERENCES links(simulation_id, link_id)
        ON DELETE CASCADE ON UPDATE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_hotstart_link_state_lookup
    ON hotstart_link_state(simulation_id, slot_name, link_id);

-- Per-subcatchment hydrology state — runoff + infiltration (6-double) +
-- groundwater zone + snowpack water-equivalent / free-water / ATI per
-- surface. Matches the legacy hotstart.c saveRunoff() layout.
CREATE TABLE IF NOT EXISTS hotstart_subcatch_state (
    simulation_id     TEXT NOT NULL,
    slot_name         TEXT NOT NULL,
    subcatch_id       TEXT NOT NULL,
    runoff            REAL,
    infil_model       INTEGER NOT NULL,
    infil_state_0     REAL, infil_state_1 REAL, infil_state_2 REAL,
    infil_state_3     REAL, infil_state_4 REAL, infil_state_5 REAL,
    gw_theta_upper    REAL,
    gw_lower_depth    REAL,
    snow_we_plowable  REAL, snow_we_imperv REAL, snow_we_perv REAL,
    snow_fw_plowable  REAL, snow_fw_imperv REAL, snow_fw_perv REAL,
    snow_ati          REAL,
    PRIMARY KEY (simulation_id, slot_name, subcatch_id),
    FOREIGN KEY (simulation_id, slot_name)
        REFERENCES hotstart_slots(simulation_id, slot_name)
        ON DELETE CASCADE ON UPDATE CASCADE,
    FOREIGN KEY (simulation_id, subcatch_id)
        REFERENCES subcatchments(simulation_id, subcatch_id)
        ON DELETE CASCADE ON UPDATE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_hotstart_subcatch_state_lookup
    ON hotstart_subcatch_state(simulation_id, slot_name, subcatch_id);

-- Water-quality state split by object kind so each row can FK into its
-- owning state table and the model's pollutants. A polymorphic single
-- table cannot express that — SQLite has no discriminated-union FK.

CREATE TABLE IF NOT EXISTS hotstart_node_pollutant_state (
    simulation_id   TEXT NOT NULL,
    slot_name       TEXT NOT NULL,
    node_id         TEXT NOT NULL,
    pollutant_id    TEXT NOT NULL,
    concentration   REAL NOT NULL,
    PRIMARY KEY (simulation_id, slot_name, node_id, pollutant_id),
    FOREIGN KEY (simulation_id, slot_name, node_id)
        REFERENCES hotstart_node_state(simulation_id, slot_name, node_id)
        ON DELETE CASCADE ON UPDATE CASCADE,
    FOREIGN KEY (simulation_id, pollutant_id)
        REFERENCES pollutants(simulation_id, pollutant_id)
        ON DELETE CASCADE ON UPDATE CASCADE
);

CREATE TABLE IF NOT EXISTS hotstart_link_pollutant_state (
    simulation_id   TEXT NOT NULL,
    slot_name       TEXT NOT NULL,
    link_id         TEXT NOT NULL,
    pollutant_id    TEXT NOT NULL,
    concentration   REAL NOT NULL,
    PRIMARY KEY (simulation_id, slot_name, link_id, pollutant_id),
    FOREIGN KEY (simulation_id, slot_name, link_id)
        REFERENCES hotstart_link_state(simulation_id, slot_name, link_id)
        ON DELETE CASCADE ON UPDATE CASCADE,
    FOREIGN KEY (simulation_id, pollutant_id)
        REFERENCES pollutants(simulation_id, pollutant_id)
        ON DELETE CASCADE ON UPDATE CASCADE
);

-- Subcatchment pollutant state additionally carries surface-buildup mass
-- and ponded concentration to match the legacy water-quality fields
-- written by hotstart.c saveRunoff().
CREATE TABLE IF NOT EXISTS hotstart_subcatch_pollutant_state (
    simulation_id        TEXT NOT NULL,
    slot_name            TEXT NOT NULL,
    subcatch_id          TEXT NOT NULL,
    pollutant_id         TEXT NOT NULL,
    surface_buildup      REAL,
    ponded_concentration REAL,
    PRIMARY KEY (simulation_id, slot_name, subcatch_id, pollutant_id),
    FOREIGN KEY (simulation_id, slot_name, subcatch_id)
        REFERENCES hotstart_subcatch_state(simulation_id, slot_name, subcatch_id)
        ON DELETE CASCADE ON UPDATE CASCADE,
    FOREIGN KEY (simulation_id, pollutant_id)
        REFERENCES pollutants(simulation_id, pollutant_id)
        ON DELETE CASCADE ON UPDATE CASCADE
);

-- ----------------------------------------------------------------------------
-- Raingage rainfall records (replaces external rain-file content).
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS raingage_data (
    simulation_id    TEXT NOT NULL,
    gage_id          TEXT NOT NULL,
    record_time      TEXT NOT NULL,
    rainfall_value   REAL NOT NULL,
    quality_flag     TEXT,
    station_id       TEXT,
    PRIMARY KEY (simulation_id, gage_id, record_time),
    FOREIGN KEY (simulation_id, gage_id)
        REFERENCES rain_gages(simulation_id, gage_id)
        ON DELETE CASCADE ON UPDATE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_raingage_data
    ON raingage_data(simulation_id, gage_id, record_time);

-- ----------------------------------------------------------------------------
-- Climate observations (Tmin/Tmax/evap/wind/sky/humidity). Daily-grain
-- matches SWMM's TEMPERATURE / EVAPORATION file formats.
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS climate_data (
    simulation_id   TEXT NOT NULL,
    record_date     TEXT NOT NULL,
    tmin            REAL,
    tmax            REAL,
    evaporation     REAL,
    wind_speed      REAL,
    sky_cover       REAL,
    humidity        REAL,
    quality_flag    TEXT,
    PRIMARY KEY (simulation_id, record_date),
    FOREIGN KEY (simulation_id)
        REFERENCES simulations(simulation_id)
        ON DELETE CASCADE ON UPDATE CASCADE
);

-- ----------------------------------------------------------------------------
-- Routing interface files. Three-way split so each table can FK into the
-- right owning object kind (a polymorphic table cannot).
-- ----------------------------------------------------------------------------

-- INFLOWS / OUTFLOWS / RDII — per-node flow records.
CREATE TABLE IF NOT EXISTS routing_interface_node (
    simulation_id   TEXT NOT NULL,
    role            TEXT NOT NULL,        -- 'INFLOWS' | 'OUTFLOWS' | 'RDII'
    direction       TEXT NOT NULL,        -- 'USE' | 'SAVE'
    node_id         TEXT NOT NULL,
    record_time     TEXT NOT NULL,
    flow_value      REAL NOT NULL,
    PRIMARY KEY (simulation_id, role, direction, node_id, record_time),
    FOREIGN KEY (simulation_id, node_id)
        REFERENCES nodes(simulation_id, node_id)
        ON DELETE CASCADE ON UPDATE CASCADE
);

-- RUNOFF — per-subcatchment flow records.
CREATE TABLE IF NOT EXISTS routing_interface_subcatch (
    simulation_id   TEXT NOT NULL,
    role            TEXT NOT NULL,        -- 'RUNOFF'
    direction       TEXT NOT NULL,
    subcatch_id     TEXT NOT NULL,
    record_time     TEXT NOT NULL,
    flow_value      REAL NOT NULL,
    PRIMARY KEY (simulation_id, role, direction, subcatch_id, record_time),
    FOREIGN KEY (simulation_id, subcatch_id)
        REFERENCES subcatchments(simulation_id, subcatch_id)
        ON DELETE CASCADE ON UPDATE CASCADE
);

-- RAINFALL — per-gage rainfall records (separate from the model's own
-- raingage_data above because routing interface files are a distinct
-- legacy concept: pre-processed gage output captured for re-feeding).
CREATE TABLE IF NOT EXISTS routing_interface_gage (
    simulation_id   TEXT NOT NULL,
    role            TEXT NOT NULL,        -- 'RAINFALL'
    direction       TEXT NOT NULL,
    gage_id         TEXT NOT NULL,
    record_time     TEXT NOT NULL,
    rainfall_value  REAL NOT NULL,
    PRIMARY KEY (simulation_id, role, direction, gage_id, record_time),
    FOREIGN KEY (simulation_id, gage_id)
        REFERENCES rain_gages(simulation_id, gage_id)
        ON DELETE CASCADE ON UPDATE CASCADE
);

-- Pollutant concentrations attached to node-keyed routing rows (the only
-- legacy routing format that carries water-quality columns).
CREATE TABLE IF NOT EXISTS routing_interface_node_pollutants (
    simulation_id   TEXT NOT NULL,
    role            TEXT NOT NULL,
    direction       TEXT NOT NULL,
    node_id         TEXT NOT NULL,
    record_time     TEXT NOT NULL,
    pollutant_id    TEXT NOT NULL,
    concentration   REAL NOT NULL,
    PRIMARY KEY (simulation_id, role, direction, node_id, record_time, pollutant_id),
    FOREIGN KEY (simulation_id, role, direction, node_id, record_time)
        REFERENCES routing_interface_node(simulation_id, role, direction, node_id, record_time)
        ON DELETE CASCADE ON UPDATE CASCADE,
    FOREIGN KEY (simulation_id, pollutant_id)
        REFERENCES pollutants(simulation_id, pollutant_id)
        ON DELETE CASCADE ON UPDATE CASCADE
);
)SQL";

// ============================================================================
// Part E — 2D surface-routing mesh (model definition only; 2D simulation
// RESULTS are always written to the CF/UGRID HDF5 output file referenced by
// the `2D_OUTPUT_FILE` option key, never to GeoPackage tables).
//
// Canonical storage is the relational index form: vertex coordinates plus
// triangle connectivity (v0/v1/v2 ordinals into mesh_2d_vertices). Derived
// topology — neighbour adjacency, areas/centroids, edge geometry, vertex
// stencils — is intentionally NOT stored; SurfaceRouter2D::initialize()
// rebuilds it from the primary data exactly as it does for the .inp path.
//
// The `geom` columns are DERIVED presentation copies registered as
// GeoPackage feature layers so GIS tools (QGIS) can render the mesh
// alongside nodes/links. They are IGNORED on read; editing them in a GIS
// does not change the model. Same for mesh_2d_triangles.bed_elev and
// .coupled_node (styling-only convenience attributes).
//
// Coordinates are stored in the AUTHORED project/map units — the same
// coordinate space as the nodes/links feature layers — so the layers stay
// aligned in projected CRSs with non-metric linear units. The authored
// units flag round-trips via the `2D_MESH_UNITS_SI` option key.
//
// No FK to simulations(simulation_id): write_model() does not insert a
// simulations row (only the output plugin does), matching every other
// Part A model table.
// ============================================================================

static const char* MESH_2D_DDL = R"SQL(
-- ----------------------------------------------------------------------------
-- 2D mesh vertices (POINT feature layer; x/y/z are canonical, geom derived).
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS mesh_2d_vertices (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    vertex_idx      INTEGER NOT NULL,
    geom            BLOB,
    x               REAL NOT NULL,
    y               REAL NOT NULL,
    z               REAL NOT NULL,
    tag             TEXT,
    UNIQUE(simulation_id, vertex_idx)
);

-- ----------------------------------------------------------------------------
-- 2D mesh triangles (POLYGON feature layer; v0/v1/v2 are canonical, geom /
-- bed_elev / coupled_node derived for GIS styling only).
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS mesh_2d_triangles (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    tri_idx         INTEGER NOT NULL,
    geom            BLOB,
    v0              INTEGER NOT NULL,
    v1              INTEGER NOT NULL,
    v2              INTEGER NOT NULL,
    mannings_n      REAL NOT NULL DEFAULT 0.035,
    tag             TEXT,
    bed_elev        REAL,
    coupled_node    TEXT,
    UNIQUE(simulation_id, tri_idx),
    FOREIGN KEY (simulation_id, v0)
        REFERENCES mesh_2d_vertices(simulation_id, vertex_idx)
        ON DELETE CASCADE ON UPDATE CASCADE,
    FOREIGN KEY (simulation_id, v1)
        REFERENCES mesh_2d_vertices(simulation_id, vertex_idx)
        ON DELETE CASCADE ON UPDATE CASCADE,
    FOREIGN KEY (simulation_id, v2)
        REFERENCES mesh_2d_vertices(simulation_id, vertex_idx)
        ON DELETE CASCADE ON UPDATE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_mesh2d_tri_v0 ON mesh_2d_triangles(simulation_id, v0);
CREATE INDEX IF NOT EXISTS idx_mesh2d_tri_v1 ON mesh_2d_triangles(simulation_id, v1);
CREATE INDEX IF NOT EXISTS idx_mesh2d_tri_v2 ON mesh_2d_triangles(simulation_id, v2);

-- ----------------------------------------------------------------------------
-- Per-edge boundary conditions; canonical (tri_idx, edge) form matching
-- [2D_BOUNDARY_CONDITIONS]. Rows exist only for non-default edges (absent
-- row == WALL). bc_type uses the .inp grammar tokens (WALL | NORMAL_FLOW |
-- SPECIFIED_STAGE | TS_STAGE | SPECIFIED_FLOW | TS_FLOW | RATING_CURVE).
-- param1 = slope / head / unit-flow; ref_name = timeseries or rating-curve
-- name (mutually exclusive with param1 by type); bc_group = optional GROUP
-- label ("group" is an SQL keyword).
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS mesh_2d_boundary_conditions (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    tri_idx         INTEGER NOT NULL,
    edge            INTEGER NOT NULL CHECK (edge BETWEEN 0 AND 2),
    bc_type         TEXT NOT NULL,
    param1          REAL,
    ref_name        TEXT,
    bc_group        TEXT,
    UNIQUE(simulation_id, tri_idx, edge),
    FOREIGN KEY (simulation_id, tri_idx)
        REFERENCES mesh_2d_triangles(simulation_id, tri_idx)
        ON DELETE CASCADE ON UPDATE CASCADE
);

-- ----------------------------------------------------------------------------
-- Edge conveyance; canonical undirected vertex-pair form matching
-- [2D_EDGE_CONVEYANCE] (resolution-independent; interior-edge mirroring is
-- re-derived in SurfaceRouter2D::initialize). Rows exist only for
-- conveyance != 1.0. Stored normalized with v_from < v_to.
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS mesh_2d_edge_conveyance (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    v_from          INTEGER NOT NULL,
    v_to            INTEGER NOT NULL,
    conveyance      REAL NOT NULL CHECK (conveyance >= 0.0 AND conveyance <= 1.0),
    CHECK (v_from <> v_to),
    UNIQUE(simulation_id, v_from, v_to),
    FOREIGN KEY (simulation_id, v_from)
        REFERENCES mesh_2d_vertices(simulation_id, vertex_idx)
        ON DELETE CASCADE ON UPDATE CASCADE,
    FOREIGN KEY (simulation_id, v_to)
        REFERENCES mesh_2d_vertices(simulation_id, vertex_idx)
        ON DELETE CASCADE ON UPDATE CASCADE
);

-- ----------------------------------------------------------------------------
-- 1D-2D coupling maps ([2D_VERTEX_NODE_MAP] / [2D_TRIANGLE_NODE_MAP]).
-- Sparse: one row per coupled vertex/triangle. coupling_area is in authored
-- project-length-squared units (scaled together with the mesh coordinates).
-- The hard FK to nodes keeps coupling consistent under node rename/delete.
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS mesh_2d_vertex_coupling (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    vertex_idx      INTEGER NOT NULL,
    node_id         TEXT NOT NULL,
    coupling_cd     REAL NOT NULL DEFAULT 0.65,
    coupling_area   REAL NOT NULL DEFAULT 1.0,
    UNIQUE(simulation_id, vertex_idx),
    FOREIGN KEY (simulation_id, vertex_idx)
        REFERENCES mesh_2d_vertices(simulation_id, vertex_idx)
        ON DELETE CASCADE ON UPDATE CASCADE,
    FOREIGN KEY (simulation_id, node_id)
        REFERENCES nodes(simulation_id, node_id)
        ON DELETE CASCADE ON UPDATE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_mesh2d_vc_node
    ON mesh_2d_vertex_coupling(simulation_id, node_id);

CREATE TABLE IF NOT EXISTS mesh_2d_triangle_coupling (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    tri_idx         INTEGER NOT NULL,
    node_id         TEXT NOT NULL,
    coupling_cd     REAL NOT NULL DEFAULT 0.65,
    coupling_area   REAL NOT NULL DEFAULT 1.0,
    UNIQUE(simulation_id, tri_idx),
    FOREIGN KEY (simulation_id, tri_idx)
        REFERENCES mesh_2d_triangles(simulation_id, tri_idx)
        ON DELETE CASCADE ON UPDATE CASCADE,
    FOREIGN KEY (simulation_id, node_id)
        REFERENCES nodes(simulation_id, node_id)
        ON DELETE CASCADE ON UPDATE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_mesh2d_tc_node
    ON mesh_2d_triangle_coupling(simulation_id, node_id);
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
    exec(db, PART_D_DDL);
    exec(db, MESH_2D_DDL);
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
