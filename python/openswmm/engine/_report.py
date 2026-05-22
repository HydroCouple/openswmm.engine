"""
Report Snapshot
===============

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Provides :func:`get_report_snapshot` — a single call that assembles the
programmatic equivalent of the SWMM ``.rpt`` file into structured Python
dataclasses.

Example::

    from openswmm.engine import Solver, get_report_snapshot

    with Solver("model.inp", "model.rpt", "model.out") as s:
        s.start()
        while s.step() == 0:
            pass
        s.end()
        report = get_report_snapshot(s)

        # Routing convergence
        diag = report.routing_diagnostics
        print(f"Steps not converged: {diag.n_steps_not_converged} "
              f"({diag.pct_not_converged:.1f}%)")
        print(f"Max Courant number: {diag.max_courant:.3f}")

        # Node flooding table
        for entry in report.node_flooding:
            print(f"{entry.node_id}: {entry.total_flood_volume:.1f} vol flooded")

        # Pump summary
        for pump in report.pump_summary:
            print(f"{pump.link_id}: {pump.num_startups} starts, "
                  f"{pump.pct_time_on:.1f}% on-time")
"""

from __future__ import annotations

from dataclasses import dataclass


# =============================================================================
# Dataclasses — one per .rpt section
# =============================================================================


@dataclass
class RoutingDiagnostics:
    """Routing Time Step Summary section of the .rpt file.

    Exposes convergence metrics from the dynamic wave solver.

    @ivar avg_time_step: Average routing time step (seconds).
    @ivar min_time_step: Minimum routing time step (seconds).
    @ivar max_time_step: Maximum routing time step (seconds).
    @ivar n_steps: Total number of routing steps taken.
    @ivar pct_not_converged: Percentage of steps where the solver did not converge.
    @ivar n_steps_not_converged: Absolute count of non-converging steps
        (derived as C{round(n_steps * pct_not_converged / 100)}).
    @ivar avg_iterations: Average Newton iterations per routing step.
    @ivar max_courant: Maximum Courant number encountered during the run.
    """

    avg_time_step: float
    min_time_step: float
    max_time_step: float
    n_steps: int
    pct_not_converged: float
    n_steps_not_converged: int
    avg_iterations: float
    max_courant: float


@dataclass
class RunoffContinuity:
    """Runoff Quantity Continuity table from the .rpt file.

    All volume values are in the project's volume units (e.g. acre-feet or
    megalitres depending on the flow-unit system).

    @ivar continuity_error_pct: Runoff continuity error expressed as a
        percentage of total inflow.
    """

    continuity_error_pct: float
    total_rainfall: float
    total_evaporation: float
    total_infiltration: float
    total_runoff: float
    total_snow_removal: float
    initial_storage: float
    final_storage: float


@dataclass
class RoutingContinuity:
    """Flow Routing Continuity table from the .rpt file.

    @ivar continuity_error_pct: Routing continuity error expressed as a
        percentage of total inflow.
    """

    continuity_error_pct: float
    dry_weather_inflow: float
    wet_weather_inflow: float
    groundwater_inflow: float
    rdii_inflow: float
    external_inflow: float
    total_flooding: float
    total_outflow: float
    evaporation_loss: float
    seepage_loss: float
    initial_storage: float
    final_storage: float


@dataclass
class QualityContinuity:
    """Quality Routing Continuity entry for a single pollutant.

    @ivar pollutant_id: Pollutant identifier string.
    @ivar continuity_error_pct: Quality continuity error as a percentage.
    @ivar seep_loss: Total mass lost via seepage.
    @ivar evap_loss: Total mass lost via evaporation.
    """

    pollutant_id: str
    continuity_error_pct: float
    seep_loss: float
    evap_loss: float


@dataclass
class NodeFloodingEntry:
    """One row of the Node Flooding Summary table.

    @ivar node_id: Node identifier.
    @ivar node_type: Node type name (JUNCTION, OUTFALL, STORAGE, DIVIDER).
    @ivar max_depth: Maximum water depth at this node (project length units).
    @ivar max_overflow_rate: Peak overflow / flooding rate (project flow units).
    @ivar total_flood_volume: Total flooded volume over the simulation.
    @ivar time_flooded: Total time the node was flooded (hours).
    """

    node_id: str
    node_type: str
    max_depth: float
    max_overflow_rate: float
    total_flood_volume: float
    time_flooded: float


