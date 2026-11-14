"""Read-only authoring previews and safe configuration edits across build variants."""

from datetime import datetime, timedelta

import pytest

from openswmm.engine import (
    EngineError,
    ErrorCode,
    EvapPreviewStatus,
    EvapType,
    Heat2DValue,
    Solver,
    StaleObjectError,
    TableType,
)

MODEL = """[OPTIONS]
FLOW_UNITS CMS
FLOW_ROUTING DYNWAVE
START_DATE 01/01/2026
START_TIME 00:00:00
END_DATE 01/01/2026
END_TIME 00:10:00
REPORT_STEP 00:01:00
ROUTING_STEP 5
[JUNCTIONS]
J1 -2 4 0 0 0
[OUTFALLS]
O1 -2.5 FREE NO
[CONDUITS]
C1 J1 O1 30 0.013 0 0 0
[XSECTIONS]
C1 CIRCULAR 0.5 0 0 0 1
[TIMESERIES]
INPUT 0:00 0.5
INPUT 1:00 0.5
[CURVES]
SHAPE STORAGE 0 1
SHAPE 1 2
[EVAPORATION]
CONSTANT 0.25
[REPORT]
INPUT NO
"""


@pytest.fixture
def solver(tmp_path):
    path = tmp_path / "model.inp"
    path.write_text(MODEL)
    s = Solver(path, path.with_suffix(".rpt"), path.with_suffix(".out"))
    s.open()
    try:
        yield s
    finally:
        s.destroy()


@pytest.fixture
def mesh_solver(tmp_path):
    pytest.importorskip("openswmm.engine._2d")
    from tests.engine._groundwater_cases import MODEL as mesh_model

    path = tmp_path / "mesh.inp"
    path.write_text(
        mesh_model.replace("C1 CIRCULAR 0.5 0 0 0 1", "C1 RECT_OPEN 1 1 0 0 1")
        + "[COORDINATES]\nJ1 1 5\nO1 9 5\n"
    )
    s = Solver(path, path.with_suffix(".rpt"), path.with_suffix(".out"))
    s.open()
    try:
        yield s
    finally:
        s.destroy()


def test_evaporation_preview_current_and_draft_do_not_mutate(solver):
    start = datetime(2026, 1, 1)
    end = start + timedelta(minutes=10)
    before = solver.climate.evap_monthly
    generation = solver.generation
    current = solver.climate.preview_evaporation(start, end, 4 / 1440)
    assert len(current["samples"]) == 3
    assert current["summary"]["source_type"] == EvapType.CONSTANT
    assert current["summary"]["runtime_forcing_unknown"] is True
    assert current["summary"]["available_count"] == 3
    assert all(
        row["status"] == EvapPreviewStatus.AVAILABLE for row in current["samples"]
    )
    assert current["samples"][0]["source_rate"] == pytest.approx(0.25)
    draft = solver.climate.preview_evaporation(
        start,
        end,
        4 / 1440,
        config={"evap_type": EvapType.MONTHLY, "evap_monthly": [0.75] * 12},
    )
    assert draft["summary"]["source_type"] == EvapType.MONTHLY
    assert draft["samples"][0]["source_rate"] == pytest.approx(0.75)
    assert solver.climate.evap_type == EvapType.CONSTANT
    assert solver.climate.evap_monthly == before
    assert solver.generation == generation


@pytest.mark.parametrize(
    "config",
    [
        {"unknown": 1},
        {"evap_monthly": [1]},
        {"climate_file_path": "bad\0path"},
    ],
)
def test_invalid_preview_draft_is_refused(solver, config):
    with pytest.raises(ValueError):
        solver.climate.preview_evaporation(46023, 46023.001, 0.001, config=config)
    assert solver.climate.evap_type == EvapType.CONSTANT


def test_preview_validation_and_native_diagnostic(solver):
    with pytest.raises(EngineError, match="simulation dates"):
        solver.climate.preview_evaporation(0, 1, 0.5)
    with pytest.raises(EngineError):
        solver.climate.preview_evaporation(46023, 46023.001, 0)


def test_complete_snapshot_restores_climate_and_refuses_stale_owner(solver):
    original = solver.climate.evap_monthly
    with solver.heat.snapshot(complete=True) as saved:
        solver.climate.evap_monthly = [0.8] * 12
        solver.climate.latitude = 42
        saved.restore()
        assert solver.climate.evap_monthly == original
        assert solver.climate.latitude != 42
        solver.climate.evap_monthly = [0.7] * 12
        saved.restore()
        assert solver.climate.evap_monthly == original
    with pytest.raises(ValueError):
        saved.restore()
    saved = solver.heat.snapshot(complete=True)
    solver.tables.rename("INPUT", "RAIN")
    with pytest.raises(StaleObjectError):
        saved.restore()
    solver.destroy()
    saved.close()
    saved.close()


