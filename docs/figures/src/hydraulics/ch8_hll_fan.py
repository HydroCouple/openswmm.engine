"""The Riemann fan at a finite-volume face (hydraulics Chapter 8, §8.5.3).

Left: a wet–wet face. The left and right states U_L, U_R are separated by
the fastest left- and right-going signals S_L and S_R; the hydrodynamic
flux is HLL, one averaged state across the whole fan. The contact wave
S* = v is resolved only for the species flux (FV_RIEMANN HLLC). Right: a
dry-bed face, where the right signal is the tail of the rarefaction
fanning into the dry cell, S_R = v_L + 2 c_L. The Davis signal-speed
estimates are written under each panel. Synthetic; no run.
"""
from __future__ import annotations

REQUIRES = ()


def _panel(ax, style, dry):
    FV, DW, OR = style.COLORS["fv"], style.COLORS["dw"], style.COLORS["fv-lts"]
    sl, sr = (-0.75, 1.15) if not dry else (-0.55, 1.45)
    T = 1.0
    ax.fill_between([sl * T, 0], 0, T, color=FV, alpha=0.10)
    ax.fill_between([0, sr * T], 0, T, color=DW if not dry else style.GRIDC, alpha=0.10 if not dry else 0.35)
    ax.fill_between([-1.6, sl * T], 0, T, color=style.WATER, alpha=0.06)
    ax.plot([0, sl * T], [0, T], color=FV, lw=2.0)
    ax.plot([0, sr * T], [0, T], color=DW if not dry else OR, lw=2.0)
    if not dry:
        ax.plot([0, 0.35 * T], [0, T], color=OR, lw=1.6, ls=(0, (4, 2)))
        ax.text(0.35 * T + 0.03, T - 0.02, "S* = v\n(species only)", fontsize=8, color=OR, va="top")
        ax.text(-1.55, 0.5 * T, "U_L", fontsize=10, color=style.INK)
        ax.text((sl + sr) * T / 2 - 0.05, 0.62 * T, "U_HLL", fontsize=9.5, color=style.INK, ha="center")
        ax.text(1.45, 0.5 * T, "U_R", fontsize=10, color=style.INK)
    else:
        for k in range(1, 6):
            s = sl + (sr - sl) * k / 6
            ax.plot([0, s * T], [0, T], color=OR, lw=0.7, alpha=0.7)
        ax.text(-1.55, 0.5 * T, "U_L", fontsize=10, color=style.INK)
        ax.text(0.45 * T, 0.68 * T, "rarefaction\nfan", fontsize=9, color=style.INK, ha="center")
        ax.text(1.55, 0.5 * T, "dry", fontsize=10, color=style.MUTED)
    ax.text(sl * T - 0.04, T + 0.03, "S_L", fontsize=9, color=FV, ha="right")
    ax.text(sr * T + 0.04, T + 0.03, "S_R", fontsize=9, color=DW if not dry else OR)
    ax.axhline(0, color=style.INK, lw=1.0)
    ax.plot([0, 0], [-0.06, 0.04], color=style.INK, lw=1.0)
    ax.text(0, -0.1, "face", fontsize=8.5, ha="center", va="top", color=style.MUTED)
    ax.annotate("", xy=(0, T + 0.22), xytext=(0, 0.02), arrowprops=dict(arrowstyle="-|>", color=style.MUTED, lw=0.8))
    ax.text(0.04, T + 0.18, "t", fontsize=9, color=style.MUTED)
    ax.annotate("", xy=(1.85, 0), xytext=(-1.65, 0), arrowprops=dict(arrowstyle="-|>", color=style.MUTED, lw=0.8))
    ax.text(1.85, -0.1, "x", fontsize=9, color=style.MUTED, ha="right", va="top")
    ax.set_xlim(-1.7, 1.9)
    ax.set_ylim(-0.42, T + 0.3)
    ax.axis("off")


def _draw(style):
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(1, 2, figsize=(9.0, 4.0))
    fig.patch.set_facecolor(style.SURFACE)
    _panel(axes[0], style, dry=False)
    _panel(axes[1], style, dry=True)
    axes[0].set_title("(a) Wet–wet face: HLL averages the fan;\nS* serves only the species flux",
                      fontsize=8.6, loc="left", color=style.INK)
    axes[1].set_title("(b) Dry-bed face: the rarefaction tail\nbounds the front", fontsize=8.6, loc="left", color=style.INK)
    fig.text(0.02, 0.105, "Davis estimates:  S_L = min(v_L − c_L, v_R − c_R),   S_R = max(v_L + c_L, v_R + c_R),   c = √(g A / T)",
             fontsize=8, color=style.MUTED)
    fig.text(0.02, 0.06, "Dry right cell:  S_L = v_L − c_L,   S_R = v_L + 2 c_L   (mirror-wise for a dry left cell: v_R − 2 c_R, v_R + c_R)",
             fontsize=8, color=style.MUTED)
    fig.tight_layout(rect=[0, 0.13, 1, 1])
    return fig


def build(sink):
    import style
    sink.save(_draw(style), "hydraulics_ch8_hll_fan")
