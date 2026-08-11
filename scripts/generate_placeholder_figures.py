#!/usr/bin/env python3
"""Generate placeholder / schematic figures for the reference manuals.

Figures 3-8 and 3-9 (hydraulics) are programmatic sketches watermarked
PLACEHOLDER — they communicate the intended content and are meant to be
replaced by final drawings. Figure 7-11 (hydrology) is a synthetic
schematic illustrating emergent seasonal RDII behavior; it can likewise
be replaced with a rendering from a real simulation.

Re-run any time: outputs are deterministic.
"""
import numpy as np
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Circle, Rectangle, FancyArrowPatch
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "docs" / "manuals" / "reference"
HYDRA = ROOT / "hydraulics" / "media" / "media"
HYDRO = ROOT / "hydrology" / "media" / "media"


def watermark(ax):
    ax.text(0.5, 0.5, "PLACEHOLDER", transform=ax.transAxes, fontsize=40,
            color="0.85", ha="center", va="center", rotation=20, zorder=0)


def fig_3_8():
    """Dynamic Preissmann slot: conduit cross-section + P decay inset."""
    fig, (ax, ax2) = plt.subplots(1, 2, figsize=(9, 4.2), width_ratios=[1.1, 1])
    watermark(ax)
    # conduit
    ax.add_patch(Circle((0, 0), 1.0, fill=False, lw=2))
    # slot above crown
    slot_w = 0.16
    ax.add_patch(Rectangle((-slot_w / 2, 1.0), slot_w, 1.1, fill=False, lw=2))
    # water surface in slot
    hs = 1.75
    ax.plot([-slot_w / 2, slot_w / 2], [hs, hs], color="tab:blue", lw=2)
    ax.fill_between([-slot_w / 2, slot_w / 2], 1.0, hs, color="tab:blue", alpha=0.25)
    circ = plt.Circle((0, 0), 1.0, color="tab:blue", alpha=0.25)
    ax.add_patch(circ)
    ax.annotate(r"$A_{full}$", (0, -0.15), ha="center", fontsize=12)
    ax.annotate(r"$T_s$", (slot_w / 2 + 0.06, 1.45), fontsize=12)
    ax.annotate(r"$A_s$", (0, 1.32), ha="center", fontsize=11, color="tab:blue")
    ax.annotate("", xy=(0.55, hs), xytext=(0.55, 1.0),
                arrowprops=dict(arrowstyle="<->"))
    ax.annotate(r"$h_s$", (0.62, 1.35), fontsize=12)
    ax.plot([-1.4, -1.05], [1.0, 1.0], "k--", lw=0.8)
    ax.annotate("crown", (-1.42, 1.0), ha="right", va="center", fontsize=9)
    ax.set_xlim(-1.9, 1.6); ax.set_ylim(-1.3, 2.3)
    ax.set_aspect("equal"); ax.axis("off")
    ax.set_title("Surcharged conduit with dynamic slot", fontsize=10)
    # inset: Preissmann number decay
    t = np.linspace(0, 5, 200)
    P0 = 3.2
    P = 1 + (P0 - 1) * np.exp(-t / 0.9)
    ax2.plot(t, P, lw=2)
    ax2.axhline(1.0, color="k", ls="--", lw=0.8)
    ax2.annotate(r"$\hat{P}_0$", (0.05, P0), fontsize=12)
    ax2.annotate(r"$P \rightarrow 1$", (3.6, 1.18), fontsize=11)
    ax2.set_xlabel(r"time since pressurization  $t - t_s$")
    ax2.set_ylabel(r"Preissmann number $P$")
    ax2.set_title("Slot-width state decay", fontsize=10)
    ax2.spines[["top", "right"]].set_visible(False)
    fig.suptitle("Figure 3-8 (placeholder) — replace with final drawing",
                 fontsize=9, color="0.4")
    fig.tight_layout()
    fig.savefig(HYDRA / "figure3-8-placeholder.png", dpi=160)
    plt.close(fig)