@dataclass
class LinkFlowEntry:
    """One row of the Link Flow Summary table.

    @ivar link_id: Link identifier.
    @ivar link_type: Link type name (CONDUIT, PUMP, ORIFICE, WEIR, OUTLET).
    @ivar max_flow: Peak flow rate (project flow units).
    @ivar max_velocity: Peak flow velocity (project length / time).
    @ivar max_filling: Maximum depth-to-full-depth ratio (0–1 for non-
        pressurised; > 1 when surcharged).
    @ivar total_volume: Total volume routed through the link.
    @ivar surcharge_time: Total time the link was surcharged (hours).
    """

    link_id: str
    link_type: str
    max_flow: float
    max_velocity: float
    max_filling: float
    total_volume: float
    surcharge_time: float


@dataclass
class PumpEntry:
    """One row of the Pump Summary table.

    @ivar link_id: Link (pump) identifier.
    @ivar pump_curve_idx: Index of the pump curve used (-1 = ideal pump).
    @ivar num_startups: Total number of on-cycles during the simulation.
    @ivar total_on_time: Cumulative on-time (seconds).
    @ivar total_volume: Total volume pumped over the simulation.
    @ivar pct_time_on: Percentage of simulation duration the pump was on.
    """

    link_id: str
    pump_curve_idx: int
    num_startups: int
    total_on_time: float
    total_volume: float
    pct_time_on: float


@dataclass
class SubcatchmentEntry:
    """One row of the Subcatchment Runoff Summary table.

    @ivar subcatch_id: Subcatchment identifier.
    @ivar total_precip: Total precipitation depth / volume over the simulation.
    @ivar total_runoff_vol: Total runoff volume generated.
    @ivar max_runoff_rate: Peak runoff rate (project flow units).
    @ivar runoff_coefficient: Ratio of runoff to precipitation
        (C{total_runoff_vol / total_precip}); 0 when precipitation is zero.
    """

    subcatch_id: str
    total_precip: float
    total_runoff_vol: float
    max_runoff_rate: float
    runoff_coefficient: float


@dataclass
class ReportSnapshot:
    """Full programmatic equivalent of the SWMM ``.rpt`` report file.

    Sections map directly to the corresponding tables in the text report:

    * :attr:`routing_diagnostics` → Routing Time Step Summary
    * :attr:`runoff_continuity`   → Runoff Quantity Continuity
    * :attr:`routing_continuity`  → Flow Routing Continuity
    * :attr:`quality_continuity`  → Quality Routing Continuity (per pollutant)
    * :attr:`node_flooding`       → Node Flooding Summary (flooded nodes only)
    * :attr:`storage_summary`     → Storage Volume Summary (all STORAGE nodes)
    * :attr:`link_flow_summary`   → Link Flow Summary (all links)
    * :attr:`pump_summary`        → Pumping Summary (PUMP links only)
    * :attr:`subcatchment_summary`→ Subcatchment Runoff Summary

    Sections not covered (data not available in the C API):
    Outfall Loading Summary, Flow Classification Summary, LID Performance.
    """

    routing_diagnostics: RoutingDiagnostics
    runoff_continuity: RunoffContinuity
    routing_continuity: RoutingContinuity
    quality_continuity: list[QualityContinuity]
    node_flooding: list[NodeFloodingEntry]
    storage_summary: list[NodeFloodingEntry]
    link_flow_summary: list[LinkFlowEntry]
    pump_summary: list[PumpEntry]
    subcatchment_summary: list[SubcatchmentEntry]


# =============================================================================
# Public function
# =============================================================================


