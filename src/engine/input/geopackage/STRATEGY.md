# GeoPackage I/O Strategy for OpenSWMM

## 1. Overview

This document defines the strategy for implementing GeoPackage-based persistence for OpenSWMM, covering three core use cases:

1. **Model Input** -- Read/write SWMM model definitions (network geometry, parameters, options) as a GeoPackage, serving as a spatial alternative to the `.inp` text format.
2. **Simulation Output & Reports** -- Write timestep results, summary statistics, and report attributes to the same GeoPackage, keyed by a simulation run ID. **Multiple simulation runs coexist in a single file** -- each run is isolated by its `simulation_id`, enabling scenario comparison, calibration iteration tracking, and historical run archival without creating separate output files.
3. **Observed / Sensor Data** -- Store independent measured timeseries (e.g., flow gauges, water-level sensors, rain gauges) that can be associated with model objects for comparison and calibration workflows.

All three concerns live in a **single `.gpkg` file** (SQLite database with OGC GeoPackage extensions). One GeoPackage acts as the complete project container: model definition in, multiple simulation results and reports out, observed data alongside. GIS tools (QGIS, ArcGIS, FME) can visualize model geometry, results from any run, and observations in one artifact.

### Single-File, Multi-Run Architecture

```
                        ┌─────────────────────────────┐
                        │     project.gpkg              │
                        │                               │
                        │  ┌── Model Definition ──────┐ │
                        │  │  nodes, links, subcatch   │ │
                        │  │  options, curves, etc.    │ │  ← written once per scenario
                        │  └──────────────────────────┘ │
                        │                               │
                        │  ┌── Simulation Run A ──────┐ │
                        │  │  simulation_id = "run_a"  │ │
                        │  │  engine_version = "6.1.0" │ │
                        │  │  result_timeseries        │ │  ← each run appends
                        │  │  result_summary (report)  │ │    under its own ID
                        │  │  continuity errors        │ │
                        │  └──────────────────────────┘ │
                        │                               │
                        │  ┌── Simulation Run B ──────┐ │
                        │  │  simulation_id = "run_b"  │ │
                        │  │  engine_version = "6.2.0" │ │
                        │  │  result_timeseries        │ │
                        │  │  result_summary (report)  │ │
                        │  │  continuity errors        │ │
                        │  └──────────────────────────┘ │
                        │                               │
                        │  ┌── Observed Data ─────────┐ │
                        │  │  observed_series          │ │  ← independent of runs,
                        │  │  observed_values          │ │    shared calibration targets
                        │  └──────────────────────────┘ │
                        └─────────────────────────────┘
```

Every table that holds per-run data includes `simulation_id` as part of its primary or unique key. Querying a specific run is always a single `WHERE simulation_id = ?` predicate. Deleting a run and all its data is a cascading delete from the `simulations` table. The engine version that produced each run is recorded in `simulations.engine_version`, enabling reproducibility and compatibility tracking across engine releases.

---

## 2. Design Principles

| Principle | Rationale |
|-----------|-----------|
| **Relational but pragmatic** | Normalize timeseries storage and object-attribute catalogs; keep feature tables flat for GIS usability. Inspired by Observations Data Model (ODM) but not a full ODM implementation. |
| **Simulation-run isolation** | Every simulation writes under a unique `simulation_id`. Multiple runs (scenarios, calibration iterations) coexist in one file. |
| **Spatial-first features** | Nodes, links, and subcatchments are GeoPackage feature tables with native geometry columns, leveraging the existing `SpatialFrame` CRS support. |
| **Append-friendly timeseries** | Timeseries data (results and observations) is stored in a single long table with foreign keys to a variable catalog. No per-variable columns -- scales to arbitrary pollutant counts and custom variables. |
| **Network topology as data** | Explicit node-link connectivity tables enable graph traversal via SQL (upstream/downstream traces, path finding, contributing area) on top of OGC Simple Features geometry. |
| **Plugin integration** | Implement as `IInputPlugin` / `IOutputPlugin` / `IReportPlugin` implementations, fitting into the existing plugin lifecycle (`initialize` -> `validate` -> `prepare` -> `update` -> `finalize`). |

---

## 3. Schema Design

### 3.1 Entity-Relationship Summary

```
    ┌─────────────────────────────────────────────────────────────┐
    │                   gpkg_spatial_ref_sys                       │
    │  (OGC standard CRS registry -- EPSG codes, WKT, PROJ)      │
    └────────────────────────────┬────────────────────────────────┘
                                 │ srs_id referenced by all feature tables
                                 │
                          ┌──────┴──────────┐
                          │   simulations    │
                          │─────────────────│
                          │ simulation_id PK │
                          │ name             │
                          │ description      │
                          │ created_at       │
                          │ inp_hash         │
                          │ engine_version   │
                          │ status           │
                          └────────┬─────────┘
                                   │ 1
                    ┌──────────────┼──────────────┐
                    │              │              │
               ┌────┴────┐   ┌────┴────┐   ┌────┴─────────┐
               │  nodes  │   │  links  │   │ subcatchments │
               │ (POINT) │   │ (LINE)  │   │  (POLYGON)   │
               └──┬───┬──┘   └──┬───┬──┘   └──────┬───────┘
                  │   │         │   │              │
                  │   │    ┌────┘   │              │
                  │   │    │        │       ┌──────┴────────┐
               ┌──┴───┴────┴──┐     │       │subcatch_routes│
               │  node_links  │     │       │───────────────│
               │  (topology)  │     │       │subcatch_id FK │
               │──────────────│     │       │outlet_type    │
               │ link_id  FK ─┼─────┘       │outlet_node FK │
               │ from_node FK │             │outlet_sub FK  │
               │ to_node  FK  │             └───────────────┘
               │ direction    │
               └──────────────┘
                                    │
                    ┌───────────────┘
                    │ FK: object_type + object_id
                    │
    ┌──────────────┐  ┌─────────────────────┐  ┌──────────────────┐
    │   variables  │──│  result_timeseries   │  │  observed_series │
    │──────────────│  │─────────────────────│  │──────────────────│
    │ variable_id  │  │ simulation_id FK     │  │ series_id PK     │
    │ name         │  │ object_type          │  │ name             │
    │ object_type  │  │ object_id            │  │ source           │
    │ units        │  │ variable_id FK       │  │ variable_id FK   │
    │ category     │  │ elapsed_time         │  │ object_type      │
    │ description  │  │ value                │  │ object_id        │
    └──────────────┘  └─────────────────────┘  └───────┬──────────┘
                                                        │ 1
                                                 ┌──────┴──────────┐
                                                 │ observed_values  │
                                                 │─────────────────│
                                                 │ series_id FK     │
                                                 │ timestamp        │
                                                 │ value            │
                                                 │ quality_flag     │
                                                 └─────────────────┘
```

