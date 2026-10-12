#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Engine input features that the C API cannot reach.

The binding audits (``api_drift_audit.py``) check that every C function is
wrapped. This script checks the step before: whether every ``.inp`` section
and option key the engine *parses* is reachable through some C function at
all — otherwise no binding (Python, WebAssembly, ...) can read or write it.

Ground truth, all read from source text (no build needed):

* section tags registered with ``SectionRegistry`` (main deck) and the
  process-component config sections (ARD transport, heat, reactions, water age);
* option keys parsed by the ``[OPTIONS]``, ``[REPORT]``, ``[2D_OPTIONS]`` and
  ``[TRANSPORT_OPTIONS]`` handlers, each traced to the field it sets;
* C prototypes from the engine headers (via ``api_drift_audit``).

A key is reachable when a generic key/value API accepts it (``swmm_options_*``,
``swmm_options_*_ext``) or when any C API implementation touches the field
the parser writes. A section is reachable through the C functions listed for
it in ``SECTION_API`` below; their names classify them as readers or writers.

Gap classes::

    G1  section with no C function at all
    G2  option key the parser accepts but no API reads or writes
    G3  read-only: functions read the section but none write it
    G4  write-only: functions write the section but none read it back
    UNMAPPED  section missing from SECTION_API (add a row, then re-run)

``--check`` fails on any gap not listed in ``engine_feature_gaps_allow.json``
(each entry carries a reason), so new parser features without an API are
caught while known ones stay documented.

Usage::

    python3 scripts/engine_feature_gaps.py                  # markdown report
    python3 scripts/engine_feature_gaps.py --json out.json  # machine-readable
    python3 scripts/engine_feature_gaps.py --check
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import re
import sys
from pathlib import Path

PYTHON_DIR = Path(__file__).resolve().parents[1]
REPO = PYTHON_DIR.parent
SRC = REPO / "src" / "engine"
ALLOW = Path(__file__).with_name("engine_feature_gaps_allow.json")

_spec = importlib.util.spec_from_file_location(
    "_api_drift_audit", Path(__file__).with_name("api_drift_audit.py"))
_audit = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_audit)

