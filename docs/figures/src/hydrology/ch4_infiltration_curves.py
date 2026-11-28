"""Infiltration curves for hydrology Chapter 4, plotted from the chapter's
own equations.

Figure 4-2  Horton infiltration capacity under an intermittent hyetograph
Figure 4-3  cumulative infiltration as the area under the Horton curve
Figure 4-4  regeneration of capacity during dry weather (the recovery curve)
Figure 4-6  Green-Ampt capacity versus cumulative infiltration
Figure 4-7  Green-Ampt recovery parameters versus saturated conductivity

No engine run: the curves are Equations 4-1, 4-3, 4-6, 4-27, 4-29 and
4-35 to 4-37 evaluated in numpy with the parameter values the text quotes,
so a change to a formula in the chapter is a change here.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()

F0, FINF, KD = 3.0, 0.5, 4.0          # in/hr, in/hr, 1/hr — a Horton sandy loam
KR = KD / 6.0                          # recovery constant (the engine's default kd/6 ratio)


def _horton(t):
    return FINF + (F0 - FINF) * np.exp(-KD * t)


def _fig_4_2(style):
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(7.2, 3.6))
    fig.patch.set_facecolor(style.SURFACE)
    style.style_ax(ax)
    t = np.linspace(0, 2.0, 400)
    fp = _horton(t)
    # a stepped hyetograph that exceeds capacity twice
    steps = [(0.0, 0.25, 1.2), (0.25, 0.5, 3.5), (0.5, 0.8, 0.8), (0.8, 1.1, 2.2), (1.1, 1.4, 0.4), (1.4, 2.0, 1.4)]
    i = np.zeros_like(t)
    for a, b, v in steps:
        i[(t >= a) & (t < b)] = v
    ax.step(t, i, where="post", color=style.MUTED, lw=1.2, label="rainfall intensity i(t)")
    ax.plot(t, fp, color=style.COLORS["fv"], lw=2.0, label="infiltration capacity f_p(t) = f_∞ + (f_0 − f_∞) e^(−k_d t)")
    f = np.minimum(fp, i)
    ax.fill_between(t, 0, f, color=style.COLORS["fv"], alpha=0.15, label="actual infiltration f = min(f_p, i)")
    ax.fill_between(t, f, i, where=i > fp, color=style.COLORS["dw-slot"], alpha=0.35, label="runoff (i exceeds f_p)")
    ax.axhline(FINF, color=style.MUTED, lw=0.8, ls=(0, (3, 2)))
    ax.text(1.98, FINF + 0.06, "f_∞", fontsize=8, color=style.MUTED, ha="right")
    ax.text(0.02, F0 - 0.15, "f_0", fontsize=8, color=style.COLORS["fv"])
    ax.set_xlabel("time from the start of the storm t (h)")
    ax.set_ylabel("rate (in/h)")
    ax.set_xlim(0, 2)
    ax.set_ylim(0, 3.8)
    ax.legend(loc="upper right", fontsize=7)
    fig.tight_layout()
    return fig


def _fig_4_3(style):
    import matplotlib.pyplot as plt
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(7.6, 3.2))
    fig.patch.set_facecolor(style.SURFACE)
    for ax in (a1, a2):
        style.style_ax(ax)
    t = np.linspace(0, 2.0, 400)
    tp = 0.6
    a1.plot(t, _horton(t), color=style.COLORS["fv"], lw=2.0)
    m = t <= tp
    a1.fill_between(t[m], 0, _horton(t[m]), color=style.COLORS["fv"], alpha=0.2)
    a1.axvline(tp, color=style.MUTED, lw=0.8, ls=(0, (3, 2)))
    a1.text(tp + 0.03, 3.3, "t_p", fontsize=8, color=style.MUTED)
    a1.text(tp / 2, 0.9, "F(t_p)", fontsize=9, color=style.COLORS["fv"], ha="center")
    a1.set_xlabel("t (h)")
    a1.set_ylabel("f_p (in/h)")
    a1.set_title("F is the area under the capacity curve", fontsize=9)
    a1.set_ylim(0, 3.6)
    F = FINF * t + (F0 - FINF) / KD * (1 - np.exp(-KD * t))
    a2.plot(t, F, color=style.COLORS["fv"], lw=2.0)
    a2.plot(t, FINF * t + (F0 - FINF) / KD, color=style.MUTED, lw=0.8, ls=(0, (3, 2)))
    a2.text(1.4, FINF * 1.4 + (F0 - FINF) / KD + 0.08, "asymptote f_∞ t + (f_0 − f_∞)/k_d", fontsize=7, color=style.MUTED)
    a2.set_xlabel("t_p (h)")
    a2.set_ylabel("F (in)")
    a2.set_title("F(t_p) = f_∞ t_p + (f_0 − f_∞)(1 − e^(−k_d t_p))/k_d", fontsize=9)
    fig.tight_layout()
    return fig


def _fig_4_4(style):
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(7.2, 3.6))
    fig.patch.set_facecolor(style.SURFACE)
    style.style_ax(ax)
    t1 = 0.4                                   # rain stops; recovery begins at f_r
    kr = KD / 8.0                              # a slow recovery makes the projection visible
    fr = _horton(t1)
    tw = t1 - np.log((F0 - FINF) / (F0 - fr)) / kr   # Equation 4-8
    t_wet = np.linspace(0, t1, 200)
    t_dry = np.linspace(t1, 6.0, 400)
    rec = F0 - (F0 - FINF) * np.exp(-kr * (t_dry - tw))     # Equation 4-6
    t_proj = np.linspace(tw, t1, 100)
    ax.plot(t_wet, _horton(t_wet), color=style.COLORS["fv"], lw=2.0, label="capacity decays while wet (Eq. 4-1)")
    ax.plot(t_dry, rec, color=style.COLORS["dw"], lw=2.0, label="capacity recovers while dry (Eq. 4-6)")
    ax.plot(t_proj, F0 - (F0 - FINF) * np.exp(-kr * (t_proj - tw)), color=style.COLORS["dw"], lw=1.0,
            ls=(0, (3, 2)), label="recovery curve projected back to t_w, where f_p = f_∞ (Eq. 4-8)")
    ax.axhline(F0, color=style.MUTED, lw=0.8, ls=(0, (3, 2)))
    ax.axhline(FINF, color=style.MUTED, lw=0.8, ls=(0, (3, 2)))
    ax.plot([t1], [fr], marker="o", ms=5, color=style.INK)
    ax.text(t1 + 0.1, fr + 0.12, "f_r at t_pr, when rain stops", fontsize=8, color=style.INK)
    ax.plot([tw], [FINF], marker="o", ms=5, mfc="white", mec=style.COLORS["dw"])
    ax.text(tw, FINF - 0.22, "t_w", fontsize=8, color=style.COLORS["dw"], ha="center")
    ax.axvspan(0, t1, color=style.WATER, alpha=0.07)
    ax.text(t1 / 2, 3.45, "wet", fontsize=8, color=style.MUTED, ha="center")
    ax.text(1.4, 3.45, "dry", fontsize=8, color=style.MUTED, ha="center")
    ax.text(5.95, F0 - 0.2, "f_0", fontsize=8, color=style.MUTED, ha="right")
    ax.text(5.95, FINF + 0.06, "f_∞", fontsize=8, color=style.MUTED, ha="right")
    ax.set_xlabel("time (h)")
    ax.set_ylabel("f_p (in/h)")
    ax.set_xlim(min(tw, 0) - 0.15, 6)
    ax.set_ylim(0, 3.8)
    ax.legend(loc="center right", fontsize=7)
    fig.tight_layout()
    return fig


def _fig_4_6(style):
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(7.2, 3.6))
    fig.patch.set_facecolor(style.SURFACE)
    style.style_ax(ax)
    Ks, psi, thd, i = 0.25, 6.5, 0.20, 1.0    # in/h, in, —, in/h (the chapter's example)
    Fs = Ks * psi * thd / (i - Ks)             # Equation 4-29
    F = np.linspace(0.01, 4.0, 600)
    fp = Ks * (1 + psi * thd / F)              # Equation 4-27
    ax.plot(F[F <= Fs], np.full((F <= Fs).sum(), i), color=style.COLORS["fv"], lw=2.2, label="before saturation: f = i")
    ax.plot(F[F >= Fs], fp[F >= Fs], color=style.COLORS["dw"], lw=2.2, label="after saturation: f_p = K_s (1 + ψ_s θ_d / F)")
    ax.plot(F, fp, color=style.COLORS["dw"], lw=0.8, ls=(0, (2, 3)), alpha=0.7)
    ax.axhline(Ks, color=style.MUTED, lw=0.8, ls=(0, (3, 2)))
    ax.axvline(Fs, color=style.MUTED, lw=0.8, ls=(0, (3, 2)))
    ax.text(Fs + 0.05, 1.9, f"F_s = K_s ψ_s θ_d / (i − K_s) = {Fs:.2f} in", fontsize=8, color=style.MUTED)
    ax.text(3.95, Ks - 0.09, "K_s = 0.25 in/h", fontsize=8, color=style.MUTED, ha="right", va="top")
    ax.set_xlabel("cumulative infiltration F (in)")
    ax.set_ylabel("infiltration rate (in/h)")
    ax.set_xlim(0, 4)
    ax.set_ylim(0, 2.1)
    ax.set_title("K_s = 0.25 in/h, ψ_s = 6.5 in, θ_d = 0.20, rainfall i = 1.0 in/h", fontsize=9)
    ax.legend(loc="upper right", fontsize=7.5)
    fig.tight_layout()
    return fig


def _fig_4_7(style):
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(1, 3, figsize=(8.4, 2.9))
    fig.patch.set_facecolor(style.SURFACE)
    Ks = np.linspace(0.02, 2.0, 300)
    series = [("L_u = 4 √K_s  (in)", 4 * np.sqrt(Ks), style.COLORS["fv"]),
              ("k_r = √K_s / 75  (1/h)", np.sqrt(Ks) / 75, style.COLORS["dw"]),
              ("T_r = 4.5 / √K_s  (h)", 4.5 / np.sqrt(Ks), style.COLORS["fv-lts"])]
    for ax, (label, y, c) in zip(axes, series):
        style.style_ax(ax)
        ax.plot(Ks, y, color=c, lw=2.0)
        ax.set_title(label, fontsize=9)
        ax.set_xlabel("K_s (in/h)")
        ax.set_xlim(0, 2)
    axes[0].set_ylabel("upper-zone depth L_u")
    axes[1].set_ylabel("recovery rate k_r")
    axes[2].set_ylabel("minimum dry time T_r")
    fig.tight_layout()
    return fig


def build(sink):
    import style
    for fid, fn in (("hydrology_ch4_horton_curve", _fig_4_2), ("hydrology_ch4_horton_cumulative", _fig_4_3),
                    ("hydrology_ch4_horton_recovery", _fig_4_4), ("hydrology_ch4_green_ampt_curve", _fig_4_6),
                    ("hydrology_ch4_green_ampt_recovery_params", _fig_4_7)):
        if fid in sink.expected:
            sink.save(fn(style), fid)
