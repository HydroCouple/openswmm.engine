#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# Copyright 2026 Caleb Buahin
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Regenerate the masonry cross-section tables of both engines (issue #27, USEPA #160).

The seven masonry shapes (EGGSHAPED, HORSESHOE, GOTHIC, CATENARY, SEMIELLIPTICAL,
BASKETHANDLE, SEMICIRCULAR) are described by normalized tables: W(Y) width, A(Y)
area, R(Y) hydraulic radius, Y(A) depth from area and S(A) section factor, each on
a uniform grid of the normalized depth or area. EPA SWMM's tables were digitized
from charts and do not follow from one another (the area table of a shape is not
the integral of its width table). This script derives every table of a shape from
one description of its geometry, so they agree by construction:

- EGGSHAPED and HORSESHOE have classic circular-arc constructions, which reproduce
  the full-section constants EPA SWMM carries (area 0.5105 H^2 and hydraulic radius
  0.1931 H for the egg; 0.8293 D^2 and 0.2538 D for the horseshoe) to four digits.
  Every table and constant of these two shapes comes from the exact geometry.
- The other five shapes have no known construction. Their EPA SWMM 5.2.4 width
  tables and full-section constants are kept as given (frozen below), and their
  other tables are derived from the width table by exact integration of its
  piecewise-linear width and the perimeter of the polygon it describes.

Writes (with --write), in place:
  src/legacy/engine/xsect.dat          tables (OpenSWMM 5.3.0)
  src/legacy/engine/xsect.c            full-section constants and Amax[]
  src/engine/hydraulics/xsect_tables.hpp  tables and Amax[] (OpenSWMM 6.0)
  src/engine/hydraulics/XSection.cpp   full-section constants
  docs/manuals/reference/hydraulics/sections/Appendix.md  Appendix F tables
Run clang-format on xsect_tables.hpp afterwards. --check regenerates in memory and
exits 1 when any of those files differs from what it would write.

Lookups in both engines index a uniform grid, so table size has no runtime cost.
"""

import argparse
import math
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
N = 51          # entries of every regenerated table (grid step 0.02), like the circle's
FINE = 100000   # integration intervals on the normalized depth [0, 1]

# Width tables W/Wmax(Y/Yfull) of EPA SWMM 5.2.4 for the shapes with no known construction,
# kept as given. Their grids are uniform with len-1 intervals.
W524 = {
    "Gothic": [0.0, 0.286, 0.643, 0.762, 0.833, 0.905, 0.952, 0.976, 0.976, 1.0, 1.0,
               0.976, 0.976, 0.952, 0.905, 0.833, 0.762, 0.667, 0.524, 0.357, 0.0],
    "Catenary": [0.0, 0.6667, 0.8222, 0.9111, 0.9778, 1.0, 1.0, 0.9889, 0.9778, 0.9556,
                 0.9333, 0.8889, 0.8444, 0.8, 0.7556, 0.7, 0.6333, 0.5556, 0.4444, 0.3333, 0.0],
    "SemiEllip": [0.0, 0.7, 0.98, 1.0, 1.0, 1.0, 0.99, 0.98, 0.96, 0.94, 0.91, 0.88, 0.84,
                  0.8, 0.75, 0.7, 0.64, 0.56, 0.46, 0.34, 0.0],
    "BasketHandle": [0.0, 0.49, 0.667, 0.82, 0.93, 1.0, 1.0, 1.0, 0.997, 0.994, 0.988, 0.982,
                     0.967, 0.948, 0.928, 0.904, 0.874, 0.842, 0.798, 0.75, 0.697, 0.637,
                     0.567, 0.467, 0.342, 0.0],
    "SemiCirc": [0.0, 0.5488, 0.8537, 1.0, 1.0, 0.9939, 0.9878, 0.9756, 0.9634, 0.9451,
                 0.9207, 0.8902, 0.8537, 0.8171, 0.7683, 0.7073, 0.6463, 0.5732, 0.4756,
                 0.3354, 0.0],
}


# ----------------------------------------------------------------------------
# Half-width functions x(y), with the full depth 1 and x in the same unit
# ----------------------------------------------------------------------------
def egg_halfwidth(y):
    """Egg: H = 3r. Top semicircle radius r at (0, 2r); side arcs radius 3r centered at
    (+-2r, 2r), tangent to the top circle at y = 2r and to the bottom circle at y = 0.2r;
    bottom circle radius r/2 at (0, r/2)."""
    r = 1.0 / 3.0
    if y <= 0.2 * r:
        return math.sqrt(max(0.0, (r / 2) ** 2 - (y - r / 2) ** 2))
    if y <= 2 * r:
        return math.sqrt(9 * r * r - (y - 2 * r) ** 2) - 2 * r
    return math.sqrt(max(0.0, r * r - (y - 2 * r) ** 2))


HORSESHOE_JOIN = 0.5 - (math.sqrt(7.0) - 1.0) / 4.0  # 0.0886 D, where the invert arc meets the sides


def horseshoe_halfwidth(y):
    """Horseshoe: height = width = D. Top semicircle radius D/2 at (0, D/2); side arcs radius
    D centered at (+-D/2, D/2); invert arc radius D centered at the crown (0, D)."""
    if y <= HORSESHOE_JOIN:
        return math.sqrt(max(0.0, 1.0 - (y - 1.0) ** 2))
    if y <= 0.5:
        return math.sqrt(1.0 - (y - 0.5) ** 2) - 0.5
    return math.sqrt(max(0.0, 0.25 - (y - 0.5) ** 2))


def table_halfwidth(W, wmax_ratio):
    """Half-width from a frozen W/Wmax table (piecewise linear) and the code's Wmax/Yfull."""
    n = len(W) - 1

    def hw(y):
        if y <= 0.0:
            return 0.5 * wmax_ratio * W[0]
        if y >= 1.0:
            return 0.5 * wmax_ratio * W[-1]
        t = y * n
        i = min(int(t), n - 1)
        return 0.5 * wmax_ratio * (W[i] + (t - i) * (W[i + 1] - W[i]))

    return hw


# name, legacy enum, kind, halfwidth, kept constants (aFull/Yfull^2, rFull/Yfull, wMax/Yfull,
# ywMax/Yfull) for table shapes, and the table names in both engines' files.
SHAPES = [
    dict(name="Egg", enum="EGGSHAPED", kind="exact", hw=egg_halfwidth,
         tables=dict(A="A_Egg", R="R_Egg", Y="Y_Egg", S="S_Egg", W="W_Egg")),
    dict(name="Horseshoe", enum="HORSESHOE", kind="exact", hw=horseshoe_halfwidth,
         tables=dict(A="A_Horseshoe", R="R_Horseshoe", Y="Y_Horseshoe", S="S_Horseshoe",
                     W="W_Horseshoe")),
    dict(name="Gothic", enum="GOTHIC", kind="table", consts=(0.6554, 0.2269, 0.84, 0.45),
         tables=dict(Y="Y_Gothic", S="S_Gothic")),
    dict(name="Catenary", enum="CATENARY", kind="table", consts=(0.70277, 0.23172, 0.9, 0.25),
         tables=dict(Y="Y_Catenary", S="S_Catenary")),
    dict(name="SemiEllip", enum="SEMIELLIPTICAL", kind="table", consts=(0.785, 0.242, 1.0, 0.15),
         tables=dict(Y="Y_SemiEllip", S="S_SemiEllip")),
    dict(name="BasketHandle", enum="BASKETHANDLE", kind="table",
         consts=(0.7862, 0.2464, 0.944, 0.2),
         tables=dict(A="A_Baskethandle", R="R_Baskethandle", Y="Y_BasketHandle",
                     S="S_BasketHandle")),
    dict(name="SemiCirc", enum="SEMICIRCULAR", kind="table", consts=(1.2697, 0.2946, 1.64, 0.15),
         tables=dict(Y="Y_SemiCirc", S="S_SemiCirc")),
]


def derive(shape):
    """Every table and constant of a shape from its half-width function."""
    if shape["kind"] == "exact":
        hw = shape["hw"]
    else:
        hw = table_halfwidth(W524[shape["name"]], shape["consts"][2])
    ys = [i / FINE for i in range(FINE + 1)]
    xs = [hw(y) for y in ys]
    A = [0.0]
    P = [2.0 * xs[0]]
    for i in range(FINE):
        dy = 1.0 / FINE
        A.append(A[-1] + (xs[i] + xs[i + 1]) * dy)
        P.append(P[-1] + 2.0 * math.hypot(dy, xs[i + 1] - xs[i]))
    afull, pfull = A[-1], P[-1]
    rfull = afull / pfull
    sfull = afull * rfull ** (2.0 / 3.0)
    An = [a / afull for a in A]
    Rn = [(a / p) / rfull if p > 0.0 else 0.0 for a, p in zip(A, P)]
    Sn = [an * rn ** (2.0 / 3.0) for an, rn in zip(An, Rn)]

    def at(y):  # index into the fine grid
        return min(int(round(y * FINE)), FINE)

    def y_of_alpha(alpha):  # invert the monotone An
        lo, hi = 0, FINE
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if An[mid] < alpha:
                lo = mid
            else:
                hi = mid
        if An[hi] == An[lo]:
            return ys[lo]
        return ys[lo] + (alpha - An[lo]) / (An[hi] - An[lo]) * (ys[hi] - ys[lo])

    grid = [i / (N - 1) for i in range(N)]
    out = {}
    if "A" in shape["tables"]:
        out["A"] = [An[at(g)] for g in grid]
    if "R" in shape["tables"]:
        out["R"] = [Rn[at(g)] for g in grid]
    ya = [y_of_alpha(g) for g in grid]
    out["Y"] = ya
    out["S"] = [Sn[at(y)] for y in ya]
    wmax = 2.0 * max(xs)
    if shape["kind"] == "exact":
        out["W"] = [2.0 * hw(g) / wmax for g in grid]
        consts = (afull, rfull, wmax, ys[xs.index(max(xs))])
    else:
        consts = shape["consts"]
    i_smax = max(range(FINE + 1), key=lambda i: Sn[i])
    return dict(kind=shape["kind"], tables=out, aFull=consts[0], rFull=consts[1], wMax=consts[2],
                ywMax=consts[3], sMax=Sn[i_smax], Amax=An[i_smax], afull_from_width=afull,
                rfull_from_width=rfull)


def fmt(v):
    return f"{v:.5f}"


# ----------------------------------------------------------------------------
# Writers
# ----------------------------------------------------------------------------
def c_rows(values, indent):
    rows, row = [], []
    for v in values:
        row.append(fmt(v))
        if len(row) == 9:
            rows.append(", ".join(row))
            row = []
    if row:
        rows.append(", ".join(row))
    return (",\n" + indent).join(rows)


def replace_once(text, pattern, repl, what):
    new, n = re.subn(pattern, repl, text, count=1, flags=re.S)
    if n != 1:
        sys.exit(f"could not find {what}")
    return new


def write_legacy_dat(text, name, values):
    pat = rf"int\s+N_{name}\s*=\s*\d+;\s*\n\s*double\s+{name}\[\d+\]\s*=\s*\n?\s*\{{[^}}]*\}};"
    body = c_rows(values, " ")
    repl = f"int    N_{name}   = {len(values)};\ndouble {name}[{len(values)}] =\n{{{body}}};"
    return replace_once(text, pat, lambda m: repl, f"legacy table {name}")


def write_hpp(text, name, values):
    pat = (rf"static constexpr int N_{name} = \d+;\s*\n\s*static constexpr double {name}\[\d+\]"
           rf"\s*=\s*\{{[^}}]*\}};")
    body = c_rows(values, "    ")
    repl = (f"static constexpr int N_{name} = {len(values)};\n"
            f"static constexpr double {name}[{len(values)}] = {{\n    {body}}};")
    return replace_once(text, pat, lambda m: repl, f"6.0 table {name}")


def write_consts(text, enum, d, legacy):
    """Full-section constants in the shape's case block of xsect.c or XSection.cpp."""
    start = text.index(f"case {'' if legacy else 'XSectShape::'}{enum}:")
    end = text.index("break;" if legacy else "return 0;", start)
    block = text[start:end]
    a, r, s, y = "aFull", "rFull", "sMax", "ywMax"
    if not legacy:
        a, r, s, y = "a_full", "r_full", "s_max", "yw_max"
    # sMax is the maximum of the regenerated S(A) table, so it is rewritten for every shape;
    # aFull and rFull only for the shapes whose geometry gives them (the others keep the
    # constants as written).
    sub = [(rf"({s} = )[0-9.]+( \*)", f"{d['sMax']:.5f}")]
    if d["kind"] == "exact":
        sub += [(rf"({a} = )[0-9.]+( \*)", fmt(d["aFull"])),
                (rf"({r} = )[0-9.]+( \*)", fmt(d["rFull"]))]
    if enum == "EGGSHAPED":  # the exact maximum width sits at 2/3 of the height, not 0.64
        full = "xsect->yFull" if legacy else r"xs\.y_full"
        sub.append((rf"({y} = )[0-9. /]+?( \* {full})", "2. / 3." if legacy else "2.0 / 3.0"))
    for pat, val in sub:
        block, n = re.subn(pat, lambda m, v=val: m.group(1) + v + m.group(2), block, count=1)
        if n != 1:
            sys.exit(f"could not find {pat} in {enum}")
    return text[:start] + block + text[end:]


def write_amax(text, enum, value):
    # xsect.c writes "0.96,   //  EGGSHAPED"; xsect_tables.hpp "0.96,   // 15 - EGGSHAPED".
    return replace_once(text, rf"[0-9.]+(,?\s*//\s*(?:\d+\s*-\s*)?{enum}\b)",
                        lambda m: f"{value:.4f}" + m.group(1), f"Amax {enum}")


def doc_table(title, header, columns, grid_cols):
    """A Markdown table: first column the grid, then one column per (name, values)."""
    rows = [f"**{title}**", "| " + " | ".join(header) + " |",
            "| " + " | ".join(":---" for _ in header) + " |"]
    n = max(len(v) for _, v in columns)
    for i in range(n):
        cells = [f"{i / (n - 1):.2f}"]
        for _, v in columns:
            if len(v) == n:
                cells.append(fmt(v[i]))
            else:  # a coarser table: a value only where its own grid has a node
                k = i * (len(v) - 1)
                cells.append(fmt(v[k // (n - 1)]) if k % (n - 1) == 0 else "")
        rows.append("| " + " | ".join(cells) + " |")
    return "\n".join(rows) + "\n"


def write_appendix(text, R):
    def col(shape, key):
        s = R[shape]
        if key == "W" and "W" not in s["tables"]:
            return W524[shape]
        return s["tables"][key]

    yy, aa = "**Y/Y<sub>full</sub>**", "**A/A<sub>full</sub>**"
    tables = [
        ("Table F‑1 Area of masonry sewers as function of depth",
         [yy, "**Basket Handle**", "**Egg**", "**Horseshoe**"],
         [("BasketHandle", col("BasketHandle", "A")), ("Egg", col("Egg", "A")),
          ("Horseshoe", col("Horseshoe", "A"))]),
        ("Table F‑2 Width of masonry sewers as function of depth - I",
         [yy, "**Basket Handle**", "**Egg**", "**Horseshoe**"],
         [("BasketHandle", col("BasketHandle", "W")), ("Egg", col("Egg", "W")),
          ("Horseshoe", col("Horseshoe", "W"))]),
        ("Table F‑3 Width of masonry sewers as function of depth - II",
         [yy, "**Gothic**", "**Catenary**", "**Semi-Elliptical**", "**Semi-Circular**"],
         [("Gothic", col("Gothic", "W")), ("Catenary", col("Catenary", "W")),
          ("SemiEllip", col("SemiEllip", "W")), ("SemiCirc", col("SemiCirc", "W"))]),
        ("Table F‑4 Hydraulic radius of masonry sewers as function of depth",
         [yy, "**R/R<sub>full</sub> (Basket Handle)**", "**R/R<sub>full</sub> (Egg)**",
          "**R/R<sub>full</sub> (Horseshoe)**"],
         [("BasketHandle", col("BasketHandle", "R")), ("Egg", col("Egg", "R")),
          ("Horseshoe", col("Horseshoe", "R"))]),
        ("Table F‑5 Depth of masonry sewers as function of area - I",
         [aa, "**Y/Y<sub>full</sub> (Basket Handle)**", "**Y/Y<sub>full</sub> (Egg)**",
          "**Y/Y<sub>full</sub> (Horseshoe)**"],
         [("BasketHandle", col("BasketHandle", "Y")), ("Egg", col("Egg", "Y")),
          ("Horseshoe", col("Horseshoe", "Y"))]),
        ("Table F‑6 Depth of masonry sewers as function of area - II",
         [aa, "**Y/Y<sub>full</sub> (Catenary)**", "**Y/Y<sub>full</sub> (Gothic)**",
          "**Y/Y<sub>full</sub> (Semi-Circular)**", "**Y/Y<sub>full</sub> (Semi-Elliptical)**"],
         [("Catenary", col("Catenary", "Y")), ("Gothic", col("Gothic", "Y")),
          ("SemiCirc", col("SemiCirc", "Y")), ("SemiEllip", col("SemiEllip", "Y"))]),
        ("Table F‑7 Section factor for masonry sewers as function of area - I",
         [aa, "**Ψ/Ψ<sub>full</sub> (Basket Handle)**", "**Ψ/Ψ<sub>full</sub> (Egg)**",
          "**Ψ/Ψ<sub>full</sub> (Horseshoe)**"],
         [("BasketHandle", col("BasketHandle", "S")), ("Egg", col("Egg", "S")),
          ("Horseshoe", col("Horseshoe", "S"))]),
        ("Table F‑8 Section factor for masonry sewers as function of area - II",
         [aa, "**Ψ/Ψ<sub>full</sub> (Catenary)**", "**Ψ/Ψ<sub>full</sub> (Gothic)**",
          "**Ψ/Ψ<sub>full</sub> (Semi-Circular)**", "**Ψ/Ψ<sub>full</sub> (Semi-Elliptical)**"],
         [("Catenary", col("Catenary", "S")), ("Gothic", col("Gothic", "S")),
          ("SemiCirc", col("SemiCirc", "S")), ("SemiEllip", col("SemiEllip", "S"))]),
    ]
    start = text.index("## F. Section Properties of Masonry Sewers")
    end = text.index("## G. ", start)
    intro = ("## F. Section Properties of Masonry Sewers\n\n"
             "These tables are generated by `scripts/gen_xsect_masonry_tables.py`, which also writes "
             "them into both engines. The egg and horseshoe tables come from the exact circular-arc "
             "constructions of those sections; the other five shapes keep the EPA SWMM 5 width "
             "tables, and their remaining tables are derived from them. A blank cell in Table F-2 "
             "is a depth at which the basket handle's 26-entry width table has no node.\n\n")
    body = "\n".join(doc_table(t, h, c, None) for t, h, c in tables)
    return text[:start] + intro + body + "\n" + text[end:]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--write", action="store_true", help="rewrite the engine and manual files")
    ap.add_argument("--check", action="store_true", help="exit 1 when any file differs from the generated text")
    args = ap.parse_args()

    R = {s["name"]: derive(s) for s in SHAPES}
    for s in SHAPES:
        d = R[s["name"]]
        print(f"{s['name']:13s} {s['kind']:5s} aFull/Y2 {fmt(d['aFull'])} rFull/Y {fmt(d['rFull'])} "
              f"wMax/Y {d['wMax']:.5f} ywMax/Y {d['ywMax']:.5f} sMax/sFull {d['sMax']:.5f} "
              f"Amax {d['Amax']:.4f}"
              + ("" if s["kind"] == "exact" else
                 f"   (width table integrates to {fmt(d['afull_from_width'])}, R {fmt(d['rfull_from_width'])})"))
        S = d["tables"]["S"]
        peak = max(range(N), key=lambda i: S[i])
        if peak < N - 3:
            sys.exit(f"{s['name']}: S(A) peaks at index {peak}; invLookup needs it at {N - 3} or later")

    files = {
        "dat": ROOT / "src/legacy/engine/xsect.dat",
        "c": ROOT / "src/legacy/engine/xsect.c",
        "hpp": ROOT / "src/engine/hydraulics/xsect_tables.hpp",
        "cpp": ROOT / "src/engine/hydraulics/XSection.cpp",
        "md": ROOT / "docs/manuals/reference/hydraulics/sections/Appendix.md",
    }
    text = {k: p.read_text() for k, p in files.items()}
    new = dict(text)
    for s in SHAPES:
        d = R[s["name"]]
        for key, name in s["tables"].items():
            new["dat"] = write_legacy_dat(new["dat"], name, d["tables"][key])
            new["hpp"] = write_hpp(new["hpp"], name, d["tables"][key])
        new["c"] = write_consts(new["c"], s["enum"], d, legacy=True)
        new["cpp"] = write_consts(new["cpp"], s["enum"], d, legacy=False)
        new["c"] = write_amax(new["c"], s["enum"], d["Amax"])
        new["hpp"] = write_amax(new["hpp"], s["enum"], d["Amax"])
    new["md"] = write_appendix(new["md"], R)

    changed = [k for k in files if new[k] != text[k]]
    if args.check:
        # clang-format reflows the generated C++ lines, so compare the files' tokens, not
        # their layout: a value, count or name that differs still fails the check.
        tokens = lambda s: re.sub(r"\s+", "", s)
        drift = [k for k in files if tokens(new[k]) != tokens(text[k])]
        for k in drift:
            print(f"differs: {files[k].relative_to(ROOT)}")
        sys.exit(1 if drift else 0)
    if args.write:
        for k in changed:
            files[k].write_text(new[k])
            print(f"wrote {files[k].relative_to(ROOT)}")
    elif changed:
        print("would change: " + ", ".join(str(files[k].relative_to(ROOT)) for k in changed))


if __name__ == "__main__":
    main()