# ---------------------------------------------------------------------------
# Section -> C functions that read or write it (regexes over function names).
# Reviewed by hand; a section missing here is reported as UNMAPPED. An empty
# list means "no C function exists" (G1) and must be justified in the
# allow-list or closed with a new API.
# ---------------------------------------------------------------------------
SECTION_API: dict[str, list[str]] = {
    # General
    "TITLE": [r"^swmm_title_"],
    "OPTIONS": [r"^swmm_options_"],
    "REPORT": [r"^swmm_options_(get|set)$"],  # RPT_* keys
    "FILES": [r"^swmm_files?_"],
    "EVENTS": [r"^swmm_events_"],
    "USER_FLAGS": [r"^swmm_userflag_"],
    "USER_FLAG_VALUES": [r"^swmm_userflag_.*value"],
    "PLUGINS": [r"^swmm_plugins?_"],
    "PROCESS_COMPONENTS": [r"^swmm_process_", r"^swmm_component_"],
    # Climate
    "EVAPORATION": [r"^swmm_climate_.*evap"],
    "TEMPERATURE": [r"^swmm_climate_.*(temp|snow|wind|adc|melt)"],
    "ADJUSTMENTS": [r"^swmm_climate_.*adjust"],
    "RAINGAGES": [r"^swmm_gage_"],
    "SNOWPACKS": [r"^swmm_snowpack_"],
    # Nodes
    "JUNCTIONS": [r"^swmm_node_"],
    "OUTFALLS": [r"^swmm_node_.*outfall"],
    "DIVIDERS": [r"^swmm_node_.*divider"],
    "STORAGE": [r"^swmm_node_.*storage"],
    "VIRTUAL_JUNCTIONS": [r"^swmm_virtual_junction_", r"^swmm_node_.*virtual"],
    "INLET_JUNCTIONS": [r"^swmm_inlet_junction_", r"^swmm_node_.*inlet"],
    # Links
    "CONDUITS": [r"^swmm_link_", r"^swmm_conduit_"],
    "PUMPS": [r"^swmm_link_.*pump"],
    "ORIFICES": [r"^swmm_link_.*orifice"],
    "WEIRS": [r"^swmm_link_.*weir"],
    "OUTLETS": [r"^swmm_link_.*outlet"],
    "XSECTIONS": [r"^swmm_link_.*xsect", r"^swmm_xsect_"],
    "TRANSECTS": [r"^swmm_transect_"],
    "LOSSES": [r"^swmm_link_.*(loss|flap|seep)"],
    "STREETS": [r"^swmm_street_"],
    "INLETS": [r"^swmm_inlet_(?!usage|junction)"],
    "INLET_USAGE": [r"^swmm_inlet_usage"],
    # Subcatchments and hydrology
    "SUBCATCHMENTS": [r"^swmm_subcatch_"],
    "SUBAREAS": [r"^swmm_subcatch_.*(imperv|perv|subarea|route|routing)"],
    "INFILTRATION": [r"^swmm_subcatch_.*infil"],
    "AQUIFERS": [r"^swmm_aquifer_"],
    "GROUNDWATER": [r"^swmm_subcatch_.*gw"],
    "GWF": [r"^swmm_gwf_", r"^swmm_subcatch_.*gwf"],
    "LID_CONTROLS": [r"^swmm_lid_(?!usage|node|richards)"],
    "LID_USAGE": [r"^swmm_lid_usage", r"^swmm_subcatch_.*lid"],
    "LID_LAYER_TREATMENT": [r"^swmm_lid_.*treatment", r"^swmm_lid_node_configure"],
    "LID_NODES": [r"^swmm_lid_node_", r"^swmm_node_(get|set)_lid"],
    "LID_NODE_OUTLETS": [r"^swmm_lid_node_.*outlet"],
    "LID_RICHARDS": [r"^swmm_lid_richards_", r"^swmm_lid_node_configure"],
    # Inflows
    "INFLOWS": [r"^swmm_ext_inflow", r"^swmm_inflow_", r"^swmm_node_.*ext_inflow"],
    "DWF": [r"^swmm_dwf_"],
    "RDII": [r"^swmm_rdii_"],
    "HYDROGRAPHS": [r"^swmm_hydrograph_"],
    "RDII_DECAY": [r"^swmm_(rdii|hydrograph)_.*decay"],
    # Tables
    "PATTERNS": [r"^swmm_pattern_"],
    "CURVES": [r"^swmm_curve_", r"^swmm_table_"],
    "TIMESERIES": [r"^swmm_timeseries_", r"^swmm_table_"],
    # Quality
    "POLLUTANTS": [r"^swmm_pollutant_"],
    "LANDUSES": [r"^swmm_landuse_"],
    "COVERAGES": [r"^swmm_subcatch_.*coverage", r"^swmm_landuse_.*coverage"],
    "LOADINGS": [r"^swmm_subcatch_.*loading"],
    "BUILDUP": [r"^swmm_buildup_", r"^swmm_landuse_.*buildup"],
    "WASHOFF": [r"^swmm_washoff_", r"^swmm_landuse_.*washoff"],
    "TREATMENT": [r"^swmm_treatment_"],
    "INITIAL_QUALITY": [r"^swmm_init_"],
    "CONTROLS": [r"^swmm_control_"],
    # Map / GUI
    "COORDINATES": [r"^swmm_spatial_.*node_coord"],
    "VERTICES": [r"^swmm_spatial_.*link_vert"],
    "POLYGONS": [r"^swmm_spatial_.*polygon"],
    "SYMBOLS": [r"^swmm_spatial_.*gage_coord"],
    "MAP": [r"^swmm_spatial_.*(map|bound|dimension|extent|units)"],
    "TAGS": [r"^swmm_(node|link|subcatch|gage)_(get|set)_tag"],
    "LABELS": [r"label"],
    "BACKDROP": [r"backdrop"],
    "PROFILE": [r"profile"],
    "PROFILES": [r"profile"],
    # 2D surface
    "2D_OPTIONS": [r"^swmm_options_(get|set)_ext$"],
    "2D_MESH_FILE": [r"^swmm_options_(get|set)_ext$"],  # MESH_FILE ext key
    "2D_VERTICES": [r"^swmm_2d_.*vert(ex|ices)"],
    "2D_TRIANGLES": [r"^swmm_2d_.*(triangle|cell)"],
    "2D_QUADS": [r"^swmm_2d_.*(quad|cell|triangle)"],  # quads are cells; setters are triangle-named
    "2D_INITIAL_VELOCITY": [r"^swmm_2d_.*(initial_vel|velocity)"],
    "2D_VERTEX_NODE_MAP": [r"^swmm_2d_.*vertex_coupl"],
    "2D_TRIANGLE_NODE_MAP": [r"^swmm_2d_.*(triangle|cell)_coupl"],
    "2D_BOUNDARY_CONDITIONS": [r"^swmm_2d_.*edge_bc"],
    "2D_EDGE_CONVEYANCE": [r"^swmm_2d_.*conveyance"],
    "2D_INITIAL_QUALITY": [r"^swmm_2d_.*(initial_(quality|conc)|cell_conc)"],
    "2D_BOUNDARY_QUALITY": [r"^swmm_2d_.*(bc_conc|boundary_quality)"],
    "2D_INFILTRATION_OPTIONS": [r"^swmm_infil2d_.*option"],
    "2D_INFILTRATION_DEFAULTS": [r"^swmm_infil2d_.*default"],
    "2D_INFILTRATION": [r"^swmm_infil2d_"],
    "2D_COVERAGES": [r"^swmm_2d_.*coverage"],
    "2D_LOADINGS": [r"^swmm_2d_.*loading"],
    "2D_CURB_LENGTH": [r"^swmm_2d_.*curb"],
    "2D_SURFACE_OWNERSHIP": [r"^swmm_surface_owner_", r"^swmm_infil2d_.*owner"],
    "2D_AQUIFER_OPTIONS": [r"^swmm_gw2d_option"],
    "2D_AQUIFER": [r"^swmm_gw2d_"],
    "2D_AQUIFER_NODE": [r"^swmm_gw2d_.*node"],
    "2D_AQUIFER_LINKS": [r"^swmm_gw2d_.*link"],
    # Groundwater transport
    "GW_TRANSPORT_OPTIONS": [r"^swmm_gw_transport_option"],
    "GW_TRANSPORT_PARAMS": [r"^swmm_gw_.*param"],
    "GW_SORPTION": [r"^swmm_gw_.*sorption"],
    "GW_INITIAL_QUALITY": [r"^swmm_gw_init"],
    "GW_BOUNDARY_QUALITY": [r"^swmm_gw_.*boundar"],
    "GW_SOURCES": [r"^swmm_gw_.*source"],
    # Process-component config files
    "TRANSPORT_OPTIONS": [r"^swmm_transport_"],
    "CONDUIT_DISPERSION": [r"^swmm_transport_.*conduit"],
    "TRANSPORT_BOUNDARIES": [r"^swmm_transport_.*boundar"],
    "TRANSPORT_SOURCES": [r"^swmm_transport_.*source"],
    "STORAGE_MIXING": [r"mixing"],
    "HEAT_FLUXES": [r"^swmm_heat_.*(module|flux)"],
    "HEAT_SOURCES": [r"^swmm_heat_.*source"],
    "SEDIMENT_EXCHANGE": [r"^swmm_heat_.*sediment"],
    "SOLAR_RADIATION": [r"^swmm_heat_.*solar"],
    "CLOUD_COVER": [r"^swmm_heat_.*cloud"],
    "RADIATIVE_FLUXES": [r"^swmm_heat_.*radiative"],
    "REACTION_OPTIONS": [r"^swmm_reactions?_option", r"^swmm_reactions_(apply_text|serialize)$"],
    "REACTION_SPECIES": [r"^swmm_reactions?_.*species", r"^swmm_reactions_(apply_text|serialize)$"],
    "REACTION_COEFFICIENTS": [r"^swmm_reactions?_.*coeff", r"^swmm_reactions_(apply_text|serialize)$"],
    "REACTION_PARAMETERS": [r"^swmm_reactions?_.*param", r"^swmm_reactions_(apply_text|serialize)$"],
    "REACTION_TERMS": [r"^swmm_reactions?_.*term", r"^swmm_reactions_(apply_text|serialize)$"],
    "REACTION_PIPES": [r"^swmm_reactions?_.*(pipe|scope|expr)", r"^swmm_reactions_(apply_text|serialize)$"],
    "REACTION_TANKS": [r"^swmm_reactions?_.*(tank|scope|expr)", r"^swmm_reactions_(apply_text|serialize)$"],
    "REACTION_SUBCATCHMENTS": [r"^swmm_reactions?_.*(subcatch|scope|expr)", r"^swmm_reactions_(apply_text|serialize)$"],
    "REACTION_SOURCES": [r"^swmm_reactions?_.*source", r"^swmm_reactions_(apply_text|serialize)$"],
    "REACTION_QUALITY": [r"^swmm_reactions?_.*(quality|initial)", r"^swmm_reactions_(apply_text|serialize)$"],
    "REACTION_PATTERNS": [r"^swmm_reactions?_.*pattern", r"^swmm_reactions_(apply_text|serialize)$"],
    "REACTION_REPORT": [r"^swmm_reactions?_.*report", r"^swmm_reactions_(apply_text|serialize)$"],
    "WATER_AGE_SOURCES": [r"^swmm_water_age_"],
}