### 3.2 Part A: Model Input Schema

These tables define the SWMM model -- the network geometry, hydraulic parameters, boundary conditions, and supporting data that replace the `.inp` text format. This is the **first thing written** when creating a GeoPackage, and the only thing needed to run a simulation.

#### 3.2.1 Coordinate Reference System & GeoPackage Spatial Metadata

GeoPackage defines three mandatory metadata tables that govern how geometry is stored and interpreted. OpenSWMM populates these during schema creation using the CRS information from `SpatialFrame`.

**`gpkg_spatial_ref_sys`** -- Coordinate Reference System Registry (OGC standard table)

This table is defined by the GeoPackage standard. OpenSWMM inserts the model's CRS on write and reads it back on load. The `SpatialFrame` fields `crs` (e.g., `"EPSG:4326"`) and `is_geographic` map directly to entries in this table.

```sql
-- Standard GeoPackage table (created automatically)
-- OpenSWMM inserts the model's CRS if not already present:
INSERT INTO gpkg_spatial_ref_sys (
    srs_name,               -- e.g., "WGS 84"
    srs_id,                 -- e.g., 4326
    organization,           -- e.g., "EPSG"
    organization_coordsys_id, -- e.g., 4326
    definition,             -- WKT2 CRS definition
    description             -- human-readable
) VALUES (
    'WGS 84', 4326, 'EPSG', 4326,
    'GEOGCS["WGS 84", DATUM["WGS_1984", ...], PRIMEM["Greenwich",0], UNIT["degree",0.0174532925199433]]',
    'World Geodetic System 1984 - Geographic 2D'
);
```

The `srs_id` from this table is referenced by every feature table registration below.

**`gpkg_contents`** -- Feature Table Registry

Each feature table (nodes, links, subcatchments, rain_gages) is registered here with its spatial extent and CRS reference:

```sql
INSERT INTO gpkg_contents (
    table_name,   -- 'nodes', 'links', 'subcatchments', 'rain_gages'
    data_type,    -- 'features'
    identifier,   -- human-readable name
    description,
    last_change,  -- ISO 8601 timestamp
    min_x, min_y, max_x, max_y,  -- bounding box from SpatialFrame map extents
    srs_id        -- FK to gpkg_spatial_ref_sys
) VALUES (
    'nodes', 'features', 'Network Nodes',
    'Junctions, outfalls, dividers, and storage units',
    datetime('now'),
    :map_x1, :map_y1, :map_x2, :map_y2,  -- from SpatialFrame
    :srs_id                                -- resolved from SpatialFrame.crs
);
-- Repeat for 'links', 'subcatchments', 'rain_gages'
```

**`gpkg_geometry_columns`** -- Geometry Column Registry

Maps each feature table's geometry column to its geometry type and CRS:

```sql
INSERT INTO gpkg_geometry_columns (
    table_name,    column_name,  geometry_type_name,  srs_id,  z,  m
) VALUES
    ('nodes',         'geom', 'POINT',        :srs_id, 0, 0),
    ('links',         'geom', 'LINESTRING',   :srs_id, 0, 0),
    ('subcatchments', 'geom', 'MULTIPOLYGON', :srs_id, 0, 0),
    ('rain_gages',    'geom', 'POINT',        :srs_id, 0, 0);
```

**CRS Mapping from SpatialFrame**

| SpatialFrame field | GeoPackage mapping |
|---|---|
| `crs` (e.g., `"EPSG:4326"`) | Parse to `organization` = `"EPSG"`, `organization_coordsys_id` = `4326`. Look up WKT definition from EPSG database or PROJ. |
| `is_geographic` | Determines whether coordinates are lat/lon (geographic) or easting/northing (projected). Encoded in the WKT `GEOGCS` vs `PROJCS` root element. |
| `map_units` (FEET / METERS / DEGREES) | Consistent with CRS linear/angular units. Validated against CRS definition on read. |
| `map_x1, map_y1, map_x2, map_y2` | Written to `gpkg_contents.min_x/min_y/max_x/max_y` as the dataset bounding box. |

**Geometry Encoding**

All geometries use GeoPackage Standard Binary encoding (GeoPackage spec Annex F):

```
[GP header: 8 bytes] [ISO WKB payload]

GP header:
  - Magic: 0x4750 ("GP")
  - Version: 0x00
  - Flags: 1 byte (byte order, envelope type, empty flag)
  - SRS ID: 4 bytes (int32, matches gpkg_spatial_ref_sys.srs_id)
  - Envelope: variable (0/32/48/64 bytes depending on flags)

WKB payload:
  - Standard ISO 13249 Well-Known Binary
```

