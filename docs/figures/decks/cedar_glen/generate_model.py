#!/usr/bin/env python3
"""Generate the Cedar Glen to FRS demonstration model.

A synthetic raw-water conveyance in the same style as a GIS-derived
client network: a long trunk built from short survey segments chained by
virtual junctions, two lateral branches, and a terminal flow-regulating
storage (FRS) basin with orifice outlets. All geometry, elevations, and
names are invented; nothing here comes from any client dataset.

Writes cedar_glen_fv.inp and cedar_glen_dw.inp (identical decks except
FLOW_ROUTING; the FV_* options are inert under dynamic wave).

Run: python generate_model.py
"""

from __future__ import annotations

import math
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
RNG = np.random.default_rng(20260816)

FT_PER_SEG = (60.0, 140.0)          # survey segment length range, ft


def alignment(n_pts: int, start_xy, heading_deg, drift_deg=0.0, wobble=7.0,
              seg_range=FT_PER_SEG):
    """Meandering polyline from a smooth random walk. Returns (xy, s)."""
    xy = [np.asarray(start_xy, dtype=float)]
    s = [0.0]
    theta = math.radians(heading_deg)
    drift = math.radians(drift_deg)
    for _ in range(n_pts - 1):
        theta += drift + math.radians(RNG.normal(0.0, wobble))
        # relax back toward the drift direction so the line keeps going
        theta = 0.92 * theta + 0.08 * (math.radians(heading_deg) + drift * len(s))
        L = RNG.uniform(*seg_range)
        step = np.array([math.cos(theta), math.sin(theta)]) * L
        xy.append(xy[-1] + step)
        s.append(s[-1] + L)
    return np.array(xy), np.array(s)


def profile(s, elev_top, reaches):
    """Piecewise-slope invert profile along chainage s.

    reaches: list of (fraction_of_length, slope). Fractions sum to 1.
    """
    total = s[-1]
    z = np.empty_like(s)
    z[0] = elev_top
    bounds = np.cumsum([f for f, _ in reaches]) * total
    slopes = [sl for _, sl in reaches]
    for i in range(1, len(s)):
        ds = s[i] - s[i - 1]
        k = np.searchsorted(bounds, s[i], side="left")
        k = min(k, len(slopes) - 1)
        z[i] = z[i - 1] - slopes[k] * ds
    return z


class Net:
    def __init__(self):
        self.junctions = []           # (name, invert, maxdepth, surdepth)
        self.virtual = []             # (name, invert, rim)
        self.conduits = []            # (name, n1, n2, length, diam_ft)
        self.coords = {}              # name -> (x, y)

    def add_run(self, prefix, xy, s, z, diam, first_node=None, last_node=None,
                real_every_ft=2600.0):
        """Add a chain of short conduits; interior nodes are virtual
        junctions except a real access junction every ~real_every_ft."""
        n = len(s)
        names = []
        next_real = real_every_ft
        for i in range(n):
            if i == 0 and first_node:
                names.append(first_node)
                continue
            if i == n - 1 and last_node:
                names.append(last_node)
                self.coords[last_node] = tuple(xy[i])
                continue
            name = f"{prefix}{s[i]:07.0f}"
            names.append(name)
            self.coords[name] = tuple(xy[i])
            if i == 0 or i == n - 1:
                # run endpoints are real junctions (heads and tie-ins can
                # carry inflows or a third conduit; VJs take exactly two)
                self.junctions.append((name, z[i], 0.0, 40.0))
            elif s[i] >= next_real and i < n - 1:
                self.junctions.append((name, z[i], 0.0, 40.0))
                next_real += real_every_ft
            else:
                self.virtual.append((name, z[i], diam + 5.0))
        for i in range(n - 1):
            self.conduits.append((f"{prefix}C{i:03d}", names[i], names[i + 1],
                                  s[i + 1] - s[i], diam))
        return names


