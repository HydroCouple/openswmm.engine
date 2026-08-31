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
    Heat,
    HeatCloudParam,
    HeatFluxModule,
    HeatNodeOverride,
    HeatRadiativeParam,
    HeatShortwaveMode,
    HeatSolarParam,
    HeatSourceKind,
    HotStart,
    InfilModel,
    InitialQuality,
    InitialQualityEntry,
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
    ProcessComponent,
    ProcessComponents,
    ExpressionDiagnostic,
    ReactionCoefficient,
    ReactionExprForm,
    ReactionFunction,
    ReactionHydVar,
    ReactionInitialEntry,
    ReactionScope,
    ReactionSpecies,
    ReactionTerm,
    Reactions,
    RouteModel,
    RoutingTotal,
    RunoffTotal,
    Solver,
    StaleObjectError,
    SurfaceBoundaryType,
    SurfaceForcingMode,
    WashoffFunc,
    WaterAge,
    WaterAgeOverride,
    WaterAgeSource,
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


def check_heat(s: Solver) -> None:
    """``solver.heat`` — mappings return floats/bools, rows return NamedTuples."""
    heat: Heat = s.heat
    enabled: bool = heat.enabled

    # [HEAT_FLUXES] toggles: bool in, bool out.
    module_on: bool = heat.modules[HeatFluxModule.RADIATIVE_EXCHANGE]
    heat.modules[HeatFluxModule.SURFACE_EXCHANGE] = True
    heat.modules["LAYER_CONDUCTION"] = False
    heat.modules[0] = True
    n_modules: int = len(heat.modules)
    for module in heat.modules:
        module_member: HeatFluxModule = module
        _ = module_member

    # [RADIATIVE_FLUXES] / [SOLAR_RADIATION] / [CLOUD_COVER]: float mappings.
    albedo: float = heat.radiative[HeatRadiativeParam.ALBEDO]
    heat.radiative[HeatRadiativeParam.SHORTWAVE] = 250.0
    latitude: float = heat.solar[HeatSolarParam.LATITUDE]
    heat.solar[HeatSolarParam.LONGITUDE] = -111.83
    sited: bool = heat.solar_sited
    fraction: float = heat.cloud[HeatCloudParam.FRACTION]
    heat.cloud[HeatCloudParam.LW_CLOUD_K] = 0.3
    cloud_configured: bool = heat.cloud.configured
    cloud_now: float = heat.cloud.current
    heat.cloud.set_timeseries("cloud_ts")
    heat.cloud.clear()

    # Shortwave mode.
    mode: HeatShortwaveMode = heat.shortwave_mode
    heat.shortwave_mode = HeatShortwaveMode.CONSTANT
    heat.set_shortwave_timeseries("sw_ts")
    wm2: float = heat.current_shortwave

    # [HEAT_SOURCES].
    n_sources: int = len(heat.sources)
    temp: float = heat.sources[HeatSourceKind.DWF]
    heat.sources[HeatSourceKind.DWF] = 18.5
    configured: bool = heat.sources.is_configured(HeatSourceKind.DWF)
    heat.sources.clear(HeatSourceKind.DWF)
    effective: float = heat.sources.effective(HeatSourceKind.DWF, "J1")
    effective_by_index: float = heat.sources.effective(HeatSourceKind.DWF, 0)
    in_table: bool = HeatSourceKind.DWF in heat.sources
    for source in heat.sources:
        source_member: HeatSourceKind = source
        _ = source_member

    # NODE overrides.
    n_rows: int = len(heat.node_overrides)
    row: HeatNodeOverride = heat.node_overrides[0]
    row_source: HeatSourceKind = row.source
    row_node: int = row.node_index
    row_temp: float = row.temp_c
    heat.node_overrides.set(HeatSourceKind.DWF, "J1", 22.0)
    heat.node_overrides.remove(0)
    for override in heat.node_overrides:
        override_row: HeatNodeOverride = override
        _ = override_row

    _ = (enabled, module_on, n_modules, albedo, latitude, sited, fraction,
         cloud_configured, cloud_now, mode, wm2, n_sources, temp, configured,
         effective, effective_by_index, in_table, n_rows, row_source,
         row_node, row_temp)


