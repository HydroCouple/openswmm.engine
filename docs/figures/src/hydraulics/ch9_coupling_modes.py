"""Where a node spill is booked (hydraulics Chapter 9, §9.7.6).

`COUPLING_IN_FLOODING` moves one number between two rows of the 1D routing
continuity table and changes nothing else. With `NO`, the default, water
leaving a node into the mesh is reported as coupling outflow, and the
flooding row holds only water genuinely lost from the model. With `YES`,
the spill is folded into the flooding loss, which is what a reader
comparing against a 1D-only run of the same model expects to see. The
totals, the ledger and the continuity error are identical either way; the
2D block below is untouched. Synthetic; the numbers are illustrative and
sum exactly.
"""
from __future__ import annotations

REQUIRES = ()

ROWS = [
    ("Dry Weather Inflow", 0.000, 0.000),
    ("Wet Weather Inflow", 12.480, 12.480),
    ("Groundwater Inflow", 0.000, 0.000),
    ("RDII Inflow", 0.000, 0.000),
    ("External Inflow", 0.000, 0.000),
    ("External Outflow", 9.120, 9.120),
    ("Flooding Loss", 0.000, 3.210),
    ("2D Coupling Outflow", 3.210, 0.000),
    ("Evaporation Loss", 0.040, 0.040),
    ("Initial Stored Volume", 0.310, 0.310),
    ("Final Stored Volume", 0.418, 0.418),
]
MOVED = {"Flooding Loss", "2D Coupling Outflow"}
ERROR = -0.002


def _table(ax, style, col, title, subtitle):
    from matplotlib.patches import Rectangle
    OR = style.COLORS["fv-lts"]
    ax.text(0.0, len(ROWS) + 1.55, title, fontsize=9, weight="bold", color=style.INK)
    ax.text(0.0, len(ROWS) + 1.05, subtitle, fontsize=7.4, color=style.MUTED)
    ax.text(0.0, len(ROWS) + 0.35, "Flow Routing Continuity", fontsize=8, weight="bold", color=style.INK)
    ax.text(5.4, len(ROWS) + 0.35, "Volume (10⁶ gal)", fontsize=7.6, ha="right", color=style.MUTED)
    for k, (name, a, b) in enumerate(ROWS):
        y = len(ROWS) - 1 - k
        v = a if col == 0 else b
        hot = name in MOVED
        if hot:
            ax.add_patch(Rectangle((-0.12, y - 0.02), 5.6, 0.62, fc=OR, alpha=0.16 if v else 0.06, ec="none"))
        ax.text(0.0, y + 0.28, name, fontsize=7.6, va="center",
                color=OR if hot and v else (style.MUTED if hot else style.INK))
        ax.text(5.35, y + 0.28, f"{v:.3f}", fontsize=7.6, ha="right", va="center",
                color=OR if hot and v else (style.MUTED if hot else style.INK))
    ax.plot([-0.12, 5.4], [-0.35, -0.35], color=style.MUTED, lw=0.9)
    ax.text(0.0, -0.85, "Continuity Error (%)", fontsize=7.8, weight="bold", color=style.INK)
    ax.text(5.35, -0.85, f"{ERROR:.3f}", fontsize=7.8, ha="right", weight="bold", color=style.INK)
    ax.set_xlim(-0.4, 5.8)
    ax.set_ylim(-1.9, len(ROWS) + 2.1)
    ax.axis("off")


def _draw(style):
    import matplotlib.pyplot as plt
    fig, (a0, a1) = plt.subplots(1, 2, figsize=(9.0, 4.6))
    fig.patch.set_facecolor(style.SURFACE)
    _table(a0, style, 0, "COUPLING_IN_FLOODING NO   (default)",
           "the spill is its own row; Flooding Loss holds only water lost from the model")
    _table(a1, style, 1, "COUPLING_IN_FLOODING YES",
           "the spill is folded into Flooding Loss, as a 1D-only run reports it")
    fig.text(0.5, 0.055, "One number moves between two rows. The totals, the ledger and the continuity error are "
             "identical, and the 2D block of §9.9 is unchanged.", fontsize=7.6, color=style.MUTED, ha="center")
    fig.text(0.5, 0.018, "The key changes reporting only: it never changes what the solvers exchange.",
             fontsize=7.6, color=style.INK, ha="center")
    fig.tight_layout(rect=[0, 0.085, 1, 1], w_pad=2.5)
    return fig


def build(sink):
    import style
    sink.save(_draw(style), "hydraulics_ch9_coupling_modes")