Geometry construction from `SpatialFrame` arrays:

| Object type | Geometry type | Source arrays | Construction |
|---|---|---|---|
| Node | POINT | `node_x[i]`, `node_y[i]` | Single point |
| Link | LINESTRING | `node_coords[from]`, `link_vertices_x[i][]`/`link_vertices_y[i][]`, `node_coords[to]` | Polyline: start node -> interior vertices -> end node |
| Subcatchment | MULTIPOLYGON | `subcatch_polygon_x[i][]`, `subcatch_polygon_y[i][]` | Closed polygon ring (first == last point enforced) |
| Rain gage | POINT | `gage_x[i]`, `gage_y[i]` | Single point from [SYMBOLS] section |

**Spatial Indexing**

GeoPackage R-Tree spatial indexes are created for each feature table using SQLite's built-in R-Tree module. This enables efficient spatial queries (bounding box intersection, nearest neighbor):

```sql
-- GeoPackage standard R-Tree trigger pattern (auto-generated)
CREATE VIRTUAL TABLE rtree_nodes_geom USING rtree(
    id, minx, maxx, miny, maxy
);
-- Triggers on INSERT/UPDATE/DELETE of nodes.geom maintain the index
```

#### 3.2.2 `options` -- Simulation Options

Flat key-value store for all simulation options (flow units, routing method, time steps, etc.).

```sql
CREATE TABLE options (
    simulation_id  TEXT NOT NULL,
    key            TEXT NOT NULL,
    value          TEXT NOT NULL,
    PRIMARY KEY (simulation_id, key)
);
```

#### 3.2.3 Feature Tables (GeoPackage Geometry)

These are registered in `gpkg_contents` and `gpkg_geometry_columns` as proper GeoPackage feature tables, enabling native GIS rendering.

**`nodes`** -- Junctions, Outfalls, Dividers, Storage Units

```sql
CREATE TABLE nodes (
    fid            INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id  TEXT NOT NULL REFERENCES simulations(simulation_id),
    node_id        TEXT NOT NULL,
    node_type      TEXT NOT NULL,  -- JUNCTION | OUTFALL | DIVIDER | STORAGE
    geom           POINT,         -- GeoPackage geometry column

    -- Common properties
    invert_elev    REAL,
    max_depth      REAL,
    init_depth     REAL,
    surcharge_depth REAL,
    ponded_area    REAL,

    -- Type-specific (NULL when not applicable)
    outfall_type   TEXT,       -- FREE | NORMAL | FIXED | TIDAL | TIMESERIES
    divider_type   TEXT,       -- CUTOFF | OVERFLOW | TABULAR | WEIR
    storage_curve  TEXT,       -- curve name reference

    -- Tags / user flags
    tag            TEXT,

    UNIQUE(simulation_id, node_id)
);
```

**`links`** -- Conduits, Pumps, Orifices, Weirs, Outlets

```sql
CREATE TABLE links (
    fid            INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id  TEXT NOT NULL REFERENCES simulations(simulation_id),
    link_id        TEXT NOT NULL,
    link_type      TEXT NOT NULL,  -- CONDUIT | PUMP | ORIFICE | WEIR | OUTLET
    geom           LINESTRING,    -- vertices from SpatialFrame

    from_node      TEXT NOT NULL,
    to_node        TEXT NOT NULL,
    offset1        REAL,
    offset2        REAL,

    -- Conduit properties
    xsect_shape    TEXT,
    xsect_geom1    REAL,  -- max depth / diameter
    xsect_geom2    REAL,  -- bottom width or 0
    xsect_geom3    REAL,
    xsect_geom4    REAL,
    xsect_barrels  INTEGER,
    roughness      REAL,
    length         REAL,
    loss_inlet     REAL,
    loss_outlet    REAL,
    loss_avg       REAL,
    has_flap_gate  INTEGER,  -- boolean
    seep_rate      REAL,

    -- Pump properties
    pump_curve     TEXT,

    -- Weir/Orifice properties
    crest_height   REAL,
    discharge_coeff REAL,

    tag            TEXT,

    UNIQUE(simulation_id, link_id)
);
```

**`subcatchments`** -- Subcatchment Areas

```sql
CREATE TABLE subcatchments (
    fid            INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id  TEXT NOT NULL REFERENCES simulations(simulation_id),
    subcatch_id    TEXT NOT NULL,
    geom           MULTIPOLYGON,  -- polygon from SpatialFrame

    outlet_node    TEXT,
    outlet_subcatch TEXT,
    rain_gage      TEXT,
    area           REAL,
    width          REAL,
    slope          REAL,
    curb_length    REAL,
    frac_imperv    REAL,

    -- Subarea properties
    n_imperv       REAL,
    n_perv         REAL,
    ds_imperv      REAL,
    ds_perv        REAL,
    pct_zero_imperv REAL,
    subarea_routing TEXT,  -- TO_OUTLET | TO_IMPERV | TO_PERV
    pct_routed     REAL,

    -- Infiltration
    infil_model    TEXT,  -- HORTON | MOD_HORTON | GREEN_AMPT | MOD_GREEN_AMPT | CURVE_NUMBER
    infil_p1       REAL,
    infil_p2       REAL,
    infil_p3       REAL,
    infil_p4       REAL,
    infil_p5       REAL,

    tag            TEXT,

    UNIQUE(simulation_id, subcatch_id)
);
```

**`rain_gages`** -- Rain Gage Definitions

