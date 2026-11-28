"""A layered LID on a storage node: the runtime cells, their two water
inventories, ports at three elevations, and the optional Richards column.

Synthetic tier. One figure, cited by the hydraulics reference (Section
2.1.4), the Engine Manual (Chapter 6) and the quality reference (Section
6.7). It draws the Engine Manual's example stack
(docs/manuals/engine/sections/Chapter6-LidStorageNodes.md, "Stack":
SURFACE 6 in with vegetation 0.1; MEDIA 12 in, porosity 0.45, field
capacity 0.20, wilting point 0.08, Ks 0.01 in/hr; AGGREGATE 6 in,
porosity 0.40, Ks 100 in/hr) and the same chapter's illustrative
[LID_RICHARDS] materials (theta_r 0.03, alpha 2.0 1/m, n 1.6, l 0.5,
Ss 1e-4 1/m, 8 cells per porous layer). The engine has no default
retention law; every porous layer needs an authored one.

What is taken from the engine source:

* cells: SURFACE is one cell, each MEDIA layer five cells, AGGREGATE one
  cell (Richards: cells_per_layer for MEDIA and AGGREGATE); surface void
  fraction is 1 - vegetation (src/engine/hydrology/LidNode.cpp:546-566);
* a port belongs to the cell whose [bottom, top) holds its offset, so an
  interface belongs to the cell above (LidNode.cpp:1012-1025);
* a SURFACE port sees the ponding head bottom + theta/porosity * dz when
  that exceeds the mobile table (LidNode.cpp:1037-1050);
* reverse inflow above the mobile table into a wholly exposed cell first
  fills that cell's retained capacity (porosity - theta) * G; a cell whose
  bottom is below the table passes the whole receipt to mobile storage
  (LidNode.cpp:1094-1114);
* van Genuchten theta(psi) and Mualem K(psi) (src/engine/hydrology/
  RichardsColumn.cpp:37-61);
* face conductances: pond contact (Ks + K1) over dz1, the distance-
  weighted mean within one material, half-cell resistances in series
  across unlike materials (RichardsColumn.cpp:236-245), and free
  drainage min(K_N, K_bottom) at the base (RichardsColumn.cpp:262-263).

The state in panel (a) is an illustrative instant chosen to show each
rule, not a run result: the mobile table at 0.60 ft, 0.06 ft of ponding,
media moisture drying upward from the wetted top.
"""
from __future__ import annotations

import importlib.util
from pathlib import Path

import numpy as np

REQUIRES = ()

FIG_ID = "hydraulics_ch2_lid_storage_node"

# Example stack (inches) from Engine Manual Chapter 6, converted to feet.
SURF_DZ, MEDIA_DZ, AGG_DZ = 6 / 12, 12 / 12, 6 / 12
VEG = 0.10
MEDIA = dict(phi=0.45, fc=0.20, wp=0.08, ks=0.01)       # Ks in/hr
AGG = dict(phi=0.40, fc=0.00, ks=100.0)
RET = dict(theta_r=0.03, alpha=2.0, n=1.6, l=0.5, ss=1e-4)   # alpha, Ss in 1/m
CELLS_PER_LAYER = 8

# Illustrative state for panel (a)
H_TABLE = 0.60                                    # mobile water-table depth, ft
H_POND = 0.06                                     # ponded depth on SURFACE, ft
MEDIA_THETA = [0.34, 0.30, 0.27, 0.25, 0.22]      # retained theta, top to bottom
VALVE_Y, OUTLET_Y, WEIR_Y = 0.0, 1.0, 1.5         # port offsets, ft above invert