def fig_3_9():
    """Virtual junction vs regular junction at a grade break (profile)."""
    fig, axes = plt.subplots(1, 2, figsize=(9, 3.6), sharey=True)
    for ax, title in zip(axes, ["Regular junction", "Virtual junction"]):
        watermark(ax)
        # upstream / downstream inverts with a grade break
        ax.plot([0, 4], [2.0, 1.0], "k", lw=2)
        ax.plot([4, 8], [1.0, 0.7], "k", lw=2)
        ax.plot([0, 4], [3.0, 2.0], "k", lw=1)
        ax.plot([4, 8], [2.0, 1.7], "k", lw=1)
        ax.set_title(title, fontsize=10)
        ax.axis("off")
    # left: manhole storage
    axes[0].add_patch(Rectangle((3.7, 1.0), 0.6, 2.2, fill=True,
                                color="0.85", ec="k", lw=1.5))
    axes[0].annotate("MIN_SURFAREA\nstorage", (4.0, 3.4), ha="center", fontsize=9)
    axes[0].annotate("stagnation\nvolume", (4.0, 0.35), ha="center", fontsize=9)
    # right: interior point, momentum carried through
    axes[1].plot([4], [1.0], "ko", ms=6)
    axes[1].annotate("zero storage,\ncontinuous invert", (4.0, 3.2),
                     ha="center", fontsize=9)
    arrow = FancyArrowPatch((2.2, 1.85), (5.8, 1.15), arrowstyle="-|>",
                            mutation_scale=18, color="tab:blue", lw=2)
    axes[1].add_patch(arrow)
    axes[1].annotate("momentum flux", (4.0, 2.15), ha="center", fontsize=9,
                     color="tab:blue")
    fig.suptitle("Figure 3-10 (placeholder) — replace with final drawing",
                 fontsize=9, color="0.4")
    fig.tight_layout()
    fig.savefig(HYDRA / "figure3-10-placeholder.png", dpi=160)
    plt.close(fig)


def fig_7_11():
    """Seasonal behavior of the exponential-decay IA model (schematic)."""
    rng = np.random.default_rng(7)
    days = np.arange(365)
    temp = 10 + 15 * np.sin(2 * np.pi * (days - 100) / 365)
    rain = np.where(rng.random(365) < 0.25, rng.gamma(2.0, 6.0, 365), 0.0)
    ia_max, ia = 25.0, 12.0
    k_dep, r_rec, ddf, t_freeze = 0.08, 0.6, 0.12, 0.0
    ia_series, rdii = [], []
    for d in days:
        depl = k_dep * rain[d] * ia / ia_max * 3
        ia = max(ia - depl, 0.0)
        rec = 0.0 if temp[d] < t_freeze else r_rec * (1 + ddf * max(temp[d], 0)) * 0.15
        ia = min(ia + rec, ia_max)
        ia_series.append(ia)
        excess = max(rain[d] - (ia / ia_max) * rain[d], 0.0)
        rdii.append(excess * 0.12)
    fig, axes = plt.subplots(3, 1, figsize=(8.5, 6.2), sharex=True)
    axes[0].bar(days, rain, color="tab:blue", width=1.0, label="rainfall")
    ax0b = axes[0].twinx()
    ax0b.plot(days, temp, color="tab:red", lw=1.2, label="temperature")
    ax0b.axhline(t_freeze, color="tab:red", ls=":", lw=0.8)
    axes[0].set_ylabel("rain (mm)")
    ax0b.set_ylabel("temp (°C)", color="tab:red")
    axes[1].plot(days, ia_series, color="tab:green", lw=1.5)
    axes[1].axhline(ia_max, color="k", ls="--", lw=0.8)
    axes[1].annotate(r"$IA_{max}$", (5, ia_max + 0.5), fontsize=10)
    axes[1].set_ylabel(r"$IA_{avail}$ (mm)")
    frozen = temp < t_freeze
    axes[1].fill_between(days, 0, ia_max * 1.1, where=frozen, color="0.9",
                         label="frozen ground (recovery suspended)")
    axes[1].legend(loc="lower right", fontsize=8)
    axes[2].plot(days, rdii, color="tab:purple", lw=1.2)
    axes[2].set_ylabel("RDII (schematic)")
    axes[2].set_xlabel("day of year")
    for ax in axes:
        ax.spines[["top", "right"]].set_visible(False)
    fig.suptitle("Figure 7-11 — schematic; regenerate from a real simulation "
                 "if preferred", fontsize=9, color="0.4")
    fig.tight_layout()
    fig.savefig(HYDRO / "figure7-11-schematic.png", dpi=160)
    plt.close(fig)