def check_water_age(s: Solver) -> None:
    """``solver.water_age`` — HOURS everywhere, signed."""
    age: WaterAge = s.water_age
    enabled: bool = age.enabled

    n_sources: int = len(age.globals)
    hours: float = age.globals[WaterAgeSource.DWF]
    age.globals[WaterAgeSource.DWF] = 4.0
    age.globals["GW"] = -2.5           # negative is legal (D-NS1)
    age.globals[0] = 1.0
    present: bool = WaterAgeSource.DWF in age.globals
    for source in age.globals:
        source_member: WaterAgeSource = source
        _ = source_member

    n_rows: int = len(age.node_overrides)
    row: WaterAgeOverride = age.node_overrides[0]
    row_source: WaterAgeSource = row.source
    row_node: int = row.node_index
    row_hours: float = row.hours
    age.node_overrides.set(WaterAgeSource.EXTERNAL_INFLOW, "J0", 6.0)
    age.node_overrides.set(WaterAgeSource.DWF, 0, -1.5)
    age.node_overrides.remove(WaterAgeSource.DWF, "J0")
    for override in age.node_overrides:
        override_row: WaterAgeOverride = override
        _ = override_row

    age.save("saved.age")
    age.save(Path("saved.age"))

    _ = (enabled, n_sources, hours, present, n_rows, row_source, row_node,
         row_hours)


def check_initial_quality(s: Solver) -> None:
    """``solver.initial_quality`` — the [INITIAL_QUALITY] row table."""
    iq: InitialQuality = s.initial_quality
    n: int = len(iq)
    row: InitialQualityEntry = iq[0]
    is_link: bool = row.is_link
    elem: int = row.elem_index
    constituent: str = row.constituent
    value: float = row.value

    water_age_name: str = InitialQuality.WATER_AGE
    temperature_name: str = InitialQuality.TEMPERATURE

    iq.set("TSS", 12.5, node="J1")
    iq.set("TSS", 12.5, link="C1")
    iq.set(water_age_name, -6.0, node=0)
    iq.set(temperature_name, 18.5, link=0)
    iq.remove(0)
    for entry in iq:
        entry_row: InitialQualityEntry = entry
        _ = entry_row

    _ = (n, is_link, elem, constituent, value, water_age_name,
         temperature_name)


def check_process_components(s: Solver) -> None:
    """``solver.process_components`` — the [PROCESS_COMPONENTS] table."""
    pcs: ProcessComponents = s.process_components
    n: int = len(pcs)
    by_index: ProcessComponent = pcs[0]
    by_id: ProcessComponent = pcs["org.hydrocouple.openswmm.reactions"]
    idx: int = by_index.component_index
    cid: str = by_index.id
    config: str = by_index.config
    resolved: str = by_index.resolved
    present: bool = "org.hydrocouple.openswmm.reactions" in pcs
    found: int = pcs.get_index("org.hydrocouple.openswmm.reactions")
    registered: ProcessComponent = pcs.register("my.component", "my.rxn")
    bare: ProcessComponent = pcs.register("my.other.component")
    pcs.remove(0)
    pcs.remove("my.component")
    for comp in pcs:
        comp_row: ProcessComponent = comp
        _ = comp_row

    _ = (n, by_id, idx, cid, config, resolved, present, found, registered,
         bare)


