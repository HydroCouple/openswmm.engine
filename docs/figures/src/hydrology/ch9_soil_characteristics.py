"""The four soil-characteristic laws of the mesh aquifer (hydrology
Chapter 9, §9.3): effective saturation and relative conductivity against
suction, at the engine's defaults.

Suction is measured upward from the water table, so the abscissa is also
the height above it: psi = 0 at the table, psi = L at the ground. The
curves are the table of §9.3 evaluated directly. Brooks-Corey is the one
law with a kink — everything is saturated below its air-entry head. Note
the log conductivity axis: at one metre above the table the four laws
disagree by two orders of magnitude, which is why the recharge closure
they feed carries the Experimental caveat for all but Gardner.
Synthetic; no model run.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()

ALPHA = 2.0      # 1/m, Gardner and Russo sorptive number (and van Genuchten)
PSI_B = 0.20     # m, Brooks-Corey air entry
LAMBDA = 0.40    # Brooks-Corey pore-size index
N_VG = 1.6       # van Genuchten n
L_M = 0.5        # Mualem tortuosity exponent


def _gardner(p):
    s = np.exp(-ALPHA * p)
    return s, s


def _russo(p):
    base = (1.0 + 0.5 * ALPHA * p) * np.exp(-0.5 * ALPHA * p)
    return base ** (2.0 / (2.0 + L_M)), base ** 2


def _brooks_corey(p):
    se = np.where(p <= PSI_B, 1.0, (PSI_B / np.maximum(p, 1e-12)) ** LAMBDA)
    kr = np.where(p <= PSI_B, 1.0, (PSI_B / np.maximum(p, 1e-12)) ** (2.0 + 3.0 * LAMBDA))
    return se, kr


def _van_genuchten(p):
    m = 1.0 - 1.0 / N_VG
    se = (1.0 + (ALPHA * p) ** N_VG) ** (-m)
    kr = se ** L_M * (1.0 - (1.0 - se ** (1.0 / m)) ** m) ** 2
    return se, kr


def _draw(style):
    import matplotlib.pyplot as plt
    psi = np.linspace(0.0, 3.0, 900)
    fig, (a0, a1) = plt.subplots(1, 2, figsize=(9.0, 3.8))
    fig.patch.set_facecolor(style.SURFACE)
    style.style_ax(a0)
    style.style_ax(a1)
    laws = [("GARDNER", _gardner, style.COLORS["fv"], "-"),
            ("RUSSO  (default)", _russo, style.COLORS["dw"], "-"),
            ("BROOKS_COREY", _brooks_corey, style.COLORS["fv-lts"], (0, (5, 2))),
            ("VAN_GENUCHTEN", _van_genuchten, style.COLORS["dw-slot"], (0, (2, 1.6)))]
    for name, fn, col, ls in laws:
        se, kr = fn(psi)
        a0.plot(psi, se, color=col, lw=1.7, ls=ls, label=name)
        a1.plot(psi, np.maximum(kr, 1e-8), color=col, lw=1.7, ls=ls)
    a0.axvline(PSI_B, color=style.MUTED, lw=0.8, ls=(0, (1, 2)))
    a0.text(PSI_B + 0.05, 0.06, "air entry ψ_b = 0.2 m", fontsize=7.4, color=style.MUTED)
    a1.axvline(PSI_B, color=style.MUTED, lw=0.8, ls=(0, (1, 2)))
    a0.set_xlabel("suction ψ above the water table  (m)")
    a1.set_xlabel("suction ψ above the water table  (m)")
    a0.set_ylabel("effective saturation  S_e")
    a1.set_ylabel("relative conductivity  K_r = K / K_s")
    a0.set_ylim(0, 1.05)
    a0.set_xlim(0, 3)
    a1.set_xlim(0, 3)
    a1.set_yscale("log")
    a1.set_ylim(1e-6, 1.6)
    a0.set_title("(a) Retention: what the column holds", fontsize=9, loc="left")
    a1.set_title("(b) Conductivity: how fast it moves", fontsize=9, loc="left")
    a0.legend(fontsize=7.4, frameon=False, loc="upper right")
    a1.text(0.08, 2.2e-6, "α = 2 /m   ψ_b = 0.2 m   λ = 0.4   n = 1.6   L_M = 0.5\n"
                          "θ(ψ) = θ_r + S_e (θ_s − θ_r);  K(ψ) = K_s K_r(ψ)",
            fontsize=7.2, color=style.MUTED, linespacing=1.4)
    fig.tight_layout()
    return fig


def build(sink):
    import style
    sink.save(_draw(style), "hydrology_ch9_soil_characteristics")
