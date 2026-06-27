#!/usr/bin/env python3
"""
create_schema_review_db.py
==========================
Creates a GeoPackage-compliant SQLite database (``docs/openswmm_schema_review.gpkg``)
containing the complete OpenSWMM schema so it can be inspected in
DB Browser for SQLite (sqlitebrowser), plus a readable SQL dump
(``docs/openswmm_schema_review.sql``) annotated with the ontology overlay.

The DDL is EXTRACTED from the C++ source of truth
(``src/engine/input/geopackage/GeoPackageSchema.cpp``) at run time — the
raw-string blocks (GPKG_METADATA_DDL, PART_A_DDL … MESH_2D_DDL) and the
default-variable vocabulary are parsed out of the .cpp, so this script can
never drift from the implementation again. Re-run it after any schema change:

    python python/create_schema_review_db.py

The resulting file can be opened with:
    sqlitebrowser docs/openswmm_schema_review.gpkg
"""

import re
import sqlite3
from pathlib import Path

REPO_ROOT  = Path(__file__).resolve().parent.parent
SCHEMA_CPP = REPO_ROOT / "src" / "engine" / "input" / "geopackage" / "GeoPackageSchema.cpp"

# Order + display titles of the DDL blocks defined in GeoPackageSchema.cpp.
DDL_BLOCKS = [
    ("GPKG_METADATA_DDL", "OGC GeoPackage Container Metadata"),
    ("PART_A_DDL",        "Part A – Model Input (physical system & forcing)"),
    ("PART_B_DDL",        "Part B – Simulation Runs & Results (provenance + observations)"),
    ("PART_C_DDL",        "Part C – Observed / Sensor Data"),
    ("PART_D_DDL",        "Part D – External-File Content (state snapshots & interface exchanges)"),
    ("MESH_2D_DDL",       "Part E – 2D Surface-Routing Mesh (model definition)"),
]

# ============================================================================
# Extraction from the C++ source of truth
# ============================================================================

def extract_ddl_blocks(cpp_text: str) -> dict:
    """Parse every `static const char* NAME = R"SQL( ... )SQL";` block."""
    blocks = {}
    for m in re.finditer(
        r'static\s+const\s+char\*\s+(\w+)\s*=\s*R"SQL\((.*?)\)SQL"\s*;',
        cpp_text, re.DOTALL,
    ):
        blocks[m.group(1)] = m.group(2).strip()
    return blocks


def extract_default_variables(cpp_text: str):
    """Parse the VarDef defs[] initializer in populate_default_variables."""
    m = re.search(r"static\s+const\s+VarDef\s+defs\[\]\s*=\s*\{(.*?)\n\s*\};",
                  cpp_text, re.DOTALL)
    if not m:
        raise RuntimeError("VarDef defs[] not found in GeoPackageSchema.cpp")
    rows = re.findall(
        r'\{\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\}',
        m.group(1),
    )
    if not rows:
        raise RuntimeError("No VarDef rows parsed from GeoPackageSchema.cpp")
    return rows


# ── GeoPackage feature-layer registrations ──────────────────────────────────
# Mirrors the register_feature_table() calls made at write time:
#   - GeoPackageWriter.cpp  write_model()   → nodes/links/subcatchments/rain_gages
#   - GeoPackageWriter.cpp  write_mesh_2d() → mesh_2d_vertices/mesh_2d_triangles
# (mesh layers are registered only when a 2D mesh is actually written).
FEATURE_TABLES = [
    # (table_name, geom_type, identifier, description)
    ("nodes",            "POINT",        "nodes",            "SWMM drainage nodes (junctions, outfalls, dividers, storage)"),
    ("links",            "LINESTRING",   "links",            "SWMM conduits, pumps, orifices, weirs, outlets"),
    ("subcatchments",    "MULTIPOLYGON", "subcatchments",    "SWMM subcatchment drainage areas"),
    ("rain_gages",       "POINT",        "rain_gages",       "Precipitation measurement stations"),
    ("mesh_2d_vertices", "POINT",        "2D Mesh Vertices", "2D surface-routing mesh vertices"),
    ("mesh_2d_triangles","POLYGON",      "2D Mesh Triangles","2D surface-routing mesh cells"),
]

