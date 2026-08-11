"""2D SPECIFIED_STAGE boundary — timeseries timing and unit regression tests.

Two historical defects are pinned here:

1. Time base: ``resolveBoundaryValues`` looked tables up with ELAPSED SECONDS
   while timeseries x-values are absolute OADate days, so a TS_STAGE boundary
   clamped to the series' first value for the whole run (elapsed seconds stay
   below ~46k OADate days for the first ~12.8 h of a 2026-dated sim).

2. Units: constant and TS stage heads parsed from the file are authored in
   project display units (feet for US FLOW_UNITS) but were applied raw as SI
   metres, overstating a US stage boundary by 3.28x.
"""
import os

import pytest

from openswmm.engine import Solver


def _mesh_sections(size: float) -> str:
    """Two right triangles tiling a flat square bed at z=0."""
    s = size
    return f"""
[2D_VERTICES]
;; X    Y    Z
0.0     0.0   0.0
{s}     0.0   0.0
{s}     {s}   0.0
0.0     {s}   0.0

[2D_TRIANGLES]
;; V1  V2  V3  MANNINGS_N
0      1   2   0.03
0      2   3   0.03
"""


def _options(flow_units: str, hours: int) -> str:
    return f"""[OPTIONS]
FLOW_UNITS           {flow_units}
FLOW_ROUTING         DYNWAVE
START_DATE           01/01/2026
START_TIME           00:00:00
END_DATE             01/01/2026
END_TIME             0{hours}:00:00
REPORT_STEP          00:05:00
ROUTING_STEP         5

[2D_OPTIONS]
MAX_TIMESTEP         5
DRY_DEPTH            0.001

[JUNCTIONS]
J1      0.0         3.0

[OUTFALLS]
O1      -1.0        FREE

[CONDUITS]
C1      J1  O1  10  0.013  0  0

[XSECTIONS]
C1      CIRCULAR  0.5  0  0  0
"""


def _run_sampling(inp_path, rpt_path, out_path, sample_hours, max_hours):
    """Run the model, returning {hour: depth_of_triangle_0}."""
    sv = Solver(str(inp_path), str(rpt_path), str(out_path))
    sv.open()
    sv.initialize()
    sv.start()
    td = sv.surface2d
    assert td.is_active, "2D surface inactive — mesh sections not parsed"

    wanted = sorted(sample_hours)
    got = {}
    while True:
        dt = sv.step()
        h = sv.elapsed.total_seconds() / 3600.0
        while wanted and h >= wanted[0]:
            got[wanted[0]] = float(td.get_depths()[0])
            wanted.pop(0)
        if not dt or dt.total_seconds() <= 0 or h >= max_hours:
            break
    sv.end()
    sv.close()
    return got


def test_ts_stage_tracks_series_timing(tmp_path):
    """SI project: a 0.5 -> 2.0 m ramp over 2 h must be tracked, not frozen
    at the first value (the elapsed-seconds-vs-OADate-days bug)."""
    inp = tmp_path / "ts_stage_si.inp"
    inp.write_text(
        "[TITLE]\nTS stage timing\n\n"
        + _options("CMS", 3)
        + """
[TIMESERIES]
;;Name    Time  Value  (decimal hours, relative)
TideTS    0.0   0.5
TideTS    1.0   1.0
TideTS    2.0   2.0
TideTS    3.0   2.0
"""
        + _mesh_sections(10.0)
        + """
[2D_BOUNDARY_CONDITIONS]
;; TRI  EDGE  TYPE      PARAM_1  PARAM_2  GROUP
   0    0     TS_STAGE  TideTS   *        *
"""
    )
    got = _run_sampling(inp, tmp_path / "r.rpt", tmp_path / "r.out",
                        sample_hours=[0.5, 1.0, 1.5, 2.5], max_hours=2.6)

    # Flat bed at z=0 => boundary-cell depth ~= stage. Generous tolerances:
    # the boundary cell equilibrates within a few steps of each lookup.
    assert got[0.5] == pytest.approx(0.75, abs=0.10)
    assert got[1.0] == pytest.approx(1.00, abs=0.10)
    assert got[1.5] == pytest.approx(1.50, abs=0.10)
    assert got[2.5] == pytest.approx(2.00, abs=0.10)
    # And explicitly: NOT frozen at the first series value.
    assert got[2.5] > 1.5


