"""Atomic authored records preserve all fields and reject a bad final member."""

from copy import deepcopy
from pathlib import Path

import pytest

from openswmm.engine import (
    LifecycleError,
    EngineError,
    HAS_2D,
    LidNodeLayer,
    LidNodeLayerKind,
    LidType,
    Solver,
)
from tests.engine.test_lid_nodes import DECK


@pytest.fixture
def opened(tmp_path):
    engines = []

    def create(deck=DECK):
        path = tmp_path / f"model-{len(engines)}.inp"
        path.write_text(deck)
        solver = Solver(path, path.with_suffix(".rpt"), path.with_suffix(".out"))
        solver.open()
        engines.append(solver)
        return solver

    yield create
    for solver in reversed(engines):
        solver.destroy()


@pytest.mark.parametrize("units", ["CFS", "CMS"])
def test_storage_snapshot_prepare_rollback_and_exact_restore(opened, units):
    solver = opened(DECK.replace("CFS", units))
    lids = solver.infrastructure.lids
    lids.add("Stack", LidType.NODE)
    lids.set_layers(
        "Stack",
        [
            LidNodeLayer(LidNodeLayerKind.SURFACE, (6, 0.1)),
            LidNodeLayer(LidNodeLayerKind.MEDIA, (12, 0.45, 0.2, 0.08, 2, 10, 3)),
        ],
    )
    lids.assign_node("S", "Stack")
    lids.set_outlet_anchor("D", 1, position="TOP")
    before = lids.node_snapshot("S")
    proposed = lids.prepare_node("S", "Stack", initial_saturation=25)
    assert lids.node_snapshot("S") == before
    lids.validate_node("S", proposed)
    assert lids.node_snapshot("S") == before
    lids.replace_node("S", proposed)
    assert lids.node_assignment("S") == (0, 25)
    assert lids.node_snapshot("S") == proposed
    bad = deepcopy(proposed)
    bad["anchors"][-1]["crest_ft"] = float("nan")
    for method in [lids.validate_node, lids.replace_node]:
        with pytest.raises(EngineError):
            method("S", bad)
        assert lids.node_snapshot("S") == proposed
    lids.replace_node("S", before)
    assert lids.node_snapshot("S") == before
    assert lids.node_assignment("S") == (0, 0)
    cleared = lids.prepare_node("S", None)
    lids.replace_node("S", cleared)
    assert lids.node_assignment("S") is None
    assert lids.node_snapshot("S") == cleared


@pytest.mark.parametrize("units", ["CFS", "CMS"])
def test_full_lid_placements_keep_unicode_paths_and_other_owners(
    opened, tmp_path, units
):
    deck = (
        DECK.replace("CFS", units)
        + """
[RAINGAGES]
R INTENSITY 0:01 1 TIMESERIES Rain
[SUBCATCHMENTS]
A R S 1 50 100 .5 0
B R S 1 50 100 .5 0
[SUBAREAS]
A .01 .1 .05 .1 25 OUTLET 100
B .01 .1 .05 .1 25 OUTLET 100
[INFILTRATION]
A 3 .5 4 7 0
B 3 .5 4 7 0
[TIMESERIES]
Rain 01/01/2004 00:00 0
"""
    )
    solver = opened(deck)
    lids = solver.infrastructure.lids
    control = lids.add("Bio", LidType.BIO_CELL)
    lids.usage_add("A", control, number=1, area=10, width=2)
    lids.usage_add("B", control, number=2, area=20, width=3)
    first, other = lids.usage_record(0), lids.usage_record(1)
    changed = dict(
        first,
        report_file="résultats/雨.txt",
        report_file_resolved=str(tmp_path / "résultats" / "雨.txt"),
        drain_to="S",
        from_perv=12.125,
    )
    lids.validate_usage(changed)
    assert lids.usage_record(0) == first
    lids.replace_usages("A", [changed])
    assert lids.usage_record(0) == changed
    assert lids.usage_record(1) == other
    invalid = dict(changed, width=float("nan"))
    with pytest.raises(EngineError):
        lids.replace_usages("A", [changed, invalid])
    assert lids.usage_count() == 2
    assert lids.usage_record(0) == changed
    assert lids.usage_record(1) == other
    with pytest.raises(ValueError):
        lids.validate_usage(dict(changed, drain_to="S\0suffix"))
    lids.replace_usages("A", [])
    assert lids.usage_count() == 1
    assert lids.usage_record(0) == other
    assert not Path(changed["report_file_resolved"]).exists()


