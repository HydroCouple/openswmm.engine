"""
Static-typing test for the v1 Pythonic bindings.

Run with ``mypy --strict python/tests/typing/test_surface.py`` (not pytest).
The file imports every public symbol on ``openswmm.engine`` and uses
:func:`typing.reveal_type` / direct assignments in shapes that would
fail under mypy strict mode if any of the ``.pyi`` return types are
wrong.

The actual *bodies* of the functions below are never executed —
``if False:`` keeps the runtime cost at zero while still letting mypy
walk the AST.
"""

# pyright: reportUnnecessaryTypeIgnoreComment=true
from __future__ import annotations

from datetime import datetime, timedelta
from pathlib import Path
from typing import Dict, Iterator, List, Tuple

import numpy as np

from openswmm.engine import (
    BadIndexError,
    BadParamError,
    BuildupFunc,
    ConcentrationUnits,
    CRSError,
    DependencyError,
    EngineError,
    EngineState,
    FileError,
    FlowUnits,
    ForcingMode,
    ForcingTarget,
    GageDataSource,
    GageRainType,
    HotStart,
    InfilModel,
    LidType,
    LifecycleError,
    LinkType,
    NodeType,
    OrificeType,
    OutfallType,
    OutletRatingType,
    OutLinkVar,
    OutNodeVar,
    OutSubcatchVar,
    OutSystemVar,
    OutputReader,
    PatternType,
    RouteModel,
    RoutingTotal,
    RunoffTotal,
    Solver,
    StaleObjectError,
    SurfaceBoundaryType,
    SurfaceForcingMode,
    WashoffFunc,
    WeirType,
    XSectShape,
    datetime_to_oadate,
    oadate_to_datetime,
    run,
    run_with_callback,
)


def check_solver_lifecycle() -> None:
    """Lifecycle methods raise; no integer rc."""
    s = Solver(Path("model.inp"))
    s.open()
    s.initialize()
    s.start()
    elapsed: timedelta = s.step()
    stride: timedelta = s.stride(5)
    s.end()
    s.report()
    s.close()
    s.destroy()
    _ = (elapsed, stride)


def check_solver_properties(s: Solver) -> None:
    state: EngineState = s.state
    el: timedelta = s.elapsed
    rs: timedelta = s.routing_step
    sd: datetime = s.start_datetime
    ed: datetime = s.end_datetime
    cd: datetime = s.current_datetime
    rsd: datetime = s.report_start_datetime
    handle_int: int = s.handle
    gen: int = s.generation
    crs: str = s.crs
    between: bool = s.is_between_events
    skip: bool = s.steady_state_skip
    s.steady_state_skip = True
    _ = (state, el, rs, sd, ed, cd, rsd, handle_int, gen, crs, between, skip)


def check_iteration(s: Solver) -> None:
    it: Iterator[timedelta] = s.steps()
    for elapsed in it:
        reveal: timedelta = elapsed
        _ = reveal
    until_dt: timedelta = s.until(datetime(2024, 1, 1))
    until_td: timedelta = s.until(timedelta(hours=1))
    _ = (until_dt, until_td)


def check_views(s: Solver) -> None:
    # options: MutableMapping[str, str].
    flow_units: str = s.options["FLOW_UNITS"]
    s.options["FLOW_UNITS"] = "CMS"
    _ = flow_units
    # userflags returns bool|int|float.
    val = s.userflags["X"]
    _ = val
    s.userflags["X"] = True
    s.userflags["Y"] = 7
    s.userflags["Z"] = 3.14
    # events.
    n_events: int = len(s.events)
    _ = n_events
    for ev in s.events:
        _ = ev.start, ev.end


def check_nodes(s: Solver) -> None:
    n: int = len(s.nodes)
    _ = n
    for node in s.nodes:
        nid: str = node.id
        ni: int = node.index
        nt: NodeType = node.type
        depth: float = node.depth
        head: float = node.head
        node.lateral_inflow = 0.5
        _ = (nid, ni, nt, depth, head)
    by_str = s.nodes["J1"]
    by_int = s.nodes[0]
    assert by_str == by_int
    depths: np.ndarray = s.nodes.depths
    s.nodes.depths = depths
    ids: np.ndarray = s.nodes.ids
    _ = ids
    # Sub-views.
    j1 = s.nodes["J1"]
    j1_stats_md: float = j1.stats.max_depth
    _ = j1_stats_md


def check_links(s: Solver) -> None:
    n: int = len(s.links)
    _ = n
    c1 = s.links["C1"]
    flow: float = c1.flow
    from_node = c1.from_node
    to_node = c1.to_node
    _ = (flow, from_node.id, to_node.id)
    flows: np.ndarray = s.links.flows
    _ = flows
    # xsect tuple-assignable.
    c1.xsect = (XSectShape.CIRCULAR, 1.0, 0.0, 0.0, 0.0)


