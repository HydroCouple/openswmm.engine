"""The aquifer beneath the meshed patch (Application Manual, the
mesh-hydrology chapter).

The `GROUNDWATER` group of `REPORT_2D_VARIABLES` is part of `DEFAULT`
(src/engine/2d/data/SolverOptions2D.hpp:229-239), so a run with a resolved
`[2D_AQUIFER]` writes the per-cell aquifer fields, the domain ledger and the
per-bed cumulative node exchange to its 2D results file
(src/engine/2d/output/Default2DOutputPlugin.cpp:1654-1667 fields,
:1820 `groundwater_ledger`, :1835 `groundwater_node_exchange_cum`). The
figure reads three of them back:

* (a) `Mesh2_face_gw_table_elev`, the water-table elevation of every cell;
* (b) `Mesh2_face_gw_recharge`, the recharge from the unsaturated column
  to the table (positive down, negative capillary rise);
* (c) the cumulative domain volumes from `groundwater_ledger` (`infil_in`,
  `recharge`, `storage - init_storage`) and the bed's
  `groundwater_node_exchange_cum`.

The deck places `J1` on the drain vertex in `[COORDINATES]`, because
`NODE_ENROLMENT AUTO` enrolls only nodes whose coordinates fall in a cell
(src/engine/2d/subsurface/SubsurfaceData.hpp:180-185); that row gives the
aquifer its one node bed. The module refuses a run whose report does not
say a node was enrolled.

Simulated tier: one run of docs/figures/decks/mesh_hydrology as committed
(the same run as Figure 7-1, cache key `base`), cached under
docs/figures/cache. Needs h5py as well as the openswmm package.
"""
from __future__ import annotations

import re

import numpy as np

REQUIRES = ("openswmm", "h5py")

DECK = "mesh_hydrology/mesh_hydrology.inp"


def _section(text, name):
    m = re.search(r"^\[" + name + r"\]\s*$(.*?)(?=^\[|\Z)", text, re.M | re.S)
    if not m:
        return []
    return [ln.split(";")[0].split() for ln in m.group(1).splitlines() if ln.split(";")[0].strip()]


def _drain(deck):
    """The node the drain vertex couples to, the vertex, and the node's [COORDINATES]."""
    vertex, node = _section(deck, "2D_VERTEX_NODE_MAP")[0][:2]
    xy = next(((float(r[1]), float(r[2])) for r in _section(deck, "COORDINATES") if r[0] == node), None)
    return node, int(vertex), xy


def _hours(h5):
    t = np.asarray(h5["time"], float)
    return (t - t[0]) * 24.0