```sql
CREATE TABLE rain_gages (
    fid            INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id  TEXT NOT NULL REFERENCES simulations(simulation_id),
    gage_id        TEXT NOT NULL,
    geom           POINT,

    rain_type      TEXT,   -- INTENSITY | VOLUME | CUMULATIVE
    rain_interval  TEXT,   -- e.g., "0:05"
    snow_catch     REAL,
    data_source    TEXT,   -- TIMESERIES | FILE
    source_name    TEXT,   -- timeseries name or file path
    station_id     TEXT,
    rain_units     TEXT,

    UNIQUE(simulation_id, gage_id)
);
```

#### 3.2.4 Network Topology Tables

The network topology layer sits on top of the Simple Features geometry, making the directed graph structure of the SWMM drainage network directly queryable via SQL. This enables upstream/downstream traversal, path finding, contributing area delineation, and connectivity validation without application-level graph logic.

**`node_links`** -- Edge-Node Connectivity (Directed Graph)

Each row represents a directed connection from one node to another via a link. This is the core topology table that makes the network traversable.

```sql
CREATE TABLE node_links (
    fid            INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id  TEXT NOT NULL REFERENCES simulations(simulation_id),
    link_id        TEXT NOT NULL,
    from_node      TEXT NOT NULL,
    to_node        TEXT NOT NULL,
    link_type      TEXT NOT NULL,  -- CONDUIT | PUMP | ORIFICE | WEIR | OUTLET
    direction      INTEGER NOT NULL DEFAULT 1,  -- 1 = normal, -1 = reversed (backflow)

    FOREIGN KEY (simulation_id, link_id)    REFERENCES links(simulation_id, link_id),
    FOREIGN KEY (simulation_id, from_node)  REFERENCES nodes(simulation_id, node_id),
    FOREIGN KEY (simulation_id, to_node)    REFERENCES nodes(simulation_id, node_id),
    UNIQUE(simulation_id, link_id)
);

CREATE INDEX idx_node_links_from ON node_links(simulation_id, from_node);
CREATE INDEX idx_node_links_to   ON node_links(simulation_id, to_node);
```

**`subcatch_routing`** -- Subcatchment Outlet Connectivity

Subcatchments discharge to either a node or another subcatchment (cascading subcatchment routing). This table makes the runoff routing topology explicit and queryable.

```sql
CREATE TABLE subcatch_routing (
    fid              INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id    TEXT NOT NULL REFERENCES simulations(simulation_id),
    subcatch_id      TEXT NOT NULL,
    outlet_type      TEXT NOT NULL,  -- NODE | SUBCATCHMENT
    outlet_node      TEXT,           -- FK to nodes.node_id (NULL if outlet is subcatchment)
    outlet_subcatch  TEXT,           -- FK to subcatchments.subcatch_id (NULL if outlet is node)

    FOREIGN KEY (simulation_id, subcatch_id) REFERENCES subcatchments(simulation_id, subcatch_id),
    UNIQUE(simulation_id, subcatch_id),
    CHECK (
        (outlet_type = 'NODE' AND outlet_node IS NOT NULL AND outlet_subcatch IS NULL) OR
        (outlet_type = 'SUBCATCHMENT' AND outlet_subcatch IS NOT NULL AND outlet_node IS NULL)
    )
);

CREATE INDEX idx_subcatch_routing_outlet_node ON subcatch_routing(simulation_id, outlet_node);
CREATE INDEX idx_subcatch_routing_outlet_sub  ON subcatch_routing(simulation_id, outlet_subcatch);
```

**Topology Query Examples**

*Find all links connected to a node (incoming and outgoing):*

```sql
SELECT link_id, from_node, to_node, link_type,
       CASE WHEN from_node = 'J1' THEN 'outgoing' ELSE 'incoming' END AS direction
FROM node_links
WHERE simulation_id = ?
  AND (from_node = 'J1' OR to_node = 'J1');
```

*Trace upstream from an outfall using recursive CTE:*

```sql
WITH RECURSIVE upstream(node_id, depth, path) AS (
    -- Start at the outfall
    SELECT 'OUT1', 0, 'OUT1'
    UNION ALL
    -- Walk upstream: find nodes that flow INTO the current node
    SELECT nl.from_node, u.depth + 1, u.path || ' <- ' || nl.from_node
    FROM upstream u
    JOIN node_links nl ON nl.to_node = u.node_id
                      AND nl.simulation_id = ?
    WHERE u.depth < 100  -- safety limit
)
SELECT node_id, depth, path FROM upstream ORDER BY depth;
```

*Find all subcatchments that contribute to a node (direct + cascading):*

```sql
WITH RECURSIVE contributing(subcatch_id, depth) AS (
    -- Subcatchments draining directly to the target node
    SELECT sr.subcatch_id, 0
    FROM subcatch_routing sr
    WHERE sr.outlet_type = 'NODE' AND sr.outlet_node = 'J1'
      AND sr.simulation_id = ?
    UNION ALL
    -- Subcatchments draining into other subcatchments that eventually reach this node
    SELECT sr.subcatch_id, c.depth + 1
    FROM contributing c
    JOIN subcatch_routing sr ON sr.outlet_type = 'SUBCATCHMENT'
                            AND sr.outlet_subcatch = c.subcatch_id
                            AND sr.simulation_id = ?
    WHERE c.depth < 50
)
SELECT s.subcatch_id, s.area, s.geom, c.depth
FROM contributing c
JOIN subcatchments s ON s.subcatch_id = c.subcatch_id AND s.simulation_id = ?
ORDER BY c.depth;
```

*Compute total contributing area at each node:*