def fig_8_3():
    """Hydrostatic reconstruction at a wet/dry front (two panels)."""
    fig, axes = plt.subplots(1, 2, figsize=(9, 3.8), sharey=True)
    for ax, (eta_l, title) in zip(
            axes, [(1.9, "Wetting front"), (1.15, "Emerged bank (wall)")]):
        watermark(ax)
        z_l, z_r = 0.5, 1.4          # cell-centre beds; z* = max = 1.4
        # beds
        ax.plot([0, 4], [z_l, z_l], "k", lw=2)
        ax.plot([4, 8], [z_r, z_r], "k", lw=2)
        ax.plot([4, 4], [z_l, z_r], "k", lw=2)
        # interface bed z*
        ax.plot([2.6, 5.4], [z_r, z_r], "k--", lw=1)
        ax.annotate(r"$z^{*}=\max(z_L,z_R)$", (5.5, z_r - 0.05), fontsize=9,
                    va="top")
        # water in the left cell
        ax.fill_between([0, 4], z_l, eta_l, color="tab:blue", alpha=0.3)
        ax.plot([0, 4], [eta_l, eta_l], color="tab:blue", lw=2)
        ax.annotate(r"$\eta_L$", (0.3, eta_l + 0.06), fontsize=11,
                    color="tab:blue")
        ax.annotate(r"$z_L$", (0.3, z_l - 0.22), fontsize=10)
        ax.annotate(r"$z_R$", (7.3, z_r - 0.22), fontsize=10)
        h_star = max(0.0, eta_l - z_r)
        if h_star > 0:
            ax.annotate("", xy=(4.35, z_r + h_star), xytext=(4.35, z_r),
                        arrowprops=dict(arrowstyle="<->", color="tab:blue"))
            ax.annotate(r"$h^{*}_L=\eta_L-z^{*}$", (4.5, z_r + h_star / 2),
                        fontsize=9, color="tab:blue", va="center")
            arrow = FancyArrowPatch((3.2, eta_l + 0.15), (5.2, eta_l + 0.15),
                                    arrowstyle="-|>", mutation_scale=14,
                                    color="tab:blue")
            ax.add_patch(arrow)
            ax.annotate("front advances", (4.2, eta_l + 0.28), ha="center",
                        fontsize=9, color="tab:blue")
        else:
            ax.annotate(r"$h^{*}_L=h^{*}_R=0$" + "\nzero flux",
                        (4.0, z_r + 0.35), ha="center", fontsize=9)
        ax.annotate(r"$h^{*}_R=0$ (dry)", (6.4, z_r + 0.12), fontsize=9)
        ax.set_xlim(0, 8); ax.set_ylim(0, 2.6)
        ax.set_title(title, fontsize=10)
        ax.axis("off")
    fig.suptitle("Figure 8-3 (placeholder) — replace with final drawing",
                 fontsize=9, color="0.4")
    fig.tight_layout()
    fig.savefig(HYDRA / "figure8-3-placeholder.png", dpi=160)
    plt.close(fig)


def fig_8_4():
    """Node ghost-state construction at a coupling face (profile)."""
    fig, ax = plt.subplots(figsize=(7.5, 4.0))
    watermark(ax)
    # manhole shaft
    ax.add_patch(Rectangle((0.6, 0.4), 1.4, 3.2, fill=False, lw=2))
    H = 2.6
    ax.fill_between([0.6, 2.0], 0.4, H, color="tab:blue", alpha=0.3)
    ax.plot([0.6, 2.0], [H, H], color="tab:blue", lw=2)
    ax.annotate(r"head $H$", (1.3, H + 0.12), ha="center", fontsize=10,
                color="tab:blue")
    # conduit and end cell
    z_f = 1.0
    ax.plot([2.0, 7.6], [z_f, 0.8], "k", lw=2)
    ax.plot([2.0, 7.6], [z_f + 1.0, 1.8], "k", lw=1)
    ax.fill_between([2.0, 7.6], [z_f, 0.8], [z_f + 0.55, 0.35 + 0.8],
                    color="tab:blue", alpha=0.25)
    # coupling face
    ax.plot([2.0, 2.0], [0.4, 3.6], "k-.", lw=1.2)
    ax.annotate("coupling face", (2.05, 3.4), fontsize=9)
    ax.annotate(r"$z_f$ (invert + offset)", (2.1, z_f - 0.35), fontsize=9)
    # ghost depth
    ax.annotate("", xy=(1.75, H), xytext=(1.75, z_f),
                arrowprops=dict(arrowstyle="<->"))
    ax.annotate(r"$h_g = H - z_f$", (0.35, 1.8), fontsize=10, rotation=90,
                va="center")
    # interior velocity carried onto the ghost
    arrow = FancyArrowPatch((3.2, 1.25), (4.8, 1.15), arrowstyle="-|>",
                            mutation_scale=16, color="tab:red", lw=2)
    ax.add_patch(arrow)
    ax.annotate(r"$v_{int}$ (end cell)", (4.0, 1.5), ha="center", fontsize=9,
                color="tab:red")
    ax.annotate(r"ghost: $A(h_g),\ v_g=v_{int}$", (2.4, 2.9), fontsize=10)
    ax.annotate("end cell", (3.6, 0.55), fontsize=9)
    ax.set_xlim(0, 8); ax.set_ylim(0, 4.0)
    ax.set_aspect("equal"); ax.axis("off")
    ax.set_title("Ghost state built from the node head", fontsize=10)
    fig.suptitle("Figure 8-4 (placeholder) — replace with final drawing",
                 fontsize=9, color="0.4")
    fig.tight_layout()
    fig.savefig(HYDRA / "figure8-4-placeholder.png", dpi=160)
    plt.close(fig)