_READ = re.compile(r"_(get|count|find|list|names?|ids?|index|is|has|query|lookup|read|"
                   r"describe|stat|statistics|validate|references|serialize)(_|$)")
_WRITE = re.compile(r"_(set|add|remove|delete|insert|fuse|rename|clear|append|replace|"
                    r"configure|create|assign|apply|load|attach|detach|reload|reset)(_|$)")


# ---------------------------------------------------------------------------
# Ground truth from the parsers
# ---------------------------------------------------------------------------
def _text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="ignore")


def parsed_sections() -> dict[str, str]:
    """Section tag -> file that registers or dispatches it."""
    out: dict[str, str] = {}
    for p in sorted(SRC.rglob("*.cpp")):
        for m in re.finditer(r'register_(?:builtin|custom)\(\s*"([A-Z0-9_]+)"', _text(p)):
            if m.group(1) != "MY_CUSTOM":  # doc example in SectionRegistry
                out.setdefault(m.group(1), str(p.relative_to(REPO)))
    for p in sorted((SRC / "transport" / "components").rglob("*Component*.cpp")) + sorted(
            (SRC / "transport" / "components").rglob("*Config*.cpp")):
        t = _text(p)
        tags = re.findall(r'(?:sec\.first|tag)\s*[!=]=\s*"([A-Z0-9_]+)"', t)
        tags += re.findall(r'"((?:REACTION_[A-Z_]+)|WATER_AGE_SOURCES)"', t)
        for tag in tags:
            out.setdefault(tag, str(p.relative_to(REPO)))
    return out