```sql
WITH RECURSIVE upstream_nodes(node_id, depth) AS (
    SELECT node_id, 0 FROM nodes WHERE simulation_id = ? AND node_type = 'OUTFALL'
    UNION ALL
    SELECT nl.from_node, un.depth + 1
    FROM upstream_nodes un
    JOIN node_links nl ON nl.to_node = un.node_id AND nl.simulation_id = ?
    WHERE un.depth < 100
),
node_areas AS (
    SELECT sr.outlet_node AS node_id, SUM(s.area) AS direct_area
    FROM subcatch_routing sr
    JOIN subcatchments s ON s.subcatch_id = sr.subcatch_id AND s.simulation_id = sr.simulation_id
    WHERE sr.outlet_type = 'NODE' AND sr.simulation_id = ?
    GROUP BY sr.outlet_node
)
SELECT un.node_id, COALESCE(na.direct_area, 0) AS direct_area
FROM upstream_nodes un
LEFT JOIN node_areas na ON na.node_id = un.node_id;
```

*Validate network connectivity (find disconnected nodes):*

```sql
SELECT n.node_id, n.node_type
FROM nodes n
WHERE n.simulation_id = ?
  AND n.node_id NOT IN (
      SELECT from_node FROM node_links WHERE simulation_id = ?
      UNION
      SELECT to_node FROM node_links WHERE simulation_id = ?
  )
  AND n.node_type != 'OUTFALL';  -- orphaned outfalls are valid if no links exist yet
```

#### 3.2.5 Supporting Definition Tables

**`curves`** -- Curve Definitions (storage, pump, tidal, etc.)

```sql
CREATE TABLE curves (
    fid            INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id  TEXT NOT NULL REFERENCES simulations(simulation_id),
    curve_id       TEXT NOT NULL,
    curve_type     TEXT NOT NULL,  -- STORAGE | PUMP1..4 | TIDAL | DIVERSION | RATING | SHAPE | ...
    x_value        REAL NOT NULL,
    y_value        REAL NOT NULL,
    ordinal        INTEGER NOT NULL  -- preserves point ordering
);
CREATE INDEX idx_curves_lookup ON curves(simulation_id, curve_id, ordinal);
```

**`input_timeseries`** -- Model-defined timeseries (rainfall patterns, inflow hydrographs, etc.)

```sql
CREATE TABLE input_timeseries (
    fid            INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id  TEXT NOT NULL REFERENCES simulations(simulation_id),
    series_id      TEXT NOT NULL,
    timestamp      TEXT NOT NULL,  -- ISO 8601 or elapsed time
    value          REAL NOT NULL,
    ordinal        INTEGER NOT NULL
);
CREATE INDEX idx_input_ts_lookup ON input_timeseries(simulation_id, series_id, ordinal);
```

**`patterns`** -- Temporal patterns (monthly, daily, hourly, weekend)

```sql
CREATE TABLE patterns (
    fid            INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id  TEXT NOT NULL REFERENCES simulations(simulation_id),
    pattern_id     TEXT NOT NULL,
    pattern_type   TEXT NOT NULL,  -- MONTHLY | DAILY | HOURLY | WEEKEND
    ordinal        INTEGER NOT NULL,
    factor         REAL NOT NULL
);
```

**`pollutants`** -- Pollutant definitions

```sql
CREATE TABLE pollutants (
    fid            INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id  TEXT NOT NULL REFERENCES simulations(simulation_id),
    pollutant_id   TEXT NOT NULL,
    units          TEXT NOT NULL,  -- MG/L | UG/L | #/L
    rain_conc      REAL,
    gw_conc        REAL,
    ii_conc        REAL,
    decay_coeff    REAL,
    snow_only      INTEGER,
    co_pollutant   TEXT,
    co_fraction    REAL,
    UNIQUE(simulation_id, pollutant_id)
);
```

**`transects`** -- Irregular cross-section data

```sql
CREATE TABLE transects (
    fid            INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id  TEXT NOT NULL REFERENCES simulations(simulation_id),
    transect_id    TEXT NOT NULL,
    station        REAL NOT NULL,
    elevation      REAL NOT NULL,
    ordinal        INTEGER NOT NULL,
    -- NC line values
    n_left         REAL,
    n_right        REAL,
    n_channel      REAL,
    -- GR line overbank stations
    left_overbank  REAL,
    right_overbank REAL
);
```

---

### 3.3 Part B: Simulation Results & Report Schema

These tables store simulation outputs -- per-timestep results, summary statistics, and report attributes. Each simulation run is registered in the `simulations` table and identified by a unique `simulation_id`. Multiple runs (scenarios, calibration iterations, different engine versions) coexist in the same GeoPackage file. The engine version that produced each run is always recorded.

#### 3.3.1 `simulations` -- Run Registry

Every simulation execution is registered here. This is the root table for all per-run data -- model input features, result timeseries, and summary statistics all reference back to a row in this table via `simulation_id`. Deleting a simulation cascades to all its associated data.

```sql
CREATE TABLE simulations (
    simulation_id  TEXT PRIMARY KEY,  -- UUID or user-supplied ID
    name           TEXT NOT NULL,
    description    TEXT,
    created_at     TEXT NOT NULL,     -- ISO 8601
    engine_version TEXT NOT NULL,     -- e.g., "6.1.0" -- always recorded for reproducibility
    engine_build   TEXT,              -- e.g., "Release", commit hash, or build ID
    start_date     TEXT,              -- simulation start (ISO 8601)
    end_date       TEXT,              -- simulation end   (ISO 8601)
    report_step    REAL,              -- seconds
    wet_step       REAL,              -- seconds
    dry_step       REAL,              -- seconds
    routing_step   REAL,              -- seconds
    routing_model  TEXT,              -- DYNWAVE | KINWAVE | STEADY
    inp_hash       TEXT,              -- SHA-256 of source .inp (provenance)
    status         TEXT DEFAULT 'created',  -- created | running | completed | failed
    elapsed_wall_time REAL,           -- wall-clock seconds for the run
    -- Continuity errors (report attributes)
    continuity_error_runoff  REAL,
    continuity_error_flow    REAL,
    continuity_error_quality REAL
);
```

