"""The transport policy matrix and the gates behind one of its cells
(quality Chapter 10, §10.1).

Left: the four domains against the four species classes, each cell
enabled, disabled by a named key, or unavailable with a reason. This is
the matrix the report header prints and the C API and GUI read; the
example shown is a project with pollutants, a reaction system holding one
wall species, water age on and heat off. Right: the ladder the 2D-surface
row walks for one class, in the order the resolver applies it — the first
gate that fires names itself as the reason. Synthetic; no model run.
"""
from __future__ import annotations

REQUIRES = ()

DOMAINS = ["Runoff / LID", "Groundwater", "1D network", "2D surface"]
CLASSES = ["Pollutants", "MSX", "Age", "Temperature"]

# state: on / off:<key> / n/a:<reason>
MATRIX = [
    ["on(3)", "on(2)", "n/a:age is not\ncarried on\nsubcatchments", "n/a:HEAT_TRANSPORT\nOFF"],
    ["n/a:groundwater\ntransport is\nplanned", "n/a:planned", "n/a:planned", "n/a:planned"],
    ["on(3)", "on(2)", "on(1)", "n/a:HEAT_TRANSPORT\nOFF"],
    ["on(3)", "off:a WALL species\nis declared", "on(1)", "n/a:HEAT_TRANSPORT\nOFF"],
]

LADDER = [
    ("Is there a mesh?", "no mesh → n/a"),
    ("Is the class declared at project level?", "nothing to carry → n/a"),
    ("[OPTIONS] IGNORE_2D", "YES → off, naming the key"),
    ("[OPTIONS] IGNORE_QUALITY", "YES → off; pollutants and MSX only"),
    ("A WALL species in the reaction system?", "yes → MSX n/a, with a warning"),
    ("[2D_OPTIONS] TRANSPORT_<CLASS>", "NO → off, naming the key"),
    ("otherwise", "on"),
]


def _matrix(ax, style):
    from matplotlib.patches import Rectangle
    ON, OFF, NA = "#1baf7a", "#eda100", "#c9c7c1"
    cw, ch = 2.35, 1.35
    for j, c in enumerate(CLASSES):
        ax.text(1.2 + j * cw + cw / 2, len(DOMAINS) * ch + 0.28, c, fontsize=7.6, ha="center", va="bottom",
                color=style.INK, weight="bold", linespacing=1.3)
    for i, d in enumerate(DOMAINS):
        y = (len(DOMAINS) - 1 - i) * ch
        ax.text(1.1, y + ch / 2, d, fontsize=8, ha="right", va="center", color=style.INK, weight="bold")
        for j in range(len(CLASSES)):
            x = 1.2 + j * cw
            cell = MATRIX[i][j]
            if cell.startswith("on"):
                fc, txt, tc = ON, cell, "#ffffff"
            elif cell.startswith("off:"):
                fc, txt, tc = OFF, "off\n" + cell[4:], "#ffffff"
            else:
                fc, txt, tc = NA, "n/a\n" + cell.split(":", 1)[1], style.INK
            ax.add_patch(Rectangle((x, y), cw - 0.1, ch - 0.1, fc=fc, ec=style.SURFACE, lw=1.6, alpha=0.92))
            ax.text(x + (cw - 0.1) / 2, y + (ch - 0.1) / 2, txt, fontsize=6.4, ha="center", va="center",
                    color=tc, linespacing=1.25)
    ax.text(1.2, -0.55, "on(n) carries n rows · off names the key that disabled it · n/a says what is missing",
            fontsize=7.2, color=style.MUTED)
    ax.text(1.2, -0.95, "canonical row order: pollutants, MSX in declaration order, __WATER_AGE__, "
            "__TEMPERATURE__ last", fontsize=7.2, color=style.MUTED)
    ax.set_xlim(-2.6, 1.3 + len(CLASSES) * cw)
    ax.set_ylim(-1.35, len(DOMAINS) * ch + 1.15)
    ax.axis("off")
    ax.set_title("(a) The matrix the report header prints", fontsize=9, loc="left")


def _ladder(ax, style):
    from matplotlib.patches import FancyBboxPatch
    for k, (q, outcome) in enumerate(LADDER):
        yy = (len(LADDER) - 1 - k) * 1.0
        last = k == len(LADDER) - 1
        fc = "#1baf7a" if last else style.SURFACE
        ax.add_patch(FancyBboxPatch((0.1, yy - 0.34), 8.9, 0.7, boxstyle="round,pad=0.06",
                                    fc=fc, ec="#1baf7a" if last else style.MUTED, lw=1.0))
        ax.text(0.3, yy + 0.09, q, fontsize=7.4, va="center", color="#ffffff" if last else style.INK)
        ax.text(0.3, yy - 0.18, outcome, fontsize=6.9, va="center",
                color="#ffffff" if last else style.COLORS["fv-lts"])
        if not last:
            ax.annotate("", xy=(0.5, yy - 0.48), xytext=(0.5, yy - 0.34),
                        arrowprops=dict(arrowstyle="-|>", color=style.MUTED, lw=1.0, mutation_scale=9))
    ax.text(0.1, len(LADDER) - 0.28, "the first gate that fires is the reason the cell reports",
            fontsize=7.2, color=style.MUTED)
    ax.set_xlim(0, 9.4)
    ax.set_ylim(-0.9, len(LADDER) + 0.15)
    ax.axis("off")
    ax.set_title("(b) The gate ladder behind one 2D-surface cell", fontsize=9, loc="left")


def _draw(style):
    import matplotlib.pyplot as plt
    fig, (a0, a1) = plt.subplots(1, 2, figsize=(9.6, 3.9), gridspec_kw=dict(width_ratios=[1.28, 1.0]))
    fig.patch.set_facecolor(style.SURFACE)
    _matrix(a0, style)
    _ladder(a1, style)
    fig.tight_layout(pad=0.3)
    return fig


def build(sink):
    import style
    sink.save(_draw(style), "quality_ch10_transport_policy")
