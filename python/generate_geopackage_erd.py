#!/usr/bin/env python3
"""
generate_geopackage_erd.py
==========================
Generates two documentation artefacts from the OpenSWMM GeoPackage schema
(defined in GeoPackageSchema.cpp):

  docs/GEOPACKAGE_SCHEMA_ERD.md      – Mermaid entity-relationship diagram
  docs/GEOPACKAGE_SCHEMA_ERD.drawio  – draw.io (mxGraph) XML diagram

Run from the repository root:
    python python/generate_geopackage_erd.py
"""

import textwrap
import xml.etree.ElementTree as ET
from pathlib import Path

# ---------------------------------------------------------------------------
# Schema definition
# ---------------------------------------------------------------------------

# Column entry: (name, sql_type, key_flags)
#   key_flags: "PK", "FK", "PK,FK", ""
TABLES = [
    # ── OGC GeoPackage standard ────────────────────────────────────────────
    ("gpkg_spatial_ref_sys", "OGC GeoPackage Standard", [
        ("srs_id",                   "INTEGER", "PK"),
        ("srs_name",                 "TEXT",    ""),
        ("organization",             "TEXT",    ""),
        ("organization_coordsys_id", "INTEGER", ""),
        ("definition",               "TEXT",    ""),
        ("description",              "TEXT",    ""),
    ]),
    ("gpkg_contents", "OGC GeoPackage Standard", [
        ("table_name",  "TEXT",    "PK"),
        ("data_type",   "TEXT",    ""),
        ("identifier",  "TEXT",    ""),
        ("description", "TEXT",    ""),
        ("last_change", "TEXT",    ""),
        ("min_x",       "DOUBLE",  ""),
        ("min_y",       "DOUBLE",  ""),
        ("max_x",       "DOUBLE",  ""),
        ("max_y",       "DOUBLE",  ""),
        ("srs_id",      "INTEGER", "FK"),
    ]),
    ("gpkg_geometry_columns", "OGC GeoPackage Standard", [
        ("table_name",         "TEXT",    "PK,FK"),
        ("column_name",        "TEXT",    "PK"),
        ("geometry_type_name", "TEXT",    ""),
        ("srs_id",             "INTEGER", "FK"),
        ("z",                  "TINYINT", ""),
        ("m",                  "TINYINT", ""),
    ]),

    # ── Simulation registry ────────────────────────────────────────────────
    ("simulations", "Part B – Simulation Results", [
        ("simulation_id",            "TEXT", "PK"),
        ("name",                     "TEXT", ""),
        ("description",              "TEXT", ""),
        ("created_at",               "TEXT", ""),
        ("engine_version",           "TEXT", ""),
        ("engine_build",             "TEXT", ""),
        ("start_date",               "TEXT", ""),
        ("end_date",                 "TEXT", ""),
        ("report_step",              "REAL", ""),
        ("wet_step",                 "REAL", ""),
        ("dry_step",                 "REAL", ""),
        ("routing_step",             "REAL", ""),
        ("routing_model",            "TEXT", ""),
        ("inp_hash",                 "TEXT", ""),
        ("status",                   "TEXT", ""),
        ("elapsed_wall_time",        "REAL", ""),
        ("continuity_error_runoff",  "REAL", ""),
        ("continuity_error_flow",    "REAL", ""),
        ("continuity_error_quality", "REAL", ""),
    ]),

    # ── Part A – Model Input ───────────────────────────────────────────────
    ("options", "Part A – Model Input", [
        ("simulation_id", "TEXT", "PK,FK"),
        ("key",           "TEXT", "PK"),
        ("value",         "TEXT", ""),
    ]),
    ("nodes", "Part A – Model Input", [
        ("fid",                   "INTEGER", "PK"),
        ("simulation_id",         "TEXT",    "FK"),
        ("node_id",               "TEXT",    ""),
        ("node_type",             "TEXT",    ""),
        ("geom",                  "BLOB",    ""),
        ("invert_elev",           "REAL",    ""),
        ("max_depth",             "REAL",    ""),
        ("init_depth",            "REAL",    ""),
        ("surcharge_depth",       "REAL",    ""),
        ("ponded_area",           "REAL",    ""),
        ("outfall_type",          "TEXT",    ""),
        ("outfall_stage",         "REAL",    ""),
        ("outfall_has_flap_gate", "INTEGER", ""),
        ("divider_type",          "TEXT",    ""),
        ("divider_cutoff",        "REAL",    ""),
        ("divider_curve",         "TEXT",    ""),
        ("storage_curve",         "TEXT",    ""),
        ("storage_a",             "REAL",    ""),
        ("storage_b",             "REAL",    ""),
        ("storage_c",             "REAL",    ""),
        ("tag",                   "TEXT",    ""),
    ]),
    ("links", "Part A – Model Input", [
        ("fid",              "INTEGER", "PK"),
        ("simulation_id",    "TEXT",    "FK"),
        ("link_id",          "TEXT",    ""),
        ("link_type",        "TEXT",    ""),
        ("geom",             "BLOB",    ""),
        ("from_node",        "TEXT",    "FK"),
        ("to_node",          "TEXT",    "FK"),
        ("offset1",          "REAL",    ""),
        ("offset2",          "REAL",    ""),
        ("xsect_shape",      "TEXT",    ""),
        ("xsect_geom1",      "REAL",    ""),
        ("xsect_geom2",      "REAL",    ""),
        ("xsect_geom3",      "REAL",    ""),
        ("xsect_geom4",      "REAL",    ""),
        ("xsect_barrels",    "INTEGER", ""),
        ("xsect_culvert",    "INTEGER", ""),
        ("xsect_curve",      "TEXT",    "FK"),
        ("roughness",        "REAL",    ""),
        ("length",           "REAL",    ""),
        ("loss_inlet",       "REAL",    ""),
        ("loss_outlet",      "REAL",    ""),
        ("loss_avg",         "REAL",    ""),
        ("has_flap_gate",    "INTEGER", ""),
        ("seep_rate",        "REAL",    ""),
        ("q0",               "REAL",    ""),
        ("q_limit",          "REAL",    ""),
        ("pump_curve",       "TEXT",    "FK"),
        ("pump_init_state",  "REAL",    ""),
        ("pump_startup",     "REAL",    ""),
        ("pump_shutoff",     "REAL",    ""),
        ("crest_height",     "REAL",    ""),
        ("discharge_coeff",  "REAL",    ""),
        ("end_contractions", "INTEGER", ""),
        ("can_surcharge",    "INTEGER", ""),
        ("tag",              "TEXT",    ""),
    ]),
    ("subcatchments", "Part A – Model Input", [
        ("fid",             "INTEGER", "PK"),
        ("simulation_id",   "TEXT",    "FK"),
        ("subcatch_id",     "TEXT",    ""),
        ("geom",            "BLOB",    ""),
        ("outlet_node",     "TEXT",    "FK"),
        ("outlet_subcatch", "TEXT",    "FK"),
        ("rain_gage",       "TEXT",    "FK"),
        ("area",            "REAL",    ""),
        ("width",           "REAL",    ""),
        ("slope",           "REAL",    ""),
        ("curb_length",     "REAL",    ""),
        ("frac_imperv",     "REAL",    ""),
        ("n_imperv",        "REAL",    ""),
        ("n_perv",          "REAL",    ""),
        ("ds_imperv",       "REAL",    ""),
        ("ds_perv",         "REAL",    ""),
        ("pct_zero_imperv", "REAL",    ""),
        ("subarea_routing", "TEXT",    ""),
        ("pct_routed",      "REAL",    ""),
        ("infil_model",     "TEXT",    ""),
        ("infil_p1",        "REAL",    ""),
        ("infil_p2",        "REAL",    ""),
        ("infil_p3",        "REAL",    ""),
        ("infil_p4",        "REAL",    ""),
        ("infil_p5",        "REAL",    ""),
        ("tag",             "TEXT",    ""),
    ]),
    ("rain_gages", "Part A – Model Input", [
        ("fid",           "INTEGER", "PK"),
        ("simulation_id", "TEXT",    "FK"),
        ("gage_id",       "TEXT",    ""),
        ("geom",          "BLOB",    ""),
        ("rain_type",     "TEXT",    ""),
        ("rain_interval", "TEXT",    ""),
        ("snow_catch",    "REAL",    ""),
        ("data_source",   "TEXT",    ""),
        ("source_name",   "TEXT",    ""),
        ("station_id",    "TEXT",    ""),
        ("rain_units",    "TEXT",    ""),
    ]),

    # ── Network topology ───────────────────────────────────────────────────
    ("node_links", "Part A – Network Topology", [
        ("fid",           "INTEGER", "PK"),
        ("simulation_id", "TEXT",    "FK"),
        ("link_id",       "TEXT",    "FK"),
        ("from_node",     "TEXT",    "FK"),
        ("to_node",       "TEXT",    "FK"),
        ("link_type",     "TEXT",    ""),
        ("direction",     "INTEGER", ""),
    ]),
    ("subcatch_routing", "Part A – Network Topology", [
        ("fid",             "INTEGER", "PK"),
        ("simulation_id",   "TEXT",    "FK"),
        ("subcatch_id",     "TEXT",    "FK"),
        ("outlet_type",     "TEXT",    ""),
        ("outlet_node",     "TEXT",    "FK"),
        ("outlet_subcatch", "TEXT",    "FK"),
    ]),

    # ── Input data series ──────────────────────────────────────────────────
    ("curves", "Part A – Input Data", [
        ("fid",           "INTEGER", "PK"),
        ("simulation_id", "TEXT",    "FK"),
        ("curve_id",      "TEXT",    ""),
        ("curve_type",    "TEXT",    ""),
        ("x_value",       "REAL",    ""),
        ("y_value",       "REAL",    ""),
        ("ordinal",       "INTEGER", ""),
    ]),
    ("input_timeseries", "Part A – Input Data", [
        ("fid",           "INTEGER", "PK"),
        ("simulation_id", "TEXT",    "FK"),
        ("series_id",     "TEXT",    ""),
        ("timestamp",     "TEXT",    ""),
        ("value",         "REAL",    ""),
        ("ordinal",       "INTEGER", ""),
    ]),
    ("patterns", "Part A – Input Data", [
        ("fid",           "INTEGER", "PK"),
        ("simulation_id", "TEXT",    "FK"),
        ("pattern_id",    "TEXT",    ""),
        ("pattern_type",  "TEXT",    ""),
        ("ordinal",       "INTEGER", ""),
        ("factor",        "REAL",    ""),
    ]),

    # ── Climate / Hydrology ────────────────────────────────────────────────
    ("evaporation", "Part A – Climate/Hydrology", [
        ("fid",           "INTEGER", "PK"),
        ("simulation_id", "TEXT",    "FK"),
        ("evap_type",     "TEXT",    ""),
        ("evap_values",   "TEXT",    ""),
        ("ts_name",       "TEXT",    "FK"),
        ("pan_coeff",     "TEXT",    ""),
        ("recovery_pat",  "TEXT",    "FK"),
        ("dry_only",      "INTEGER", ""),
    ]),
    ("climate_settings", "Part A – Climate/Hydrology", [
        ("fid",            "INTEGER", "PK"),
        ("simulation_id",  "TEXT",    "FK"),
        ("temp_source",    "TEXT",    ""),
        ("temp_ts_name",   "TEXT",    "FK"),
        ("temp_file",      "TEXT",    ""),
        ("temp_file_start","REAL",    ""),
        ("wind_type",      "TEXT",    ""),
        ("wind_speed",     "TEXT",    ""),
        ("snow_divt",      "REAL",    ""),
        ("snow_ati_wt",    "REAL",    ""),
        ("snow_nrg_ratio", "REAL",    ""),
        ("snow_lat",       "REAL",    ""),
        ("snow_min_melt",  "REAL",    ""),
        ("snow_max_melt",  "REAL",    ""),
        ("adc_imperv",     "TEXT",    ""),
        ("adc_perv",       "TEXT",    ""),
    ]),
    ("snowpacks", "Part A – Climate/Hydrology", [
        ("fid",              "INTEGER", "PK"),
        ("simulation_id",    "TEXT",    "FK"),
        ("snowpack_id",      "TEXT",    ""),
        ("surface_type",     "TEXT",    ""),
        ("p1",               "REAL",    ""),
        ("p2",               "REAL",    ""),
        ("p3",               "REAL",    ""),
        ("p4",               "REAL",    ""),
        ("p5",               "REAL",    ""),
        ("p6",               "REAL",    ""),
        ("p7",               "REAL",    ""),
        ("removal_subcatch", "TEXT",    "FK"),
    ]),
    ("adjustments", "Part A – Climate/Hydrology", [
        ("fid",           "INTEGER", "PK"),
        ("simulation_id", "TEXT",    "FK"),
        ("adjust_type",   "TEXT",    ""),
        ("adj_values",    "TEXT",    ""),
    ]),
    ("subcatch_adjustments", "Part A – Climate/Hydrology", [
        ("fid",           "INTEGER", "PK"),
        ("simulation_id", "TEXT",    "FK"),
        ("subcatch_id",   "TEXT",    "FK"),
        ("adjust_type",   "TEXT",    ""),
        ("pattern_id",    "TEXT",    "FK"),
    ]),

    # ── Water quality ──────────────────────────────────────────────────────
    ("pollutants", "Part A – Water Quality", [
        ("fid",           "INTEGER", "PK"),
        ("simulation_id", "TEXT",    "FK"),
        ("pollutant_id",  "TEXT",    ""),
        ("units",         "TEXT",    ""),
        ("rain_conc",     "REAL",    ""),
        ("gw_conc",       "REAL",    ""),
        ("ii_conc",       "REAL",    ""),
        ("decay_coeff",   "REAL",    ""),
        ("snow_only",     "INTEGER", ""),
        ("co_pollutant",  "TEXT",    "FK"),
        ("co_fraction",   "REAL",    ""),
    ]),
    ("treatment", "Part A – Water Quality", [
        ("fid",           "INTEGER", "PK"),
        ("simulation_id", "TEXT",    "FK"),
        ("node_id",       "TEXT",    "FK"),
        ("pollutant_id",  "TEXT",    "FK"),
        ("expression",    "TEXT",    ""),
    ]),

    # ── LID ───────────────────────────────────────────────────────────────
    ("lid_controls", "Part A – LID", [
        ("fid",           "INTEGER", "PK"),
        ("simulation_id", "TEXT",    "FK"),
        ("lid_id",        "TEXT",    ""),
        ("layer_type",    "TEXT",    ""),
        ("p1",            "REAL",    ""),
        ("p2",            "REAL",    ""),
        ("p3",            "REAL",    ""),
        ("p4",            "REAL",    ""),
        ("p5",            "REAL",    ""),
        ("p6",            "REAL",    ""),
        ("p7",            "REAL",    ""),
    ]),
    ("lid_usage", "Part A – LID", [
        ("fid",           "INTEGER", "PK"),
        ("simulation_id", "TEXT",    "FK"),
        ("subcatch_id",   "TEXT",    "FK"),
        ("lid_id",        "TEXT",    "FK"),
        ("number",        "INTEGER", ""),
        ("area",          "REAL",    ""),
        ("width",         "REAL",    ""),
        ("init_sat",      "REAL",    ""),
        ("from_imperv",   "REAL",    ""),
        ("to_perv",       "INTEGER", ""),
        ("rpt_file",      "TEXT",    ""),
        ("drain_to",      "TEXT",    "FK"),
        ("from_perv",     "REAL",    ""),
    ]),

    # ── RDII / Unit Hydrographs ────────────────────────────────────────────
    ("rdii_assignments", "Part A – RDII", [
        ("fid",           "INTEGER", "PK"),
        ("simulation_id", "TEXT",    "FK"),
        ("node_name",     "TEXT",    "FK"),
        ("uh_name",       "TEXT",    "FK"),
        ("sewer_area",    "REAL",    ""),
    ]),
    ("unit_hydrographs", "Part A – RDII", [
        ("fid",           "INTEGER", "PK"),
        ("simulation_id", "TEXT",    "FK"),
        ("uh_name",       "TEXT",    ""),
        ("gage_name",     "TEXT",    "FK"),
        ("month",         "TEXT",    ""),
        ("response",      "TEXT",    ""),
        ("r",             "REAL",    ""),
        ("t",             "REAL",    ""),
        ("k",             "REAL",    ""),
        ("dmax",          "REAL",    ""),
        ("drecov",        "REAL",    ""),
        ("dinit",         "REAL",    ""),
    ]),
    ("transects", "Part A – Geometry", [
        ("fid",            "INTEGER", "PK"),
        ("simulation_id",  "TEXT",    "FK"),
        ("transect_id",    "TEXT",    ""),
        ("station",        "REAL",    ""),
        ("elevation",      "REAL",    ""),
        ("ordinal",        "INTEGER", ""),
        ("n_left",         "REAL",    ""),
        ("n_right",        "REAL",    ""),
        ("n_channel",      "REAL",    ""),
        ("left_overbank",  "REAL",    ""),
        ("right_overbank", "REAL",    ""),
    ]),

    # ── Part B – Results ───────────────────────────────────────────────────
    ("variables", "Part B – Simulation Results", [
        ("variable_id", "INTEGER", "PK"),
        ("name",        "TEXT",    ""),
        ("object_type", "TEXT",    ""),
        ("category",    "TEXT",    ""),
        ("units",       "TEXT",    ""),
        ("description", "TEXT",    ""),
    ]),
    ("result_timeseries", "Part B – Simulation Results", [
        ("simulation_id", "TEXT",    "FK"),
        ("object_type",   "TEXT",    ""),
        ("object_id",     "TEXT",    ""),
        ("variable_id",   "INTEGER", "FK"),
        ("elapsed_time",  "REAL",    ""),
        ("value",         "REAL",    ""),
    ]),
    ("result_summary", "Part B – Simulation Results", [
        ("simulation_id", "TEXT",    "PK,FK"),
        ("object_type",   "TEXT",    "PK"),
        ("object_id",     "TEXT",    "PK"),
        ("variable_id",   "INTEGER", "PK,FK"),
        ("value",         "REAL",    ""),
    ]),

    # ── Part C – Observed Data ─────────────────────────────────────────────
    ("observed_series", "Part C – Observed Data", [
        ("series_id",         "INTEGER", "PK"),
        ("name",              "TEXT",    ""),
        ("description",       "TEXT",    ""),
        ("source",            "TEXT",    ""),
        ("source_id",         "TEXT",    ""),
        ("variable_id",       "INTEGER", "FK"),
        ("object_type",       "TEXT",    ""),
        ("object_id",         "TEXT",    ""),
        ("units",             "TEXT",    ""),
        ("time_zone",         "TEXT",    ""),
        ("collection_method", "TEXT",    ""),
        ("start_date",        "TEXT",    ""),
        ("end_date",          "TEXT",    ""),
        ("record_count",      "INTEGER", ""),
    ]),
    ("observed_values", "Part C – Observed Data", [
        ("series_id",    "INTEGER", "FK"),
        ("timestamp",    "TEXT",    ""),
        ("value",        "REAL",    ""),
        ("quality_flag", "TEXT",    ""),
        ("qualifier",    "TEXT",    ""),
    ]),
]