def main():
    net = Net()

    # Trunk: ~26,000 ft, 72 in upper, 78 in middle, 90 in lower.
    n_tr = 260
    xy, s = alignment(n_tr, (10000.0, 42000.0), heading_deg=-55.0,
                      drift_deg=-0.28)
    # between the tie-ins the invert dips into pocket A, climbs a STEEP
    # adverse riser to a crest at ~455 ft, drops into pocket B, and lips up
    # again: the double-pocket grade a valved raw-water conveyance carries
    z = profile(s, 468.0, [(0.22, 0.0030), (0.18, 0.0018),
                           (0.04, 0.0008),                  # pocket A floor
                           (0.03, -0.0155),                 # riser to ~455
                           (0.07, 0.0065),                  # steep fall
                           (0.05, 0.0008),                  # pocket B floor
                           (0.02, -0.0050),                 # second lip
                           (0.21, 0.0022), (0.18, 0.0028)])
    thirds = [int(n_tr * 0.38), int(n_tr * 0.66)]
    tie_a, tie_b = thirds

    trunk_names = []
    seg_specs = [(0, tie_a, 6.0, "TU"), (tie_a, tie_b, 6.5, "TM"),
                 (tie_b, n_tr - 1, 7.5, "TL")]
    for k, (i0, i1, diam, pref) in enumerate(seg_specs):
        first = trunk_names[-1] if trunk_names else None
        names = net.add_run(pref, xy[i0:i1 + 1], s[i0:i1 + 1] - s[i0],
                            z[i0:i1 + 1], diam, first_node=first,
                            last_node="FRS_Basin" if k == 2 else None)
        trunk_names.extend(names if not first else names[1:])

    tie_a_node, tie_b_node = trunk_names[tie_a], trunk_names[tie_b]

    # Branch A: 7,400 ft of 48 in joining the trunk at tie_a.
    n_a = 74
    xy_a, s_a = alignment(n_a, tuple(xy[tie_a] + (5200.0, 5600.0)),
                          heading_deg=-125.0, drift_deg=0.10)
    xy_a = xy_a - xy_a[-1] + xy[tie_a]           # land exactly on the tie-in
    z_a = profile(s_a, z[tie_a] + 118.0, [(0.5, 0.0060), (0.5, 0.0040)])
    z_a += z[tie_a] - z_a[-1]
    net.add_run("BA", xy_a, s_a, z_a, 4.0, last_node=tie_a_node)

    # Branch B: 5,600 ft of 42 in joining the trunk at tie_b.
    n_b = 56
    xy_b, s_b = alignment(n_b, tuple(xy[tie_b] + (-6100.0, 4200.0)),
                          heading_deg=-35.0, drift_deg=-0.12)
    xy_b = xy_b - xy_b[-1] + xy[tie_b]
    z_b = profile(s_b, z[tie_b] + 86.0, [(0.6, 0.0055), (0.4, 0.0040)])
    z_b += z[tie_b] - z_b[-1]
    net.add_run("BB", xy_b, s_b, z_b, 3.5, last_node=tie_b_node)

    frs_invert = z[-1] - 2.0
    net.coords["FRS_Basin"] = tuple(xy[-1])
    net.coords["OUT_RIVER"] = tuple(xy[-1] + (900.0, -700.0))
    net.coords["OUT_OVERFLOW"] = tuple(xy[-1] + (1100.0, 300.0))

    head_trunk = trunk_names[0]
    head_a = f"BA{0:07.0f}"
    head_b = f"BB{0:07.0f}"

    n_vj = len(net.virtual)
    n_j = len(net.junctions)
    n_c = len(net.conduits)

    def fmt_ts(pairs):
        return "\n".join(f"{name:<18} {h:>7} {q:>10.2f}"
                         for name, h, q in pairs)

    def hydrograph(name, base, peak, t_rise, t_peak0, t_peak1, t_end):
        # two surge-and-drain cycles across the 24 h run; the second peaks
        # at 85 % of the first
        knots = [(0.0, base), (t_rise, base), (t_peak0, peak), (t_peak1, peak),
                 (t_end, base)]
        off, peak2 = 12.0, 0.85 * peak
        knots += [(t_rise + off, base), (t_peak0 + off, peak2),
                  (t_peak1 + off, peak2), (t_end + off, base), (24.0, base)]
        pts = []
        for hh, q in knots:
            hstr = f"{int(hh)}:{int(round((hh % 1) * 60)):02d}"
            pts.append((name, hstr, q))
        return pts

    ts = (hydrograph("Trunk_Surge", 12.0, 140.0, 1.0, 2.5, 4.0, 7.0)
          + hydrograph("BranchA_Surge", 4.0, 45.0, 1.5, 3.0, 4.5, 7.5)
          + hydrograph("BranchB_Surge", 3.0, 30.0, 2.0, 3.5, 5.0, 8.0))

    options = """[OPTIONS]
;;Option               Value
FLOW_UNITS           MGD
INFILTRATION         HORTON
FLOW_ROUTING         {routing}
LINK_OFFSETS         DEPTH
MIN_SLOPE            0
ALLOW_PONDING        YES
SKIP_STEADY_STATE    NO

START_DATE           01/01/2026
START_TIME           00:00:00
REPORT_START_DATE    01/01/2026
REPORT_START_TIME    00:00:00
END_DATE             01/02/2026
END_TIME             00:00:00
REPORT_STEP          0:01:00
WET_STEP             0:05:00
DRY_STEP             1:00:00
ROUTING_STEP         0:00:30

INERTIAL_DAMPING     PARTIAL
NORMAL_FLOW_LIMITED  BOTH
FORCE_MAIN_EQUATION  H-W
SURCHARGE_METHOD     {surcharge}
VARIABLE_STEP        0.75
LENGTHENING_STEP     0
MIN_SURFAREA         0
MAX_TRIALS           8
HEAD_TOLERANCE       0.005
SYS_FLOW_TOL         5
LAT_FLOW_TOL         5
MINIMUM_STEP         0.5

FV_LTS               YES
FV_LTS_MAX_TIERS     4
"""

    body = ["[TITLE]",
            ";;Cedar Glen to FRS -- synthetic conveyance demonstration",
            ";;Generated by generate_model.py (seed 20260816); all data invented.",
            "", "@OPTIONS@", "",
            "[JUNCTIONS]",
            ";;Name             Elev       MaxDepth   InitDepth  SurDepth   Aponded"]
    for name, inv, mx, sur in net.junctions:
        body.append(f"{name:<18} {inv:>10.4f} {mx:>10.4f} {0.0:>10.4f} "
                    f"{sur:>10.4f} {0.0:>10.4f}")

    body += ["", "[VIRTUAL_JUNCTIONS]",
             ";;Name             Elev       Rim"]
    for name, inv, rim in net.virtual:
        body.append(f"{name:<18} {inv:>10.4f} {rim:>10.4f}")

    body += ["", "[OUTFALLS]",
             ";;Name             Elev       Type       Gated",
             f"{'OUT_RIVER':<18} {frs_invert - 3.0:>10.4f} FREE       NO",
             f"{'OUT_OVERFLOW':<18} {frs_invert + 6.0:>10.4f} FREE       NO",
             "", "[STORAGE]",
             ";;Name             Elev       MaxDepth   InitDepth  Shape      "
             "Coeff    Expon    Const    SurDepth Fevap",
             f"{'FRS_Basin':<18} {frs_invert:>10.4f} {22.0:>10.4f} {2.0:>10.4f} "
             f"FUNCTIONAL 0        0        120000   0        0",
             "", "[CONDUITS]",
             ";;Name             FromNode           ToNode             "
             "Length     Roughness  InOffset   OutOffset  InitFlow   MaxFlow"]
    for name, n1, n2, length, _ in net.conduits:
        body.append(f"{name:<18} {n1:<18} {n2:<18} {length:>10.2f} "
                    f"{0.013:>10.4f} {0.0:>10.4f} {0.0:>10.4f} "
                    f"{0.0:>10.4f} {0.0:>10.4f}")

    body += ["", "[ORIFICES]",
             ";;Name             FromNode           ToNode             "
             "Type       Offset     Cd         Gated",
             f"{'FRS_Outlet':<18} {'FRS_Basin':<18} {'OUT_RIVER':<18} "
             f"SIDE       {0.0:>10.4f} {0.65:>10.4f} NO",
             f"{'FRS_Overflow':<18} {'FRS_Basin':<18} {'OUT_OVERFLOW':<18} "
             f"SIDE       {8.0:>10.4f} {1.0:>10.4f} NO",
             "", "[XSECTIONS]",
             ";;Link             Shape        Geom1      Geom2 Geom3 Geom4 Barrels"]
    for name, _, _, _, diam in net.conduits:
        body.append(f"{name:<18} CIRCULAR     {diam:>10.4f} 0     0     0     1")
    body.append(f"{'FRS_Outlet':<18} CIRCULAR     {3.0:>10.4f} 0     0     0")
    body.append(f"{'FRS_Overflow':<18} RECT_CLOSED  {4.0:>10.4f} 8     0     0")

    body += ["", "[INFLOWS]",
             ";;Node             Constituent  TimeSeries       Type   Mfactor Sfactor",
             f"{head_trunk:<18} FLOW         Trunk_Surge      FLOW   1.0     1.0",
             f"{head_a:<18} FLOW         BranchA_Surge    FLOW   1.0     1.0",
             f"{head_b:<18} FLOW         BranchB_Surge    FLOW   1.0     1.0",
             "", "[TIMESERIES]",
             ";;Name             Time    Value",
             fmt_ts(ts),
             "", "[REPORT]",
             "INPUT      NO", "CONTINUITY YES", "FLOWSTATS  YES",
             "", "[COORDINATES]",
             ";;Node             X-Coord        Y-Coord"]
    for name, (x, y) in net.coords.items():
        body.append(f"{name:<18} {x:>14.4f} {y:>14.4f}")

    # metadata for the animation / figure scripts
    import json
    diam_at = np.empty(n_tr)
    for i0, i1, diam, _ in seg_specs:
        diam_at[i0:i1 + 1] = diam
    meta = {
        "trunk": [{"name": trunk_names[i], "s": float(s[i]),
                   "z": float(z[i]), "diam": float(diam_at[i])}
                  for i in range(n_tr)],
        "conduits": [{"name": c[0], "n1": c[1], "n2": c[2],
                      "diam": c[4]} for c in net.conduits],
        "coords": {k: [float(v[0]), float(v[1])] for k, v in net.coords.items()},
        "frs_invert": float(frs_invert),
        "tie_nodes": [tie_a_node, tie_b_node],
        "heads": [head_trunk, head_a, head_b],
    }
    (HERE / "model_meta.json").write_text(json.dumps(meta, indent=1))

    template = "\n".join(body) + "\n"
    decks = (("FV", "SLOT", "cedar_glen_fv.inp"),
             ("DYNWAVE", "SLOT", "cedar_glen_dw.inp"),
             ("DYNWAVE", "EXTRAN", "cedar_glen_dw_extran.inp"))
    for routing, surcharge, fname in decks:
        text = template.replace("@OPTIONS@", options.format(
            routing=routing, surcharge=surcharge))
        (HERE / fname).write_text(text)

    print(f"junctions={n_j} virtual={n_vj} conduits={n_c} "
          f"trunk_len={s[-1]:.0f}ft  heads: {head_trunk} {head_a} {head_b}")


if __name__ == "__main__":
    main()
