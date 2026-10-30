"""Areal depletion curves (hydrology Figure 6-4 and the Engine Manual's
Figure 1-6).

The ADC maps the dimensionless snow depth AWESI = WSNOW / SI to the areal
snow cover fraction ASC. The natural-area curve is Anderson's (1973)
typical shape; the temporary curve is the straight line the engine uses
for new snow falling on a partly bare pack, from the origin to the point
of the natural curve where the new-snow event began. Plotted from the
tabulated points; replaces a 371 × 274 px bitmap.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()

# Anderson (1973) typical natural-area ADC, as tabulated in the chapter's example
AWESI = np.array([0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0])
ASC = np.array([0.0, 0.35, 0.53, 0.66, 0.76, 0.84, 0.90, 0.94, 0.97, 0.99, 1.0])


def _draw(style):
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(6.4, 4.0))
    fig.patch.set_facecolor(style.SURFACE)
    style.style_ax(ax)
    x = np.linspace(0, 1, 200)
    ax.plot(x, np.interp(x, AWESI, ASC), color=style.COLORS["fv"], lw=2.2, label="natural-area curve (Anderson, 1973)")
    ax.plot(AWESI, ASC, marker="o", ms=3.5, color=style.COLORS["fv"], lw=0)
    a0 = 0.35
    ax.plot([0, a0], [0, np.interp(a0, AWESI, ASC)], color=style.COLORS["dw-slot"], lw=1.8, ls=(0, (4, 2)),
            label="temporary curve for new snow on a partly bare pack")
    ax.plot([a0], [np.interp(a0, AWESI, ASC)], marker="o", ms=6, mfc="white", mec=style.COLORS["dw-slot"])
    ax.text(a0 + 0.02, np.interp(a0, AWESI, ASC) - 0.07, "pack state when\nthe new snow fell", fontsize=7.4,
            color=style.COLORS["dw-slot"])
    ax.plot([1, 1.15], [1, 1], color=style.COLORS["fv"], lw=2.2)
    ax.text(1.14, 0.955, "ASC = 1 for AWESI ≥ 1", fontsize=7.4, color=style.MUTED, ha="right", va="top")
    ax.set_xlabel("AWESI = WSNOW / SI  (snow depth as a fraction of the depth at 100 % cover)")
    ax.set_ylabel("ASC  (areal snow cover fraction)")
    ax.set_xlim(0, 1.15)
    ax.set_ylim(0, 1.05)
    ax.legend(loc="lower right", fontsize=7.5)
    fig.tight_layout()
    return fig


def build(sink):
    import style
    sink.save(_draw(style), "hydrology_ch6_areal_depletion_curve")