# ---------------------------------------------------------------------------
# Relationships  (from_table, from_col, to_table, to_col, label)
# ---------------------------------------------------------------------------
RELATIONSHIPS = [
    # OGC standard
    ("gpkg_contents",          "srs_id",      "gpkg_spatial_ref_sys", "srs_id",      "references SRS"),
    ("gpkg_geometry_columns",  "table_name",  "gpkg_contents",        "table_name",  "describes table"),
    ("gpkg_geometry_columns",  "srs_id",      "gpkg_spatial_ref_sys", "srs_id",      "references SRS"),

    # All input tables → simulations
    ("options",              "simulation_id", "simulations", "simulation_id", "belongs to"),
    ("nodes",                "simulation_id", "simulations", "simulation_id", "belongs to"),
    ("links",                "simulation_id", "simulations", "simulation_id", "belongs to"),
    ("subcatchments",        "simulation_id", "simulations", "simulation_id", "belongs to"),
    ("rain_gages",           "simulation_id", "simulations", "simulation_id", "belongs to"),
    ("node_links",           "simulation_id", "simulations", "simulation_id", "belongs to"),
    ("subcatch_routing",     "simulation_id", "simulations", "simulation_id", "belongs to"),
    ("curves",               "simulation_id", "simulations", "simulation_id", "belongs to"),
    ("input_timeseries",     "simulation_id", "simulations", "simulation_id", "belongs to"),
    ("patterns",             "simulation_id", "simulations", "simulation_id", "belongs to"),
    ("evaporation",          "simulation_id", "simulations", "simulation_id", "belongs to"),
    ("climate_settings",     "simulation_id", "simulations", "simulation_id", "belongs to"),
    ("snowpacks",            "simulation_id", "simulations", "simulation_id", "belongs to"),
    ("adjustments",          "simulation_id", "simulations", "simulation_id", "belongs to"),
    ("subcatch_adjustments", "simulation_id", "simulations", "simulation_id", "belongs to"),
    ("pollutants",           "simulation_id", "simulations", "simulation_id", "belongs to"),
    ("lid_controls",         "simulation_id", "simulations", "simulation_id", "belongs to"),
    ("lid_usage",            "simulation_id", "simulations", "simulation_id", "belongs to"),
    ("rdii_assignments",     "simulation_id", "simulations", "simulation_id", "belongs to"),
    ("unit_hydrographs",     "simulation_id", "simulations", "simulation_id", "belongs to"),
    ("treatment",            "simulation_id", "simulations", "simulation_id", "belongs to"),
    ("transects",            "simulation_id", "simulations", "simulation_id", "belongs to"),

    # Network topology cross-references
    ("links",                "from_node",     "nodes",          "fid",          "from node"),
    ("links",                "to_node",       "nodes",          "fid",          "to node"),
    ("links",                "xsect_curve",   "curves",         "fid",          "uses curve"),
    ("links",                "pump_curve",    "curves",         "fid",          "uses curve"),
    ("node_links",           "link_id",       "links",          "fid",          "mirrors link"),
    ("node_links",           "from_node",     "nodes",          "fid",          "from node"),
    ("node_links",           "to_node",       "nodes",          "fid",          "to node"),
    ("subcatchments",        "rain_gage",     "rain_gages",     "fid",          "uses gage"),
    ("subcatchments",        "outlet_node",   "nodes",          "fid",          "drains to"),
    ("subcatch_routing",     "subcatch_id",   "subcatchments",  "fid",          "routes"),
    ("subcatch_routing",     "outlet_node",   "nodes",          "fid",          "outlet node"),
    ("subcatch_routing",     "outlet_subcatch","subcatchments", "fid",          "outlet subcatch"),

    # Adjustments / patterns
    ("subcatch_adjustments", "subcatch_id",   "subcatchments",  "fid",          "adjusts"),
    ("subcatch_adjustments", "pattern_id",    "patterns",       "fid",          "uses pattern"),
    ("evaporation",          "ts_name",       "input_timeseries","fid",         "uses timeseries"),
    ("evaporation",          "recovery_pat",  "patterns",       "fid",          "uses pattern"),
    ("climate_settings",     "temp_ts_name",  "input_timeseries","fid",         "uses timeseries"),
    ("snowpacks",            "removal_subcatch","subcatchments", "fid",          "removes to"),

    # Water quality
    ("treatment",            "node_id",       "nodes",          "fid",          "applied at node"),
    ("treatment",            "pollutant_id",  "pollutants",     "fid",          "treats"),
    ("pollutants",           "co_pollutant",  "pollutants",     "fid",          "co-pollutant"),

    # LID
    ("lid_usage",            "subcatch_id",   "subcatchments",  "fid",          "placed in"),
    ("lid_usage",            "lid_id",        "lid_controls",   "fid",          "uses control"),
    ("lid_usage",            "drain_to",      "nodes",          "fid",          "drains to"),

    # RDII
    ("rdii_assignments",     "node_name",     "nodes",          "fid",          "assigned to"),
    ("rdii_assignments",     "uh_name",       "unit_hydrographs","fid",         "uses UH"),
    ("unit_hydrographs",     "gage_name",     "rain_gages",     "fid",          "uses gage"),

    # Results
    ("result_timeseries",    "simulation_id", "simulations",    "simulation_id","reports for"),
    ("result_timeseries",    "variable_id",   "variables",      "variable_id",  "measures"),
    ("result_summary",       "simulation_id", "simulations",    "simulation_id","reports for"),
    ("result_summary",       "variable_id",   "variables",      "variable_id",  "measures"),

    # Observed
    ("observed_series",      "variable_id",   "variables",      "variable_id",  "measures"),
    ("observed_values",      "series_id",     "observed_series","series_id",    "belongs to"),
]

