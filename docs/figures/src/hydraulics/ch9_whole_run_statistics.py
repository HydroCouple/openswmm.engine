"""Whole-run statistics of the 2D mesh (hydraulics Chapter 9, §9.9 Reporting,
Table 9-4 ENVELOPES).

Three maps of the coupled 1D-2D worked example, each cell colored by a
statistic the engine computed over the whole six-hour run and wrote to the
results file once per face:

(a) Mesh2_face_max_depth, the maximum depth (m);
(b) Mesh2_face_time_max_depth, the time of that maximum (hours from the
    start of the simulation);
(c) Mesh2_face_time_max_velocity, the time of the maximum speed (hours).

Panels (b) and (c) share one color scale, so a cell whose depth and speed
peak at different times shows two different colors: the two envelopes are
sampled independently and their peaks need not coincide (§9.9). No value
is derived here; the generator only reads the datasets and draws them.

Simulated tier: one run of docs/figures/decks/coupled_1d2d/2d_complete_example.inp
as committed (its [2D_OPTIONS] OUTPUT_FILE writes the results file, and
ENVELOPES is in the DEFAULT variable set), cached under docs/figures/cache.
The dataset names and units come from the file itself
(src/engine/2d/output/Default2DOutputPlugin.cpp; Table 9-4). Needs h5py.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ("h5py",)

DECK = "coupled_1d2d/2d_complete_example.inp"
RUN_HOURS = 6.0          # START 00:00 to END 06:00 in the deck's [OPTIONS]


def _hours_label(v):
    if not np.isfinite(v):
        return "NaN"
    return f"{v:.2f} h" if v >= 1.0 else f"{v * 60.0:.1f} min"


def _map(ax, style, x, y, faces, vals, cmap, lo, hi, fmt, title):
    import matplotlib.pyplot as plt
    from matplotlib.patches import Polygon
    cm = plt.get_cmap(cmap)
    for k, (f, v) in enumerate(zip(faces, vals)):
        t = 0.12 + 0.78 * (v - lo) / ((hi - lo) or 1.0) if np.isfinite(v) else None
        fc = cm(t) if t is not None else "#ffffff"
        ax.add_patch(Polygon(np.column_stack([x[f], y[f]]), closed=True, fc=fc,
                             ec=style.MUTED, lw=0.9, zorder=1))
    cx = np.array([x[f].mean() for f in faces])
    cy = np.array([y[f].mean() for f in faces])
    for k, (px, py, v) in enumerate(zip(cx, cy, vals)):
        dark = np.isfinite(v) and (v - lo) / ((hi - lo) or 1.0) > 0.55
        ax.text(px, py + 0.45, fmt(v), fontsize=7.6, ha="center", va="center",
                color="#ffffff" if dark else style.INK, zorder=3)
        ax.text(px, py - 0.95, f"T{k}", fontsize=6.6, ha="center", va="center",
                color="#ffffff" if dark else style.INK, alpha=0.75, zorder=3)
    ax.set_xlim(-1.0, 21.0)
    ax.set_ylim(-1.2, 21.0)
    ax.set_aspect("equal", adjustable="box")
    ax.axis("off")
    ax.set_title(title, fontsize=9, loc="left")
    return cm


def _colorbar(fig, ax, cmap, lo, hi, label):
    import matplotlib as mpl
    import matplotlib.colors as mcolors
    import matplotlib.pyplot as plt
    base = plt.get_cmap(cmap)
    trimmed = mcolors.LinearSegmentedColormap.from_list(
        f"{cmap}_trim", base(np.linspace(0.12, 0.90, 256)))
    sm = mpl.cm.ScalarMappable(norm=mcolors.Normalize(lo, hi), cmap=trimmed)
    cb = fig.colorbar(sm, ax=ax, orientation="horizontal", fraction=0.05, pad=0.03, aspect=28)
    cb.set_label(label, fontsize=8)
    cb.ax.tick_params(labelsize=7.4)
    cb.outline.set_visible(False)


def build(sink):
    import h5py
    import matplotlib.pyplot as plt
    import style
    run = sink.run(DECK, None, "hyd9_base")
    with h5py.File(run.h5, "r") as h5:
        x = np.asarray(h5["Mesh2_node_x"])
        y = np.asarray(h5["Mesh2_node_y"])
        faces = np.asarray(h5["Mesh2_face_nodes"])
        dmax = np.asarray(h5["Mesh2_face_max_depth"], dtype=float)
        t_d = np.asarray(h5["Mesh2_face_time_max_depth"], dtype=float)
        t_v = np.asarray(h5["Mesh2_face_time_max_velocity"], dtype=float)
        vmax = np.asarray(h5["Mesh2_face_max_velocity"], dtype=float)
        units = {k: h5[k].attrs.get("units", b"").decode() for k in
                 ("Mesh2_face_max_depth", "Mesh2_face_time_max_depth", "Mesh2_face_time_max_velocity")}
    assert units == {"Mesh2_face_max_depth": "m", "Mesh2_face_time_max_depth": "h",
                     "Mesh2_face_time_max_velocity": "h"}, units

    fig, axes = plt.subplots(1, 3, figsize=(10.0, 4.3))
    fig.patch.set_facecolor(style.SURFACE)
    _map(axes[0], style, x, y, faces, dmax, "Blues", 0.0, float(np.nanmax(dmax)),
         lambda v: f"{v:.2f} m", "(a) Maximum depth")
    _map(axes[1], style, x, y, faces, t_d, "Purples", 0.0, RUN_HOURS, _hours_label,
         "(b) Time of the maximum depth")
    _map(axes[2], style, x, y, faces, t_v, "Purples", 0.0, RUN_HOURS, _hours_label,
         "(c) Time of the maximum speed")
    _colorbar(fig, axes[0], "Blues", 0.0, float(np.nanmax(dmax)), "Mesh2_face_max_depth (m)")
    _colorbar(fig, axes[1], "Purples", 0.0, RUN_HOURS, "Mesh2_face_time_max_depth (h)")
    _colorbar(fig, axes[2], "Purples", 0.0, RUN_HOURS, "Mesh2_face_time_max_velocity (h)")
    # the two coupling points of the deck: vertex v4 to J1, cell T6 (tag vault) to ST1
    axes[0].plot(10.0, 10.0, marker="o", ms=6, mfc="#ffffff", mec=style.INK, mew=1.1, zorder=5)
    axes[0].text(10.4, 9.0, "J1", fontsize=7.2, color=style.INK, zorder=5)
    differ = int(np.sum(np.abs(t_d - t_v) > 1e-6))
    fig.text(0.01, 0.015, f"Coupled 1D-2D example (8 triangles, 20 m x 20 m, 6 h). In {differ} of 8 cells "
             f"the depth and the speed peak at different times. Times under one hour are printed in minutes.",
             fontsize=7.4, color=style.MUTED)
    fig.tight_layout(rect=(0, 0.04, 1, 1))
    print(f"    max depth {dmax.min():.3f} to {dmax.max():.3f} m; max speed {vmax.min():.2f} to "
          f"{vmax.max():.2f} m/s")
    print(f"    time of max depth (h): {np.round(t_d, 4).tolist()}")
    print(f"    time of max speed (h): {np.round(t_v, 4).tolist()}; cells where the two differ: {differ}")
    sink.save(fig, "hydraulics_ch9_whole_run_statistics")