def check_reactions(s: Solver) -> None:
    """``solver.reactions`` — collections, expressions, and the text tab."""
    rxn: Reactions = s.reactions

    # Species.
    n_species: int = len(rxn.species)
    species: ReactionSpecies = rxn.species["X"]
    by_index: ReactionSpecies = rxn.species[0]
    sp_index: int = species.index
    sp_name: str = species.name
    sp_wall: bool = species.is_wall
    sp_units: str = species.units
    sp_atol: float = species.atol
    sp_rtol: float = species.rtol
    sp_initial: float = species.initial
    species.initial = 0.5
    pipe: Tuple[ReactionExprForm, str] = species.pipe_expression
    tank: Tuple[ReactionExprForm, str] = species.tank_expression
    got: Tuple[ReactionExprForm, str] = species.get_expression(
        ReactionScope.PIPE)
    species.set_expression(ReactionScope.PIPE, ReactionExprForm.RATE,
                           "-Kb * X")
    species.set_expression(ReactionScope.TANK, ReactionExprForm.NONE)
    added_species: ReactionSpecies = rxn.species.add(
        "Z", wall=True, units="MG", atol=1e-6, rtol=1e-6)
    has_species: bool = "X" in rxn.species
    species_index: int = rxn.species.get_index("X")
    rxn.species.remove("Z")
    for sp in rxn.species:
        sp_row: ReactionSpecies = sp
        _ = sp_row

    # Coefficients.
    n_coeffs: int = len(rxn.coefficients)
    coeff: ReactionCoefficient = rxn.coefficients["Kb"]
    coeff_index: int = coeff.index
    coeff_name: str = coeff.name
    coeff_is_param: bool = coeff.is_param
    coeff_value: float = coeff.value
    coeff.value = 0.05
    added_coeff: ReactionCoefficient = rxn.coefficients.add(
        "Kc", parameter=True, value=1.0)
    has_coeff: bool = "Kb" in rxn.coefficients
    coeff_idx: int = rxn.coefficients.get_index("Kb")
    rxn.coefficients.remove("Kc")
    for c in rxn.coefficients:
        coeff_row: ReactionCoefficient = c
        _ = coeff_row

    # Terms.
    n_terms: int = len(rxn.terms)
    term: ReactionTerm = rxn.terms["Kf"]
    term_index: int = term.index
    term_name: str = term.name
    term_expr: str = term.expression
    term.expression = "0.5 * RE"
    added_term: ReactionTerm = rxn.terms.add("Kg", "2.0 * D")
    has_term: bool = "Kf" in rxn.terms
    term_idx: int = rxn.terms.get_index("Kf")
    rxn.terms.remove("Kg")
    for t in rxn.terms:
        term_row: ReactionTerm = t
        _ = term_row

    # Per-element initial quality.
    n_initial: int = len(rxn.initial)
    initial_row: ReactionInitialEntry = rxn.initial[0]
    init_is_link: bool = initial_row.is_link
    init_elem: int = initial_row.elem_index
    init_species: int = initial_row.species_index
    init_value: float = initial_row.value
    rxn.initial.set(False, 0, "X", 3.0)
    rxn.initial.set(True, 0, 0, 3.0)
    rxn.initial.remove(0)
    for entry in rxn.initial:
        init_entry: ReactionInitialEntry = entry
        _ = init_entry

    # Validation, options, text tab.
    diag: ExpressionDiagnostic = rxn.validate("-Kb * X", ReactionScope.PIPE)
    default_scope: ExpressionDiagnostic = rxn.validate("-Kb * X")
    diag_valid: bool = diag.valid
    diag_message: str = diag.message
    diag_column: int = diag.column
    option: str = rxn.get_option("RATE_UNITS")
    rxn.set_option("RATE_UNITS", "SEC")
    text: str = rxn.serialize()
    check: ExpressionDiagnostic = rxn.check_text(text)
    rxn.apply_text(text)
    rxn.save()
    rxn.save("model.rxn")

    # Engine-less statics: reachable on the CLASS, with no Solver at all.
    hydvars: Tuple[ReactionHydVar, ...] = Reactions.hydraulic_variables()
    functions: Tuple[ReactionFunction, ...] = Reactions.functions()
    hv_name: str = hydvars[0].name
    hv_desc: str = hydvars[0].description
    fn_name: str = functions[0].name
    fn_arity: int = functions[0].arity

    _ = (n_species, by_index, sp_index, sp_name, sp_wall, sp_units, sp_atol,
         sp_rtol, sp_initial, pipe, tank, got, added_species, has_species,
         species_index, n_coeffs, coeff_index, coeff_name, coeff_is_param,
         coeff_value, added_coeff, has_coeff, coeff_idx, n_terms, term_index,
         term_name, term_expr, added_term, has_term, term_idx, n_initial,
         init_is_link, init_elem, init_species, init_value, default_scope,
         diag_valid, diag_message, diag_column, option, text, check,
         hv_name, hv_desc, fn_name, fn_arity)


def check_transport_enums() -> None:
    # The nine transport enums are IntEnums with the C storage codes.
    assert int(HeatFluxModule.SURFACE_EXCHANGE) == 0
    assert int(HeatShortwaveMode.CONSTANT) == 0
    assert int(HeatRadiativeParam.SHORTWAVE) == 0
    assert int(HeatSolarParam.LATITUDE) == 0
    assert int(HeatCloudParam.FRACTION) == 0
    assert int(HeatSourceKind.RAINFALL) == 0
    assert int(WaterAgeSource.RAINFALL) == 0
    assert int(ReactionScope.TERM) == 0
    assert int(ReactionExprForm.NONE) == 0


def check_exception_subclasses() -> None:
    # Each subclass also inherits from a stdlib exception.
    assert issubclass(BadIndexError, IndexError)
    assert issubclass(BadParamError, ValueError)
    assert issubclass(LifecycleError, RuntimeError)
    assert issubclass(FileError, IOError)
    assert issubclass(CRSError, ValueError)
    assert issubclass(DependencyError, RuntimeError)
    assert issubclass(StaleObjectError, LifecycleError)