# ---------------------------------------------------------------------------
# Group colours (for draw.io)
# ---------------------------------------------------------------------------
GROUP_COLORS = {
    "OGC GeoPackage Standard":    ("#dae8fc", "#6c8ebf"),  # light blue
    "Part B – Simulation Results": ("#fff2cc", "#d6b656"),  # yellow
    "Part A – Model Input":        ("#d5e8d4", "#82b366"),  # green
    "Part A – Network Topology":   ("#e1f5e1", "#5d9966"),  # light green
    "Part A – Input Data":         ("#e8f5e1", "#7da462"),  # pale green
    "Part A – Climate/Hydrology":  ("#dae3ff", "#5a7fb5"),  # periwinkle
    "Part A – Water Quality":      ("#fce4ec", "#c2185b"),  # pink
    "Part A – LID":                ("#f3e5f5", "#8e24aa"),  # purple
    "Part A – RDII":               ("#fff3e0", "#ef6c00"),  # orange
    "Part A – Geometry":           ("#e8f5e9", "#2e7d32"),  # dark green
    "Part C – Observed Data":      ("#fbe9e7", "#bf360c"),  # deep orange
}

DEFAULT_COLOR = ("#f5f5f5", "#666666")

# ---------------------------------------------------------------------------
# Layout: manual (x, y) starting position per table
# Table width = 240, row height = 24, header = 32
# ---------------------------------------------------------------------------
COL_W   = 240
ROW_H   = 24
HDR_H   = 32
H_GAP   = 40   # horizontal gap between tables
V_GAP   = 50   # vertical gap between rows of tables

