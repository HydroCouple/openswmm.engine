"""The reaction species itself, along the transport demonstration line
(Application Manual, the multi-species-reactions chapter).

The native results file carries every reaction species as a named column
after the pollutants, water age and temperature, and `[REACTION_REPORT]`
selects a subset (src/engine/core/SimulationContext.hpp:731-745; an empty
selection means all species, src/engine/data/ReactionData.hpp:197-199;
the section is parsed at
src/engine/transport/components/ReactionModule/ReactionsComponent.cpp:851-889).
The figure reads `CL2` back with OutputReader:

* (a) the concentration at each node as the line fills from empty;
* (b) the steady concentration at each node against the travel time from
  the inflow, with the heat component on (the computed water temperature)
  and with `HEAT_TRANSPORT OFF` (the `[REACTION_OPTIONS] TEMPERATURE`
  fallback), drawn over the first-order solution of the .rxn expression,
  C = C_in exp(-kb theta^(T-20) tau). kb, theta, the fallback temperature
  and the inflow's source age are read from the committed sidecar files.

The committed deck doses nothing: its only CL2 is a `[REACTION_QUALITY]`
initial value in an empty network, so every node reads about zero. The run
behind the figure therefore derives its .rxn in code from the committed one
by appending one `[REACTION_SOURCES]` row, `CONC J1 CL2 2.0`, which
multiplies the concentration by the external inflow at J1
(ReactionsComponent.cpp:723-777), and the two-row `[REACTION_REPORT]` block
the chapter quotes. Nothing else changes.

Simulated tier: two runs of docs/figures/decks/transport_demo (heat on and
HEAT_TRANSPORT OFF), cached under docs/figures/cache.
"""
from __future__ import annotations

import re

import numpy as np

REQUIRES = ("openswmm",)

DECK = "transport_demo/transport_demo.inp"
RXN = "transport_demo/transport_demo.rxn"
AGE = "transport_demo/transport_demo.age"
LINE = ["J1", "J2", "J3", "J4", "J5"]
SPECIES = "CL2"
DOSE_NODE = "J1"
DOSE = 2.0                                       # mg/L on the external inflow
FINE = {"REPORT_STEP": "0:01:00"}


def _section(text, name):
    m = re.search(r"^\[" + name + r"\]\s*$(.*?)(?=^\[|\Z)", text, re.M | re.S)
    if not m:
        return []
    return [ln.split(";")[0].split() for ln in m.group(1).splitlines() if ln.split(";")[0].strip()]


def _rxn_variant(rxn):
    """The committed .rxn plus one CONC source and the chapter's [REACTION_REPORT] block."""
    extra = ""
    if not _section(rxn, "REACTION_SOURCES"):
        extra += f"\n[REACTION_SOURCES]\nCONC  {DOSE_NODE}  {SPECIES}  {DOSE}\n"
    if not _section(rxn, "REACTION_REPORT"):
        extra += f"\n[REACTION_REPORT]\nSPECIES ALL NO\nSPECIES {SPECIES} YES\n"
    return rxn.rstrip("\n") + "\n" + extra


def _constants(rxn, age):
    coef = {r[1]: float(r[2]) for r in _section(rxn, "REACTION_COEFFICIENTS") if len(r) >= 3}
    opts = {r[0].upper(): r[1] for r in _section(rxn, "REACTION_OPTIONS") if len(r) >= 2}
    src = {r[0].upper(): float(r[2]) for r in _section(age, "WATER_AGE_SOURCES") if len(r) >= 3}
    if opts.get("RATE_UNITS", "HR").upper() != "HR":
        raise RuntimeError("the closed form below assumes RATE_UNITS HR")
    return coef["kb"], coef["theta"], float(opts.get("TEMPERATURE", 20.0)), src["EXTERNAL_INFLOW"]


def _read(sink, options, key, sidecars):
    from openswmm.engine import OutNodeVar, OutputReader
    r = sink.run(DECK, options, key, sidecars=sidecars)
    with OutputReader(r.out) as o:
        ids = list(o.pollutant_ids)
        if SPECIES not in ids:
            raise RuntimeError(f"{SPECIES} is not reported by this run: {ids}")
        units = o.pollutant_units[ids.index(SPECIES)]
        base = int(OutNodeVar.POLLUT_BASE)
        t = np.asarray(o.period_times, dtype="datetime64[ms]")
        start = np.datetime64(o.start_datetime, "ms")
        minutes = (t - start).astype(np.float64) / 60000.0

        def series(name):
            j = ids.index(name)
            return {nd: np.asarray([o.node_attributes(nd, period=k)[base + j]
                                    for k in range(o.period_count)], float) for nd in LINE}
        out = {"t": minutes, "c": series(SPECIES), "age": series("__WATER_AGE__"),
               "temp": series("__TEMPERATURE__") if "__TEMPERATURE__" in ids else None,
               "units": units, "ids": ids}
    return out


