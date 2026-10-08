"""R0 baseline for runtime 2D coupling: what the API does today, mid-run.

Plan: ``openswmm.gui/workplans/RUNTIME_2D_COUPLING_API_AND_PYTHON_PLAN_2026-09-26.md``,
work package R0. Every forcing, boundary and edit call below is made after
``start()``, between ordinary ``step()`` calls, and each test asserts a physical
effect (stored volume, boundary volume, ledger) rather than a setter/getter
round trip.

The regression cases are required to pass; the isolated aquifer fixture starts
without surface ponding so infiltration cannot dilute its initial quality.

Models, results and the provenance/capability record are written to
``output/runtime_coupling_r0/`` next to this file.
"""
from __future__ import annotations

import ctypes
import json
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

try:
    import openswmm.engine._2d  # noqa: F401
except ImportError as exc:  # pragma: no cover - environment dependent
    pytest.skip(f"requires compiled engine with 2D: {exc}", allow_module_level=True)

import openswmm
from openswmm.engine import (
    BadIndexError, CellScope, ForcingPersist, GroundwaterClosure,
    GroundwaterInitialQuality, GroundwaterLedger, GroundwaterSource,
    GroundwaterVariable, GroundwaterZone, LifecycleError, Solver,
    SurfaceBoundaryType, SurfaceForcingMode,
)
from tests.engine._groundwater_cases import MODEL as GW_MODEL

_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parents[2]
_OUT = _HERE / "output" / "runtime_coupling_r0"

# Two right triangles tiling a flat 10 m x 10 m bed at z = 0, walled by
# default, with one pollutant so the surface carries a transport row.
SURFACE_MODEL = """[OPTIONS]
FLOW_UNITS CMS
FLOW_ROUTING DYNWAVE
START_DATE 01/01/2026
START_TIME 00:00:00
END_DATE 01/01/2026
END_TIME 02:00:00
REPORT_STEP 00:05:00
ROUTING_STEP 5
[POLLUTANTS]
TSS MG/L 0 0 0 0
[JUNCTIONS]
J1 0.0 3.0
[OUTFALLS]
O1 -1.0 FREE
[CONDUITS]
C1 J1 O1 10 0.013 0 0
[XSECTIONS]
C1 CIRCULAR 0.5 0 0 0
[2D_OPTIONS]
MAX_TIMESTEP 5
DRY_DEPTH 0.001
[2D_VERTICES]
0 0 0
10 0 0
10 10 0
0 10 0
[2D_TRIANGLES]
0 1 2 0.03
0 2 3 0.03
"""

PERSIST = dict(mode=SurfaceForcingMode.OVERRIDE, persist=ForcingPersist.PERSIST)


def _open(name, text):
    _OUT.mkdir(parents=True, exist_ok=True)
    inp = _OUT / f"{name}.inp"
    inp.write_text(text)
    solver = Solver(inp, inp.with_suffix(".rpt"), inp.with_suffix(".out"))
    solver.open()
    return solver


def _close(solver):
    try:
        solver.close()
    finally:
        solver.destroy()


def _advance(solver, seconds):
    """Step until at least ``seconds`` more have elapsed; return the elapsed span."""
    t0 = solver.elapsed.total_seconds()
    while solver.elapsed.total_seconds() - t0 < seconds:
        solver.step()
    return solver.elapsed.total_seconds() - t0


def _edge_length(surface, cell, edge):
    length, _, _ = surface.get_edge_geometry_bulk()
    return float(length[cell * surface.edge_stride + edge])


def _start_groundwater(name, *, source_flow=None):
    """The shared mixed tri/quad model with an aquifer carrying TSS at 5."""
    solver = _open(name, GW_MODEL.replace("0.03 0.1", "0.03 0"))
    gw = solver.surface2d.groundwater
    gw.options["NODE_ENROLMENT"] = "ROWS"
    gw.add_row(CellScope.GLOBAL, 36, 0.5, 0.45, 0.1, 2)
    gw.set_row_property(0, "HG0", 0.2)
    gw.transport.options["TRANSPORT_POLLUTANTS"] = "YES"
    gw.transport.set_initial_quality(GroundwaterInitialQuality(species="TSS", value=5))
    if source_flow is not None:
        gw.transport.set_source(GroundwaterSource(name="well", cell=0, flow=source_flow))
    solver.initialize()
    solver.start()
    return solver


def _native_libraries():
    """Paths of the openswmm shared libraries mapped into this process."""
    found = set()
    maps = Path("/proc/self/maps")
    if maps.exists():
        for line in maps.read_text().splitlines():
            parts = line.split(maxsplit=5)
            if len(parts) == 6 and Path(parts[5].strip()).name.startswith("libopenswmm"):
                found.add(parts[5].strip())
    elif sys.platform == "darwin":
        try:
            dyld = ctypes.CDLL("/usr/lib/libSystem.B.dylib")
            dyld._dyld_image_count.restype = ctypes.c_uint32
            dyld._dyld_get_image_name.restype = ctypes.c_char_p
        except (OSError, AttributeError):
            return []
        for i in range(dyld._dyld_image_count()):
            image = dyld._dyld_get_image_name(i).decode()
            if Path(image).name.startswith("libopenswmm"):
                found.add(image)
    return [{"path": p, "mtime": Path(p).stat().st_mtime} for p in sorted(found)
            if Path(p).exists()]


