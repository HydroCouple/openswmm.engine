"""A coupled one- and two-dimensional model (Application Manual, the
coupled chapter).

Two figures from the engine's complete 2D example: the mesh itself, with
every boundary condition and both kinds of coupling point marked, drawn
from the results file and the deck; and the depth history of one cell
under the three momentum closures, which is the choice the chapter is
about.

Simulated tier: four runs of docs/figures/decks/coupled_1d2d, cached
under docs/figures/cache. The results file is HDF5, so this generator
needs h5py as well as the openswmm package.
"""
from __future__ import annotations

import re

import numpy as np

REQUIRES = ("openswmm", "h5py")

DECK = "coupled_1d2d/2d_complete_example.inp"
CLOSURES = [("LOCAL_INERTIAL", "local inertial (the default)", "fv"),
            ("FULL_SWE", "full shallow water", "dw"),
            ("DIFFUSIVE_WAVE", "diffusive wave", "fv-lts")]
WATCH = 4          # the cell whose depth history the second figure follows
BC_COLOUR = {"NORMAL_FLOW": "#1baf7a", "TS_STAGE": "#2a78d6", "RATING_CURVE": "#6f42c1",
             "SPECIFIED_FLOW": "#eb6834", "SPECIFIED_STAGE": "#eda100"}


def _section(text, name):
    m = re.search(rf"^\[{name}\]\s*$(.*?)(?=^\[|\Z)", text, re.M | re.S)
    if not m:
        return []
    out = []
    for ln in m.group(1).splitlines():
        s = ln.split(";")[0].strip()
        if s:
            out.append(s.split())
    return out


def _mesh(h5):
    import h5py  # noqa: F401
    x = np.asarray(h5["Mesh2_node_x"])
    y = np.asarray(h5["Mesh2_node_y"])
    tri = np.asarray(h5["Mesh2_face_nodes"])
    z = np.asarray(h5["Mesh2_face_z"])
    depth = np.asarray(h5["Mesh2_face_max_depth"])          # the engine's own per-cell maximum
    cx = np.asarray(h5["Mesh2_face_x"])
    cy = np.asarray(h5["Mesh2_face_y"])
    return x, y, tri, z, depth, cx, cy


def _draw_mesh(style, run, deck_text):
    import h5py
    import matplotlib.pyplot as plt
    from matplotlib.patches import Polygon
    with h5py.File(run.h5, "r") as h5:
        x, y, tri, z, depth, cx, cy = _mesh(h5)
    fig, (a0, a1) = plt.subplots(1, 2, figsize=(9.4, 4.3))
    fig.patch.set_facecolor(style.SURFACE)
    for ax, vals, title, cmap, unit in (
            (a0, z, "(a) The mesh: bed elevation, boundaries and coupling points", "terrain", "m"),
            (a1, depth, "(b) The maximum depth each cell reached", "Blues", "m")):
        cm = plt.get_cmap(cmap)
        lo, hi = float(np.min(vals)), float(np.max(vals))
        rng = (hi - lo) or 1.0
        for t, v in zip(tri, vals):
            ax.add_patch(Polygon(np.column_stack([x[t], y[t]]), closed=True,
                                 fc=cm(0.25 + 0.6 * (v - lo) / rng), ec=style.MUTED, lw=0.8))
        for i, (px, py) in enumerate(zip(cx, cy)):
            ax.text(px, py, f"{i}", fontsize=6.6, ha="center", va="center", color=style.INK, alpha=0.6)
        mx, my = 0.10 * (x.max() - x.min()), 0.10 * (y.max() - y.min())
        ax.set_xlim(x.min() - mx, x.max() + mx)
        ax.set_ylim(y.min() - 3.2 * my, y.max() + my)
        ax.set_aspect("equal", adjustable="box")
        ax.axis("off")
        ax.set_title(title, fontsize=9, loc="left")
        ax.text(x.min(), y.min() - 0.8 * my, f"{lo:.2f} to {hi:.2f} {unit}", fontsize=7.2, color=style.MUTED)
    # boundary conditions, on panel (a)
    seen = set()
    for row in _section(deck_text, "2D_BOUNDARY_CONDITIONS"):
        t, e, kind = int(row[0]), int(row[1]), row[2]
        v = tri[t]
        a, b = v[(e + 1) % 3], v[(e + 2) % 3]
        a0.plot([x[a], x[b]], [y[a], y[b]], color=BC_COLOUR.get(kind, style.INK), lw=3.2,
                solid_capstyle="round", zorder=4,
                label=kind.replace("_", " ").lower() if kind not in seen else None)
        seen.add(kind)
    # coupling points
    for row in _section(deck_text, "2D_VERTEX_NODE_MAP"):
        v = int(row[0])
        a0.plot(x[v], y[v], marker="o", ms=10, mfc="none", mec="#e8484c", mew=2.0, zorder=6,
                label="vertex coupled to a node")
        a0.text(x[v], y[v] - 2.2, f"vertex {v} → {row[1]}", fontsize=7.0, ha="center", color="#e8484c", zorder=6)
    for row in _section(deck_text, "2D_TRIANGLE_NODE_MAP"):
        a0.plot([], [], marker="s", ms=8, mfc="none", mec="#e8484c", mew=2.0, lw=0,
                label=f"cells tagged {row[0]} coupled to {row[1]}")
    a0.legend(fontsize=6.6, frameon=False, loc="lower left", ncol=2, handlelength=1.4,
              columnspacing=1.0)
    a1.text(x.min(), y.min() - 2.4, "cell numbers are the triangle indices the deck uses",
            fontsize=7.0, color=style.MUTED)
    fig.tight_layout()
    return fig


