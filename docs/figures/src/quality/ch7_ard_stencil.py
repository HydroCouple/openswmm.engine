"""Flux-limited advection in the Eulerian (ARD) engine (quality Chapter 7,
§7.3.2).

Left: the face stencil. First-order upwind carries the cell value C_i to
the face; MUSCL reconstructs a slope inside the cell and the limiter
clips it so the face value never leaves the range of its neighbours.
Right: a step front advected 60 cells at Courant 0.5 by each scheme the
`SCALAR_SCHEME`/`LIMITER` keys select — UPWIND smears it, MINMOD less so,
VANLEER keeps it sharp, SUPERBEE sharpest but stair-steps a smooth ramp.
The demonstration is a 40-line numpy transcription of the flux-limited
form, not an engine run; it reproduces the qualitative ordering the text
describes. Synthetic.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()


def _limiter(name, r):
    if name == "UPWIND":
        return np.zeros_like(r)
    if name == "MINMOD":
        return np.maximum(0.0, np.minimum(1.0, r))
    if name == "VANLEER":
        return (r + np.abs(r)) / (1.0 + np.abs(r))
    if name == "SUPERBEE":
        return np.maximum.reduce([np.zeros_like(r), np.minimum(2.0 * r, 1.0), np.minimum(r, 2.0)])
    raise KeyError(name)


def _advect(c0, nu, steps, name):
    c = c0.copy()
    for _ in range(steps):
        dc = np.diff(c)                                  # C_{i+1} - C_i, length n-1
        num = np.concatenate([[0.0], dc[:-1]])           # C_i - C_{i-1} at the same faces
        with np.errstate(divide="ignore", invalid="ignore"):
            r = np.where(np.abs(dc) > 1e-12, num / dc, 0.0)
        phi = _limiter(name, r)
        face = c[:-1] + 0.5 * phi * (1.0 - nu) * dc       # C_{i+1/2} for u > 0, i = 0 .. n-2
        face = np.concatenate([[c[0]], face, [c[-1]]])   # inflow face holds the boundary value; outflow is upwind
        c -= nu * (face[1:] - face[:-1])
    return c


def _stencil(ax, style):
    vals = [1.0, 1.0, 0.85, 0.35, 0.1, 0.1]
    FV, OR, DW = style.COLORS["fv"], style.COLORS["fv-lts"], style.COLORS["dw"]
    for i, v in enumerate(vals):
        ax.add_patch(__import__("matplotlib").patches.Rectangle((i, 0), 1, v, fc=style.WATER, alpha=0.18, ec=style.MUTED, lw=0.8))
        ax.plot([i, i + 1], [v, v], color=style.INK, lw=1.6)
    i = 2
    ax.axvline(i + 1, color=style.INK, lw=1.0, ls=(0, (3, 2)))
    ax.text(i + 1, 1.16, "face i+½", fontsize=8, ha="center", color=style.INK)
    ax.text(i + 0.5, -0.09, "cell i", fontsize=8, ha="center", color=style.MUTED)
    ax.text(i + 1.5, -0.09, "cell i+1", fontsize=8, ha="center", color=style.MUTED)
    ax.text(i - 0.5, -0.09, "cell i−1", fontsize=8, ha="center", color=style.MUTED)
    ax.annotate("", xy=(i + 0.95, vals[i]), xytext=(i + 0.55, vals[i]),
                arrowprops=dict(arrowstyle="-|>", color=OR, lw=1.4, mutation_scale=11))
    ax.text(i + 0.5, vals[i] - 0.14, "upwind: C_i", fontsize=7.8, color=OR, ha="center", va="top")
    slope_free = (vals[i + 1] - vals[i - 1]) / 2.0
    slope_lim = 0.55 * slope_free
    xs = np.array([i, i + 1])
    ax.plot(xs, vals[i] + slope_free * (xs - i - 0.5), color=style.MUTED, lw=1.0, ls=(0, (2, 2)))
    ax.plot(xs, vals[i] + slope_lim * (xs - i - 0.5), color=FV, lw=1.8)
    ax.plot([i + 1], [vals[i] + 0.5 * slope_lim], "o", color=FV, ms=6)
    ax.text(i + 1.08, vals[i] + 0.5 * slope_lim - 0.02, "MUSCL: C_i + ½ φ(r) ΔC", fontsize=7.8, color=FV, va="top")
    ax.text(i + 0.45, 1.09, "unlimited slope (dashed)\nwould overshoot the\nupstream value", fontsize=7, color=style.MUTED, va="bottom", ha="right")
    ax.text(0.1, 0.30, "r = (C_i − C_{i−1}) / (C_{i+1} − C_i)\nφ(r): MINMOD · VANLEER · SUPERBEE", fontsize=7.6, color=style.INK)
    ax.text(3.9, 0.58, "limited slope never leaves\n[C_{i+1}, C_{i−1}]: no new extrema", fontsize=7.4, color=DW, va="top")
    ax.annotate("", xy=(5.6, 0.25), xytext=(4.4, 0.25), arrowprops=dict(arrowstyle="-|>", color=style.WATER, lw=1.6, mutation_scale=12))
    ax.text(5.0, 0.30, "u > 0", fontsize=8, ha="center", color=style.WATER)
    ax.set_xlim(-0.2, 6.3)
    ax.set_ylim(-0.18, 1.28)
    ax.axis("off")
    ax.set_title("(a) One face of the ARD stencil", fontsize=9, loc="left")


def _demo(ax, style):
    n = 160
    x = np.arange(n) + 0.5
    c0 = np.where(x < 20, 1.0, 0.0)
    nu, steps = 0.5, 120
    exact = np.where(x < 20 + nu * steps, 1.0, 0.0)
    ax.plot(x, exact, color=style.INK, lw=1.0, ls=(0, (2, 2)), label="exact (shifted step)")
    for name, col in (("UPWIND", style.COLORS["dw-legacy"]), ("MINMOD", style.COLORS["dw-slot"]),
                      ("VANLEER", style.COLORS["fv"]), ("SUPERBEE", style.COLORS["dw"])):
        c = _advect(c0, nu, steps, name)
        lab = "UPWIND" if name == "UPWIND" else f"MUSCL + {name}"
        ax.plot(x, c, color=col, lw=1.5, label=lab)
    ax.set_xlim(40, 130)
    ax.set_ylim(-0.05, 1.12)
    ax.set_xlabel("cell index")
    ax.set_ylabel("concentration C / C_0")
    ax.legend(fontsize=7.2, frameon=False, loc="center right")
    ax.set_title("(b) A step front after 60 cells of travel, Courant 0.5", fontsize=9, loc="left")


def _draw(style):
    import matplotlib.pyplot as plt
    fig, (a0, a1) = plt.subplots(1, 2, figsize=(9.4, 3.7), gridspec_kw=dict(width_ratios=[1.05, 1.0]))
    fig.patch.set_facecolor(style.SURFACE)
    style.style_ax(a1)
    _stencil(a0, style)
    _demo(a1, style)
    fig.tight_layout()
    return fig


def build(sink):
    import style
    sink.save(_draw(style), "quality_ch7_ard_stencil")