#### 3.3.2 Variable Catalog (ODM-inspired)

Central registry of all measurable/computed quantities. Shared by simulation results and observed data.

```sql
CREATE TABLE variables (
    variable_id    INTEGER PRIMARY KEY AUTOINCREMENT,
    name           TEXT NOT NULL,       -- e.g., "depth", "flow", "TSS_concentration"
    object_type    TEXT NOT NULL,       -- NODE | LINK | SUBCATCH | SYSTEM
    category       TEXT NOT NULL,       -- STATE | STAT | QUALITY | CLIMATE
    units          TEXT,                -- e.g., "m", "m3/s", "mg/L"
    description    TEXT,
    UNIQUE(name, object_type)
);
```

Pre-populated with the engine's known output variables:

| object_type | name | category | units |
|-------------|------|----------|-------|
| NODE | depth | STATE | m or ft |
| NODE | head | STATE | m or ft |
| NODE | volume | STATE | m3 or ft3 |
| NODE | lateral_inflow | STATE | CFS or CMS |
| NODE | total_inflow | STATE | CFS or CMS |
| NODE | overflow | STATE | CFS or CMS |
| NODE | max_depth | STAT | m or ft |
| NODE | max_overflow | STAT | CFS or CMS |
| NODE | time_flooded | STAT | hours |
| LINK | flow | STATE | CFS or CMS |
| LINK | depth | STATE | m or ft |
| LINK | velocity | STATE | ft/s or m/s |
| LINK | volume | STATE | m3 or ft3 |
| LINK | froude | STATE | - |
| LINK | max_flow | STAT | CFS or CMS |
| LINK | max_velocity | STAT | ft/s or m/s |
| LINK | max_filling | STAT | fraction |
| LINK | time_surcharged | STAT | hours |
| SUBCATCH | rainfall | STATE | in/hr or mm/hr |
| SUBCATCH | runoff | STATE | CFS or CMS |
| SUBCATCH | evap_loss | STATE | in or mm |
| SUBCATCH | infil_loss | STATE | in or mm |
| SUBCATCH | gw_flow | STATE | CFS or CMS |
| SUBCATCH | precip_volume | STAT | volume |
| SUBCATCH | runoff_volume | STAT | volume |
| SYSTEM | air_temp | CLIMATE | deg F or C |
| SYSTEM | rainfall | CLIMATE | in/hr or mm/hr |

#### 3.3.3 Simulation Result Timeseries

Single long-format table for all per-timestep simulation output. This is the core of the relational timeseries model.

```sql
CREATE TABLE result_timeseries (
    simulation_id  TEXT NOT NULL REFERENCES simulations(simulation_id),
    object_type    TEXT NOT NULL,   -- NODE | LINK | SUBCATCH | SYSTEM
    object_id      TEXT NOT NULL,   -- node_id, link_id, subcatch_id, or "SYSTEM"
    variable_id    INTEGER NOT NULL REFERENCES variables(variable_id),
    elapsed_time   REAL NOT NULL,   -- seconds from simulation start
    value          REAL NOT NULL
);

-- Composite index for efficient querying
CREATE INDEX idx_result_ts_obj
    ON result_timeseries(simulation_id, object_type, object_id, variable_id, elapsed_time);

-- Index for variable-centric queries (e.g., "all nodes' depth at time T")
CREATE INDEX idx_result_ts_var
    ON result_timeseries(simulation_id, variable_id, elapsed_time);
```

#### 3.3.4 Summary Statistics & Report Attributes

Per-object summary statistics written at simulation end. Stored separately from timeseries for fast report-style queries.

```sql
CREATE TABLE result_summary (
    simulation_id  TEXT NOT NULL REFERENCES simulations(simulation_id),
    object_type    TEXT NOT NULL,
    object_id      TEXT NOT NULL,
    variable_id    INTEGER NOT NULL REFERENCES variables(variable_id),
    value          REAL NOT NULL,
    PRIMARY KEY (simulation_id, object_type, object_id, variable_id)
);
```

---

### 3.4 Part C: Observed / Sensor Data Schema

These tables store independent measured timeseries from real-world sensors, gauges, and monitoring stations. They are **not tied to any simulation run** -- they persist across runs and serve as calibration targets, validation datasets, and comparison baselines. Observed series can optionally be associated with model objects (nodes, links, subcatchments) for automated sim-vs-observed comparison.

#### 3.4.1 Observed / Sensor Data

**`observed_series`** -- Series metadata and object association

```sql
CREATE TABLE observed_series (
    series_id      INTEGER PRIMARY KEY AUTOINCREMENT,
    name           TEXT NOT NULL UNIQUE,  -- e.g., "USGS_01585200_flow"
    description    TEXT,
    source         TEXT,          -- e.g., "USGS NWIS", "City SCADA", "Field sensor #42"
    source_id      TEXT,          -- external identifier (USGS site number, sensor serial, etc.)
    variable_id    INTEGER NOT NULL REFERENCES variables(variable_id),

    -- Optional association to a model object (NULL if unlinked)
    object_type    TEXT,          -- NODE | LINK | SUBCATCH | NULL
    object_id      TEXT,          -- model object ID or NULL

    -- Metadata
    units          TEXT,
    time_zone      TEXT,          -- IANA tz (e.g., "America/New_York")
    collection_method TEXT,       -- e.g., "continuous", "grab_sample"
    start_date     TEXT,          -- ISO 8601
    end_date       TEXT,          -- ISO 8601
    record_count   INTEGER
);
```

