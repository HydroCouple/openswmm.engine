"""Writable physical junction storage: openswmm.engine#10 / USEPA#200."""
from pathlib import Path
import pytest
from openswmm.engine import Solver, BadParamError
from openswmm.legacy.engine import Solver as LegacySolver, SWMMObjects, SWMMNodeProperties

ROOT = Path(__file__).resolve().parents[3] / "tests/verification/junction_storage_issue10/runs"

@pytest.mark.parametrize("units", ["CFS", "LPS"])
def test_storage_area_setter_in_both_python_apis(units):
    ROOT.mkdir(parents=True, exist_ok=True)
    path = ROOT / f"python-area-{units}.inp"
    path.write_text(f"""[OPTIONS]
FLOW_UNITS {units}
FLOW_ROUTING DYNWAVE
START_DATE 01/01/2026
END_DATE 01/01/2026
END_TIME 00:10:00
[JUNCTIONS]
J1 0 10 0 0 0
[OUTFALLS]
O1 0 FREE NO
""")
    s = Solver(str(path))
    try:
        s.open()
        s.nodes["J1"].storage_area = 0.125
        assert s.nodes["J1"].storage_area == 0.125
        s.nodes["J1"].storage_area = 0.25
        assert s.nodes["J1"].storage_area == 0.25
        with pytest.raises(BadParamError):
            s.nodes["J1"].storage_area = -1
        with pytest.raises(BadParamError):
            s.nodes["J1"].storage_area = float("nan")
    finally:
        s.close()
        s.destroy()
    with LegacySolver(str(path)) as s:
        s.set_value(SWMMObjects.NODE, SWMMNodeProperties.STORAGE_AREA, "J1", 0.125)
        assert s.get_value(SWMMObjects.NODE, SWMMNodeProperties.STORAGE_AREA, "J1") == 0.125
