"""Buildup and washoff on one mesh cell (quality Chapter 10, §10.5-§10.8).

A cell declares which of the model's land uses cover it and by what
percent; the buildup store is held per (cell, land use, species) in user
mass per normalizer unit, where the normalizer is the covered area or, for
a per-curb land use, the covered curb length. Accrual runs on the runoff
step, sweeping removes a fraction on its own schedule, and washoff — driven
by the cell's net outflow expressed as a runoff rate — moves mass out of
the store and into the cell's species row, which then advects across the
mesh and hands mass to the network at a coupling point. Synthetic; no
model run.
"""
from __future__ import annotations

REQUIRES = ()

SLICES = [("Residential", 0.60, "#cfe0c3"), ("Commercial", 0.25, "#e6d9bd"), ("bare", 0.15, "#dcdcd6")]
STORE = {"Residential": 0.78, "Commercial": 0.52, "bare": 0.0}   # relative store height


def _draw(style):
    import matplotlib.pyplot as plt
    from matplotlib.patches import Polygon, Rectangle, Circle
    fig, ax = plt.subplots(figsize=(9.4, 4.7))
    fig.patch.set_facecolor(style.SURFACE)
    INK, WA, OR, DW, FV = style.INK, style.WATER, style.COLORS["fv-lts"], style.COLORS["dw"], style.COLORS["fv"]
    # the cell, in plan, cut into coverage slices
    x0, y0, w, h = 0.4, 1.2, 4.6, 2.6
    x = x0
    for name, frac, fc in SLICES:
        ww = frac * w
        ax.add_patch(Rectangle((x, y0), ww, h, fc=fc, ec=INK, lw=1.0))
        ax.text(x + ww / 2, y0 + h - 0.22, f"{int(frac * 100)} %", fontsize=7.6, ha="center", va="top", color=INK)
        ax.text(x + ww / 2, y0 + h - 0.62, name, fontsize=7.0, ha="center", va="top", color=INK,
                rotation=0 if frac > 0.2 else 90)
        # the buildup store as a bar standing on the slice
        s = STORE[name]
        if s > 0:
            ax.add_patch(Rectangle((x + ww * 0.25, y0 + 0.3), ww * 0.5, s * 1.1, fc=OR, alpha=0.75, ec=OR, lw=1.0))
            ax.text(x + ww / 2, y0 + 0.3 + s * 1.1 + 0.08, f"B = {s:.2f}", fontsize=6.8, ha="center", color=OR)
        else:
            ax.text(x + ww / 2, y0 + 1.15, "no land use\nno store", fontsize=6.6, ha="center", va="center",
                    color=style.MUTED)
        x += ww
    ax.text(x0, y0 + h + 1.52, "one mesh cell — [2D_COVERAGES] replaces the whole set for its scope, "
            "ladder  *  >  TAG  >  CELL", fontsize=7.8, color=INK)
    ax.text(x0, y0 + h + 1.22, "buildup per (cell, land use, species), in user mass per normalizer: "
            "N = f·A (per area) or f·L (per curb)", fontsize=7.2, color=style.MUTED)
    A = lambda col, lw=1.6: dict(arrowstyle="-|>", color=col, lw=lw, mutation_scale=12)  # noqa: E731
    # accrual on the runoff step, and sweeping, both from above the cell
    ax.annotate("", xy=(1.2, y0 + h + 0.05), xytext=(1.2, y0 + h + 0.62), arrowprops=A(OR))
    ax.text(1.32, y0 + h + 0.62, "accrual on the runoff step\n(Chapter 3's functions, unchanged)", fontsize=7.2,
            va="top", color=OR)
    ax.annotate("", xy=(3.6, y0 + h + 0.62), xytext=(3.6, y0 + h + 0.05), arrowprops=A(DW))
    ax.text(3.72, y0 + h + 0.62, "sweeping removes a fraction\non its own schedule", fontsize=7.2, va="top", color=DW)
    # washoff out of the store into the species row
    ax.annotate("", xy=(6.0, y0 + 0.9), xytext=(5.05, y0 + 0.9), arrowprops=A(WA, 2.0))
    ax.text(5.5, y0 + 1.08, "washoff", fontsize=7.8, ha="center", color=WA)
    ax.text(5.5, y0 + 0.72, "driven by the cell's net\noutflow as a runoff rate", fontsize=6.9, ha="center",
            va="top", color=WA)
    # the species row
    rows = ["Pollutant TSS", "Pollutant Lead", "MSX species"]
    for k, r in enumerate(rows):
        yy = y0 + 1.55 - k * 0.5
        ax.add_patch(Rectangle((6.05, yy), 2.3, 0.42, fc=WA, alpha=0.18 + 0.12 * k, ec=WA, lw=1.0))
        ax.text(7.2, yy + 0.21, r, fontsize=7.0, ha="center", va="center", color=INK)
    ax.text(7.2, y0 + 2.25, "the cell's species mass rows  m_{s,i}", fontsize=7.6, ha="center", color=INK)
    ax.text(7.2, y0 + 0.28, "species-major; concentration is derived", fontsize=6.9, ha="center", color=style.MUTED)
    # advection across the mesh, then to a node
    ax.annotate("", xy=(9.5, y0 + 1.0), xytext=(8.45, y0 + 1.0), arrowprops=A(FV, 2.0))
    ax.text(8.98, y0 + 1.15, "advection across\nthe mesh (§10.2)", fontsize=7.0, ha="center", color=FV)
    ax.add_patch(Circle((9.95, y0 + 1.0), 0.28, fc="#d9e6f5", ec=INK, lw=1.1))
    ax.text(9.95, y0 + 1.42, "coupled node", fontsize=7.0, ha="center", va="bottom", color=INK)
    ax.annotate("", xy=(9.95, y0 + 0.18), xytext=(9.95, y0 + 0.68), arrowprops=A(DW, 1.4))
    ax.text(9.95, y0 + 0.08, "into the 1D\ntransport", fontsize=6.8, ha="center", va="top", color=DW)
    ax.text(0.4, 0.55, "Per-curb land uses need a curb length on every cell they cover ([2D_CURB_LENGTH]); a covered cell "
            "without one is refused at open, not given zero.", fontsize=7.2, color=style.MUTED)
    ax.text(0.4, 0.2, "A cell whose centroid lies in a subcatchment that carries its own coverages is reported in one "
            "ownership warning: the same storm would load twice.", fontsize=7.2, color=style.MUTED)
    ax.set_xlim(0.1, 10.6)
    ax.set_ylim(0.0, y0 + h + 1.95)
    ax.axis("off")
    fig.tight_layout(pad=0.2)
    return fig


def build(sink):
    import style
    sink.save(_draw(style), "quality_ch10_cell_buildup_washoff")