def table_height(table_name):
    for t, _, cols in TABLES:
        if t == table_name:
            return HDR_H + len(cols) * ROW_H
    return HDR_H

# Grid layout  (row, col) → zero-based
LAYOUT = {
    # row 0  – OGC standard
    "gpkg_spatial_ref_sys":    (0, 0),
    "gpkg_contents":           (0, 1),
    "gpkg_geometry_columns":   (0, 2),

    # row 1  – Simulation registry (tall)
    "simulations":             (1, 0),

    # row 1  – Core model spatial features
    "nodes":                   (1, 1),
    "links":                   (1, 2),
    "subcatchments":           (1, 3),
    "rain_gages":              (1, 4),

    # row 2  – Options, network topology
    "options":                 (2, 0),
    "node_links":              (2, 1),
    "subcatch_routing":        (2, 2),

    # row 2  – Input data series
    "curves":                  (2, 3),
    "input_timeseries":        (2, 4),
    "patterns":                (2, 5),

    # row 3  – Climate / Hydrology
    "evaporation":             (3, 0),
    "climate_settings":        (3, 1),
    "snowpacks":               (3, 2),
    "adjustments":             (3, 3),
    "subcatch_adjustments":    (3, 4),

    # row 4  – Quality, LID, RDII, Geometry
    "pollutants":              (4, 0),
    "treatment":               (4, 1),
    "lid_controls":            (4, 2),
    "lid_usage":               (4, 3),
    "rdii_assignments":        (4, 4),
    "unit_hydrographs":        (4, 5),
    "transects":               (4, 6),

    # row 5  – Results + observed
    "variables":               (5, 0),
    "result_timeseries":       (5, 1),
    "result_summary":          (5, 2),
    "observed_series":         (5, 3),
    "observed_values":         (5, 4),
}

