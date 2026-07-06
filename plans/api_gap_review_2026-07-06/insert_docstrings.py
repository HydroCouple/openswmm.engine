#!/usr/bin/env python3
"""Insert authored one-line docstrings into the 105 flagged methods.

Keyed by (module, def-line) — many methods share names (add/clear/rename/
get_id/...), so line numbers are the safe key. Inserts are applied bottom-up
per file so earlier line numbers stay valid. Idempotent-ish: skips a method
that already has a docstring as its first body statement.
"""
from __future__ import annotations

import re
from pathlib import Path

ENG = Path(__file__).resolve().parents[2] / "python" / "openswmm" / "engine"

# (module, def_line) -> docstring text (no quotes; single line)
DOCS: dict[tuple[str, int], str] = {
    ("_controls", 116): "Insert *value* at *idx* (emulated via clear + re-add; the C rule API is append-only).",
    ("_controls", 127): "Append a control rule parsed from *value* (an INP ``RULE`` block or rule mapping).",
    ("_controls", 133): "Remove all control rules from the model.",

    ("_forcing", 87): "Force a lateral inflow (flow units) at *node* for the current step; *mode* selects replace/add/scale and *persist* keeps it across steps.",
    ("_forcing", 94): "Force a head/stage boundary at *node* for the current step (see *mode* / *persist*).",
    ("_forcing", 101): "Force a pollutant mass-rate inflow of *pollutant* at *node* (see *mode* / *persist*).",
    ("_forcing", 111): "Force the flow through *link* for the current step (see *mode* / *persist*).",
    ("_forcing", 118): "Force the control setting (0-1) of *link* for the current step (see *mode* / *persist*).",
    ("_forcing", 127): "Force the rainfall rate on subcatchment *sub* for the current step (see *mode* / *persist*).",
    ("_forcing", 354): "Force the rainfall rate reported by rain *gage* for the current step (see *mode* / *persist*).",
    ("_forcing", 409): "Clear all active runtime forcing overrides.",

    ("_gages", 294): "Return the zero-based index of rain gage *gage_id* (raises if unknown).",
    ("_gages", 301): "Return the ID string of the rain gage at *idx*.",
    ("_gages", 309): "Add a new rain gage *gage_id* and return its :class:`Gage` handle.",
    ("_gages", 316): "Rename the rain gage identified by *key* to *new_id*.",

    ("_hotstart", 85): "Close the hot-start file and release its handle.",
    ("_hotstart", 160): "Seed the initial water depth at *node_id* in the hot-start state.",
    ("_hotstart", 164): "Seed the initial hydraulic head at *node_id* in the hot-start state.",
    ("_hotstart", 168): "Seed the initial flow at *link_id* in the hot-start state.",
    ("_hotstart", 172): "Seed the initial depth at *link_id* in the hot-start state.",
    ("_hotstart", 176): "Seed the initial runoff at *sub_id* in the hot-start state.",
    ("_hotstart", 270): "Insert *value* at *idx* (emulated via clear + re-add; the C save API is append-only).",
    ("_hotstart", 283): "Append a hot-start save-schedule entry *value*.",
    ("_hotstart", 289): "Remove all entries from the hot-start save schedule.",

    ("_inflows", 252): "Add an RDII inflow at *node* driven by unit-hydrograph group *uh_name* over sewered *area*.",
    ("_inflows", 258): "Return the :class:`RDIIEntry` at *idx*.",
    ("_inflows", 285): "Add an RTK unit-hydrograph (response R/T/K) to group *uh_name* for *month*.",
    ("_inflows", 294): "Return the :class:`HydrographEntry` at *idx*.",
    ("_inflows", 396): "Associate rain *gage_name* with unit-hydrograph group *uh_name*.",
    ("_inflows", 402): "Return the :class:`HydrographGageEntry` at *idx*.",
    ("_inflows", 420): "Return the ID of the unit-hydrograph group at *idx*.",
    ("_inflows", 430): "Add an exponential-decay RDII entry to unit-hydrograph *uh_name*.",
    ("_inflows", 438): "Return the :class:`RDIIDecayEntry` at *idx*.",

    ("_infrastructure", 58): "Add a natural-channel transect *transect_id* and return its index.",
    ("_infrastructure", 65): "Set the left-bank, right-bank and channel Manning's n for transect *idx*.",
    ("_infrastructure", 70): "Append a ``(station, elevation)`` point to transect *idx*.",
    ("_infrastructure", 265): "Add a street cross-section *street_id* and return its index.",
    ("_infrastructure", 272): "Set the geometry and roughness parameters for street *idx*.",
    ("_infrastructure", 363): "Add an inlet *inlet_id* of *inlet_type* and return its index.",
    ("_infrastructure", 371): "Set the geometry parameters for inlet *idx*.",
    ("_infrastructure", 522): "Set the surface-layer parameters for LID *idx*.",
    ("_infrastructure", 527): "Set the soil-layer parameters for LID *idx*.",
    ("_infrastructure", 533): "Set the storage-layer parameters for LID *idx*.",
    ("_infrastructure", 538): "Set the underdrain parameters for LID *idx*.",

    ("_links", 185): "Return the cross-section as a ``(shape, geom1, geom2, geom3, geom4)`` tuple.",
    ("_links", 761): "Return the current concentration of *pollutant* in this link.",
    ("_links", 869): "Return the zero-based index of link *link_id* (raises if unknown).",
    ("_links", 876): "Return the ID string of the link at *idx*.",
    ("_links", 884): "Add a new link *link_id* of *link_type* and return its :class:`Link` handle.",
    ("_links", 891): "Remove the most recently added link, which must be *link_id*.",
    ("_links", 896): "Rename the link identified by *key* to *new_id*.",
    ("_links", 1005): "Return an array of *pollutant* concentrations for every link.",

    ("_output_reader", 85): "Close the binary output file and release its handle.",
    ("_output_reader", 275): "Return an array of *var* values for every link at output *period*.",
    ("_output_reader", 286): "Return an array of *var* values for every subcatchment at output *period*.",
    ("_output_reader", 297): "Return the system-wide *var* value at output *period*.",
    ("_output_reader", 321): "Return the time series of *var* for *node* over the reporting window.",
    ("_output_reader", 339): "Return the time series of *var* for *link* over the reporting window.",
    ("_output_reader", 357): "Return the time series of *var* for *sub* over the reporting window.",
    ("_output_reader", 375): "Return the system-wide time series of *var* over the reporting window.",
    ("_output_reader", 396): "Return all output variables for *node* at output *period* as a dict.",
    ("_output_reader", 408): "Return all output variables for *link* at output *period* as a dict.",
    ("_output_reader", 420): "Return all output variables for *sub* at output *period* as a dict.",

    ("_pollutants", 301): "Return the zero-based index of pollutant *pollut_id* (raises if unknown).",
    ("_pollutants", 308): "Return the ID string of the pollutant at *idx*.",
    ("_pollutants", 316): "Add a new pollutant *pollut_id* measured in *units* and return its :class:`Pollutant`.",

    ("_quality", 134): "Return the zero-based index of land use *landuse_id* (raises if unknown).",
    ("_quality", 141): "Return the ID string of the land use at *idx*.",
    ("_quality", 147): "Add a new land use *landuse_id* and return its :class:`Landuse` handle.",
    ("_quality", 214): "Set the washoff function and coefficients for *landuse* / *pollutant*.",
    ("_quality", 223): "Return the washoff parameters for *landuse* / *pollutant* as a dict.",
    ("_quality", 241): "Set the treatment *expression* for *pollutant* at *node*.",
    ("_quality", 248): "Return the treatment expression for *pollutant* at *node*.",
    ("_quality", 256): "Remove any treatment for *pollutant* at *node*.",

    ("_solver", 1552): "Insert *value* at *idx* (emulated via clear + re-add; the C event API is append-only).",
    ("_solver", 1565): "Append a reporting / hot-start event *value*.",
    ("_solver", 1572): "Remove all scheduled events.",

    ("_spatial", 84): "Return the ``(x, y)`` coordinate of *node*.",
    ("_spatial", 91): "Set the ``(x, y)`` coordinate of *node*.",
    ("_spatial", 125): "Return the ``(x, y)`` mid-point coordinate of *link*.",
    ("_spatial", 132): "Set the ``(x, y)`` mid-point coordinate of *link*.",
    ("_spatial", 149): "Set the polyline vertices of *link* from an ``(N, 2)`` array.",
    ("_spatial", 168): "Return the ``(x, y)`` centroid coordinate of *sub*.",
    ("_spatial", 175): "Set the ``(x, y)`` centroid coordinate of *sub*.",
    ("_spatial", 180): "Return the boundary polygon of *sub* as an ``(N, 2)`` array.",
    ("_spatial", 191): "Set the boundary polygon of *sub* from an ``(N, 2)`` array.",
    ("_spatial", 210): "Return the ``(x, y)`` coordinate of rain *gage*.",
    ("_spatial", 217): "Set the ``(x, y)`` coordinate of rain *gage*.",

    ("_subcatchments", 178): "Set Horton infiltration parameters (f0, fmin, decay, dry-time) for this subcatchment.",
    ("_subcatchments", 194): "Set Green-Ampt infiltration parameters (suction, conductivity, initial deficit).",
    ("_subcatchments", 210): "Set the SCS curve-number infiltration parameter.",
    ("_subcatchments", 657): "Return the current runoff concentration of *pollutant* for this subcatchment.",
    ("_subcatchments", 665): "Return the ponded-water concentration of *pollutant* for this subcatchment.",
    ("_subcatchments", 673): "Set the ponded-water *mass* of *pollutant* for this subcatchment.",
    ("_subcatchments", 830): "Return the zero-based index of subcatchment *sub_id* (raises if unknown).",
    ("_subcatchments", 837): "Return the ID string of the subcatchment at *idx*.",
    ("_subcatchments", 845): "Add a new subcatchment *sub_id* and return its :class:`Subcatchment` handle.",
    ("_subcatchments", 852): "Rename the subcatchment identified by *key* to *new_id*.",
    ("_subcatchments", 915): "Return an array of *pollutant* runoff concentrations for every subcatchment.",

    ("_tables", 85): "Append an ``(x, y)`` point to the table.",
    ("_tables", 204): "Return the zero-based index of table *table_id* (raises if unknown).",
    ("_tables", 211): "Return the ID string of the table at *idx*.",
    ("_tables", 238): "Add a new time series *ts_id* and return its :class:`TimeSeries` handle.",
    ("_tables", 263): "Return a :class:`TimeSeries` view of the table identified by *key*.",
    ("_tables", 267): "Return a :class:`Curve` view of the table identified by *key*.",
}