def fig_9_2():
    """Wetting cases of a planar-bed triangular cell + wetted-edge gate."""
    fig, axes = plt.subplots(1, 4, figsize=(11, 3.4),
                             width_ratios=[1, 1, 1, 1.2])
    tri = np.array([[0.5, 0.5], [3.5, 1.0], [1.8, 3.4], [0.5, 0.5]])
    verts = [(0.5, 0.5, r"$z_1$"), (3.5, 1.0, r"$z_2$"), (1.8, 3.4, r"$z_3$")]
    cases = [
        ("η below $z_2$", [np.array([[0.5, 0.5], [1.9, 0.75], [1.05, 1.75],
                                     [0.5, 0.5]])]),
        ("$z_2$ < η < $z_3$", [np.array([[0.5, 0.5], [3.5, 1.0], [2.6, 2.3],
                                         [1.15, 2.1], [0.5, 0.5]])]),
        ("fully submerged", [tri]),
    ]
    for ax, (title, polys) in zip(axes[:3], cases):
        watermark(ax)
        ax.plot(tri[:, 0], tri[:, 1], "k", lw=2)
        for poly in polys:
            ax.fill(poly[:, 0], poly[:, 1], color="tab:blue", alpha=0.35)
        for x, y, lab in verts:
            ax.plot(x, y, "ko", ms=4)
            ax.annotate(lab, (x + 0.08, y + 0.08), fontsize=10)
        ax.annotate(r"$\eta$", (2.6, 0.35), fontsize=10, color="tab:blue")
        ax.set_xlim(0, 4); ax.set_ylim(0, 3.9)
        ax.set_title(title, fontsize=9)
        ax.set_aspect("equal"); ax.axis("off")
    # edge-profile inset: face-depth branches over a sloping shared edge
    ax = axes[3]
    watermark(ax)
    z_lo, z_hi = 0.8, 2.2
    ax.plot([0, 4], [z_lo, z_hi], "k", lw=2)
    ax.annotate(r"$z_{lo}$", (0.05, z_lo - 0.3), fontsize=10)
    ax.annotate(r"$z_{hi}$", (3.6, z_hi + 0.12), fontsize=10)
    for eta, lab in [(0.55, "blocked"), (1.5, "partial"), (2.6, "submerged")]:
        ax.plot([0, 4], [eta, eta], "--", lw=1.2, color="tab:blue")
        ax.annotate(lab, (4.06, eta), fontsize=8, va="center",
                    color="tab:blue")
    ax.fill_between([0, 2.0], [z_lo, z_lo + 0.7], 1.5,
                    where=[True, True], color="tab:blue", alpha=0.25)
    ax.set_xlim(0, 5.4); ax.set_ylim(0, 3.2)
    ax.set_title("wetted-edge face gate", fontsize=9)
    ax.axis("off")
    fig.suptitle("Figure 9-2 (placeholder) — replace with final drawing",
                 fontsize=9, color="0.4")
    fig.tight_layout()
    fig.savefig(HYDRA / "figure9-2-placeholder.png", dpi=160)
    plt.close(fig)


if __name__ == "__main__":
    fig_3_8()
    fig_3_9()
    fig_7_11()
    fig_8_3()
    fig_8_4()
    fig_9_2()
    print("wrote:",
          HYDRA / "figure3-8-placeholder.png",
          HYDRA / "figure3-10-placeholder.png",
          HYDRO / "figure7-11-schematic.png",
          HYDRA / "figure8-3-placeholder.png",
          HYDRA / "figure8-4-placeholder.png",
          HYDRA / "figure9-2-placeholder.png", sep="\n  ")
