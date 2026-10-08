"""The five phases of a Lagrangian (LARD) transport step (quality Chapter
7, §7.4): a conduit is an ordered slab of parcels, each with a volume and
a concentration, between an upstream and a downstream node. Drain sends
the volume that left the conduit into the downstream node's ledger; Mix
runs the nodes as CSTRs in topological order; Release adds a parcel at
the upstream node's new concentration and merges look-alike neighbours;
Decay applies exact exponential decay; Publish reports the volume-
weighted mean. Synthetic; no run.
"""
from __future__ import annotations

REQUIRES = ()

# (volume fraction, concentration) parcels, upstream first
SLAB = [(0.18, 0.15), (0.22, 0.55), (0.20, 0.60), (0.25, 0.95), (0.15, 0.95)]
X0, LEN, H = 1.6, 6.0, 0.42


def _shade(cm, c):
    return cm(0.15 + 0.75 * c)


def _row(ax, y, parcels, style, cm, up_c, dn_c, title, note, drain=None, new=None, merged=None, uniform=False):
    from matplotlib.patches import Circle, Rectangle
    ax.text(0.05, y + H / 2, title, fontsize=8.6, weight="bold", va="center", color=style.INK)
    ax.add_patch(Circle((X0 - 0.35, y + H / 2), 0.22, fc=_shade(cm, up_c), ec=style.INK, lw=0.9))
    ax.add_patch(Circle((X0 + LEN + 0.35, y + H / 2), 0.22, fc=_shade(cm, dn_c), ec=style.INK, lw=0.9))
    x = X0
    if uniform:
        vmean = sum(v * c for v, c in parcels) / sum(v for v, _ in parcels)
        ax.add_patch(Rectangle((X0, y), LEN, H, fc=_shade(cm, vmean), ec=style.INK, lw=0.9))
        ax.text(X0 + LEN / 2, y + H / 2, f"volume-weighted mean  C = {vmean:.2f}", fontsize=7.6, ha="center", va="center",
                color=style.SURFACE if vmean > 0.5 else style.INK)
    else:
        for k, (v, c) in enumerate(parcels):
            w = v * LEN
            ec = style.COLORS["fv-lts"] if new == k else style.INK
            ax.add_patch(Rectangle((x, y), w, H, fc=_shade(cm, c), ec=ec, lw=1.6 if new == k else 0.9, zorder=2 if new == k else 1))
            ax.text(x + w / 2, y + H / 2, f"{c:.2f}", fontsize=6.8, ha="center", va="center",
                    color=style.SURFACE if c > 0.5 else style.INK)
            x += w
    if drain:
        w = drain * LEN
        ax.add_patch(Rectangle((X0 + LEN - w, y), w, H, fc="none", ec=style.COLORS["fv-lts"], lw=1.6, hatch="///", zorder=3))
        ax.annotate("", xy=(X0 + LEN + 0.15, y + H / 2), xytext=(X0 + LEN - 0.02, y + H / 2),
                    arrowprops=dict(arrowstyle="-|>", color=style.COLORS["fv-lts"], lw=1.4, mutation_scale=11))
    if merged:
        xa = X0 + sum(v for v, _ in parcels[:merged[0]]) * LEN
        xb = X0 + sum(v for v, _ in parcels[:merged[1] + 1]) * LEN
        ax.plot([xa, xa, xb, xb], [y - 0.06, y - 0.12, y - 0.12, y - 0.06], color=style.COLORS["dw"], lw=1.0)
        ax.text((xa + xb) / 2, y - 0.14, "merged: concentrations agree within tolerance", fontsize=6.8, ha="center",
                va="top", color=style.COLORS["dw"])
    ax.text(X0 + LEN + 0.75, y + H / 2, note, fontsize=7.4, va="center", color=style.MUTED)


def _draw(style):
    import matplotlib.pyplot as plt
    cm = plt.get_cmap("Blues")
    fig, ax = plt.subplots(figsize=(9.2, 5.0))
    fig.patch.set_facecolor(style.SURFACE)
    rows = 5
    step = 0.92
    ys = [0.3 + (rows - 1 - k) * step for k in range(rows)]
    # 1 drain: the downstream 0.20 of the slab leaves
    _row(ax, ys[0], SLAB, style, cm, 0.20, 0.40, "1  Drain", "volume that left over the step goes to the\ndownstream node's inflow ledger, with its mass",
         drain=0.20)
    # 2 mix: nodes as CSTRs; downstream node now carries the mixed value
    slab2 = [(0.18, 0.15), (0.22, 0.55), (0.20, 0.60), (0.20, 0.95)]
    _row(ax, ys[1], slab2, style, cm, 0.20, 0.78, "2  Mix", "each node mixes held volume + arrivals as a CSTR,\nin flow-aware topological order; loads join here")
    # 3 release: a new parcel at the upstream node's concentration; merge look-alikes
    slab3 = [(0.20, 0.20), (0.18, 0.15), (0.42, 0.57), (0.20, 0.95)]
    _row(ax, ys[2], slab3, style, cm, 0.20, 0.78, "3  Release", "a new parcel at the upstream node's new value,\nsized so parcels again sum to the link volume",
         new=0, merged=(2, 2))
    # 4 decay
    k = 0.75
    slab4 = [(v, c * k) for v, c in slab3]
    _row(ax, ys[3], slab4, style, cm, 0.20 * k, 0.78 * k, "4  Decay", "exact exponential  C ← C e^(−k Δt)  on parcels and\nnode stores; removed mass booked as reacted")
    # 5 publish
    _row(ax, ys[4], slab4, style, cm, 0.20 * k, 0.78 * k, "5  Publish", "link concentration = volume-weighted mean of the\nparcels; the parcels themselves are kept", uniform=True)
    ax.annotate("", xy=(X0 + LEN, ys[0] + H + 0.22), xytext=(X0, ys[0] + H + 0.22),
                arrowprops=dict(arrowstyle="-|>", color=style.WATER, lw=1.6, mutation_scale=12))
    ax.text(X0 + LEN / 2, ys[0] + H + 0.27, "flow", fontsize=8, ha="center", color=style.WATER)
    ax.text(X0 - 0.35, ys[0] + H + 0.27, "upstream\nnode", fontsize=7.2, ha="center", color=style.MUTED)
    ax.text(X0 + LEN + 0.35, ys[0] + H + 0.27, "downstream\nnode", fontsize=7.2, ha="center", color=style.MUTED)
    ax.text(0.05, -0.22, "Parcel budget: adjacent look-alikes merge at Release; MAX_SEGMENTS_PER_LINK (default 100) caps the slab,\n"
            "merging the oldest first.  QUALITY_STEP substeps the whole sequence; there is no Courant limit.",
            fontsize=7.2, color=style.MUTED, va="top", linespacing=1.3)
    ax.set_xlim(0, 12.6)
    ax.set_ylim(-0.7, ys[0] + H + 0.75)
    ax.set_aspect("equal")
    ax.axis("off")
    fig.tight_layout(pad=0.2)
    return fig


def build(sink):
    import style
    sink.save(_draw(style), "quality_ch7_lard_parcels")