def grid_to_xy(row, col, table_name):
    """Convert (row, col) to pixel (x, y)."""
    # x: simply column * (COL_W + H_GAP) + margin
    x = 60 + col * (COL_W + H_GAP)

    # y: accumulate maximum heights of tables in previous rows
    y = 60
    for r in range(row):
        max_h = 0
        for tname, (tr, tc) in LAYOUT.items():
            if tr == r:
                h = table_height(tname)
                if h > max_h:
                    max_h = h
        y += max_h + V_GAP
    return x, y


# ===========================================================================
# 1. Generate Mermaid ERD (markdown)
# ===========================================================================

def mermaid_col_type(sql_type):
    mapping = {
        "INTEGER": "int",
        "REAL":    "float",
        "TEXT":    "string",
        "BLOB":    "blob",
        "DOUBLE":  "float",
        "TINYINT": "int",
    }
    return mapping.get(sql_type.upper(), sql_type.lower())

def generate_mermaid():
    lines = ["# OpenSWMM GeoPackage – Entity Relationship Diagram",
             "",
             "> Auto-generated by `python/generate_geopackage_erd.py`  ",
             "> Source: `src/engine/input/geopackage/GeoPackageSchema.cpp`",
             "",
             "```mermaid",
             "erDiagram",
             ""]

    # ── table definitions ──────────────────────────────────────────────────
    last_group = None
    for tname, group, cols in TABLES:
        if group != last_group:
            lines.append(f"    %% ── {group} ──")
            last_group = group

        lines.append(f"    {tname} {{")
        for cname, ctype, key in cols:
            mtype  = mermaid_col_type(ctype)
            keystr = ""
            if key:
                keystr = " " + key
            lines.append(f"        {mtype} {cname}{keystr}")
        lines.append("    }")
        lines.append("")

    # ── relationships ──────────────────────────────────────────────────────
    lines.append("    %% ── Relationships ──")
    for from_t, from_c, to_t, to_c, label in RELATIONSHIPS:
        lines.append(f'    {to_t} ||--o{{ {from_t} : "{label}"')

    lines.append("```")
    lines.append("")
    lines.append("## Tables by Group")
    lines.append("")

    groups = {}
    for tname, group, cols in TABLES:
        groups.setdefault(group, []).append(tname)

    for group, tnames in groups.items():
        lines.append(f"### {group}")
        lines.append("")
        for tname in tnames:
            lines.append(f"- `{tname}`")
        lines.append("")

    return "\n".join(lines)