END_SIG = re.compile(r":\s*(#.*)?$")
STR_START = ('"""', "'''", 'r"""', "r'''", '"', "'")


def main() -> int:
    by_mod: dict[str, list[int]] = {}
    for (mod, line) in DOCS:
        by_mod.setdefault(mod, []).append(line)

    total = 0
    for mod, lines_ in by_mod.items():
        path = ENG / f"{mod}.pyx"
        src = path.read_text(encoding="utf-8").splitlines()
        for def_line in sorted(lines_, reverse=True):     # bottom-up
            doc = DOCS[(mod, def_line)]
            i = def_line - 1                              # 0-based def line
            def_indent = len(src[i]) - len(src[i].lstrip())
            # advance to end of signature
            j = i
            while j < len(src) and not END_SIG.search(src[j]):
                j += 1
            # skip if a docstring already present
            k = j + 1
            while k < len(src) and not src[k].strip():
                k += 1
            if k < len(src) and src[k].lstrip().startswith(STR_START):
                continue
            indent = " " * (def_indent + 4)
            src.insert(j + 1, f'{indent}"""{doc}"""')
            total += 1
        path.write_text("\n".join(src) + "\n", encoding="utf-8")
        print(f"  {mod}.pyx: inserted {len(lines_)}")
    print(f"Total inserted: {total}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
