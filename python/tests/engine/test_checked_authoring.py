"""New native authoring APIs remain reachable through the Python CI surface."""

import numpy as np
import pytest
from openswmm.engine import (
    EngineError,
    LidExtraField,
    LidNodeLayer,
    LidNodeLayerKind,
    LidType,
)
from openswmm.engine import Solver
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


def test_atomic_table_validation_and_relative_metadata(opened):
    solver = opened()
    table = solver.tables.add_timeseries("Atomic")
    rows = [(45000.0, 1.0), (45000.25, 2.0)]
    table.replace_points(rows, relative_count=2, anchor=45000.0)
    before = table.points.copy()
    table.validate_points(rows, relative_count=2, anchor=45000.0)
    assert table.relative_info == (2, 45000.0)
    for method in [table.validate_points, table.replace_points]:
        with pytest.raises(EngineError):
            method([(45000.0, 9.0), (45000.0, 10.0)])
        np.testing.assert_array_equal(table.points, before)
        assert table.relative_info == (2, 45000.0)
    table.replace_points([], file_token="")
    assert len(table) == 0


def test_complete_controls_validate_and_roll_back(opened):
    solver = opened()
    text = "RULE First\nIF NODE S DEPTH > 1\nTHEN ORIFICE D SETTING = 0.5\n"
    solver.controls.replace_program([text])
    before = list(solver.controls)
    solver.controls.replace_program([], publish=False)
    assert list(solver.controls) == before
    with pytest.raises(EngineError):
        solver.controls.replace_program([text, "RULE Broken\nIF NONEXISTENT nonsense"])
    assert list(solver.controls) == before
    solver.controls.replace_program([])
    assert len(solver.controls) == 0


def test_checked_lid_members_and_pure_node_profile(opened):
    solver = opened()
    lids = solver.infrastructure.lids
    idx = lids.add("Bio", LidType.BIO_CELL)
    lids.validate_type(idx, LidType.BIO_CELL)
    assert lids.get_type(idx) == LidType.BIO_CELL
    for field in LidExtraField:
        value = {LidExtraField.SURFACE_VEGETATION: 0.1, LidExtraField.STORAGE_COVERED: 1.0}.get(field, 2.0)
        lids.set_extra(idx, field, value)
        assert lids.get_extra(idx, field) == pytest.approx(value)
        with pytest.raises(EngineError):
            lids.set_extra(idx, field, float("nan"))
        assert lids.get_extra(idx, field) == pytest.approx(value)
    assert lids.get_drain_curve(idx) == ""
    with pytest.raises(IndexError):
        lids.get_drain_curve(-1)
    with pytest.raises(EngineError):
        lids.validate_type(idx, 99)
    node = lids.add("Node", LidType.NODE)
    layers = [
        LidNodeLayer(LidNodeLayerKind.SURFACE, (6, 0.1)),
        LidNodeLayer(LidNodeLayerKind.MEDIA, (12, 0.45, 0.2, 0.08, 2, 10, 3)),
    ]
    lids.set_layers(node, layers)
    before = lids.get_layers(node)
    kwargs = dict(flow={"model": 0}, materials=[{}, {}])
    lids.validate_layers(node, layers, **kwargs)
    assert lids.get_layers(node) == before
    with pytest.raises(EngineError):
        lids.validate_layers(node, layers, flow={"model": 99}, materials=[{}, {}])
    assert lids.get_layers(node) == before