def _draw(style, on, off, kb, theta, t_fallback, age_in):
    import matplotlib.pyplot as plt
    fig, (a0, a1) = plt.subplots(1, 2, figsize=(9.4, 4.0))
    fig.patch.set_facecolor(style.SURFACE)
    style.style_ax(a0)
    style.style_ax(a1)
    cols = [style.COLORS["fv"], style.COLORS["dw"], style.COLORS["dw-slot"], style.COLORS["fv-lts"],
            style.COLORS["dw-legacy"]]
    units = on["units"]

    # (a) the fill transient
    m = on["t"] <= 30.0
    for nd, c in zip(LINE, cols):
        a0.plot(on["t"][m], on["c"][nd][m], color=c, lw=1.6, label=nd)
    a0.axhline(DOSE, color=style.INK, lw=0.9, ls=(0, (1, 2)))
    a0.text(29.5, DOSE + 0.03, f"dose on the inflow at {DOSE_NODE}, {DOSE:.1f} {units}",
            fontsize=7.2, color=style.INK, ha="right", va="bottom")
    a0.set_xlim(0, 30)
    a0.set_ylim(0, DOSE * 1.12)
    a0.set_xlabel("time  (minutes)")
    a0.set_ylabel(f"{SPECIES}  ({units})")
    a0.set_title(f"(a) {SPECIES} at each node as the line fills", fontsize=9, loc="left")
    a0.legend(fontsize=7.2, frameon=False, ncol=5, loc="lower right")

    # (b) steady concentration against travel time, against the closed form
    tau_on = np.array([on["age"][nd][-1] - age_in for nd in LINE])          # hours
    tau_off = np.array([off["age"][nd][-1] - age_in for nd in LINE])
    c_on = np.array([on["c"][nd][-1] for nd in LINE])
    c_off = np.array([off["c"][nd][-1] for nd in LINE])
    t_water = np.array([on["temp"][nd][-1] for nd in LINE])
    tau = np.linspace(0.0, max(tau_on.max(), tau_off.max()) * 1.08, 200)
    t_mean = float(t_water.mean())
    for T, col, lab in ((t_mean, style.COLORS["fv"], f"closed form at {t_mean:.0f} °C"),
                        (t_fallback, style.COLORS["fv-lts"], f"closed form at {t_fallback:.0f} °C")):
        a1.plot(tau * 60.0, DOSE * np.exp(-kb * theta ** (T - 20.0) * tau), color=col, lw=1.2,
                ls=(0, (4, 2)), label=lab)
    a1.plot(tau_on * 60.0, c_on, "o", ms=6.5, mfc=style.COLORS["fv"], mec=style.INK, mew=0.6,
            label="engine, heat on (computed TEMP)")
    a1.plot(tau_off * 60.0, c_off, "s", ms=6.0, mfc="none", mec=style.COLORS["fv-lts"], mew=1.3,
            label="engine, HEAT_TRANSPORT OFF")
    for nd, x, y in zip(LINE, tau_on * 60.0, c_on):
        a1.text(x, y + 0.0016, nd, fontsize=7.2, color=style.INK, ha="center", va="bottom")
    pred_on = DOSE * np.exp(-kb * theta ** (t_water - 20.0) * tau_on)
    pred_off = DOSE * np.exp(-kb * theta ** (t_fallback - 20.0) * tau_off)
    err = max(np.abs(c_on - pred_on).max(), np.abs(c_off - pred_off).max())
    a1.set_xlabel("travel time from the inflow, water age − source age  (minutes)")
    a1.set_ylabel(f"steady {SPECIES}  ({units})")
    lo = min(c_on.min(), c_off.min())
    a1.set_ylim(lo - 0.6 * (DOSE - lo), DOSE + 0.15 * (DOSE - lo))
    a1.set_xlim(-0.2, tau[-1] * 60.0)
    a1.set_title(f"(b) Steady {SPECIES} against the .rxn expression", fontsize=9, loc="left")
    a1.legend(fontsize=7.2, frameon=False, loc="lower left")
    a1.text(0.98, 0.97, f"kb = {kb:.2f} /hr, θ = {theta:.2f}\n"
            f"largest engine − closed-form gap\n{err:.1e} {units}",
            transform=a1.transAxes, fontsize=7.2, color=style.MUTED, ha="right", va="top",
            linespacing=1.45)
    fig.tight_layout()
    return fig, dict(tau_on=tau_on, c_on=c_on, c_off=c_off, t_water=t_water, err=err)


def build(sink):
    import style
    rxn = (sink.fig_dir / "decks" / RXN).read_text()
    age = (sink.fig_dir / "decks" / AGE).read_text()
    kb, theta, t_fallback, age_in = _constants(rxn, age)
    sidecars = {"transport_demo.rxn": _rxn_variant(rxn)}
    on = _read(sink, dict(FINE), "app_rx_dosed", sidecars)
    off = _read(sink, dict(FINE, HEAT_TRANSPORT="OFF"), "app_rx_dosed_heatoff", sidecars)
    fig, k = _draw(style, on, off, kb, theta, t_fallback, age_in)
    print("    reported columns: " + ", ".join(on["ids"]))
    print("    steady CL2 (mg/L) heat on: " + ", ".join(f"{nd} {c:.5f}" for nd, c in zip(LINE, k["c_on"])))
    print("    steady CL2 (mg/L) heat off: " + ", ".join(f"{nd} {c:.5f}" for nd, c in zip(LINE, k["c_off"])))
    print(f"    travel time to J5 {k['tau_on'][-1] * 60:.2f} min; water temperature {k['t_water'].min():.2f}"
          f"-{k['t_water'].max():.2f} degC; largest gap to the closed form {k['err']:.2e} mg/L")
    sink.save(fig, "workflow_ch9_reaction_species")