# ============================================================================
# Ontology overlay (documentation layer emitted into the SQL dump)
# ============================================================================

ONTOLOGY_HEADER = """\
-- ════════════════════════════════════════════════════════════════════════
-- ARCHITECTURE — A LAYERED DATA MODEL WITH AN OVERLAYING ONTOLOGY
-- ════════════════════════════════════════════════════════════════════════
--
-- The OpenSWMM GeoPackage is best read as four stacked layers. The lower
-- two are physical/normative; the upper two give the rows their meaning.
--
--   L0  PHYSICAL STORE      SQLite: WAL journal, PRAGMA foreign_keys=ON on
--                           every connection, model writes wrapped in one
--                           BEGIN IMMEDIATE transaction (RAII, rollback on
--                           any failure) → a save is atomic or absent.
--
--   L1  CONTAINER STANDARD  OGC GeoPackage 1.x (12-128r18): gpkg_contents /
--                           gpkg_geometry_columns / gpkg_spatial_ref_sys
--                           form a self-describing catalog, so any GIS
--                           (QGIS, GDAL/OGR) discovers the feature layers
--                           without OpenSWMM-specific knowledge.
--
--   L2  LOGICAL DATA MODEL  The relational tables below (Parts A–E).
--                           Every model row is scoped by simulation_id, so
--                           one container holds many model versions/runs.
--                           Composite FKs with ON DELETE/UPDATE CASCADE
--                           encode the domain's existential dependencies
--                           (delete a node → its couplings, hotstart state
--                           and interface rows go with it; rename a node →
--                           every reference follows).
--
--   L3  ONTOLOGY OVERLAY    The conceptual reading of L2, aligned with
--                           public ontologies. It is partially MATERIALIZED
--                           in the data itself:
--                             • `variables`     = the observable-property
--                               vocabulary (a small in-database ontology);
--                             • `options`       = reified configuration
--                               statements (key→value about a simulation);
--                             • `gpkg_contents` = a dataset catalog
--                               (dcat:Dataset-like entries).
--                           The rest of the overlay is interpretive and is
--                           documented per table in this file.
--
-- ONTOLOGY ALIGNMENT (prefixes used in the per-table annotations)
--   geo:   OGC GeoSPARQL          — geo:Feature, geo:hasGeometry
--   sosa:  W3C/OGC SOSA/SSN       — Observation, ObservableProperty,
--                                    Sensor/Platform, Procedure, FeatureOfInterest
--   prov:  W3C PROV-O             — Activity, Entity, Agent, wasDerivedFrom
--   qudt:  QUDT                   — units of measure (variables.units)
--   ugrid: UGRID conventions      — 2D unstructured mesh topology
--   hyf:   OGC HY_Features        — HY_Catchment and hydrologic realizations
--   swmm:  OpenSWMM domain terms  — Node, Link, Subcatchment, BoundaryCondition…
--
-- CORE CONCEPTUAL READING
--   • Identity: every domain individual is named by the composite key
--     (simulation_id, <local_id>) — i.e. a scoped IRI of the form
--     swmm://{simulation}/{Class}/{id}. The simulation_id partition is the
--     named graph: model variants coexist without interference.
--   • Class hierarchies use single-table inheritance with a discriminator
--     column (nodes.node_type → Junction|Outfall|Divider|Storage;
--     links.link_type → Conduit|Pump|Orifice|Weir|Outlet). Subtype-specific
--     columns are NULL for other subtypes.
--   • Object properties are FK columns (links.from_node → swmm:fromNode) or
--     reified relation tables when the relation carries attributes or needs
--     fast graph traversal (node_links, subcatch_routing, lid_usage,
--     mesh_2d_*_coupling).
--   • Observations follow SOSA: an Observation has a FeatureOfInterest
--     ((object_type, object_id)), an ObservableProperty (variable_id), a
--     result (value [+ units via the vocabulary]), a phenomenonTime
--     (elapsed_time / timestamp), and was made by a Procedure execution —
--     either a simulation (Part B) or a physical sensor (Part C). Sharing
--     ONE variables vocabulary across Parts B and C is what makes
--     model-vs-observed comparison well-defined by construction.
--   • Provenance follows PROV: a `simulations` row is a prov:Activity
--     executed by a prov:SoftwareAgent (engine_version/engine_build) that
--     prov:used the Part A model definition (fingerprinted by inp_hash) and
--     generated the Part B/D entities. Hotstart slots are prov:Entity state
--     snapshots that later activities can prov:used.
--   • 2D RESULTS ARE NOT STORED HERE by design: per-timestep 2D fields
--     stream to a CF/UGRID HDF5 file (more performant for dense
--     mesh-time-series). The GeoPackage stores the 2D MODEL DEFINITION
--     (Part E) and the dataset link (`options` key 2D_OUTPUT_FILE) — i.e.
--     the HDF5 file is an external dcat:Distribution of this dataset.
"""