_ROOT_OBJ = r"(?:ctx\s*\.\s*)?(?:opt|options|cfg|config|ard_config|ard)"


def _branch_fields(text: str, key_re: str) -> dict[str, set[str]]:
    """key -> qualified fields ('fv.limiter', 'water_density') its branch assigns.

    A branch runs from its key test to the next ``else if``/``else`` or the
    next key test, so a trailing branch cannot pick up unrelated code.
    """
    out: dict[str, set[str]] = {}
    matches = list(re.finditer(key_re, text))
    for i, m in enumerate(matches):
        nxt = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        stop = re.search(r"\}\s*else\b", text[m.end():nxt])
        branch = text[m.end(): m.end() + stop.start() if stop else nxt]
        fields = set()
        for path in re.findall(_ROOT_OBJ + r"\s*(?:\.|->)\s*((?:\w+\s*\.\s*)*\w+)\s*(?:=[^=]|\))",
                               branch):
            parts = [p.strip() for p in path.split(".")]
            fields.add(".".join(parts[-2:]))  # e.g. fv.limiter, or water_density
        fields |= {".".join(p.strip() for p in path.split(".")[-2:]) for path in re.findall(
            r"&\s*" + _ROOT_OBJ + r"\s*\.\s*((?:\w+\s*\.\s*)*\w+)", branch)}
        fields.discard("sets_limiter")
        out.setdefault(m.group(1), set()).update(
            f for f in fields if not f.split(".")[-1].startswith("sets_"))
    return out


def _api_impl_text(*names: str) -> str:
    """Text of C API implementation files; restricted to ``names`` (file stems) if given."""
    parts = []
    for p in sorted(SRC.rglob("*.cpp")):
        if names and p.stem not in names:
            continue
        t = _text(p)
        if "SWMM_ENGINE_API" in t:
            parts.append(t)
    return "\n".join(parts)


def _touches(text: str, field: str) -> bool:
    """A qualified access to ``field`` ('fv.limiter' -> '.fv.limiter' / 'fv->limiter')."""
    parts = [re.escape(p) for p in field.split(".")]
    return re.search(r"(?:\.|->)\s*" + r"\s*(?:\.|->)\s*".join(parts) + r"\b", text) is not None