def _draw(style, h5, node, vertex):
    import matplotlib.pyplot as plt
    t = _hours(h5)
    table = np.asarray(h5["Mesh2_face_gw_table_elev"])
    recharge = np.asarray(h5["Mesh2_face_gw_recharge"]) * 1000.0 * 3600.0     # mm/hr, + down
    exch = np.asarray(h5["Mesh2_face_gw_node_exchange"])
    bed_cell = int(np.argmax(np.abs(exch).max(axis=0)))
    terms = h5["groundwater_ledger"].attrs["terms"]
    terms = (terms.decode() if isinstance(terms, bytes) else str(terms)).split(",")
    led = np.asarray(h5["groundwater_ledger"])
    col = {name: led[:, i] for i, name in enumerate(terms)}
    bed_cum = np.asarray(h5["groundwater_node_exchange_cum"])[:, 0]

    fig, (a0, a1, a2) = plt.subplots(1, 3, figsize=(10.0, 3.9))
    fig.patch.set_facecolor(style.SURFACE)
    for ax in (a0, a1, a2):
        style.style_ax(ax)
        ax.axvspan(0.0, 1.0, color="#eef4fb", zorder=0)
        ax.set_xlim(0.0, t[-1])
        ax.set_xlabel("time  (hours)")
    hot, cold = style.COLORS["fv-lts"], style.MUTED

    # (a) water table of every cell
    for i in range(table.shape[1]):
        if i != bed_cell:
            a0.plot(t, table[:, i], color=cold, lw=1.0)
    a0.plot(t, table[:, bed_cell], color=hot, lw=2.0)
    a0.text(0.06, table[-1, bed_cell] + 0.10, f"cell {bed_cell}, which\nholds {node}'s bed",
            fontsize=7.2, color=hot, ha="left", va="top", linespacing=1.35)
    a0.text(t[-1] * 0.97, table[-1, :].max() + 0.03, "the other seven cells",
            fontsize=7.2, color=cold, ha="right", va="bottom")
    a0.set_ylabel("water-table elevation  (m)")
    a0.set_title("(a) Water table", fontsize=9, loc="left")
    lo, hi = table.min(), table.max()
    a0.set_ylim(lo - 0.12 * (hi - lo), hi + 0.22 * (hi - lo))
    a0.text(0.5, a0.get_ylim()[0], "rain", fontsize=7.2, color=style.WATER, ha="center", va="bottom")

    # (b) recharge of every cell
    for i in range(recharge.shape[1]):
        if i != bed_cell:
            a1.plot(t, recharge[:, i], color=cold, lw=1.0)
    a1.plot(t, recharge[:, bed_cell], color=hot, lw=2.0)
    a1.axhline(0.0, color=style.INK, lw=0.7)
    a1.set_ylabel("recharge to the table  (mm/hr)")
    a1.set_title("(b) Recharge, positive down", fontsize=9, loc="left")
    others = np.delete(recharge, bed_cell, axis=1)
    a1.text(0.06, recharge[:, bed_cell].max() * 0.97, f"cell {bed_cell}: the column\n"
            "drains to the falling table", fontsize=7.2, color=hot, ha="left", va="top",
            linespacing=1.35)
    a1.text(t[-1] * 0.97, recharge[:, bed_cell].max() * 0.16,
            f"other cells: {others.min():+.2f} to {others.max():+.2f} mm/hr",
            fontsize=7.2, color=cold, ha="right", va="bottom")

    # (c) the domain volumes
    infil = col["infil_in"]
    dstore = col["storage"] - col["init_storage"]
    a2.plot(t, infil, color=style.WATER, lw=1.8, label="infiltration into the aquifer")
    a2.plot(t, bed_cum, color=hot, lw=1.8, label=f"exchange out to {node}")
    a2.plot(t, dstore, color=style.INK, lw=1.4, ls=(0, (4, 2)), label="change in aquifer storage")
    a2.plot(t, col["recharge"], color=style.COLORS["dw"], lw=1.4, label="recharge (internal)")
    a2.set_ylabel("cumulative volume  (m³)")
    a2.set_title("(c) The aquifer's ledger", fontsize=9, loc="left")
    a2.legend(fontsize=7.2, frameon=False, loc="upper left")
    resid = float(np.abs(col["continuity_residual"]).max())
    a2.text(t[-1] * 0.97, 0.03 * infil.max(), f"largest continuity residual\n{resid:.1e} m³",
            fontsize=7.2, color=style.MUTED, ha="right", va="bottom", linespacing=1.35)
    a2.set_ylim(0.0, infil.max() * 1.42)
    fig.tight_layout(w_pad=1.6)
    nums = dict(bed_cell=bed_cell, table0=table[0, bed_cell], table1=table[-1, bed_cell],
                others=float(np.abs(np.delete(table[-1] - table[0], bed_cell)).max()),
                rmax=float(recharge[:, bed_cell].max()), infil=float(infil[-1]), bed=float(bed_cum[-1]),
                dstore=float(dstore[-1]), recharge=float(col["recharge"][-1]),
                caprise=float(col["caprise"][-1]), resid=resid, omin=float(others.min()),
                omax=float(others.max()))
    return fig, nums


def build(sink):
    import h5py
    import style
    deck = (sink.fig_dir / "decks" / DECK).read_text()
    node, vertex, xy = _drain(deck)
    if xy is None:
        raise RuntimeError(f"{node} has no [COORDINATES]; the aquifer would have no bed")
    x, y = xy
    run = sink.run(DECK, None, "base")
    rpt = run.rpt.read_text(errors="replace")
    if "enrolled in the node <-> aquifer exchange" not in rpt:
        raise RuntimeError(f"{node} was not enrolled in the node-aquifer exchange")
    with h5py.File(run.h5, "r") as h5:
        fig, k = _draw(style, h5, node, vertex)
    print(f"    {node} located at vertex {vertex} ({x:.1f}, {y:.1f}); its bed is in cell {k['bed_cell']}")
    print(f"    water table in cell {k['bed_cell']}: {k['table0']:.3f} -> {k['table1']:.3f} m; "
          f"other cells move at most {k['others'] * 1000:.1f} mm")
    print(f"    recharge in cell {k['bed_cell']} peaks at {k['rmax']:.2f} mm/hr; other cells "
          f"{k['omin']:+.3f} to {k['omax']:+.3f} mm/hr")
    print(f"    ledger (m3): infiltration in {k['infil']:.3f}, exchange to {node} {k['bed']:.3f}, "
          f"storage change {k['dstore']:.3f}, recharge {k['recharge']:.3f}, capillary rise "
          f"{k['caprise']:.3f}, largest residual {k['resid']:.1e}")
    sink.save(fig, "workflow_ch7_mesh_groundwater")