def _stacks_module():
    path = Path(__file__).resolve().parent.parent / "quality" / "ch6_lid_stacks.py"
    spec = importlib.util.spec_from_file_location("manual_fig_ch6_lid_stacks_helpers", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# ── engine laws (RichardsColumn.cpp:37-61) ─────────────────────────────────

def vg_theta(psi_m, theta_s, p):
    psi_m = np.asarray(psi_m, float)
    m = 1 - 1 / p["n"]
    se = (1 + (p["alpha"] * np.maximum(-psi_m, 0.0)) ** p["n"]) ** (-m)
    return np.where(psi_m >= 0, theta_s, p["theta_r"] + (theta_s - p["theta_r"]) * se)


def vg_k(psi_m, ks, p):
    psi_m = np.asarray(psi_m, float)
    m = 1 - 1 / p["n"]
    se = (1 + (p["alpha"] * np.maximum(-psi_m, 0.0)) ** p["n"]) ** (-m)
    term = -np.expm1(m * np.log1p(-se ** (1 / m)))
    return np.where(psi_m >= 0, ks, ks * se ** p["l"] * term * term)


def series_face_k(k_i, dz_i, k_j, dz_j):
    """Unlike materials: conductance A / (dz_i/(2K_i) + dz_j/(2K_j)) over d_f = (dz_i + dz_j)/2,
    expressed as the equivalent face conductivity."""
    return (dz_i + dz_j) / (dz_i / k_i + dz_j / k_j)


# ── panel (a): the cross-section ───────────────────────────────────────────

def _cells():
    """(kind, layer, bottom, top) top to bottom, as LidNode.cpp:546-584 builds them."""
    out, top = [], SURF_DZ + MEDIA_DZ + AGG_DZ
    for kind, layer, dz, count in (("SURFACE", 1, SURF_DZ, 1), ("MEDIA", 2, MEDIA_DZ, 5), ("AGGREGATE", 3, AGG_DZ, 1)):
        for _ in range(count):
            out.append((kind, layer, top - dz / count, top))
            top -= dz / count
    return out


def _port_cell(cells, offset):
    for i, (_, _, bottom, top) in enumerate(cells):
        if bottom <= offset < top:
            return i
    return 0


def _panel_section(ax, style, stacks):
    from matplotlib.patches import Rectangle
    ls = stacks.LAYER_STYLE
    x0, x1 = 0.0, 2.4
    cells = _cells()
    fill = {"SURFACE": ls["surface"], "MEDIA": ls["soil"], "AGGREGATE": ls["storage"]}
    for kind, layer, b, t in cells:
        _, fc, ec, _ = fill[kind]
        ax.add_patch(Rectangle((x0, b), x1 - x0, t - b, fc=fc, ec="none", zorder=1))
    stacks._hatch(ax, "soil", x0, x1, 0.5, 1.5, ls["soil"][2])
    stacks._hatch(ax, "gravel", x0, x1, 0.0, 0.5, ls["storage"][2])
    # native soil beneath the BOTTOM boundary
    ax.add_patch(Rectangle((x0, -0.32), x1 - x0 + 0.25, 0.32, fc=ls["native"][1], ec="none", zorder=0))
    stacks._hatch(ax, "soil", x0, x1 + 0.25, -0.32, 0.0, ls["native"][2])
    # grass rising from the media top into the ponding zone
    for x in np.linspace(x0 + 0.12, x1 - 0.12, 11):
        ax.plot([x, x - 0.03, x, x + 0.03], [1.5, 1.66, 1.5, 1.64], color=ls["surface"][2], lw=0.9, zorder=4)
    # mobile water below h, and ponding on the surface
    ax.add_patch(Rectangle((x0, 0.0), x1 - x0, H_TABLE, fc=style.WATER, alpha=0.30, ec="none", zorder=2))
    ax.plot([x0, x1], [H_TABLE, H_TABLE], color=style.WATER, lw=1.4, zorder=3)
    ax.plot([1.05], [H_TABLE + 0.045], marker="v", ms=7, color=style.WATER, zorder=5)
    ax.add_patch(Rectangle((x0, 1.5), x1 - x0, H_POND, fc=style.WATER, alpha=0.55, ec="none", zorder=3))
    # cell and layer boundaries
    for _, _, b, t in cells:
        ax.plot([x0, x1], [b, b], color=style.MUTED, lw=0.6, ls=(0, (3, 2)), zorder=4)
    for y in (0.0, 0.5, 1.5):
        ax.plot([x0, x1], [y, y], color=style.INK, lw=1.0, zorder=4)
    ax.add_patch(Rectangle((x0, 0.0), x1 - x0, 2.0, fc="none", ec=style.INK, lw=1.3, zorder=6))
    # layer labels inside the column
    lab = dict(fontsize=7.6, ha="center", va="center", color=style.INK, zorder=7,
               bbox=dict(boxstyle="round,pad=0.18", fc="white", ec="none", alpha=0.85))
    ax.text(1.2, 1.84, "1  SURFACE  6 in  (void fraction 0.90)", **lab)
    ax.text(1.2, 1.20, "2  MEDIA  12 in  =  5 numerical cells", **lab)
    ax.text(1.05, 0.25, "3  AGGREGATE  6 in  (1 cell)", **lab)
    ax.text(x1 - 0.06, H_TABLE + 0.07, "mobile table h = 0.60 ft", fontsize=7.2, ha="right", va="bottom",
            color=style.WATER, zorder=7, fontweight="bold",
            bbox=dict(boxstyle="round,pad=0.12", fc="white", ec="none", alpha=0.85))
    ax.text(1.2, -0.17, "BOTTOM: seepage boundary into native soil (a loss, not a layer)", fontsize=7.2,
            ha="center", va="center", color=style.INK, zorder=7,
            bbox=dict(boxstyle="round,pad=0.15", fc="white", ec="none", alpha=0.8))
    # storage geometry: elevations
    for y in (0.0, 0.5, 1.5, 2.0):          # 1.0 ft is labeled by the outlet
        ax.text(x0 - 0.06, y, f"{y:.1f}", fontsize=7, ha="right", va="center", color=style.MUTED)
    ax.text(x0 - 0.06, 2.17, "ft above\ninvert", fontsize=6.8, ha="right", va="bottom", color=style.MUTED,
            linespacing=1.1)

    pipe = dict(fc="#efede6", ec=style.INK, lw=0.9, zorder=5)
    rev = "#eb6834"
    # overflow weir on the right wall: crest at the bottom of SURFACE
    ax.add_patch(Rectangle((x1, WEIR_Y), 0.55, 0.12, **pipe))
    ax.annotate("", xy=(x1 + 0.80, WEIR_Y + 0.06), xytext=(x1 + 0.08, WEIR_Y + 0.06),
                arrowprops=dict(arrowstyle="-|>", color=style.WATER, lw=1.2), zorder=6)
    ax.text(x1 + 0.88, WEIR_Y + 0.14, "Overflow weir  W 1 BOTTOM\ncrest 1.50 ft; its head is the ponding\n"
            "level 1.56 ft, not the mobile table", fontsize=7.2, va="center", color=style.INK, linespacing=1.3)
    # basal valve (orifice) on the right wall at the invert
    ax.add_patch(Rectangle((x1, VALVE_Y), 0.55, 0.12, **pipe))
    ax.plot([x1 + 0.30, x1 + 0.30], [VALVE_Y - 0.02, VALVE_Y + 0.14], color=style.INK, lw=1.6, zorder=6)
    ax.annotate("", xy=(x1 + 0.80, VALVE_Y + 0.06), xytext=(x1 + 0.60, VALVE_Y + 0.06),
                arrowprops=dict(arrowstyle="-|>", color=style.WATER, lw=1.2), zorder=6)
    ax.text(x1 + 0.88, VALVE_Y + 0.27, "Basal valve (orifice)  V 3 BOTTOM\nsetting 0.4 = partial opening",
            fontsize=7.2, va="center", color=style.INK, linespacing=1.3)
    # reverse inflow at the valve: below the table, so mobile
    ax.annotate("", xy=(x1 - 0.30, VALVE_Y + 0.22), xytext=(x1 + 0.02, VALVE_Y + 0.06),
                arrowprops=dict(arrowstyle="-|>", color=rev, lw=1.3, connectionstyle="arc3,rad=0.3"), zorder=7)
    ax.text(x1 + 0.88, VALVE_Y + 0.02 - 0.06, "reverse inflow below h: all to mobile storage",
            fontsize=7.0, va="center", color=rev, fontstyle="italic")
    # treatment outlet on the left wall, explicit mid-MEDIA offset
    ax.add_patch(Rectangle((x0 - 0.55, OUTLET_Y), 0.55, 0.10, **pipe))
    ax.annotate("", xy=(x0 - 0.80, OUTLET_Y + 0.05), xytext=(x0 - 0.58, OUTLET_Y + 0.05),
                arrowprops=dict(arrowstyle="-|>", color=style.WATER, lw=1.2), zorder=6)
    ax.text(x0 - 0.88, OUTLET_Y + 0.16, "Treatment outlet T\nexplicit offset 1.00 ft\n(MEDIA cell 3)",
            fontsize=7.2, ha="right", va="center", color=style.INK, linespacing=1.3)
    # reverse inflow at the outlet: exposed cell, retained first
    ax.annotate("", xy=(x0 + 0.42, OUTLET_Y + 0.02), xytext=(x0 - 0.02, OUTLET_Y + 0.05),
                arrowprops=dict(arrowstyle="-|>", color=rev, lw=1.3), zorder=7)
    ax.annotate("", xy=(x0 + 0.62, H_TABLE + 0.03), xytext=(x0 + 0.48, OUTLET_Y - 0.03),
                arrowprops=dict(arrowstyle="-|>", color=rev, lw=1.0, ls=(0, (2, 1.5))), zorder=7)
    ax.text(x0 - 0.88, 0.62, "reverse inflow into an exposed\ncell fills its retained capacity\n"
            "(φ − θ)G first; the excess\njoins mobile storage", fontsize=7.0, ha="right", va="center",
            color=rev, fontstyle="italic", linespacing=1.3)
    ax.set_xlim(-2.55, 5.35)
    ax.set_ylim(-0.40, 2.38)
    ax.axis("off")
    ax.set_title("(a) Authored layers, runtime cells and ports (existing model)", loc="left", fontsize=9.5,
                 color=style.INK)
    return cells


def _panel_inventory(ax, style, cells):
    from matplotlib.patches import Rectangle
    solids_c, ret_c, mob_c, air_c = "#b9b6ad", "#a8814f", style.WATER, "#ffffff"
    for i, (kind, layer, b, t) in enumerate(cells):
        h = (t - b) * 0.78
        y = 0.5 * (b + t) - h / 2
        s = float(np.clip((H_TABLE - b) / (t - b), 0.0, 1.0))
        if kind == "SURFACE":
            phi, theta, mob = 1 - VEG, H_POND * (1 - VEG) / (t - b), 0.0
            parts = [(1 - phi, solids_c, None), (theta, mob_c, "//"), (phi - theta, air_c, None)]
            text = "ponded θ = 0.11"
        else:
            phi = MEDIA["phi"] if kind == "MEDIA" else AGG["phi"]
            theta = MEDIA_THETA[i - 1] if kind == "MEDIA" else 0.0
            mob = (phi - theta) * s
            parts = [(1 - phi, solids_c, None), (theta, ret_c, None), (mob, mob_c, None),
                     (phi - theta - mob, air_c, None)]
            text = f"θ = {theta:.2f}" + (f",  s = {s:.1f}" if 0 < s < 1 else (",  s = 1" if s == 1 else ""))
        x = 0.0
        for w, c, hatch in parts:
            if w <= 0:
                continue
            ax.add_patch(Rectangle((x, y), w, h, fc=c, ec=style.MUTED, lw=0.5, alpha=0.75 if c == mob_c else 1.0,
                                   hatch=hatch, zorder=2))
            x += w
        ax.text(1.03, 0.5 * (b + t), text, fontsize=6.9, va="center", color=style.INK)
    ax.plot([0, 1], [H_TABLE, H_TABLE], color=style.WATER, lw=1.0, ls=(0, (4, 2)), zorder=4)
    for y in (0.5, 1.5):
        ax.axhline(y, color=style.INK, lw=0.6, alpha=0.5)
    ax.set_xlim(0, 1.62)
    ax.set_ylim(-0.40, 2.38)
    ax.set_xticks([0, 0.5, 1.0])
    ax.set_yticks([])
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color(style.GRIDC)
    ax.spines["bottom"].set_bounds(0, 1)
    ax.tick_params(colors=style.MUTED, labelsize=7.5)
    ax.set_xlabel("fraction of cell volume G", fontsize=7.8, color=style.MUTED, loc="left")
    ax.set_facecolor(style.SURFACE)
    handles = [Rectangle((0, 0), 1, 1, fc=solids_c, ec=style.MUTED, lw=0.5),
               Rectangle((0, 0), 1, 1, fc=ret_c, ec=style.MUTED, lw=0.5),
               Rectangle((0, 0), 1, 1, fc=mob_c, alpha=0.75, ec=style.MUTED, lw=0.5),
               Rectangle((0, 0), 1, 1, fc=air_c, ec=style.MUTED, lw=0.5)]
    ax.legend(handles, ["solids  1 − φ", "retained  θᵢ", "mobile  (φᵢ − θᵢ) sᵢ(h)", "empty pores"],
              loc="upper left", bbox_to_anchor=(0.0, -0.10), ncol=2, fontsize=7.0, handlelength=1.2,
              columnspacing=0.8, frameon=False)
    ax.set_title("Inventories per cell", loc="left", fontsize=9.5, color=style.INK)


# ── panel (b): the Richards column ─────────────────────────────────────────

def _panel_richards_column(ax, style, stacks):
    from matplotlib.patches import Rectangle
    ls = stacks.LAYER_STYLE
    x0, x1 = 0.0, 0.7
    top = 1.5
    for kind, dz, n in (("MEDIA", MEDIA_DZ, CELLS_PER_LAYER), ("AGGREGATE", AGG_DZ, CELLS_PER_LAYER)):
        base = ls["soil"][1] if kind == "MEDIA" else ls["storage"][1]
        for _ in range(n):
            ax.add_patch(Rectangle((x0, top - dz / n), x1 - x0, dz / n, fc=base, ec=style.MUTED, lw=0.4, zorder=1))
            top -= dz / n
    ax.add_patch(Rectangle((x0, 1.5), x1 - x0, 0.14, fc=style.WATER, alpha=0.55, ec="none", zorder=2))
    ax.add_patch(Rectangle((x0, 1.5), x1 - x0, 0.5, fc="none", ec=style.MUTED, lw=0.6, ls=(0, (3, 2)), zorder=3))
    ax.add_patch(Rectangle((x0, 0.0), x1 - x0, 1.5, fc="none", ec=style.INK, lw=1.2, zorder=4))
    ax.plot([x0, x1], [0.5, 0.5], color=style.INK, lw=1.0, zorder=4)
    ax.text(0.35, 1.84, "pond: owned by\nthe storage node", fontsize=6.9, ha="center", va="center",
            color=style.INK, linespacing=1.2)
    for y, name in ((1.0, "MEDIA\n8 cells"), (0.25, "AGGREGATE\n8 cells")):
        ax.text(0.35, y, name, fontsize=7.0, ha="center", va="center", color=style.INK, zorder=6,
                bbox=dict(boxstyle="round,pad=0.15", fc="white", ec="none", alpha=0.85))
    notes = [
        (1.5, "pond contact: mean of\nKs and K₁ over Δz₁/2"),
        (1.5 - 3 * MEDIA_DZ / CELLS_PER_LAYER, "one material: distance-\nweighted mean of K"),
        (0.5, "unlike materials: half-\ncell resistances in series"),
        (0.0, "bottom: sealed, or free\ndrainage min(K_N, K_bot)"),
    ]
    for y, s in notes:
        ax.plot([x1, x1 + 0.18], [y, y], color=style.MUTED, lw=0.8, zorder=5)
        ax.plot([x1], [y], marker="o", ms=3, color=style.INK, zorder=6)
        ax.text(x1 + 0.22, y, s, fontsize=6.9, va="center", color=style.INK, linespacing=1.2)
    ax.set_xlim(-0.06, 2.12)
    ax.set_ylim(-0.12, 2.05)
    ax.axis("off")
    ax.set_title("(b) Richards column: cells and faces", loc="left", fontsize=9.5, color=style.INK)


def _panel_theta(ax, style):
    suction = np.logspace(-3, 2, 400)                       # m
    for name, phi, color in (("MEDIA, φ = 0.45", MEDIA["phi"], "#8a6d45"), ("AGGREGATE, φ = 0.40", AGG["phi"], "#6e6a60")):
        ax.plot(suction, vg_theta(-suction, phi, RET), color=color, lw=1.6, label=name)
    ax.axhline(MEDIA["wp"], color=style.MUTED, lw=0.8, ls=(0, (3, 2)))
    ax.text(1.3e-3, MEDIA["wp"] + 0.006, "MEDIA wilting point 0.08", fontsize=6.9, color=style.MUTED, va="bottom")
    ax.axhline(RET["theta_r"], color=style.MUTED, lw=0.8, ls=(0, (1, 2)))
    ax.text(1.3e-3, RET["theta_r"] + 0.006, "θr = 0.03", fontsize=6.9, color=style.MUTED, va="bottom")
    ax.set_xscale("log")
    ax.set_xlim(1e-3, 1e2)
    ax.set_ylim(0, 0.5)
    style.style_ax(ax)
    ax.set_xlabel("suction −ψ (m)")
    ax.set_ylabel("moisture θ")
    ax.text(1.3e-3, 0.30, "ψ ≥ 0: θ = φ, and storage\nθ = φ + Ss ψ (elastic)", fontsize=6.9, color=style.INK,
            va="center", linespacing=1.25)
    ax.legend(loc="upper right", fontsize=7.0)
    ax.set_title("(c) Retention θ(ψ)", loc="left", fontsize=9.5)


def _panel_k(ax, style):
    suction = np.logspace(-3, 2, 400)
    psi = -suction
    dz_m, dz_a = MEDIA_DZ / CELLS_PER_LAYER, AGG_DZ / CELLS_PER_LAYER
    km = vg_k(psi, MEDIA["ks"], RET)
    ka = vg_k(psi, AGG["ks"], RET)
    kf = series_face_k(km, dz_m, ka, dz_a)
    ax.plot(suction, ka, color="#6e6a60", lw=1.6, label="AGGREGATE, Ks 100 in/hr")
    ax.plot(suction, km, color="#8a6d45", lw=1.6, label="MEDIA, Ks 0.01 in/hr")
    ax.plot(suction, kf, color="#eb6834", lw=1.3, ls=(0, (4, 2)), label="MEDIA–AGGREGATE face, in series")
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlim(1e-3, 1e2)
    ax.set_ylim(1e-9, 1e6)
    style.style_ax(ax)
    ax.set_xlabel("suction −ψ (m)")
    ax.set_ylabel("conductivity K (in/hr)")
    ax.legend(loc="upper right", fontsize=6.9)
    ratio = float(kf[0] / km[0])
    ax.text(1.3e-3, 1e-6, f"face K = {ratio:.2f} × MEDIA K:\nthe low-K side controls", fontsize=6.9,
            color="#eb6834", va="center", linespacing=1.2)
    ax.set_title("(d) Conductivity K(ψ)", loc="left", fontsize=9.5)
    return ratio


def build(sink):
    import matplotlib.pyplot as plt
    import style
    stacks = _stacks_module()
    fig = style.figure(10.0, 8.0)
    gs = fig.add_gridspec(2, 12, height_ratios=[1.18, 1.0], hspace=0.34, wspace=1.4,
                          left=0.02, right=0.985, top=0.95, bottom=0.07)
    ax_a = fig.add_subplot(gs[0, 0:9])
    ax_i = fig.add_subplot(gs[0, 9:12])
    low = gs[1, :].subgridspec(1, 3, width_ratios=[1.0, 1.0, 1.0], wspace=0.36)
    ax_b = fig.add_subplot(low[0, 0])
    ax_c = fig.add_subplot(low[0, 1])
    ax_d = fig.add_subplot(low[0, 2])
    cells = _panel_section(ax_a, style, stacks)
    _panel_inventory(ax_i, style, cells)
    _panel_richards_column(ax_b, style, stacks)
    _panel_theta(ax_c, style)
    ratio = _panel_k(ax_d, style)
    # numbers worth quoting
    outlet = cells[_port_cell(cells, OUTLET_Y)]
    print(f"    cells: {len(cells)} (1 surface + 5 media + 1 aggregate); outlet at {OUTLET_Y} ft is in "
          f"{outlet[0]} cell {_port_cell(cells, OUTLET_Y)} [{outlet[2]:.2f}, {outlet[3]:.2f}) ft")
    print(f"    weir head = max(h, 1.5 + theta_s/phi_s*dz) = {max(H_TABLE, 1.5 + H_POND):.2f} ft")
    print(f"    Richards: MEDIA-AGGREGATE face K / MEDIA K = {ratio:.3f}")
    plt.figure(fig.number)
    sink.save(fig, FIG_ID)
