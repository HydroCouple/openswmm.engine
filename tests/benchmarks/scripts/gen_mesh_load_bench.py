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
"""Synthetic 2D mesh decks for the model-load benchmark.

``gen_load_bench.py`` covers the 1D load path; nothing in that corpus has a
mesh. These decks put a structured-grid ``[2D_*]`` mesh in front of
``bench_model_load`` two ways:

* ``*_sidecar``: a small ``.inp`` with ``[2D_MESH_FILE]`` plus a ``.2dm``
  (the GUI's default save mode; read by ``twoD::load2DMeshExternalFile``);
* ``*_inline``: the same sections inside the ``.inp`` (read by the main
  ``InputReader``).

The mesh bytes follow the GUI's ``make_inp_mesh_fixture.py``
(``openswmm.gui/libraries/meshkit/tests``) exactly, so the engine and GUI
benchmarks time the same files: a ``*_sidecar`` ``.2dm`` is byte-identical to
that script's output for the same cell count and options. ``mixed`` adds the
``--features`` rows (vertex/cell tags, tag-form ``[2D_VERTEX_NODE_MAP]``,
``[2D_BOUNDARY_CONDITIONS]``, ``[2D_EDGE_CONVEYANCE]``) and 50 rows of quads.

Deterministic: no RNG. Byte sizes can differ by a few bytes across platforms
(``math.sin`` in the libm); counts are exact.

The ``mixed`` decks open but do not initialize: their node maps name 1D nodes
the model does not have. Only their ``open`` cut is meaningful.

Usage::

    python3 gen_mesh_load_bench.py [--out-dir DIR] [--model NAME ...] [--list]

The default ``--out-dir`` is ``tests/benchmarks/generated/mesh2d`` and the
default model set is the 1 M decks. The 10 M deck (~765 MB) is generated only
when named; give it its own ``--out-dir`` so a whole-corpus benchmark run does
not pick it up (its full cut needs ~27 GB). stdlib only.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

_INP_HEAD = """[TITLE]
Synthetic mesh open I/O fixture ({cells} cells)
[OPTIONS]
FLOW_UNITS CMS
FLOW_ROUTING DYNWAVE
START_DATE 01/01/2026
START_TIME 00:00:00
END_DATE 01/01/2026
END_TIME 00:05:00
REPORT_STEP 00:01:00
ROUTING_STEP 5
[JUNCTIONS]
J1 -2 4 0 0 0
[OUTFALLS]
O1 -2.5 FREE NO
[CONDUITS]
C1 J1 O1 30 .013 0 0 0
[XSECTIONS]
C1 CIRCULAR .5 0 0 0 1
[2D_OPTIONS]
INTEGRATOR EXPLICIT
MAX_TIMESTEP 5
REPORT_2D NO
"""

_MESH_FILE = """[2D_MESH_FILE]
FILE {name}.2dm
"""


def _g(v: float) -> str:
    return "%.17g" % v


def _grid(cells: int, quad_rows: int) -> tuple[int, int]:
    """nx*ny grid squares; a square is two triangles, or one quad in a quad row."""
    nx = max(2, int(round(math.sqrt(cells / 2.0))))
    ny = max(2 + quad_rows, int(round(cells / (2.0 * nx))) + quad_rows // 2)
    return nx, ny


def write_mesh(f, cells: int, features: bool, quad_rows: int) -> tuple[int, int, int]:
    """Writes the mesh sections; returns (vertices, triangles, quads)."""
    nx, ny = _grid(cells, quad_rows)
    tri_rows = ny - quad_rows
    n_tris = 2 * nx * tri_rows
    n_quads = nx * quad_rows
    n_verts = (nx + 1) * (ny + 1)
    x0, y0, dx = 579466.31199999992, 6132060.1391500002, 2.1300000000000003

    f.write(";; UNITS: SI (m)\n;; SOURCE_CRS: EPSG:25832\n;; OpenSWMM 2D Mesh File\n")
    f.write(";; Synthetic fixture from make_inp_mesh_fixture.py\n")
    f.write("[2D_VERTICES]\n;; X            Y            Z            TAG\n")
    tag_every = 97 if features else 0
    for j in range(ny + 1):
        y = _g(y0 + j * dx)
        rows = []
        for i in range(nx + 1):
            v = j * (nx + 1) + i
            z = 30.0 + 0.001 * i + 0.0007 * j + 0.31 * math.sin(0.01 * v)
            row = "%s  %s  %s" % (_g(x0 + i * dx), y, _g(z))
            if tag_every and v % tag_every == 0:
                row += "  N%d" % v
            rows.append(row)
        f.write("\n".join(rows) + "\n")

    f.write("[2D_TRIANGLES]\n;; V1   V2   V3   MANNINGS_N   TAG\n")
    n = _g(0.035)
    for j in range(tri_rows):
        rows = []
        base = j * (nx + 1)
        for i in range(nx):
            v00 = base + i
            v10 = v00 + 1
            v01 = v00 + nx + 1
            v11 = v01 + 1
            if features and i % 211 == 0:
                rows.append("%d  %d  %d  %s  0.10000000000000001  zoneA" % (v00, v10, v11, n))
            else:
                rows.append("%d  %d  %d  %s" % (v00, v10, v11, n))
            rows.append("%d  %d  %d  %s" % (v00, v11, v01, n))
        f.write("\n".join(rows) + "\n")

    if n_quads:
        f.write("[2D_QUADS]\n;; V1   V2   V3   V4   MANNINGS_N   TAG\n")
        for j in range(tri_rows, ny):
            base = j * (nx + 1)
            f.write("\n".join("%d  %d  %d  %d  %s" % (base + i, base + i + 1,
                                                    base + i + nx + 2, base + i + nx + 1, n)
                              for i in range(nx)) + "\n")

    if features:
        f.write("[2D_VERTEX_NODE_MAP]\n;; VERTEX_INDEX_OR_TAG    SWMM_NODE_NAME    CD    AREA\n")
        for v in range(0, n_verts, tag_every):
            f.write("N%d        N%d  0.65000000000000002  1\n" % (v, v))
        f.write("[2D_BOUNDARY_CONDITIONS]\n;; TRI EDGE TYPE PARAM_1 PARAM_2 GROUP\n")
        for i in range(nx):  # bottom row: first triangle of each square, edge 0
            f.write("%d  0  NORMAL_FLOW  0.001  *  outlet\n" % (2 * i))
        f.write("[2D_EDGE_CONVEYANCE]\n;; FROM_VERTEX TO_VERTEX CONVEYANCE\n")
        mid = (tri_rows // 2) * (nx + 1)
        for i in range(nx):  # one interior horizontal line of edges
            f.write("%d  %d  0.5\n" % (mid + i, mid + i + 1))

    return n_verts, n_tris, n_quads


def write_deck(out_dir: Path, name: str, cells: int, features: bool, quad_rows: int,
               sidecar: bool) -> tuple[int, int, int]:
    """Writes <name>.inp (and <name>.2dm for a sidecar deck)."""
    # The title carries the rounded cell count, as the GUI script's does.
    nx, ny = _grid(cells, quad_rows)
    total_cells = 2 * nx * (ny - quad_rows) + nx * quad_rows
    head = _INP_HEAD.format(cells=total_cells)

    inp = out_dir / f"{name}.inp"
    if sidecar:
        with open(inp, "w", newline="\n") as f:
            f.write(head + _MESH_FILE.format(name=name))
        with open(out_dir / f"{name}.2dm", "w", newline="\n", buffering=1 << 22) as f:
            counts = write_mesh(f, cells, features, quad_rows)
    else:
        with open(inp, "w", newline="\n", buffering=1 << 22) as f:
            f.write(head)
            counts = write_mesh(f, cells, features, quad_rows)
    return counts


#: name -> (cells, features, quad_rows, sidecar, in the default set)
MODELS = {
    "mesh_tri_1m_sidecar":   (1_000_000, False, 0, True, True),
    "mesh_tri_1m_inline":    (1_000_000, False, 0, False, True),
    "mesh_mixed_1m_sidecar": (1_000_000, True, 50, True, True),
    "mesh_mixed_1m_inline":  (1_000_000, True, 50, False, True),
    "mesh_tri_10m_sidecar":  (10_000_000, False, 0, True, False),
}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out-dir", default=None,
                    help="destination (default: <repo>/tests/benchmarks/generated/mesh2d)")
    ap.add_argument("--model", action="append", default=None,
                    help="generate only this model (repeatable)")
    ap.add_argument("--list", action="store_true", help="list models and exit")
    ap.add_argument("--force", action="store_true",
                    help="regenerate even when the deck already exists")
    args = ap.parse_args(argv)

    if args.list:
        for name, (cells, features, quads, sidecar, default) in MODELS.items():
            kind = "sidecar" if sidecar else "inline"
            extra = " + features + %d quad rows" % quads if features else ""
            opt = "" if default else "  (only when named)"
            print(f"{name:24s} {cells:>10,d} cells, {kind}{extra}{opt}")
        return 0

    out_dir = Path(args.out_dir) if args.out_dir else (
        Path(__file__).resolve().parents[1] / "generated" / "mesh2d")
    out_dir.mkdir(parents=True, exist_ok=True)

    names = args.model or [n for n, spec in MODELS.items() if spec[4]]
    unknown = [n for n in names if n not in MODELS]
    if unknown:
        print(f"unknown model(s): {', '.join(unknown)}", file=sys.stderr)
        return 2

    for name in names:
        if (out_dir / f"{name}.inp").exists() and not args.force:
            print(f"  skip (exists)  {name}")
            continue
        cells, features, quads, sidecar, _ = MODELS[name]
        nv, nt, nq = write_deck(out_dir, name, cells, features, quads, sidecar)
        print(f"  wrote  {name:24s} {nv} vertices, {nt} triangles, {nq} quads")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