**`observed_values`** -- Measured data points

```sql
CREATE TABLE observed_values (
    series_id      INTEGER NOT NULL REFERENCES observed_series(series_id),
    timestamp      TEXT NOT NULL,  -- ISO 8601 with timezone
    value          REAL NOT NULL,
    quality_flag   TEXT,           -- e.g., "A" (approved), "P" (provisional), "E" (estimated)
    qualifier      TEXT            -- free-text qualifier (e.g., "ice-affected", "dry")
);

CREATE INDEX idx_obs_values_lookup
    ON observed_values(series_id, timestamp);
```

---

## 4. Query Patterns

The relational design enables key workflows with straightforward SQL:

### 4.1 Plot simulated vs. observed at a node

```sql
SELECT
    r.elapsed_time,
    r.value AS simulated,
    o.value AS observed
FROM result_timeseries r
JOIN observed_series os ON os.object_type = r.object_type
                       AND os.object_id  = r.object_id
                       AND os.variable_id = r.variable_id
JOIN observed_values o  ON o.series_id = os.series_id
JOIN simulations s      ON s.simulation_id = r.simulation_id
WHERE r.simulation_id = ?
  AND r.object_type = 'NODE'
  AND r.object_id = 'J1'
  AND r.variable_id = (SELECT variable_id FROM variables WHERE name='depth' AND object_type='NODE')
  -- Align timestamps
  AND abs(julianday(o.timestamp) - julianday(s.start_date, '+' || r.elapsed_time || ' seconds')) < 0.0001;
```

### 4.2 Compare two simulation runs

```sql
SELECT
    a.object_id,
    a.elapsed_time,
    a.value AS run_a,
    b.value AS run_b,
    (b.value - a.value) AS delta
FROM result_timeseries a
JOIN result_timeseries b ON  b.object_type = a.object_type
                         AND b.object_id   = a.object_id
                         AND b.variable_id = a.variable_id
                         AND b.elapsed_time = a.elapsed_time
WHERE a.simulation_id = 'run_baseline'
  AND b.simulation_id = 'run_calibrated'
  AND a.variable_id = ?;
```

### 4.3 Nash-Sutcliffe Efficiency for calibration

```sql
WITH paired AS (
    SELECT r.value AS sim, o.value AS obs
    FROM result_timeseries r
    JOIN observed_values o ON ...  -- timestamp alignment as above
    WHERE r.simulation_id = ? AND r.object_id = ? AND r.variable_id = ?
),
stats AS (
    SELECT AVG(obs) AS mean_obs FROM paired
)
SELECT 1.0 - (SUM((sim - obs)*(sim - obs)) / SUM((obs - mean_obs)*(obs - mean_obs))) AS nse
FROM paired, stats;
```

### 4.4 Spatial query -- all flooded nodes

```sql
SELECT n.node_id, n.geom, rs.value AS max_depth
FROM nodes n
JOIN result_summary rs ON rs.object_id = n.node_id
                      AND rs.simulation_id = n.simulation_id
                      AND rs.object_type = 'NODE'
JOIN variables v ON v.variable_id = rs.variable_id AND v.name = 'max_depth'
WHERE n.simulation_id = ?
  AND rs.value > n.max_depth;
```

---

## 5. Implementation Plan

### Phase 1: Core Schema & Model Input (GeoPackageWriter / GeoPackageReader)

**Goal**: Round-trip a SWMM model through GeoPackage -- read `.inp`, write `.gpkg`, read `.gpkg` back.

| Component | Description |
|-----------|-------------|
| `GeoPackageSchema` | Schema creation and migration utilities. Registers tables with GeoPackage metadata (`gpkg_contents`, `gpkg_geometry_columns`, `gpkg_spatial_ref_sys`). |
| `GeoPackageWriter` | Serializes `SimulationContext` to GeoPackage feature and attribute tables. Maps `SpatialFrame` CRS to `gpkg_spatial_ref_sys`. |
| `GeoPackageReader` | Reads a `.gpkg` and populates `SimulationContext` identically to `InputReader`. Implements `IInputPlugin`. |
| `GeoPackageInputPlugin` | Plugin wrapper implementing `IInputPlugin` lifecycle. Detects `.gpkg` extension and delegates to `GeoPackageReader`. |

**Dependencies**: SQLite3 (already available on all platforms), spatialite or header-only GeoPackage geometry encoding (WKB).

**Geometry Encoding**: Use standard GeoPackage Binary (ISO WKB with GeoPackage header). No spatialite extension required for basic point/line/polygon storage. Geometry construction from `SpatialFrame` arrays:
- Nodes -> POINT from `node_x[i]`, `node_y[i]`
- Links -> LINESTRING from `[node1_coords, ...vertices..., node2_coords]`
- Subcatchments -> POLYGON from `subcatch_polygon_x[i][]`, `subcatch_polygon_y[i][]`

### Phase 2: Simulation Output

**Goal**: Write per-timestep results and summary statistics during simulation.

| Component | Description |
|-----------|-------------|
| `GeoPackageOutputPlugin` | Implements `IOutputPlugin`. On `initialize()`, creates a `simulations` row and pre-populates `variables`. On `update()`, batch-inserts `result_timeseries` rows. On `finalize()`, writes `result_summary` and updates simulation status. |
| Batch insert strategy | Buffer N timesteps in memory (configurable, default 100), then flush via SQLite transaction. Keeps write amplification low while bounding memory. |
| Variable registry | On first run, insert all known variables into `variables` table. On subsequent runs, reuse existing variable IDs (idempotent UPSERT). |