# Short ontology preamble emitted above each part's DDL.
PART_ONTOLOGY = {
    "GPKG_METADATA_DDL": """\
-- Ontology overlay: the catalog layer. gpkg_contents enumerates datasets
-- (dcat:Dataset-like rows: features vs attributes), gpkg_geometry_columns
-- types each geo:hasGeometry property, and gpkg_spatial_ref_sys is the CRS
-- registry every geometry is interpreted against.""",

    "PART_A_DDL": """\
-- Ontology overlay: the PHYSICAL-SYSTEM ASSET MODEL — "what exists".
--   nodes           swmm:Node ⊑ geo:Feature (POINT). Discriminator
--                   node_type → swmm:Junction | swmm:Outfall | swmm:Divider
--                   | swmm:Storage (single-table inheritance).
--   links           swmm:Link ⊑ geo:Feature (LINESTRING); link_type →
--                   Conduit|Pump|Orifice|Weir|Outlet. Object properties
--                   swmm:fromNode / swmm:toNode (columns from_node/to_node).
--   node_links      the reified DIRECTED-GRAPH EDGE relation — the network
--                   topology overlay, independent of geometry, indexed both
--                   ways for graph traversal (upstream/downstream queries).
--   subcatchments   hyf:HY_Catchment ⊑ geo:Feature (MULTIPOLYGON);
--   subcatch_routing reifies swmm:drainsTo (node, subcatchment, or self).
--   rain_gages      sosa:Platform/Sensor supplying forcing; subcatchments
--                   reference them via rain_gage (swmm:forcedBy).
--   curves          swmm:FunctionalRelation — ordered (x, y) samples of a
--                   rating/pump/storage/shape function (ordinal preserves
--                   the function's domain ordering).
--   patterns        swmm:TemporalPattern (periodic multipliers).
--   input_timeseries forcing series; with source/source_filename/
--                   source_column ≈ prov:wasDerivedFrom for series that
--                   were internalized from external files.
--   pollutants      swmm:Constituent; units → qudt:Unit.
--   lid_controls    swmm:LIDControl design spec (layer_type discriminated);
--   lid_usage       qualified association LIDControl×Subcatchment with
--                   placement attributes.
--   treatment       swmm:TreatmentFunction on (node, pollutant) — an
--                   expression-valued relation.
--   transects       channel geometry sampled by station (geometry of the
--                   IRREGULAR cross-section function).
--   options         reified configuration statements about the simulation
--                   (includes the 2D_* solver keys and the 2D_OUTPUT_FILE
--                   dataset link — see Part E).""",

    "PART_B_DDL": """\
-- Ontology overlay: SIMULATION PROVENANCE + COMPUTED OBSERVATIONS.
--   simulations       prov:Activity (one run) executed by a
--                     prov:SoftwareAgent (engine_version/engine_build),
--                     prov:used the Part A definition (inp_hash =
--                     fingerprint of the used prov:Entity); lifecycle in
--                     status; QA terms (continuity errors) attached.
--   variables         the OBSERVABLE-PROPERTY VOCABULARY — the materialized
--                     fragment of the ontology: sosa:ObservableProperty
--                     rows with qudt-style units, scoped by the class of
--                     feature they apply to (object_type).
--   result_timeseries sosa:Observation — hasFeatureOfInterest =
--                     (object_type, object_id), observedProperty =
--                     variable_id, phenomenonTime = elapsed_time,
--                     madeBy the simulation Activity.
--   result_summary    aggregate observations (per-run statistics).""",

    "PART_C_DDL": """\
-- Ontology overlay: FIELD OBSERVATIONS (the measured counterpart of B).
--   observed_series  sosa:ObservationCollection produced by an external
--                    sosa:Sensor/source; optionally bound to a model
--                    feature-of-interest (object_type, object_id) and ALWAYS
--                    to the same variables vocabulary as Part B — which is
--                    what makes model-vs-observed joins semantically sound.
--   observed_values  sosa:Observation with result quality annotation
--                    (quality_flag, qualifier).""",

    "PART_D_DDL": """\
-- Ontology overlay: STATE SNAPSHOTS & INTERFACE EXCHANGES (replaces opaque
-- external files with relational, FK-checked content).
--   hotstart_slots / hotstart_*_state / hotstart_*_pollutant_state
--                    prov:Entity SYSTEM-STATE SNAPSHOTS of a simulation
--                    Activity (direction USE/SAVE = consumed vs generated).
--                    The FK chain slot → per-object state → per-pollutant
--                    state makes a snapshot existentially dependent on the
--                    objects it describes (delete/rename cascades).
--   raingage_data / climate_data
--                    internalized forcing observations (sosa:Observation
--                    streams) bound to their gage / simulation.
--   routing_interface_node / _subcatch / _gage / _node_pollutants
--                    boundary-exchange series between model activities
--                    (the relational form of SWMM routing interface files),
--                    typed by role and direction.""",

    "MESH_2D_DDL": """\
-- Ontology overlay: the 2D SURFACE-ROUTING MESH — a ugrid-style
-- unstructured-mesh model definition that is ALSO published as GIS layers.
--   mesh_2d_vertices    ugrid mesh NODES (POINT feature layer). x/y/z are
--                       canonical; geom is a derived presentation copy
--                       (ignored on read).
--   mesh_2d_triangles   ugrid mesh FACES (POLYGON feature layer). The
--                       canonical topology is the face→node connectivity
--                       (v0,v1,v2 ordinals ≈ ugrid face_node_connectivity);
--                       geom/bed_elev/coupled_node are derived styling
--                       columns. Derived adjacency (neighbours, areas, edge
--                       geometry) is intentionally NOT stored — the engine
--                       rebuilds it deterministically at initialize(), so
--                       the persisted form cannot drift from the solver.
--   mesh_2d_boundary_conditions
--                       swmm:BoundaryCondition on a (face, local-edge) pair:
--                       typed (WALL | NORMAL_FLOW | SPECIFIED_STAGE/TS_STAGE
--                       | SPECIFIED_FLOW/TS_FLOW | RATING_CURVE) with either
--                       a literal parameter or a reference to a forcing
--                       series/curve (ref_name), plus an optional group.
--   mesh_2d_edge_conveyance
--                       per-edge transmissivity ψ ∈ [0,1] on the undirected
--                       vertex pair (the integral-porosity edge factor);
--                       interior-edge mirroring is re-derived by the engine.
--   mesh_2d_vertex_coupling / mesh_2d_triangle_coupling
--                       the 1D↔2D BRIDGE: swmm:couplesTo object property
--                       from mesh entities to drainage nodes, with discharge
--                       coefficient + exchange area. Hard composite FKs to
--                       nodes(simulation_id, node_id) keep the bridge
--                       referentially intact under node rename/delete.
--   NOTE — 2D results:  per-timestep 2D fields are NEVER stored in the
--                       GeoPackage; they stream to the CF/UGRID HDF5 file
--                       referenced by the `options` key 2D_OUTPUT_FILE
--                       (an external distribution of this dataset).
--   Units:              coordinates are stored in the AUTHORED project/map
--                       units (same plane as nodes/links, so GIS layers
--                       align); the authored-units flag round-trips via the
--                       `options` key 2D_MESH_UNITS_SI.""",
}

