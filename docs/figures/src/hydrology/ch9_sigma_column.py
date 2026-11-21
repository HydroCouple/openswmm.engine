"""The capillary and kinematic sigma columns on the same cell (hydrology
Chapter 9, §9.4.4).

Two runs of docs/figures/decks/mesh_hydrology/mesh_hydrology.inp with
every cell on the sigma closure (CLOSURE SIGMA, eight layers), differing
only in CAPILLARY_DIFF YES (the capillary column) and NO (the kinematic
column). Both derive in code from the committed deck:

* [2D_AQUIFER_OPTIONS] CLOSURE AUTO -> SIGMA, plus CAPILLARY_DIFF;
* the aquifer row's HG0 1.0 -> 3.0, so the table starts 1 m below the
  ground of the 4 m column and the hour of rain can reach it;
* END_TIME 03:00 -> 06:00, so the recession shows where each column goes;
* REPORT_2D_VARIABLES DEFAULT GW_DETAILED, which writes
  Mesh2_face_gw_theta_sigma [time, layer, face], layer 0 at the ground
  (src/engine/2d/output/Default2DOutputPlugin.cpp:1936);
* [2D_OPTIONS] INFIL_DESTINATION AQUIFER_2D dropped: the engine now
  refuses that key as obsolete (aquifer-owned cells compute their own
  receiving capacity).

The figure plots, for the cell that took the most infiltration, theta in
each layer against depth below the ground at three report times, with the
water table at each, and the table's rise in both runs. Layer j's center
is at sigma = (j + 1/2)/m of the unsaturated thickness L = z_s - h_g.

Simulated tier; needs h5py.
"""
from __future__ import annotations

import re

import numpy as np

REQUIRES = ("h5py",)

DECK = "mesh_hydrology/mesh_hydrology.inp"
HG0 = "3.0"
END_TIME = "06:00:00"
TIMES_H = (0.0, 1.0, 6.0)          # report times plotted (the first record stands for t = 0)


def variant(base: str, cap: str) -> str:
    """The committed deck with the sigma closure, CAPILLARY_DIFF, a shallower table and a longer run."""
    t, n = re.subn(r"^CLOSURE\s+AUTO\s*$", f"CLOSURE              SIGMA\nCAPILLARY_DIFF       {cap}",
                   base, flags=re.M)
    assert n == 1, "deck changed: [2D_AQUIFER_OPTIONS] CLOSURE AUTO not found"
    t = re.sub(r"^INFIL_DESTINATION\s+AQUIFER_2D\s*\n", "", t, flags=re.M)
    t, n = re.subn(r"^(OUTPUT_FILE\s+mesh_hydrology\.h5)\s*$",
                   r"\1\nREPORT_2D_VARIABLES  DEFAULT GW_DETAILED", t, flags=re.M)
    assert n == 1, "deck changed: OUTPUT_FILE line not found"
    t, n = re.subn(r"\bHG0 1\.0\b", f"HG0 {HG0}", t)
    assert n == 1, "deck changed: HG0 1.0 not found"
    t, n = re.subn(r"^END_TIME\s+\S+", f"END_TIME             {END_TIME}", t, flags=re.M)
    assert n == 1
    return t


def _tags(deck_text):
    m = re.search(r"^\[2D_TRIANGLES\]\s*$(.*?)(?=^\[|\Z)", deck_text, re.M | re.S)
    return [s[5] for s in (ln.split(";")[0].split() for ln in m.group(1).splitlines()) if len(s) >= 6]


def _aquifer(deck_text):
    """KS ZS THETA_S THETA_R ALPHA of the deck's mesh-wide [2D_AQUIFER] row."""
    sec = re.search(r"^\[2D_AQUIFER\]\s*$(.*?)(?=^\[|\Z)", deck_text, re.M | re.S).group(1)
    row = next(ln.split() for ln in sec.splitlines() if ln.strip().startswith("*"))
    ks, zs, theta_s, theta_r, alpha = (float(v) for v in row[1:6])
    return zs, theta_s, theta_r


def _read(h5):
    t = np.asarray(h5["time"])
    hours = (t - np.floor(t[0])) * 24.0
    return dict(hours=hours,
                theta=np.asarray(h5["Mesh2_face_gw_theta_sigma"]),
                hg=np.asarray(h5["Mesh2_face_gw_hg"]),
                rech=np.asarray(h5["Mesh2_face_gw_recharge"]),
                infil=np.asarray(h5["Mesh2_face_infil_cum"]),
                rain=np.asarray(h5["Mesh2_face_rainfall"]))