@pytest.mark.skipif(not HAS_2D, reason="requires 2D bindings")
def test_complete_node_coupling_is_atomic_and_validates_without_writes(opened):
    solver = opened(DECK + "[JUNCTIONS]\nJ 0 2 0 0 0\n")
    index = solver.nodes["J"].index
    surface = solver.surface2d
    surface.validate_node_coupling(index, 1, {"D": 0.24, "COI": 0.6})
    assert surface.get_node_coupling_type(index) == (1, False)
    surface.replace_node_coupling(index, 1, {"D": 0.24, "COI": 0.6})
    assert surface.get_node_coupling_type(index) == (1, True)
    before = surface.get_node_coupling_param(index, "D")
    with pytest.raises(EngineError):
        surface.replace_node_coupling(index, 1, {"D": 2, "COI": float("nan")})
    assert surface.get_node_coupling_param(index, "D") == before
    with pytest.raises(ValueError):
        surface.validate_node_coupling(index, 1, {"D": ".24\0bad"})
    surface.replace_node_coupling(index, 1, authored=False)
    assert surface.get_node_coupling_type(index) == (1, False)


@pytest.mark.parametrize("units", ["CFS", "CMS"])
def test_complete_divider_record_preserves_flow_units_and_rejects_invalid_last_value(
    opened, units
):
    deck = (
        DECK.replace("CFS", units)
        + """
[DIVIDERS]
V 0 C CUTOFF 1 2 0 0 0
[CONDUITS]
C V O 10 .013 0 0
[XSECTIONS]
C CIRCULAR 1 0 0 0 1
"""
    )
    solver = opened(deck)
    divider = solver.nodes["V"].divider
    record = dict(divider.params, cutoff_flow=2.125)
    divider.params = record
    assert divider.params == record
    with pytest.raises(EngineError):
        divider.params = dict(record, weir_coefficient=float("nan"))
    assert divider.params == record


def test_inlet_validation_keeps_existing_placement(opened):
    example = (
        Path(__file__).resolve().parents[3]
        / "examples/inlets/street_inlet_junction.inp"
    )
    solver = opened(example.read_text())
    usages = solver.infrastructure.inlet_usages
    before = list(usages)
    usages.validate(before[0])
    with pytest.raises(EngineError):
        usages.validate(dict(before[0], local_width=float("nan")))
    assert list(usages) == before


@pytest.mark.skipif(not HAS_2D, reason="requires 2D bindings")
def test_editor_input_conversion_cannot_destroy_its_engine(opened):
    solver = opened(DECK + "[JUNCTIONS]\nJ 0 2 0 0 0\n")
    index = solver.nodes["J"].index

    class Parameters(dict):
        def items(self):
            solver.destroy()
            return super().items()

    with pytest.raises(LifecycleError):
        solver.surface2d.replace_node_coupling(index, 1, Parameters(D=0.24))
    assert solver.nodes["J"].index == index


def test_inlet_design_validation_normalizes_without_mutation(opened):
    example = (
        Path(__file__).resolve().parents[3]
        / "examples/inlets/street_inlet_junction.inp"
    )
    solver = opened(example.read_text())
    inlets = solver.infrastructure.inlets
    key = next(iter(inlets))
    before = inlets.get_design(key)
    normalized = inlets.validate_design(before, slot_width=123)
    assert inlets.get_design(key) == before
    assert normalized["slot_width"] == 0
    custom = inlets.get_design("Custom1")
    assert inlets.validate_design(custom, curve_kind=0) == custom
    with pytest.raises(ValueError, match="NUL"):
        inlets.validate_design(custom, curve_id="DIV_CAP\0ignored")
    with pytest.raises(EngineError):
        inlets.validate_design(before, grate_width=-1, curb_height=-1)
    assert inlets.get_design(key) == before
    inlets.set_design(key, normalized)
    assert inlets.get_design(key) == normalized