def _function_body(text: str, name: str) -> str:
    i = text.find(f"SWMM_ENGINE_API int {name}(")
    if i < 0:
        return ""
    j = text.find("\nSWMM_ENGINE_API", i + 10)
    return text[i: j if j > 0 else len(text)]


def option_keys() -> dict[str, dict[str, dict]]:
    """Section -> key -> {fields, reachable, via} for key/value sections."""
    impl_all = _api_impl_text()
    model = _text(SRC / "core" / "openswmm_model_impl.cpp")
    generic = set(re.findall(r'k\s*==\s*"([A-Z0-9_]+)"', _function_body(model, "swmm_options_get")))
    generic_set = set(re.findall(r'k\s*==\s*"([A-Z0-9_]+)"',
                                 _function_body(model, "swmm_options_set")))
    set_ext = _function_body(model, "swmm_options_set_ext")
    get_ext = _function_body(model, "swmm_options_get_ext")
    funcs = set(_audit.collect_c_functions())

    def reach(key: str, fields: set[str], *, aliases: tuple[str, ...] = (),
              generic_keys: bool = True, impl: str = impl_all,
              prefix: str = "") -> tuple[bool, str]:
        """Is ``key`` readable AND writable through some C API?"""
        names = (key, *aliases)
        if generic_keys:
            if any(n in generic and n in generic_set for n in names):
                return True, "swmm_options_get/set"
            if any(f'"{n}"' in set_ext and f'"{n}"' in get_ext for n in names):
                return True, "swmm_options_get/set_ext"
        if prefix:  # typed accessor pair, e.g. swmm_transport_get/set_target_dx
            low = key.lower()
            if f"{prefix}get_{low}" in funcs and f"{prefix}set_{low}" in funcs:
                return True, f"{prefix}get/set_{low}"
        touched = sorted(f for f in fields if _touches(impl, f))
        if touched:
            return True, "field " + ", ".join(touched)
        return False, ""

    out: dict[str, dict[str, dict]] = {}
    # [OPTIONS]
    oh = _text(SRC / "input" / "handlers" / "OptionsHandler.cpp")
    sec = {}
    for key, fields in _branch_fields(oh, r'key\s*==\s*"([A-Z0-9_]+)"').items():
        ok, via = reach(key, fields)
        sec[key] = {"fields": sorted(fields), "reachable": ok, "via": via}
    out["OPTIONS"] = sec
    # [REPORT] -> RPT_<key> in the generic getter/setter
    ch = _text(SRC / "input" / "handlers" / "ControlsHandler.cpp")
    i = ch.find("handle_report")
    rep = ch[i: ch.find("\n}", i)] if i >= 0 else ""
    sec = {}
    for key in sorted(set(re.findall(r'"([A-Z][A-Z0-9_]+)"', rep))):
        ok, via = reach(key, set(), aliases=(f"RPT_{key}",))
        sec[key] = {"fields": [], "reachable": ok, "via": via}
    out["REPORT"] = sec
    # [2D_OPTIONS]: is2DOptionKey() keys are served by swmm_options_*_ext.
    s2 = SRC / "2d" / "input" / "SectionHandlers2D.cpp"
    if s2.exists():
        t = _text(s2)
        parse = t[t.find("parse2DOptionsLine"): t.find("bool is2DOptionKey")]
        k = t.find("kKeys", t.find("bool is2DOptionKey"))
        ext_keys = set(re.findall(r'"([A-Z0-9_]+)"', t[k: t.find("};", k)]))
        sec = {}
        for key in sorted(set(re.findall(r'iequals\(\s*key\s*,\s*"([A-Z0-9_]+)"', parse))):
            ok = key in ext_keys
            sec[key] = {"fields": [], "reachable": ok,
                        "via": "swmm_options_get/set_ext" if ok else ""}
        out["2D_OPTIONS"] = sec
    # [TRANSPORT_OPTIONS] (ARD component config)
    ard = SRC / "transport" / "components" / "EulerianArdComponent" / "ArdConfig.cpp"
    if ard.exists():
        # [OPTIONS] keys of the same name are different settings: no generic
        # key path; a field counts only if the transport or options API
        # implementation touches it.
        ard_impl = _api_impl_text("openswmm_transport_impl", "openswmm_model_impl")
        sec = {}
        for key, fields in _branch_fields(_text(ard), r'key\s*==\s*"([A-Z0-9_]+)"').items():
            ok, via = reach(key, fields, generic_keys=False, impl=ard_impl,
                            prefix="swmm_transport_")
            sec[key] = {"fields": sorted(fields), "reachable": ok, "via": via}
        out["TRANSPORT_OPTIONS"] = sec
    return out


# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
def analyse() -> dict:
    funcs = sorted(_audit.collect_c_functions())
    sections = parsed_sections()
    rows, gaps = [], []
    for tag, where in sorted(sections.items()):
        if tag not in SECTION_API:
            rows.append({"section": tag, "file": where, "class": "UNMAPPED",
                         "readers": [], "writers": []})
            gaps.append(f"UNMAPPED:{tag}")
            continue
        matched = sorted({f for f in funcs for rx in SECTION_API[tag] if re.search(rx, f)})
        readers = [f for f in matched if _READ.search(f)]
        writers = [f for f in matched if _WRITE.search(f)]
        if not matched:
            cls = "G1"
        elif writers and not readers:
            cls = "G4"
        elif readers and not writers:
            cls = "G3"
        else:
            cls = "OK"
        if cls != "OK":
            gaps.append(f"{cls}:{tag}")
        rows.append({"section": tag, "file": where, "class": cls,
                     "readers": readers, "writers": writers, "other": sorted(
                         set(matched) - set(readers) - set(writers))})
    keys = option_keys()
    for sec, entries in keys.items():
        for key, info in entries.items():
            if not info["reachable"]:
                gaps.append(f"G2:{sec}.{key}")
    stale = sorted(set(SECTION_API) - set(sections))
    return {"sections": rows, "keys": keys, "gaps": sorted(gaps),
            "mapped_but_not_parsed": stale, "c_functions": len(funcs)}


def load_allow() -> dict[str, str]:
    if not ALLOW.exists():
        return {}
    return json.loads(ALLOW.read_text(encoding="utf-8"))


def markdown(result: dict, allow: dict[str, str]) -> str:
    lines = ["# Engine input features vs C API", "",
             f"{len(result['sections'])} parsed sections, {result['c_functions']} C functions.",
             "", "## Gaps", "", "| Gap | Allow-list reason |", "|---|---|"]
    for g in result["gaps"]:
        lines.append(f"| `{g}` | {allow.get(g, '**not allow-listed**')} |")
    lines += ["", "## Sections", "", "| Section | Class | Readers | Writers |", "|---|---|---|---|"]
    for r in result["sections"]:
        lines.append(f"| `[{r['section']}]` | {r['class']} | {len(r['readers'])} | "
                     f"{len(r['writers'])} |")
    for sec, entries in result["keys"].items():
        lines += ["", f"## `[{sec}]` keys", "", "| Key | Reachable via |", "|---|---|"]
        for key, info in entries.items():
            lines.append(f"| `{key}` | {info['via'] or '**none**'} |")
    if result["mapped_but_not_parsed"]:
        lines += ["", "Mapped but no longer parsed: "
                  + ", ".join(f"`{s}`" for s in result["mapped_but_not_parsed"])]
    return "\n".join(lines) + "\n"


def check() -> list[str]:
    """Gaps not justified in the allow-list, plus allow-list entries that no longer apply."""
    result = analyse()
    allow = load_allow()
    problems = [f"unjustified gap: {g}" for g in result["gaps"] if g not in allow]
    problems += [f"stale allow-list entry: {g}" for g in allow if g not in result["gaps"]]
    problems += [f"SECTION_API row for a section that is not parsed: {s}"
                 for s in result["mapped_but_not_parsed"]]
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--json", type=Path, help="write the full analysis as JSON")
    ap.add_argument("-o", "--output", type=Path, help="write the markdown report here")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    if a.check:
        problems = check()
        for p in problems:
            print(f"engine_feature_gaps: {p}", file=sys.stderr)
        print(f"engine_feature_gaps: {len(problems)} problem(s)")
        return 1 if problems else 0
    result = analyse()
    if a.json:
        a.json.write_text(json.dumps(result, indent=1) + "\n", encoding="utf-8")
    md = markdown(result, load_allow())
    if a.output:
        a.output.write_text(md, encoding="utf-8")
    else:
        sys.stdout.write(md)
    return 0


if __name__ == "__main__":
    sys.exit(main())