def _source_revision():
    """HEAD and the count of modified tracked files, when this is a git checkout."""
    def git(*args):
        return subprocess.run(["git", "-C", str(_REPO), *args], capture_output=True,
                              text=True, check=True).stdout
    try:
        return {"head": git("rev-parse", "HEAD").strip(),
                "modified_tracked_files": len(git("status", "--porcelain",
                                                  "--untracked-files=no").splitlines())}
    except (OSError, subprocess.CalledProcessError):
        return None


def test_both_domains_readable_after_start():
    """R0 gate: a Python run reads surface and groundwater state after start."""
    solver = _start_groundwater("both_domains")
    try:
        _advance(solver, 30)
        sw, gw = solver.surface2d, solver.surface2d.groundwater
        n = sw.n_triangles
        assert sw.get_depths().shape == (n,)
        assert np.isfinite(sw.total_volume)
        assert "TSS" in sw.species
        surface_tss = sw.concentrations(sw.species.index("TSS"))
        assert surface_tss.shape == (n,) and np.isfinite(surface_tss).all()
        with pytest.raises(BadIndexError):
            sw.concentrations(len(sw.species))

        assert gw.active
        cells, layers = gw.dimensions
        assert gw.cells(GroundwaterVariable.HG).shape == (cells,)
        assert gw.ledger(GroundwaterLedger.STORAGE) > 0
        tss = gw.species.index("TSS")
        # No source, sink or exchange touches the aquifer: TSS stays at 5.
        np.testing.assert_allclose(gw.concentrations(GroundwaterZone.SAT, tss), 5, rtol=1e-9)

        stats = sw.run_stats
        record = {
            "plan": "RUNTIME_2D_COUPLING_API_AND_PYTHON_PLAN_2026-09-26 R0",
            "openswmm_version": openswmm.__version__,
            "python_package": str(Path(openswmm.__file__).parent),
            "native_libraries": _native_libraries(),
            "source_revision": _source_revision(),
            "surface": {
                "cells": n, "quads": sw.n_quads, "backend": stats["backend"],
                "momentum": stats["momentum"], "lts_tiers": stats["lts_tiers"],
                "species": list(sw.species),
            },
            "groundwater": {
                "cells": cells, "sigma_layers": layers,
                "closures": sorted({GroundwaterClosure(int(c)).name
                                    for c in gw.cells(GroundwaterVariable.CLOSURE)}),
                "species": list(gw.species),
            },
        }
        (_OUT / "runtime_coupling_r0_baseline.json").write_text(json.dumps(record, indent=2))
        solver.end()
    finally:
        _close(solver)


def test_surface_coupling_flux_persist_and_replace():
    """A persistent OVERRIDE source adds exactly q * area * dt, and a new value
    replaces the old one from the next step."""
    solver = _open("flux_persist", SURFACE_MODEL)
    try:
        solver.initialize()
        solver.start()
        sw = solver.surface2d
        _advance(solver, 30)
        area = sw.get_triangle_area(0)
        for q in (1e-4, 3e-4):
            sw.force_coupling_flux(0, q, **PERSIST)
            v0 = sw.total_volume
            dt = _advance(solver, 120)
            assert sw.total_volume - v0 == pytest.approx(q * area * dt, rel=1e-9)
        solver.end()
    finally:
        _close(solver)


def test_surface_coupling_flux_clear_stops_inflow():
    solver = _open("flux_clear", SURFACE_MODEL)
    try:
        solver.initialize()
        solver.start()
        sw = solver.surface2d
        _advance(solver, 30)
        sw.force_coupling_flux(0, 1e-4, **PERSIST)
        _advance(solver, 60)
        sw.force_clear_all()
        v0 = sw.total_volume
        _advance(solver, 120)
        assert sw.total_volume == pytest.approx(v0, abs=1e-9)
    finally:
        _close(solver)


def test_surface_coupling_flux_one_shot_lasts_one_step():
    solver = _open("flux_one_shot", SURFACE_MODEL)
    try:
        solver.initialize()
        solver.start()
        sw = solver.surface2d
        _advance(solver, 30)
        sw.force_coupling_flux(0, 1e-4)  # OVERRIDE, RESET
        solver.step()
        v0 = sw.total_volume
        _advance(solver, 120)
        assert sw.total_volume == pytest.approx(v0, abs=1e-9)
    finally:
        _close(solver)