def get_report_snapshot(solver) -> ReportSnapshot:
    """Return the full post-simulation report as a :class:`ReportSnapshot`.

    Assembles all data that the engine writes to the ``.rpt`` text file
    into structured Python dataclasses, without reading the text file.
    Call this after :meth:`Solver.end` has been invoked.

    @param solver: A completed :class:`Solver` instance (state ``ENDED``).
    @type solver: Solver
    @return: Structured report covering continuity, convergence, and
        per-element summary tables.
    @rtype: ReportSnapshot
    @raise RuntimeError: If any underlying C API call fails.

    Example::

        from openswmm.engine import Solver, get_report_snapshot

        with Solver("model.inp", "model.rpt", "model.out") as s:
            s.start()
            while s.step() == 0:
                pass
            s.end()
            snap = get_report_snapshot(s)
            print(snap.routing_diagnostics.n_steps_not_converged)
    """
    from ._nodes import Nodes
    from ._links import Links
    from ._subcatchments import Subcatchments
    from ._pollutants import Pollutants
    from ._massbalance import MassBalance
    from ._statistics import Statistics
    from ._enums import NodeType, LinkType, RunoffTotal, RoutingTotal

    nodes  = Nodes(solver)
    links  = Links(solver)
    sc     = Subcatchments(solver)
    polls  = Pollutants(solver)
    mb     = MassBalance(solver)
    stats  = Statistics(solver)

    # --- Simulation duration (for pct_time_on) ---------------------------------
    start_t  = solver.get_start_time()
    end_t    = solver.get_end_time()
    sim_secs = max((end_t - start_t) * 86400.0, 1.0)  # OADate days → seconds

    # --- Routing diagnostics (convergence) ------------------------------------
    raw_rs = {}
    if hasattr(mb, "get_routing_stats"):
        try:
            raw_rs = mb.get_routing_stats() or {}
        except Exception:
            pass

    n_steps = int(raw_rs.get("n_steps", 0))
    pct_nc  = float(raw_rs.get("pct_non_converged", 0.0))

    max_courant = 0.0
    if hasattr(mb, "get_max_courant"):
        try:
            max_courant = mb.get_max_courant()
        except Exception:
            max_courant = float(raw_rs.get("max_courant", 0.0))
    else:
        max_courant = float(raw_rs.get("max_courant", 0.0))

    routing_diag = RoutingDiagnostics(
        avg_time_step=float(raw_rs.get("avg_step", 0.0)),
        min_time_step=float(raw_rs.get("min_step", 0.0)),
        max_time_step=float(raw_rs.get("max_step", 0.0)),
        n_steps=n_steps,
        pct_not_converged=pct_nc,
        n_steps_not_converged=round(n_steps * pct_nc / 100.0),
        avg_iterations=float(raw_rs.get("avg_iterations", 0.0)),
        max_courant=max_courant,
    )

    # --- Runoff continuity ----------------------------------------------------
    runoff_cont = RunoffContinuity(
        continuity_error_pct=mb.get_runoff_continuity_error(),
        total_rainfall=mb.get_runoff_total(RunoffTotal.RAINFALL),
        total_evaporation=mb.get_runoff_total(RunoffTotal.EVAP),
        total_infiltration=mb.get_runoff_total(RunoffTotal.INFIL),
        total_runoff=mb.get_runoff_total(RunoffTotal.RUNOFF),
        total_snow_removal=mb.get_runoff_total(RunoffTotal.SNOWREMOV),
        initial_storage=mb.get_runoff_total(RunoffTotal.INITSTORE),
        final_storage=mb.get_runoff_total(RunoffTotal.FINALSTORE),
    )

    # --- Routing continuity ---------------------------------------------------
    routing_cont = RoutingContinuity(
        continuity_error_pct=mb.get_routing_continuity_error(),
        dry_weather_inflow=mb.get_routing_total(RoutingTotal.DRY_WEATHER),
        wet_weather_inflow=mb.get_routing_total(RoutingTotal.WET_WEATHER),
        groundwater_inflow=mb.get_routing_total(RoutingTotal.GW_INFLOW),
        rdii_inflow=mb.get_routing_total(RoutingTotal.RDII),
        external_inflow=mb.get_routing_total(RoutingTotal.EXTERNAL),
        total_flooding=mb.get_routing_total(RoutingTotal.FLOODING),
        total_outflow=mb.get_routing_total(RoutingTotal.OUTFLOW),
        evaporation_loss=mb.get_routing_total(RoutingTotal.EVAP_LOSS),
        seepage_loss=mb.get_routing_total(RoutingTotal.SEEP_LOSS),
        initial_storage=mb.get_routing_total(RoutingTotal.INIT_STORAGE),
        final_storage=mb.get_routing_total(RoutingTotal.FINAL_STORAGE),
    )

    # --- Quality continuity (per pollutant) -----------------------------------
    quality_cont: list[QualityContinuity] = []
    for i in range(polls.count()):
        try:
            seep = mb.get_quality_seep_loss(i) if hasattr(mb, "get_quality_seep_loss") else 0.0
        except Exception:
            seep = 0.0
        try:
            evap = mb.get_quality_evap_loss(i) if hasattr(mb, "get_quality_evap_loss") else 0.0
        except Exception:
            evap = 0.0
        quality_cont.append(QualityContinuity(
            pollutant_id=polls.get_id(i),
            continuity_error_pct=mb.get_quality_continuity_error(i),
            seep_loss=seep,
            evap_loss=evap,
        ))

    # --- Node sections --------------------------------------------------------
    node_flooding: list[NodeFloodingEntry] = []
    storage_summary: list[NodeFloodingEntry] = []

    for i in range(nodes.count()):
        ntype_code = nodes.get_type(i)
        try:
            ntype_name = NodeType(ntype_code).name
        except ValueError:
            ntype_name = f"TYPE_{ntype_code}"

        entry = NodeFloodingEntry(
            node_id=nodes.get_id(i),
            node_type=ntype_name,
            max_depth=stats.node_max_depth(i),
            max_overflow_rate=stats.node_max_overflow(i),
            total_flood_volume=stats.node_vol_flooded(i),
            time_flooded=stats.node_time_flooded(i),
        )
        if entry.total_flood_volume > 0.0:
            node_flooding.append(entry)
        if ntype_code == int(NodeType.STORAGE):
            storage_summary.append(entry)

    node_flooding.sort(key=lambda e: e.total_flood_volume, reverse=True)

    # --- Link sections --------------------------------------------------------
    link_flow_summary: list[LinkFlowEntry] = []
    pump_summary: list[PumpEntry] = []

    for i in range(links.count()):
        ltype_code = links.get_type(i)
        try:
            ltype_name = LinkType(ltype_code).name
        except ValueError:
            ltype_name = f"TYPE_{ltype_code}"

        link_flow_summary.append(LinkFlowEntry(
            link_id=links.get_id(i),
            link_type=ltype_name,
            max_flow=stats.link_max_flow(i),
            max_velocity=stats.link_max_velocity(i),
            max_filling=stats.link_max_filling(i),
            total_volume=stats.link_vol_flow(i),
            surcharge_time=stats.link_surcharge_time(i),
        ))

        if ltype_code == int(LinkType.PUMP):
            on_time = links.get_stat_pump_on_time(i)
            pump_summary.append(PumpEntry(
                link_id=links.get_id(i),
                pump_curve_idx=links.get_pump_curve(i),
                num_startups=links.get_stat_pump_cycles(i),
                total_on_time=on_time,
                total_volume=links.get_stat_pump_volume(i),
                pct_time_on=min(on_time / sim_secs * 100.0, 100.0),
            ))

    link_flow_summary.sort(key=lambda e: e.max_flow, reverse=True)

    # --- Subcatchment section -------------------------------------------------
    subcatchment_summary: list[SubcatchmentEntry] = []

    for i in range(sc.count()):
        precip = stats.subcatch_precip(i)
        runoff = stats.subcatch_runoff_vol(i)
        subcatchment_summary.append(SubcatchmentEntry(
            subcatch_id=sc.get_id(i),
            total_precip=precip,
            total_runoff_vol=runoff,
            max_runoff_rate=stats.subcatch_max_runoff(i),
            runoff_coefficient=runoff / precip if precip > 0.0 else 0.0,
        ))

    return ReportSnapshot(
        routing_diagnostics=routing_diag,
        runoff_continuity=runoff_cont,
        routing_continuity=routing_cont,
        quality_continuity=quality_cont,
        node_flooding=node_flooding,
        storage_summary=storage_summary,
        link_flow_summary=link_flow_summary,
        pump_summary=pump_summary,
        subcatchment_summary=subcatchment_summary,
    )