def _draw_closures(style, series):
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(8.6, 4.0))
    fig.patch.set_facecolor(style.SURFACE)
    style.style_ax(ax)
    for (name, label, colour), (t, d, secs) in zip(CLOSURES, series):
        ax.plot(t, d, color=style.COLORS[colour], lw=1.8, label=f"MOMENTUM_EQUATION {name} — {label}")
    ax.set_xlabel("time  (hours)")
    ax.set_ylabel(f"depth in cell {WATCH}  (m)")
    ax.legend(fontsize=7.4, frameon=False, loc="center right")
    ax.set_title("The same coupled model under the three momentum closures", fontsize=9.5, loc="left")
    spread = max(float(np.max(np.abs(series[0][1] - s[1]))) for s in series[1:])
    ax.text(0.03, 0.06, "The three closures share every other part of the marcher: the same mesh, the same time\n"
            f"stepping, the same coupling. On this cell they disagree by up to {spread:.2f} m and hold that\n"
            "disagreement — the closure is not a cosmetic choice. Eight cells under strong boundary\n"
            "forcing is a feature demonstration, not a validation: see the analytic cases of Hydraulics 9.",
            transform=ax.transAxes, fontsize=7.4, color=style.MUTED, va="bottom", linespacing=1.45)
    fig.tight_layout()
    return fig


def build(sink):
    import h5py
    import style
    deck_text = (sink.fig_dir / "decks" / DECK).read_text()
    base = sink.run(DECK, None, "base")
    # MOMENTUM_EQUATION lives in [2D_OPTIONS], so the variant is made by adding
    # the line to the committed deck rather than by upserting into [OPTIONS]
    series = []
    for name, _, _ in CLOSURES:
        text = re.sub(r"^(INTEGRATOR\s+EXPLICIT.*)$", r"\1\nMOMENTUM_EQUATION       " + name,
                      deck_text, count=1, flags=re.M)
        assert "MOMENTUM_EQUATION" in text, "the deck's [2D_OPTIONS] INTEGRATOR line was not found"
        run = sink.run(DECK, None, f"closure_{name.lower()}", text=text)
        with h5py.File(run.h5, "r") as h5:
            d = np.asarray(h5["Mesh2_face_depth"])[:, WATCH]
            tt = np.asarray(h5["time"])
            t_h = (tt - tt[0]) * 24.0
        series.append((t_h, d, run.seconds))
        print(f"    {name}: {run.seconds:.2f} s, peak depth {d.max():.3f} m in cell {WATCH}")
    if "workflow_ch6_mesh_and_coupling" in sink.expected:
        sink.save(_draw_mesh(style, base, deck_text), "workflow_ch6_mesh_and_coupling")
    if "workflow_ch6_momentum_closures_depth" in sink.expected:
        sink.save(_draw_closures(style, series), "workflow_ch6_momentum_closures_depth")
