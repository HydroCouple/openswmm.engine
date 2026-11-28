"""The regime-aware 1D-2D exchange law, COUPLING_LAW WEIR_ORIFICE
(hydraulics Chapter 9, §9.7.1 "Regime-aware exchange and fine-scale ponding").

One default manhole, D = 1.2 m, so A = pi D^2 / 4 = 1.131 m^2 and
L = pi D = 3.770 m (src/engine/2d/coupling/ExchangeStructure.cpp:126-130).

(a) Free drainage (network head below the crest): discharge against the
    surface depth h_s over the crest. The free weir (2/3) Cw L sqrt(2g)
    h_s phi(h_s) governs shallow water, the orifice Coi A sqrt(2g) phi(h_s)
    deep water, and the p-norm minimum (Qw^-16 + Qo^-16)^(-1/16) follows the
    smaller of the two, 2^(-1/16) = 0.958 of it where they cross.
(b) Submergence and reversal: the surface held 0.30 m over the crest while
    the network head rises from below the crest to above the surface, so
    h2 - h1 runs from positive (drainage) through zero to negative (spill).
    DEFAULT (Csw = 2/3 Cw = 0.360) against RUBINATO2017 (Csw = 0.056); the
    free/submerged blend over the submergence ratio 0.1 is shaded.
(c) The regularized root phi of the law against sqrt(x): equal above
    eps = 0.02 m, a quadratic with finite slope 1.5 / sqrt(eps) below.

Every curve is the engine's own law, ported line for line from
src/engine/2d/coupling/ExchangeLaw.hpp:84-91 (phi), :112-115 (smoothstep),
:129-164 (softmin, p = 16), :187-224 (exchange), with kGravity = 9.80665 and
kSigmaBand = 0.1 (:62-63) and the default h_eps = 0.02 m (:79); the presets
are src/engine/2d/coupling/ExchangeStructure.cpp:78-105, where DEFAULT sets
Csw = 2/3 Cw (:113-114). The check numbers printed by build() are the
unit test's anchors (tests/unit/engine/test_2d_exchange_law.cpp:222-244):
the controls cross at h_s = 0.116 D, where the blend is 2^(-1/16) of the
limiting control.

Synthetic tier: numpy only, no model run.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()

G = 9.80665          # ExchangeLaw.hpp:62
SIGMA_BAND = 0.1     # ExchangeLaw.hpp:63
EPS = 0.02           # ExchangeLaw.hpp:79, the routed default
D = 1.2              # ExchangeStructure.cpp:126, default manhole (SI)
AREA = np.pi * D * D / 4.0
PERIM = np.pi * D
PRESETS = {          # ExchangeStructure.cpp:78-105; DEFAULT Csw = 2/3 Cw (:113-114)
    "DEFAULT": dict(cw=0.540, csw=0.540 * 2.0 / 3.0, coi=0.167, coo=0.160),
    "RUBINATO2017": dict(cw=0.540, csw=0.056, coi=0.167, coo=0.160),
}


def phi(x, eps=EPS):
    """ExchangeLaw.hpp:84-91: sqrt(x) at and above eps, a C1 quadratic below, 0 for x <= 0."""
    x = np.asarray(x, dtype=float)
    inv = 1.0 / np.sqrt(eps)
    low = 1.5 * inv * x - 0.5 * inv / eps * x * x
    return np.where(x > 0.0, np.where(x >= eps, np.sqrt(np.maximum(x, 0.0)), low), 0.0)


def smoothstep(t):
    t = np.clip(t, 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def softmin(a, b):
    """ExchangeLaw.hpp:129-164: (a^-16 + b^-16)^(-1/16), zero unless both are positive."""
    a, b = np.asarray(a, float), np.asarray(b, float)
    lo, hi = np.minimum(a, b), np.maximum(a, b)
    ok = (a > 0.0) & (b > 0.0)
    r = np.where(ok, lo / np.where(ok, hi, 1.0), 0.0)
    return np.where(ok, lo / (1.0 + r ** 16) ** (1.0 / 16.0), 0.0)


def exchange(c, h2, h1, z=0.0, parts=False):
    """ExchangeLaw.hpp:187-224; Q > 0 drains the surface into the network."""
    s2g = np.sqrt(2.0 * G)
    h2, h1 = np.broadcast_arrays(np.asarray(h2, float), np.asarray(h1, float))
    q = np.zeros(h2.shape)
    out = {k: np.full(h2.shape, np.nan) for k in ("weir", "orif", "weir_free", "weir_sub")}
    hs = h2 - z
    drain = (h2 > h1) & (hs > 0.0)
    hs_safe = np.where(drain, hs, 1.0)
    S = np.where(h1 > z, smoothstep((h1 - z) / (SIGMA_BAND * hs_safe)), 0.0)
    dh = h2 - h1
    wf = (2.0 / 3.0) * c["cw"] * PERIM * s2g * hs * phi(hs)
    ws = c["csw"] * PERIM * s2g * hs * phi(dh)
    of = c["coi"] * AREA * s2g * phi(hs)
    os_ = c["coi"] * AREA * s2g * phi(dh)
    qw = (1.0 - S) * wf + S * ws
    qo = (1.0 - S) * of + S * os_
    q = np.where(drain, softmin(qw, qo), q)
    for k, v in (("weir", qw), ("orif", qo), ("weir_free", wf), ("weir_sub", ws)):
        out[k] = np.where(drain, v, np.nan)
    spill = (h1 > h2) & (h1 > z)
    head_dry = np.where(spill, h1 - z, 1.0)
    So = np.where(h2 > z, smoothstep((h2 - z) / (SIGMA_BAND * head_dry)), 0.0)
    k = c["coo"] * AREA * s2g
    q = np.where(spill, -k * ((1.0 - So) * phi(h1 - z) + So * phi(h1 - h2)), q)
    return (q, out) if parts else q


def _free_panel(ax, style):
    c = PRESETS["DEFAULT"]
    hs = np.linspace(0.0, 0.6, 1201)
    q, p = exchange(c, hs, -1.0, parts=True)
    cross = 1.5 * c["coi"] * AREA / (c["cw"] * PERIM)
    ax.plot(hs, p["weir_free"], color=style.COLORS["dw"], lw=1.3, ls=(0, (4, 2)),
            label="free weir  (2/3) Cw L √(2g) hs φ(hs)")
    ax.plot(hs, p["orif"], color=style.COLORS["dw-slot"], lw=1.3, ls=(0, (4, 2)),
            label="orifice  Coi A √(2g) φ(hs)")
    ax.plot(hs, q, color=style.COLORS["fv"], lw=2.2, label="exchange: p-norm minimum, p = 16")
    qc = float(exchange(c, cross, -1.0))
    ax.plot([cross], [qc], marker="o", ms=5, color=style.INK, zorder=5)
    ax.annotate(f"controls cross at hs = {cross:.3f} m (0.116 D);\n"
                f"the blend is {qc / (c['coi'] * AREA * np.sqrt(2 * G) * np.sqrt(cross)):.3f} "
                f"of either there",
                xy=(cross, qc), xytext=(0.215, 0.13), fontsize=7.2, color=style.INK,
                arrowprops=dict(arrowstyle="-", color=style.MUTED, lw=0.8))
    ax.set_xlim(0.0, 0.6)
    ax.set_ylim(0.0, 1.2)
    ax.set_xlabel("surface depth over the crest, hs (m)")
    ax.set_ylabel("drainage into the network, Q (m³/s)")
    ax.set_title("(a) Free drainage (network below the crest)", loc="left", fontsize=9)
    ax.legend(loc="upper left", fontsize=7.2, handlelength=2.6)


def _submerged_panel(ax, style):
    hs0 = 0.30
    h1 = np.linspace(-0.30, 0.60, 1801)
    x = hs0 - h1                       # h2 - h1, with the crest at 0
    colors = {"DEFAULT": style.COLORS["fv"], "RUBINATO2017": style.COLORS["fv-lts"]}
    for name, c in PRESETS.items():
        q, p = exchange(c, hs0, h1, parts=True)
        ax.plot(x, q, color=colors[name], lw=2.0,
                label=f"{name}  (Csw = {c['csw']:.3f})")
        if name == "DEFAULT":
            ax.plot(x, p["orif"], color=style.COLORS["dw-slot"], lw=1.0, ls=(0, (4, 2)),
                    label="orifice term (both presets)")
        ax.plot(x, p["weir"], color=colors[name], lw=1.0, ls=(0, (1.5, 1.5)), alpha=0.9,
                label=f"weir term, {name}")
    # the free/submerged blend: network head from the crest to 0.1 hs above it
    ax.axvspan(hs0 - SIGMA_BAND * hs0, hs0, color=style.GRIDC, alpha=0.8, lw=0)
    ax.axvline(hs0, color=style.MUTED, lw=0.8)
    ax.axvline(0.0, color=style.MUTED, lw=0.8)
    ax.axhline(0.0, color=style.MUTED, lw=0.6)
    ax.text(hs0 + 0.012, 0.63, "network head\nat the crest", fontsize=7.0, color=style.MUTED, va="top")
    ax.text(hs0 - SIGMA_BAND * hs0 - 0.008, 0.075, "blend band\n(shaded)", fontsize=7.0,
            color=style.MUTED, ha="right", va="top")
    ax.annotate("spill onto the surface,\nCoo A √(2g) φ(h1 − h2)", xy=(-0.20, -0.355),
                xytext=(-0.29, 0.62), fontsize=7.0, color=style.MUTED, va="top",
                arrowprops=dict(arrowstyle="-", color=style.MUTED, lw=0.8))
    ax.set_xlim(-0.30, 0.62)
    ax.set_ylim(-0.48, 0.66)
    ax.set_xlabel("head difference h2 − h1 (m), surface held 0.30 m over the crest")
    ax.set_ylabel("Q (m³/s), positive into the network")
    ax.set_title("(b) Submerged drainage and spill", loc="left", fontsize=9)
    ax.legend(loc="lower right", fontsize=7.0, handlelength=2.4)


def _phi_panel(ax, style):
    x = np.linspace(0.0, 0.06, 601)
    ax.plot(x, np.sqrt(x), color=style.MUTED, lw=1.2, ls=(0, (4, 2)), label="√x")
    ax.plot(x, phi(x), color=style.COLORS["fv"], lw=2.0, label="φ(x), ε = 0.02 m")
    s0 = 1.5 / np.sqrt(EPS)
    xs = np.linspace(0.0, 0.012, 20)
    ax.plot(xs, s0 * xs, color=style.INK, lw=0.8)
    ax.text(0.0015, 0.215, f"slope at 0:\n1.5/√ε = {s0:.1f} m$^{{-1/2}}$", fontsize=7.2,
            color=style.INK, va="center")
    ax.axvline(EPS, color=style.MUTED, lw=0.8)
    ax.text(EPS + 0.0012, 0.235, "ε", fontsize=8.5, color=style.MUTED)
    ax.set_xlim(0.0, 0.06)
    ax.set_ylim(0.0, 0.26)
    ax.set_xlabel("head x (m)")
    ax.set_ylabel("φ(x) (m$^{1/2}$)")
    ax.set_title("(c) The regularized root φ", loc="left", fontsize=9)
    ax.legend(loc="lower right", fontsize=7.2)


def build(sink):
    import matplotlib.pyplot as plt
    import style
    fig = style.figure(10.0, 4.3)
    gs = fig.add_gridspec(1, 3, width_ratios=[1.12, 1.25, 0.78], wspace=0.34)
    axes = [fig.add_subplot(gs[0, k]) for k in range(3)]
    for ax in axes:
        style.style_ax(ax)
    _free_panel(axes[0], style)
    _submerged_panel(axes[1], style)
    _phi_panel(axes[2], style)
    c = PRESETS["DEFAULT"]
    cross = 1.5 * c["coi"] * AREA / (c["cw"] * PERIM)
    qc = float(exchange(c, cross, -1.0))
    lim = c["coi"] * AREA * np.sqrt(2 * G) * np.sqrt(cross)
    print(f"    manhole D {D} m: A {AREA:.3f} m2, L {PERIM:.3f} m; controls cross at "
          f"hs {cross:.4f} m = {cross / D:.4f} D; blend/limit {qc / lim:.6f} "
          f"(2^-1/16 = {2 ** (-1 / 16):.6f})")
    for name, cc in PRESETS.items():
        print(f"    {name}: Q(hs 0.30, h1 = crest + 0.15) = {float(exchange(cc, 0.30, 0.15)):.4f} m3/s; "
              f"spill Q(h2 = 0.30, h1 = 0.50) = {float(exchange(cc, 0.30, 0.50)):.4f} m3/s")
    plt.subplots_adjust(left=0.07, right=0.99, top=0.90, bottom=0.13)
    sink.save(fig, "hydraulics_ch9_weir_orifice_exchange")