def test_stage_head_scaled_for_us_units(tmp_path):
    """US project (CFS): stage authored in feet must land as 0.3048x metres
    on the SI solver datum — both the constant and the TS form."""
    inp = tmp_path / "ts_stage_us.inp"
    inp.write_text(
        "[TITLE]\nUS stage units\n\n"
        + _options("CFS", 2)
        + """
[TIMESERIES]
TideTS    0.0   2.0
TideTS    3.0   2.0
"""
        + _mesh_sections(32.808)  # 10 m square authored in feet
        + """
[2D_BOUNDARY_CONDITIONS]
;; TRI  EDGE  TYPE             PARAM_1  PARAM_2  GROUP
   0    0     TS_STAGE         TideTS   *        *
   1    1     SPECIFIED_STAGE  2.0      *        *
"""
    )
    sv = Solver(str(inp), str(tmp_path / "r.rpt"), str(tmp_path / "r.out"))
    sv.open()
    sv.initialize()
    sv.start()
    td = sv.surface2d
    assert td.is_active
    while True:
        dt = sv.step()
        if not dt or dt.total_seconds() <= 0 \
                or sv.elapsed.total_seconds() >= 3600:
            break
    depths = [float(d) for d in td.get_depths()]
    sv.end()
    sv.close()

    # 2 ft = 0.6096 m of stage over a z=0 bed. The unscaled bug gave ~2.0 m.
    for d in depths:
        assert d == pytest.approx(0.6096, abs=0.08), (
            f"stage not ft->m scaled: depths={depths}")


def _run_depths(inp_path, tmp_path, hours):
    sv = Solver(str(inp_path), str(tmp_path / "r.rpt"), str(tmp_path / "r.out"))
    sv.open()
    sv.initialize()
    sv.start()
    td = sv.surface2d
    assert td.is_active
    while True:
        dt = sv.step()
        if not dt or dt.total_seconds() <= 0 \
                or sv.elapsed.total_seconds() >= hours * 3600:
            break
    depths = [float(d) for d in td.get_depths()]
    sv.end()
    sv.close()
    return depths


def test_specified_flow_scaled_for_us_units(tmp_path):
    """US project (CFS): a SPECIFIED_FLOW discharge authored as display flow
    units per metre must convert CFS -> m3/s. Inflow of -0.3531 CFS/m on a
    10 m edge = 0.1 m3/s onto a walled 100 m2 square -> depth 3.6 m after
    1 h. The unscaled bug read it as -0.3531 m3/s/m -> ~12.7 m."""
    inp = tmp_path / "flow_us.inp"
    inp.write_text(
        "[TITLE]\nUS flow units\n\n"
        + _options("CFS", 2)
        + _mesh_sections(32.808)  # 10 m square authored in feet
        + """
[2D_BOUNDARY_CONDITIONS]
;; TRI  EDGE  TYPE            PARAM_1   PARAM_2  GROUP
   0    0     SPECIFIED_FLOW  -0.35315  *        *
"""
    )
    depths = _run_depths(inp, tmp_path, 1.0)
    vol = sum(depths) * 50.0
    assert vol == pytest.approx(360.0, rel=0.05), (
        f"flow not CFS->CMS scaled: vol={vol} depths={depths}")


def test_rating_curve_axes_scaled_for_us_units(tmp_path):
    """US project: a rating curve is authored display-units on BOTH axes —
    stage x in feet, discharge y in CFS per metre. With 0.1 m3/s inflow the
    outlet balances where y(h_ft) = 0.3531 CFS/m, i.e. h = 2 ft = 0.61 m.
    Unscaled axes drain far harder and settle much shallower."""
    inp = tmp_path / "rc_us.inp"
    inp.write_text(
        "[TITLE]\nUS rating curve\n\n"
        + _options("CFS", 2)
        + """
[CURVES]
;;Name   Type     X    Y
OutRC    RATING   0.0  0.0
OutRC             1.0  0.17657
OutRC             3.0  0.70629
"""
        + _mesh_sections(32.808)
        + """
[2D_BOUNDARY_CONDITIONS]
;; TRI  EDGE  TYPE            PARAM_1   PARAM_2  GROUP
   0    0     SPECIFIED_FLOW  -0.35315  *        *
   1    1     RATING_CURVE    OutRC     *        *
"""
    )
    depths = _run_depths(inp, tmp_path, 2.0)
    # Outlet-cell stage settles near the 2 ft (0.61 m) balance point; the
    # boundary-cell/outlet-cell gradient spreads the pair around it.
    mean_depth = sum(depths) / len(depths)
    assert mean_depth == pytest.approx(0.61, abs=0.20), (
        f"rating-curve axes not display-unit scaled: depths={depths}")