TABLE_ONTOLOGY_MAP = """\
-- ────────────────────────────────────────────────────────────
-- TABLE ↔ ONTOLOGY QUICK REFERENCE
-- ────────────────────────────────────────────────────────────
-- table                          class / role                        key relations (object properties)
-- ------------------------------ ----------------------------------- -------------------------------------------
-- gpkg_contents                  dataset catalog (dcat-like)         describes feature/attribute tables
-- gpkg_geometry_columns          geometry typing                     geo:hasGeometry range per table
-- gpkg_spatial_ref_sys           CRS registry                        SRS of every geometry
-- options                        reified configuration               about → simulations
-- nodes                          swmm:Node ⊑ geo:Feature             subtype by node_type
-- links                          swmm:Link ⊑ geo:Feature             swmm:fromNode / swmm:toNode → nodes
-- node_links                     directed graph edge (topology)      reifies link→(from,to) for traversal
-- subcatchments                  hyf:HY_Catchment ⊑ geo:Feature      swmm:forcedBy → rain_gages
-- subcatch_routing               swmm:drainsTo (reified)             → nodes | subcatchments
-- rain_gages                     sosa:Platform/Sensor                supplies forcing
-- curves / patterns              functional relation / pattern       referenced by name from features
-- input_timeseries               forcing series                      prov:wasDerivedFrom (source columns)
-- pollutants                     swmm:Constituent                    units → qudt:Unit
-- lid_controls / lid_usage       LID spec / qualified placement      lid_usage: subcatch × control
-- rdii_* / unit_hydrographs      RDII response functions             node × UH × gage
-- treatment                      treatment expression                node × pollutant
-- transects                      channel section geometry            sampled by station
-- simulations                    prov:Activity + sosa:Procedure run  agent = engine; used = model (inp_hash)
-- variables                      sosa:ObservableProperty VOCABULARY  shared by Parts B and C
-- result_timeseries              sosa:Observation (computed)         FoI=(object_type,object_id); prop=variable
-- result_summary                 aggregate observation               same axes, end-of-run
-- observed_series/_values        sosa:Observation (measured)         same vocabulary → comparable to results
-- hotstart_*                     prov:Entity state snapshots         cascade-bound to the objects they describe
-- raingage_data / climate_data   internalized forcing observations   → rain_gages / simulations
-- routing_interface_*            boundary-exchange series            role × direction × object
-- mesh_2d_vertices               ugrid mesh node ⊑ geo:Feature       canonical x/y/z
-- mesh_2d_triangles              ugrid mesh face ⊑ geo:Feature       face_node_connectivity = (v0,v1,v2)
-- mesh_2d_boundary_conditions    swmm:BoundaryCondition              on (face, local edge); param or ref_name
-- mesh_2d_edge_conveyance        edge transmissivity ψ               undirected vertex pair
-- mesh_2d_vertex/triangle_coupling  swmm:couplesTo (1D↔2D bridge)    mesh entity → nodes (FK, CASCADE)
-- (external) 2D results HDF5     dcat:Distribution (CF/UGRID)        linked via options key 2D_OUTPUT_FILE
"""

