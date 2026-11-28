"""UEB configuration, independent mesh assignments, and accepted SI results."""
import math
from pathlib import Path
import pytest
from openswmm.engine import Solver, StaleObjectError

DECK = """[OPTIONS]
FLOW_UNITS CMS
FLOW_ROUTING DYNWAVE
IGNORE_GROUNDWATER YES
IGNORE_QUALITY YES
START_DATE 01/01/2026
END_DATE 01/01/2026
END_TIME 01:00:00
WET_STEP 00:05:00
DRY_STEP 00:05:00
ROUTING_STEP 30
REPORT_STEP 00:05:00
[TEMPERATURE]
TIMESERIES air
SNOWMELT_MODEL UEB
SITE 45 -120 -8 1000
SHORTWAVE TIMESERIES sw
LONGWAVE TIMESERIES lw
[RAINGAGES]
R INTENSITY 0:05 1 TIMESERIES rain
[TIMESERIES]
rain 01/01/2026 00:00 0
rain 01/01/2026 02:00 0
air 01/01/2026 00:00 8
air 01/01/2026 02:00 8
sw 01/01/2026 00:00 400
sw 01/01/2026 02:00 400
lw 01/01/2026 00:00 300
lw 01/01/2026 02:00 300
[SNOWPACKS]
SP PLOWABLE .5 1.5 0 .1 150 0 0
SP IMPERVIOUS .5 1.5 0 .1 150 0 150
SP PERVIOUS .5 1.5 0 .1 150 0 150
SP REMOVAL 0 0 0 0 0 0
[SUBCATCHMENTS]
S R J 1 50 100 .5 0 SP
[SUBAREAS]
S .01 .1 .5 .5 25 OUTLET
[INFILTRATION]
S 75 12 4 7 0
[JUNCTIONS]
J 1 3
[OUTFALLS]
O 0 FREE NO
[CONDUITS]
C J O 100 .013 0 0 0
[XSECTIONS]
C CIRCULAR 1 0 0 0
"""

def opened(tmp_path):
    inp=tmp_path/'snow.inp'; inp.write_text(DECK)
    s=Solver(inp,tmp_path/'snow.rpt',tmp_path/'snow.out'); s.open()
    return s

def test_snow_edit_and_generation_lifetime(tmp_path):
    s=opened(tmp_path)
    try:
        snow=s.snow
        assert snow.model=='UEB'
        snow.step_seconds=1800
        assert snow.step_seconds==1800
        snow.monthly_ranges=[10]*12
        assert snow.monthly_ranges==[10]*12
        before=snow.pack('SP')
        with pytest.raises(ValueError):snow.set_pack('SP',unknown=2)
        assert snow.pack('SP')==before
        snow.set_pack('SP',model=1,forest_fraction=.25)
        assert snow.pack('SP')['forest_fraction']==.25
        snow.set_pack('SP', {'forest_fraction': .35})
        assert snow.pack('SP')['forest_fraction']==.35
        with pytest.raises(ValueError):snow.model='unknown'
        with pytest.raises(ValueError):snow.apply_configuration('SNOW_STEP -2')
        assert snow.step_seconds==1800
        mesh=s.surface2d.snow
        mesh.apply_configuration(options='SNOWMELT NO',defaults='* SNOWPACK SP')
        old=mesh.configuration()
        with pytest.raises(ValueError):mesh.apply_configuration(options='SNOWMELT invalid')
        assert mesh.configuration()==old
        s.close()
        with pytest.raises(StaleObjectError):_ = snow.model
    finally:s.close()

def test_snow_accepted_state_and_report_balance(tmp_path):
    s=opened(tmp_path)
    try:
        s.initialize(); s.start()
        initial=s.snow.state('S')
        for _ in s.steps():pass
        s.end();s.report()
        state=s.snow.state('S')
        assert state['swe_m']<=initial['swe_m']
        assert state['energy_kjm2']!=initial['energy_kjm2']
        assert math.isfinite(state['energy_kjm2'])
        book=s.snow.ledger()
        assert abs(book['residual_m3'])<1e-8
        assert abs(book['energy_residual_kj'])<1e-5
        report=(tmp_path/'snow.rpt').read_text()
        assert 'Snow Process Water and Energy Balance' in report
        assert 'Subcatchment UEB water residual' in report
    finally:s.close()