def test_table_edits_preserve_data_and_advance_generation(solver):
    table = solver.tables.as_curve("SHAPE")
    points = table.points.copy()
    generation = solver.generation
    solver.tables.rename("SHAPE", "RENAMED")
    assert solver.generation > generation
    assert table.id == "RENAMED"
    assert (table.points == points).all()
    assert "SHAPE" not in solver.tables
    assert solver.tables.get_type("RENAMED") == TableType.CURVE_STORAGE
    solver.tables.set_type("RENAMED", TableType.CURVE_RATING)
    assert solver.tables.get_type("RENAMED") == TableType.CURVE_RATING
    assert (solver.tables.as_curve("RENAMED").points == points).all()
    with pytest.raises(EngineError):
        solver.tables.set_type("INPUT", TableType.CURVE_STORAGE)
    with pytest.raises(EngineError):
        solver.tables.set_type("RENAMED", TableType.TIMESERIES)
    solver.climate.evap_timeseries = "INPUT"
    solver.tables.rename("INPUT", "RENAMED")  # separate namespaces
    assert solver.climate.evap_timeseries == "RENAMED"
    with pytest.raises(ValueError):
        solver.tables.rename("RENAMED", "bad\0id")


def test_bed_initial_quality_scope_crud_and_cpu_policy(mesh_solver, tmp_path):
    surface = mesh_solver.surface2d
    assert surface.initial_bed_quality == ()
    surface.set_initial_bed_quality("TSS", 1.25)
    surface.set_initial_bed_quality("TSS", 2.5, tag="PAN")
    surface.set_initial_bed_quality("TSS", 3.75, cell=0)
    assert [row.scope for row in surface.initial_bed_quality] == [0, 1, 2]
    assert [row.conc for row in surface.initial_bed_quality] == [1.25, 2.5, 3.75]
    surface.set_initial_bed_quality("TSS", 5, tag="PAN")
    assert len(surface.initial_bed_quality) == 3
    surface.remove_initial_bed_quality(1)
    assert len(surface.initial_bed_quality) == 2
    surface.set_auto_backend_cpu()
    surface.set_auto_backend_cpu(False)
    with pytest.raises(EngineError):
        surface.set_auto_backend_cpu(2)
    with pytest.raises(EngineError):
        surface.set_initial_bed_quality("TSS", -1)
    assert surface.initial_bed_quality[0].conc == 1.25
    output = tmp_path / "saved.inp"
    mesh_solver.write(output)
    assert "[2D_INITIAL_BED_QUALITY]" in output.read_text()
    while mesh_solver.surface2d.initial_bed_quality:
        mesh_solver.surface2d.remove_initial_bed_quality(0)
    mesh_solver.initialize()
    with pytest.raises(EngineError):
        mesh_solver.surface2d.set_auto_backend_cpu()


def test_channel_mapping_full_paged_and_read_only(mesh_solver):
    generation = mesh_solver.generation
    full = mesh_solver.heat.preview_channel_ground_map("C1")
    assert full["valid"]
    assert full["required"] == len(full["contacts"]) > 0
    assert all(row["layer"] == -1 for row in full["contacts"])
    assert full["covered_length_m"] == pytest.approx(full["length_m"])
    assert full["uncovered_length_m"] == pytest.approx(0)
    paged = mesh_solver.heat.preview_channel_ground_map(0, first=0, limit=1)
    assert paged["contacts"] == full["contacts"][:1]
    assert paged["required"] == full["required"]
    empty = mesh_solver.heat.preview_channel_ground_map(
        0, first=full["required"], limit=1
    )
    assert empty["contacts"] == ()
    assert mesh_solver.generation == generation
    with pytest.raises(ValueError):
        mesh_solver.heat.preview_channel_ground_map(0, first=-1)
    with pytest.raises(EngineError):
        mesh_solver.heat.preview_channel_ground_map(0, first=full["required"] + 1)


@pytest.mark.parametrize("page", [False, True])
def test_channel_preview_progress_cancellation_and_exceptions(mesh_solver, page):
    options = {"first": 0, "limit": 1} if page else {}
    calls = []

    def report(stage, percent):
        calls.append((stage, percent))
        return False

    result = mesh_solver.heat.preview_channel_ground_map(0, progress=report, **options)
    assert result["valid"]
    assert calls and all(stage in (1, 2) and 0 <= pct <= 100 for stage, pct in calls)
    with pytest.raises(EngineError) as error:
        mesh_solver.heat.preview_channel_ground_map(
            0, progress=lambda stage, pct: True, **options
        )
    assert error.value.code == ErrorCode.CANCELLED
    marker = RuntimeError("callback failure")

    def fail(stage, percent):
        raise marker

    with pytest.raises(RuntimeError) as error:
        mesh_solver.heat.preview_channel_ground_map(0, progress=fail, **options)
    assert error.value is marker
    assert mesh_solver.heat.preview_channel_ground_map(0)["valid"]


def test_new_enum_exports():
    assert Heat2DValue.THERMAL_RESIDUAL == 12
    assert EvapPreviewStatus.ESTIMATED == 3


def test_preview_callback_cannot_destroy_owner(mesh_solver):
    def destroy(stage, percent):
        mesh_solver.destroy()
        return False

    with pytest.raises(RuntimeError):
        mesh_solver.heat.preview_channel_ground_map(0, progress=destroy)
    assert mesh_solver.heat.preview_channel_ground_map(0)["valid"]


def test_mapping_preview_reports_omitted_feature(solver):
    import importlib.util

    if importlib.util.find_spec("openswmm.engine._2d") is not None:
        pytest.skip("minimal-build fallback")
    with pytest.raises(EngineError) as error:
        solver.heat.preview_channel_ground_map(0)
    assert error.value.code == ErrorCode.PLUGIN