**Performance considerations**:
- Wrap each flush in `BEGIN IMMEDIATE ... COMMIT` for WAL-mode safety.
- Use `PRAGMA journal_mode=WAL` for concurrent read during simulation.
- Use `PRAGMA synchronous=NORMAL` during simulation, `FULL` on finalize.
- Prepared statement reuse across timesteps.

### Phase 3: Report Plugin

**Goal**: Write summary report data as queryable tables instead of (or in addition to) the text `.rpt` file.

| Component | Description |
|-----------|-------------|
| `GeoPackageReportPlugin` | Implements `IReportPlugin`. Populates `result_summary` with all node/link/subcatch statistics. Writes continuity errors to `simulations` row. |

### Phase 4: Observed Data & Calibration Support

**Goal**: Import, associate, and query observed timeseries for calibration workflows.

| Component | Description |
|-----------|-------------|
| `ObservedDataImporter` | Standalone utility (not a plugin) that imports CSV, USGS NWIS, or other formats into `observed_series` + `observed_values`. |
| Object association API | Functions to link/unlink an `observed_series` to a model `object_type`/`object_id`. |
| Calibration query helpers | Utility functions that execute the sim-vs-observed join queries and compute goodness-of-fit metrics (NSE, RMSE, PBIAS). |

### Phase 5: CLI & Python Bindings

| Component | Description |
|-----------|-------------|
| CLI flags | `--output-gpkg <path>` to enable GeoPackage output alongside or instead of binary `.out`. `--input <file.gpkg>` to read model from GeoPackage. |
| Python API | Extend the Python bindings to expose `read_gpkg()` / `write_gpkg()` and observed data import. |

---

## 6. File Organization

```
src/engine/input/geopackage/
├── STRATEGY.md                  (this document)
├── GeoPackageSchema.hpp/.cpp    -- DDL, migrations, GeoPackage metadata registration
├── GeoPackageWriter.hpp/.cpp    -- SimulationContext -> GeoPackage (model definition)
├── GeoPackageReader.hpp/.cpp    -- GeoPackage -> SimulationContext (model definition)
├── GeoPackageOutputPlugin.hpp/.cpp  -- IOutputPlugin (timestep results)
├── GeoPackageReportPlugin.hpp/.cpp  -- IReportPlugin (summary statistics)
├── GeoPackageInputPlugin.hpp/.cpp   -- IInputPlugin wrapper
├── ObservedDataImporter.hpp/.cpp    -- Observed series import utilities
├── GpkgGeometry.hpp             -- GeoPackage Binary encoding/decoding (WKB)
└── GpkgUtils.hpp                -- SQLite helpers, prepared statement RAII, etc.
```

---

## 7. Key Design Decisions

### 7.1 Why a single long timeseries table?

A wide table (one column per variable) would require schema changes when pollutants are added and makes cross-variable queries awkward. The long-format `(object, variable, time, value)` design:
- Scales to arbitrary variable counts without DDL changes
- Enables uniform indexing and query patterns
- Follows the ODM "observation = value + variable + site + time" paradigm
- Trades some query verbosity for schema stability

### 7.2 Why separate `observed_values` from `result_timeseries`?

- Observed data has no `simulation_id` -- it exists independently of any model run.
- Observed data carries quality metadata (`quality_flag`, `qualifier`) that simulation results do not need.
- Separation keeps the result table lean and avoids polluting calibration targets with simulation artifacts.
- Join queries between the two are straightforward (see Section 4).

### 7.3 Why `elapsed_time` (REAL) for results but `timestamp` (TEXT) for observations?

- Simulation results are naturally indexed by elapsed seconds from a known start time. This avoids repeated datetime parsing and enables simple arithmetic.
- Observed data comes with absolute timestamps from real-world sources. Storing the original timestamp preserves timezone information and avoids lossy conversion.
- Alignment between the two is done at query time using the simulation's `start_date`.

### 7.4 Why duplicate spatial geometry per simulation?

Each simulation run stores its own copy of node/link/subcatch geometry. This supports:
- Scenario management where geometry differs between runs (e.g., adding a new pipe).
- Immutable run records -- a completed simulation's data is self-contained.
- The storage cost is minimal (geometry is small relative to timeseries).

### 7.5 Why not use SpatiaLite?

SpatiaLite adds a runtime dependency and is not available on all platforms. GeoPackage Binary geometry encoding is simple (WKB with a 8-byte header) and sufficient for the point/line/polygon geometries SWMM uses. Spatial indexing via `gpkg_rtree_*` triggers is part of the GeoPackage standard and uses SQLite's built-in R-Tree module.

---

## 8. Migration & Versioning

The GeoPackage will include a `schema_version` entry in `gpkg_extensions`:

```sql
INSERT INTO gpkg_extensions (table_name, column_name, extension_name, definition, scope)
VALUES (NULL, NULL, 'openswmm_schema_version', 'v1.0.0', 'read-write');
```

Future schema changes will increment this version and include migration SQL in `GeoPackageSchema`. The reader will check this version on open and apply migrations as needed.

---

## 9. Testing Strategy

| Level | Scope |
|-------|-------|
| Unit | Schema creation, geometry encoding/decoding, single-table read/write round-trips |
| Integration | Full `.inp` -> GeoPackage -> `SimulationContext` round-trip; verify identical simulation results |
| Regression | Run the `site_drainage_model` test case with GeoPackage output; compare summary statistics against legacy `.out` |
| Observed data | Import a sample CSV of observed flow data, link to a node, verify sim-vs-observed query |
| Performance | Benchmark GeoPackage output vs. binary `.out` for the standard test suite; target < 2x overhead |