def check_output_reader() -> None:
    with OutputReader(Path("out.out")) as out:
        sd: datetime = out.start_datetime
        rs: timedelta = out.report_step
        fu: FlowUnits = out.flow_units
        nc: int = out.node_count
        ni: List[str] = out.node_ids
        pi: List[str] = out.pollutant_ids
        pt: np.ndarray = out.period_times
        depths: np.ndarray = out.node_series("J1", OutNodeVar.DEPTH)
        attrs: Dict[OutNodeVar | int, float] = out.node_attributes("J1", 0)
        stats = out.node_stats("J1")
        md: float = stats.max_depth
        _ = (sd, rs, fu, nc, ni, pi, pt, depths, attrs, md)


def check_exceptions(s: Solver) -> None:
    try:
        s.nodes["NOPE"]
    except KeyError:
        pass
    try:
        s.nodes[999_999]
    except IndexError:
        pass
    try:
        s.open()
    except EngineError as e:
        code: int = e.code
        msg: str = e.message
        _ = (code, msg)


def check_pollutants_quality(s: Solver) -> None:
    p = s.pollutants["TSS"]
    units: ConcentrationUnits = p.units
    p.kdecay = 0.1
    s.quality.set_buildup("LU1", "TSS",
                          func=BuildupFunc.POW, c1=1.0, c2=0.1, c3=2.0)
    s.quality.set_washoff("LU1", "TSS",
                          func=WashoffFunc.EXP, coeff=1.0, expon=2.0)
    _ = units


def check_forcing(s: Solver) -> None:
    s.forcing.node_lat_inflow("J1", 0.5)
    s.forcing.node_lat_inflow("J1", 0.5, mode=ForcingMode.ADD, persist=True)
    s.forcing.clear(ForcingTarget.NODE, "J1")
    s.forcing.clear_all()


def check_datetime() -> None:
    oad: float = datetime_to_oadate(datetime(2024, 1, 1))
    back: datetime = oadate_to_datetime(oad)
    _ = (oad, back)


def check_hotstart(s: Solver) -> None:
    HotStart.save_from(s, Path("warm.hs"))
    with HotStart.open("warm.hs") as hs:
        when: datetime = hs.sim_datetime
        warnings: List[str] = hs.warnings
        hs.apply(s)
        _ = (when, warnings)


def check_enums() -> None:
    # Each is an IntEnum.
    assert int(NodeType.JUNCTION) == 0
    assert int(LinkType.CONDUIT) == 0
    assert int(XSectShape.CIRCULAR) == 0
    assert int(OutfallType.FREE) == 0
    assert int(OrificeType.SIDE) == 0
    assert int(WeirType.TRANSVERSE) == 0
    assert int(OutletRatingType.FUNCTIONAL_HEAD) == 0
    assert int(InfilModel.HORTON) == 0
    assert int(FlowUnits.CFS) == 0
    assert int(RouteModel.STEADY) == 0
    assert int(GageRainType.INTENSITY) == 0
    assert int(GageDataSource.TIMESERIES) == 0
    assert int(PatternType.MONTHLY) == 0
    assert int(LidType.BIO_CELL) == 0
    assert int(ForcingMode.REPLACE) == 0
    assert int(ForcingTarget.NODE) == 0
    # 2D surface enums use the canonical SWMM_FORCING_* codes (OVERRIDE=1, ADD=2).
    assert int(SurfaceForcingMode.NONE) == 0
    assert int(SurfaceForcingMode.OVERRIDE) == 1
    assert int(SurfaceForcingMode.ADD) == 2
    assert int(SurfaceBoundaryType.WALL) == 0
    assert int(SurfaceBoundaryType.RATING_CURVE) == 4
    assert int(RunoffTotal.RAINFALL) == 0
    assert int(RoutingTotal.DRY_WEATHER) == 0
    assert int(OutNodeVar.DEPTH) == 0
    assert int(OutLinkVar.FLOW) == 0
    assert int(OutSubcatchVar.RAINFALL) == 0
    assert int(OutSystemVar.TEMPERATURE) == 0
    assert int(ConcentrationUnits.MG_PER_L) == 0
    assert int(EngineState.CREATED) == 1


def check_exception_subclasses() -> None:
    # Each subclass also inherits from a stdlib exception.
    assert issubclass(BadIndexError, IndexError)
    assert issubclass(BadParamError, ValueError)
    assert issubclass(LifecycleError, RuntimeError)
    assert issubclass(FileError, IOError)
    assert issubclass(CRSError, ValueError)
    assert issubclass(DependencyError, RuntimeError)
    assert issubclass(StaleObjectError, LifecycleError)