def _draw(style, runs, cell, tag, zs, theta_s, theta_r):
    import matplotlib.pyplot as plt
    INK, MUTED = style.INK, style.MUTED
    fig = style.figure(9.6, 4.4)
    gs = fig.add_gridspec(1, 3, width_ratios=[1, 1, 1.15], wspace=0.34)
    axes = [fig.add_subplot(gs[0, i]) for i in range(3)]
    cols = {"YES": style.COLORS["fv"], "NO": style.COLORS["fv-lts"]}
    names = {"YES": "capillary column, CAPILLARY_DIFF YES", "NO": "kinematic column, CAPILLARY_DIFF NO"}
    shades = (0.35, 0.65, 1.0)
    for ax, (cap, lab) in zip(axes[:2], (("YES", "(a)"), ("NO", "(b)"))):
        style.style_ax(ax)
        r = runs[cap]
        m = r["theta"].shape[1]
        for frac, th in zip(shades, TIMES_H):
            k = int(np.argmin(np.abs(r["hours"] - th))) if th > 0 else 0
            L = zs - r["hg"][k, cell]
            z = (np.arange(m) + 0.5) / m * L
            col = cols[cap]
            ax.plot(r["theta"][k, :, cell], z, marker="o", ms=3.4, lw=1.5, color=col, alpha=frac,
                    label=f"{r['hours'][k]:.1f} h" if th > 0 else "start")
            ax.axhline(L, color=col, alpha=frac, lw=1.0, ls=(0, (4, 2)))
        ax.axvline(theta_s, color=MUTED, lw=0.8, ls=":")
        ax.axvline(theta_r, color=MUTED, lw=0.8, ls=":")
        ax.text(theta_s - 0.004, 0.72, "θ_s", fontsize=7.4, color=MUTED, ha="right", va="center", rotation=90)
        ax.text(theta_r + 0.004, 0.72, "θ_r", fontsize=7.4, color=MUTED, ha="left", va="center", rotation=90)
        ax.set_ylim(1.02, 0.0)
        ax.set_xlim(0.08, 0.47)
        ax.set_xlabel("θ in each layer")
        ax.set_ylabel("depth below the ground, m")
        ax.legend(loc="center right", bbox_to_anchor=(0.97, 0.56), fontsize=7.4, title="dashed: water table",
                  title_fontsize=7.2)
        ax.set_title(f"{lab} {names[cap].split(',')[0].capitalize()}", fontsize=9, loc="left")
    ax = axes[2]
    style.style_ax(ax)
    for cap in ("YES", "NO"):
        r = runs[cap]
        rise = (r["hg"][:, cell] - r["hg"][0, cell]) * 1000.0
        ax.plot(r["hours"], rise, color=cols[cap], lw=1.6,
                label={"YES": "capillary (YES)", "NO": "kinematic (NO)"}[cap])
    rain = runs["YES"]["rain"][:, cell] * 1000.0 * 3600.0
    wet = runs["YES"]["hours"][rain > 0]
    if wet.size:
        ax.axvspan(0.0, float(wet.max()), color=style.WATER, alpha=0.08, lw=0)
        ax.text(0.08, 0.97, f"rain, {rain.max():.1f} mm/hr", transform=ax.get_xaxis_transform(),
                fontsize=7.4, color=style.WATER, va="top")
    ax.set_xlim(0, float(runs["YES"]["hours"][-1]) * 1.01)
    ax.set_xlabel("hours")
    ax.set_ylabel("rise of the water table, mm")
    ax.legend(loc="center right", bbox_to_anchor=(1.0, 0.42), fontsize=7.4, title="CAPILLARY_DIFF",
              title_fontsize=7.2)
    ax.set_title("(c) The table under each column", fontsize=9, loc="left")
    fig.text(0.0, -0.02, f"Cell {cell} ({tag}); every cell on CLOSURE SIGMA, eight layers, Gardner soil, "
             f"θ_s = {theta_s:g}, θ_r = {theta_r:g}, z_s = {zs:g} m, HG0 = {HG0} m.",
             fontsize=7.4, color=MUTED)
    return fig


def build(sink):
    import h5py
    import style
    base = (sink.fig_dir / "decks" / DECK).read_text()
    zs, theta_s, theta_r = _aquifer(base)
    runs = {}
    for cap in ("YES", "NO"):
        run = sink.run(DECK, text=variant(base, cap), key=f"gw_sigma_cap_{cap.lower()}")
        with h5py.File(run.h5, "r") as h5:
            runs[cap] = _read(h5)
        cont = re.findall(r"2D Aquifer Continuity.*?Continuity Error \(%\)\s*\.+\s*([-+0-9.]+)",
                          run.rpt.read_text(errors="replace"), re.S)
        runs[cap]["continuity"] = cont[0] if cont else "?"
    cell = int(np.argmax(runs["YES"]["infil"][-1]))
    tag = _tags(base)[cell]
    for cap in ("YES", "NO"):
        r = runs[cap]
        k1 = int(np.argmin(np.abs(r["hours"] - 1.0)))
        print(f"    CAPILLARY_DIFF {cap}: cell {cell} ({tag}) infiltrated {r['infil'][-1, cell] * 1000:.2f} mm; "
              f"table rise {(r['hg'][-1, cell] - r['hg'][0, cell]) * 1000:.1f} mm; "
              f"recharge first {r['rech'][0, cell]:+.2e} m/s, min {r['rech'][:, cell].min():+.2e}; "
              f"bottom layer theta {r['theta'][0, -1, cell]:.3f} -> {r['theta'][k1, -1, cell]:.3f} (1 h) -> "
              f"{r['theta'][-1, -1, cell]:.3f}; top {r['theta'][0, 0, cell]:.3f} -> {r['theta'][k1, 0, cell]:.3f} "
              f"-> {r['theta'][-1, 0, cell]:.3f}; aquifer continuity {r['continuity']} %")
    sink.save(_draw(style, runs, cell, tag, zs, theta_s, theta_r), "hydrology_ch9_sigma_column")