# ============================================================================
# Builder
# ============================================================================

def build_schema_db(db_path: Path, ddl: dict, variables) -> None:
    if db_path.exists():
        db_path.unlink()

    con = sqlite3.connect(str(db_path))
    con.execute("PRAGMA journal_mode=WAL")
    con.execute("PRAGMA foreign_keys=ON")
    con.execute("PRAGMA application_id=0x47504B47")  # 'GPKG'

    # Execute DDL blocks in the same order create_schema() does
    for name, _title in DDL_BLOCKS:
        con.executescript(ddl[name])

    # Register feature tables in gpkg_contents + gpkg_geometry_columns
    for tname, geom_type, ident, desc in FEATURE_TABLES:
        con.execute(
            "INSERT OR REPLACE INTO gpkg_contents "
            "(table_name, data_type, identifier, description, srs_id) "
            "VALUES (?, 'features', ?, ?, 4326)",
            (tname, ident, desc),
        )
        con.execute(
            "INSERT OR REPLACE INTO gpkg_geometry_columns "
            "(table_name, column_name, geometry_type_name, srs_id, z, m) "
            "VALUES (?, 'geom', ?, 4326, 0, 0)",
            (tname, geom_type),
        )

    # Populate the default observable-property vocabulary
    con.executemany(
        "INSERT OR IGNORE INTO variables (name, object_type, category, units, description) "
        "VALUES (?, ?, ?, ?, ?)",
        variables,
    )

    con.commit()
    con.close()


