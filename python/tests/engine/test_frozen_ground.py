"""Frozen-ground rows and slab state through the Python binding."""

import pytest

from openswmm.engine import EngineError, Solver

MODEL = """[OPTIONS]
FLOW_UNITS           CMS
FLOW_ROUTING         DYNWAVE
INFILTRATION         HORTON
START_DATE           01/10/2026
START_TIME           00:00:00
REPORT_START_DATE    01/10/2026
REPORT_START_TIME    00:00:00
END_DATE             01/10/2026
END_TIME             06:00:00
REPORT_STEP          00:15:00
WET_STEP             00:05:00
DRY_STEP             00:05:00
ROUTING_STEP         0:01:00

[RAINGAGES]
RG1  INTENSITY 1:00 1.0 TIMESERIES RAIN

[TEMPERATURE]
TIMESERIES AIR

[SUBCATCHMENTS]
S1  RG1  J1  2.0  0  100  1.0  0

[SUBAREAS]
S1  0.015  0.2  1.0  2.0  0  OUTLET

[INFILTRATION]
S1  80  10  4  7  0

[TIMESERIES]
RAIN  01/10/2026 00:00 0
RAIN  01/10/2026 02:00 12
RAIN  01/10/2026 05:00 0
AIR   01/10/2026 00:00 -8
AIR   01/10/2026 06:00 -8

[JUNCTIONS]
J1  10.0  3.0  0.0  0.0  0.0

[OUTFALLS]
O1  9.0  FREE

[CONDUITS]
C1  J1  O1  100.0  0.013  0  0

[XSECTIONS]
C1  CIRCULAR  1.0  0  0  0  1
"""

FROZEN = (0.15, 1.5, 2.4e6, 0.30, 8.0, 3.0, -4.0, 11.0, 0.25, 250.0)


@pytest.fixture
def solver(tmp_path):
    path = tmp_path / "frozen.inp"
    path.write_text(MODEL)
    s = Solver(path, tmp_path / "frozen.rpt", tmp_path / "frozen.out")
    s.open()
    yield s
    s.destroy()


def test_rows_are_authored_and_read_back(solver):
    assert solver.frozen_ground_rows() == {}
    solver.set_frozen_ground("*", FROZEN)
    solver.set_frozen_ground("S1", FROZEN[:6] + (2.0,) + FROZEN[7:])
    rows = solver.frozen_ground_rows()
    assert list(rows) == ["*", "S1"]
    assert rows["*"] == FROZEN
    assert rows["S1"][6] == 2.0
    solver.remove_frozen_ground("S1")
    assert list(solver.frozen_ground_rows()) == ["*"]
    with pytest.raises(EngineError):
        solver.remove_frozen_ground("S1")
    with pytest.raises(EngineError):
        solver.set_frozen_ground("S1", (-1.0,) + FROZEN[1:])
    with pytest.raises(ValueError):
        solver.set_frozen_ground("S1", FROZEN[:4])
    # The mesh section takes only the default row.
    with pytest.raises(EngineError):
        solver.set_frozen_ground("S1", FROZEN, mesh=True)
    solver.set_frozen_ground("*", FROZEN, mesh=True)
    assert solver.frozen_ground_rows(mesh=True) == {"*": FROZEN}


def test_a_subcatchment_without_a_row_reports_none(solver):
    solver.initialize()
    assert solver.subcatchments[0].frozen_ground is None


def test_frozen_ground_reports_its_state_and_takes_no_water(solver):
    solver.set_frozen_ground("S1", FROZEN)
    solver.initialize()
    temperature, frozen, factor = solver.subcatchments[0].frozen_ground
    assert (temperature, frozen, factor) == (-4.0, 1.0, 0.0)
    solver.start()
    rain = infiltration = 0.0
    for _ in solver.steps():
        rain = max(rain, solver.subcatchments[0].rainfall)
        infiltration = max(infiltration, solver.subcatchments[0].infil)
    temperature, frozen, factor = solver.subcatchments[0].frozen_ground
    assert temperature < 0.0
    assert (frozen, factor) == (1.0, 0.0)
    assert rain > 0.0
    assert infiltration == 0.0