def test_surface_coupling_flux_add_is_held():
    solver = _open("flux_add", SURFACE_MODEL)
    try:
        solver.initialize()
        solver.start()
        sw = solver.surface2d
        _advance(solver, 30)
        area, q = sw.get_triangle_area(0), 1e-4
        sw.force_coupling_flux(0, q, mode=SurfaceForcingMode.ADD, persist=ForcingPersist.PERSIST)
        v0 = sw.total_volume
        dt = _advance(solver, 300)
        assert sw.total_volume - v0 == pytest.approx(q * area * dt, rel=1e-6)
    finally:
        _close(solver)


def test_surface_edge_bc_changes_between_steps():
    """On an edge the input file gives a non-WALL condition, type and value
    changes made mid-run take effect from the next step: inflow, a replaced
    inflow, a wall, then a stage outlet whose reported boundary volume matches
    the storage it removed."""
    text = SURFACE_MODEL + "[2D_BOUNDARY_CONDITIONS]\n0 0 SPECIFIED_FLOW 0 * *\n"
    solver = _open("edge_bc", text)
    try:
        solver.initialize()
        solver.start()
        sw = solver.surface2d
        _advance(solver, 30)
        length = _edge_length(sw, 0, 0)

        for flow in (-0.01, -0.02):  # m3/s per metre, outward positive
            sw.set_edge_bc_flow(0, 0, flow)
            v0 = sw.total_volume
            dt = _advance(solver, 120)
            assert sw.total_volume - v0 == pytest.approx(-flow * length * dt, rel=1e-9)

        sw.set_edge_bc_type(0, 0, SurfaceBoundaryType.WALL)
        v0 = sw.total_volume
        _advance(solver, 120)
        assert sw.total_volume == pytest.approx(v0, rel=1e-12)

        sw.set_edge_bc_type(0, 0, SurfaceBoundaryType.SPECIFIED_STAGE)
        sw.set_edge_bc_head(0, 0, 0.05)
        v0, out0 = sw.total_volume, sw.get_edge_bc_cum_flux(0, 0)
        _advance(solver, 600)
        drained = sw.get_edge_bc_cum_flux(0, 0) - out0
        assert drained > 0
        assert v0 - sw.total_volume == pytest.approx(drained, rel=1e-9)
        solver.end()
    finally:
        _close(solver)


def test_surface_wall_edge_takes_a_new_bc_mid_run():
    solver = _open("edge_bc_from_wall", SURFACE_MODEL)
    try:
        solver.initialize()
        solver.start()
        sw = solver.surface2d
        _advance(solver, 30)
        sw.set_edge_bc_type(0, 0, SurfaceBoundaryType.SPECIFIED_FLOW)
        sw.set_edge_bc_flow(0, 0, -0.01)
        v0 = sw.total_volume
        dt = _advance(solver, 120)
        assert sw.total_volume - v0 == pytest.approx(0.01 * _edge_length(sw, 0, 0) * dt, rel=1e-9)
    finally:
        _close(solver)


def test_surface_api_head_owns_a_time_series_edge():
    text = SURFACE_MODEL + """[TIMESERIES]
Tide 0.0 0.5
Tide 3.0 0.5
[2D_BOUNDARY_CONDITIONS]
0 0 TS_STAGE Tide * *
"""
    solver = _open("edge_ts_precedence", text)
    try:
        solver.initialize()
        solver.start()
        sw = solver.surface2d
        _advance(solver, 1800)
        assert sw.get_depths().mean() == pytest.approx(0.5, abs=0.05)
        sw.set_edge_bc_head(0, 0, 0.1)
        _advance(solver, 1800)
        assert sw.get_depths().mean() == pytest.approx(0.1, abs=0.05)
    finally:
        _close(solver)


def test_groundwater_authoring_rejects_mid_run_edits():
    """R0 gate: setup-only groundwater calls refuse running-state writes, and
    the authored source keeps running at its authored rate."""
    flow = 1e-5
    solver = _start_groundwater("gw_setup_only", source_flow=flow)
    try:
        gw = solver.surface2d.groundwater
        _advance(solver, 30)
        edits = {
            "add_row": lambda: gw.add_row(CellScope.GLOBAL, 1, 1, 0.4, 0.1, 1),
            "set_row_property": lambda: gw.set_row_property(0, "HG0", 0.3),
            "aquifer option": lambda: gw.options.__setitem__("NODE_ENROLMENT", "ROWS"),
            "set_source": lambda: gw.transport.set_source(
                GroundwaterSource(name="well2", cell=1, flow=flow)),
            "set_source_scale": lambda: gw.transport.set_source_scale(0, 2.0),
            "transport option": lambda: gw.transport.options.__setitem__(
                "TRANSPORT_POLLUTANTS", "NO"),
        }
        for name, edit in edits.items():
            with pytest.raises(LifecycleError):
                edit()
        assert gw.transport.source_scale(0) == 1.0

        in0 = gw.ledger(GroundwaterLedger.SOURCE_IN)
        dt = _advance(solver, 300)
        assert gw.ledger(GroundwaterLedger.SOURCE_IN) - in0 == pytest.approx(flow * dt, rel=1e-6)
        solver.end()
    finally:
        _close(solver)