def generate_sql_dump(sql_path: Path, ddl: dict) -> None:
    """Write the annotated, human-readable .sql review document."""
    lines = [
        "-- ================================================================",
        "-- OpenSWMM GeoPackage Schema — full implementation review",
        "-- Auto-generated by python/create_schema_review_db.py",
        "-- DDL extracted from: src/engine/input/geopackage/GeoPackageSchema.cpp",
        "-- (re-run the script after any schema change; do not edit by hand)",
        "-- ================================================================",
        "",
        ONTOLOGY_HEADER,
        TABLE_ONTOLOGY_MAP,
        "PRAGMA journal_mode=WAL;",
        "PRAGMA foreign_keys=ON;",
        "PRAGMA application_id=0x47504B47; -- 'GPKG'",
        "",
    ]
    for name, title in DDL_BLOCKS:
        lines.append(f"-- {'─'*60}")
        lines.append(f"-- {title}")
        lines.append(f"-- {'─'*60}")
        preamble = PART_ONTOLOGY.get(name)
        if preamble:
            lines.append(preamble)
            lines.append("")
        lines.append(ddl[name])
        lines.append("")

    sql_path.write_text("\n".join(lines), encoding="utf-8")


# ============================================================================
# main
# ============================================================================

def main():
    docs_dir = REPO_ROOT / "docs"
    docs_dir.mkdir(exist_ok=True)

    cpp_text = SCHEMA_CPP.read_text(encoding="utf-8")
    ddl = extract_ddl_blocks(cpp_text)
    missing = [name for name, _ in DDL_BLOCKS if name not in ddl]
    if missing:
        raise RuntimeError(f"DDL blocks not found in {SCHEMA_CPP.name}: {missing}")
    variables = extract_default_variables(cpp_text)

    db_path  = docs_dir / "openswmm_schema_review.gpkg"
    sql_path = docs_dir / "openswmm_schema_review.sql"

    build_schema_db(db_path, ddl, variables)
    print(f"✅  SQLite DB  → {db_path}")
    print(f"   Open with:  sqlitebrowser \"{db_path}\"")

    generate_sql_dump(sql_path, ddl)
    print(f"✅  SQL dump   → {sql_path}")

    # Quick sanity-check: list tables
    con = sqlite3.connect(str(db_path))
    tables = [r[0] for r in con.execute(
        "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"
    )]
    indexes = [r[0] for r in con.execute(
        "SELECT name FROM sqlite_master WHERE type='index' ORDER BY name"
    )]
    var_count = con.execute("SELECT COUNT(*) FROM variables").fetchone()[0]
    con.close()

    print(f"\n   Tables  ({len(tables)}): {', '.join(tables)}")
    print(f"   Indexes ({len(indexes)}): {', '.join(indexes)}")
    print(f"   Default variables seeded: {var_count}")


if __name__ == "__main__":
    main()
