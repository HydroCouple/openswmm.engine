"""Small mixed-cell model shared by groundwater and surface-quality tests."""
from pathlib import Path

from openswmm.engine import Solver

MODEL = '''[OPTIONS]
FLOW_UNITS CMS
FLOW_ROUTING DYNWAVE
START_DATE 01/01/2026
START_TIME 00:00:00
END_DATE 01/01/2026
END_TIME 00:10:00
REPORT_STEP 00:01:00
WET_STEP 00:01:00
ROUTING_STEP 5
[POLLUTANTS]
TSS MG/L 0 0 0 0
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
[2D_OPTIONS]
INTEGRATOR EXPLICIT
LTS_TIERS 2
MAX_TIMESTEP 5
DRY_DEPTH 0.001
REPORT_2D NO
RAINFALL_MODE SYSTEM
[2D_VERTICES]
0 0 0
10 0 0
10 10 0
0 10 0
20 0 0
[2D_TRIANGLES]
1 4 2 0.03 0.1 FAR
[2D_QUADS]
0 1 2 3 0.03 0.1 PAN
[REPORT]
INPUT NO
'''


def open_model(directory, extra=''):
    path = Path(directory) / 'model.inp'
    path.write_text(MODEL + '\n' + extra)
    solver = Solver(path, path.with_suffix('.rpt'), path.with_suffix('.out'))
    solver.open()
    return solver


def cleanup(solver):
    try:
        solver.close()
    finally:
        solver.destroy()