# ===========================================================================
# 2. Generate draw.io XML
# ===========================================================================

def drawio_escape(s):
    return (s.replace("&", "&amp;")
              .replace("<", "&lt;")
              .replace(">", "&gt;")
              .replace('"', "&quot;"))

def generate_drawio():
    root_el = ET.Element("mxfile", {
        "host":     "app.diagrams.net",
        "modified": "2026-04-12T00:00:00.000Z",
        "agent":    "OpenSWMM ERD generator",
        "version":  "21.0.0",
    })
    diagram = ET.SubElement(root_el, "diagram", {
        "name": "GeoPackage ERD",
        "id":   "openswmm-gpkg-erd",
    })
    model = ET.SubElement(diagram, "mxGraphModel", {
        "dx": "1422", "dy": "762",
        "grid": "1", "gridSize": "10",
        "guides": "1", "tooltips": "1",
        "connect": "1", "arrows": "1",
        "fold": "1", "page": "0",
        "pageScale": "1",
        "pageWidth": "3300", "pageHeight": "4600",
        "math": "0", "shadow": "0",
    })
    root_cell = ET.SubElement(model, "root")
    ET.SubElement(root_cell, "mxCell", {"id": "0"})
    ET.SubElement(root_cell, "mxCell", {"id": "1", "parent": "0"})

    cell_id = 2
    # Map table name → its mxCell id (for edge references)
    table_cell_ids: dict[str, int] = {}
    # Map (table_name, col_name) → row cell id (for edge port references)
    col_cell_ids: dict[tuple, int] = {}

    for tname, group, cols in TABLES:
        fill, stroke = GROUP_COLORS.get(group, DEFAULT_COLOR)
        row, col = LAYOUT[tname]
        x, y = grid_to_xy(row, col, tname)
        height = table_height(tname)

        tbl_id = cell_id
        table_cell_ids[tname] = tbl_id
        cell_id += 1

        # Table container
        tbl_cell = ET.SubElement(root_cell, "mxCell", {
            "id":     str(tbl_id),
            "value":  tname,
            "style":  (
                f"shape=table;startSize={HDR_H};container=1;collapsible=0;"
                f"childLayout=tableLayout;fixedRows=1;rowLines=0;"
                f"fontStyle=1;align=center;resizeLast=1;fontSize=13;"
                f"fillColor={fill};strokeColor={stroke};"
            ),
            "vertex": "1",
            "parent": "1",
        })
        ET.SubElement(tbl_cell, "mxGeometry", {
            "x": str(x), "y": str(y),
            "width": str(COL_W), "height": str(height),
            "as": "geometry",
        })

        # Column rows
        for i, (cname, ctype, key) in enumerate(cols):
            row_y = HDR_H + i * ROW_H
            row_id = cell_id
            col_cell_ids[(tname, cname)] = row_id
            cell_id += 1

            row_fill = ""
            row_style = (
                "shape=tableRow;horizontal=0;startSize=0;swimlaneHead=0;"
                "swimlaneBody=0;fillColor=none;collapsible=0;dropTarget=0;"
                "points=[[0,0.5],[1,0.5]];portConstraint=eastwest;"
                f"fontSize=11;top=0;left=0;right=0;"
                f"bottom={'0' if i < len(cols)-1 else '0'};"
            )
            row_cell = ET.SubElement(root_cell, "mxCell", {
                "id":     str(row_id),
                "value":  "",
                "style":  row_style,
                "vertex": "1",
                "parent": str(tbl_id),
            })
            ET.SubElement(row_cell, "mxGeometry", {
                "y": str(row_y),
                "width": str(COL_W), "height": str(ROW_H),
                "as": "geometry",
            })

            # Key badge cell (left 50px)
            key_id = cell_id
            cell_id += 1
            badge_style = (
                "shape=partialRectangle;connectable=0;"
                f"fillColor={'#ffe6cc' if 'PK' in key else ('none' if 'FK' not in key else '#f5f5f5')};"
                "top=0;left=0;bottom=0;right=0;"
                f"fontStyle={'1' if 'PK' in key else '0'};"
                "overflow=hidden;fontSize=10;"
            )
            key_cell = ET.SubElement(root_cell, "mxCell", {
                "id":          str(key_id),
                "value":       drawio_escape(key),
                "style":       badge_style,
                "vertex":      "1",
                "connectable": "0",
                "parent":      str(row_id),
            })
            key_geom = ET.SubElement(key_cell, "mxGeometry", {
                "width": "50", "height": str(ROW_H),
                "as": "geometry",
            })
            ET.SubElement(key_geom, "mxRectangle", {
                "width": "50", "height": str(ROW_H),
                "as": "alternateBounds",
            })

            # Column name cell (remaining width)
            name_id = cell_id
            cell_id += 1
            col_label = f"{cname} : {ctype}"
            name_style = (
                "shape=partialRectangle;connectable=0;fillColor=none;"
                "top=0;left=0;bottom=0;right=0;overflow=hidden;fontSize=11;"
            )
            name_cell = ET.SubElement(root_cell, "mxCell", {
                "id":          str(name_id),
                "value":       drawio_escape(col_label),
                "style":       name_style,
                "vertex":      "1",
                "connectable": "0",
                "parent":      str(row_id),
            })
            name_geom = ET.SubElement(name_cell, "mxGeometry", {
                "x": "50",
                "width": str(COL_W - 50), "height": str(ROW_H),
                "as": "geometry",
            })
            ET.SubElement(name_geom, "mxRectangle", {
                "width": str(COL_W - 50), "height": str(ROW_H),
                "as": "alternateBounds",
            })

    # ── Edges ──────────────────────────────────────────────────────────────
    for from_t, from_c, to_t, to_c, label in RELATIONSHIPS:
        src_id = col_cell_ids.get((from_t, from_c), table_cell_ids.get(from_t))
        tgt_id = col_cell_ids.get((to_t,   to_c),   table_cell_ids.get(to_t))
        if src_id is None or tgt_id is None:
            continue

        edge_id = cell_id
        cell_id += 1
        edge_style = (
            "edgeStyle=entityRelationEdgeStyle;endArrow=ERzeroToMany;"
            "startArrow=ERmandOne;exitX=1;exitY=0.5;exitDx=0;exitDy=0;"
            "entryX=0;entryY=0.5;entryDx=0;entryDy=0;fontSize=11;"
        )
        edge = ET.SubElement(root_cell, "mxCell", {
            "id":     str(edge_id),
            "value":  drawio_escape(label),
            "style":  edge_style,
            "edge":   "1",
            "source": str(src_id),
            "target": str(tgt_id),
            "parent": "1",
        })
        ET.SubElement(edge, "mxGeometry", {"relative": "1", "as": "geometry"})

    # Serialize
    tree = ET.ElementTree(root_el)
    ET.indent(tree, space="  ")
    from io import StringIO
    buf = StringIO()
    tree.write(buf, encoding="unicode", xml_declaration=True)
    return buf.getvalue()


# ===========================================================================
# main
# ===========================================================================

def main():
    repo_root = Path(__file__).resolve().parent.parent
    docs_dir  = repo_root / "docs"
    docs_dir.mkdir(exist_ok=True)

    md_path    = docs_dir / "GEOPACKAGE_SCHEMA_ERD.md"
    drawio_path = docs_dir / "GEOPACKAGE_SCHEMA_ERD.drawio"

    md_content = generate_mermaid()
    md_path.write_text(md_content, encoding="utf-8")
    print(f"✅  Mermaid ERD  → {md_path}")

    drawio_content = generate_drawio()
    drawio_path.write_text(drawio_content, encoding="utf-8")
    print(f"✅  draw.io XML  → {drawio_path}")


if __name__ == "__main__":
    main()

