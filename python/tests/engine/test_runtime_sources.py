"""Conservative runtime exchange gates, using rebuilt C API through Python."""
from dataclasses import replace
import numpy as np
import pytest
try:
    from openswmm.engine._coupling import RuntimeSource
except ImportError as exc:
    pytest.skip(f'requires compiled engine with runtime 2D coupling: {exc}',allow_module_level=True)
from openswmm.engine import (Solver, CellScope, GroundwaterLedger, GroundwaterZone,
                            GroundwaterInitialQuality, LifecycleError, HotStartError,
                            BadIndexError, BadParamError)
from tests.engine.test_runtime_coupling_r0 import SURFACE_MODEL
from tests.engine._groundwater_cases import MODEL as GW_MODEL

@pytest.fixture
def model(tmp_path):
    live=[]
    def create(*, groundwater=False, heat=False, msx=False, closure='CLOSED_FORM', momentum='LOCAL_INERTIAL', units='CMS', age=False, standalone=False):
        text=GW_MODEL.replace('0.03 0.1','0.03 0') if groundwater else SURFACE_MODEL
        text=text.replace('FLOW_UNITS CMS','FLOW_UNITS '+units).replace('[2D_OPTIONS]','[2D_OPTIONS]\nMOMENTUM_EQUATION '+momentum)
        if standalone:
            a=text.index('[JUNCTIONS]'); b=text.index('[2D_OPTIONS]'); text=text[:a]+text[b:]
        if age: text=text.replace('[OPTIONS]','[OPTIONS]\nWATER_AGE ON')
        if heat: text=text.replace('[OPTIONS]','[OPTIONS]\nHEAT_TRANSPORT YES')
        path=tmp_path/f'model{len(live)}.inp'
        if msx:
            rxn=path.with_suffix('.rxn')
            rxn.write_text('[REACTION_OPTIONS]\nRATE_UNITS SEC\n'
                           '[REACTION_SPECIES]\nBULK Tracer MG\n'
                           '[REACTION_PIPES]\nRATE Tracer 0\n'
                           '[REACTION_TANKS]\nRATE Tracer 0\n')
            text+='\n[PROCESS_COMPONENTS]\norg.hydrocouple.openswmm.reactions config="'+rxn.name+'"\n'
        path.write_text(text)
        s=Solver(path,path.with_suffix('.rpt'),path.with_suffix('.out'));live.append(s);s.open()
        if groundwater:
            gw=s.groundwater2d
            gw.options['NODE_ENROLMENT']='ROWS'
            gw.options['CLOSURE']=closure
            gw.add_row(CellScope.GLOBAL,36,0.5,0.45,0.1,2)
            gw.set_row_property(0,'HG0',0.2)
            gw.transport.options['TRANSPORT_POLLUTANTS']='YES'
            if heat: gw.transport.options['TRANSPORT_TEMPERATURE']='YES'
            if msx: gw.transport.options['TRANSPORT_MSX']='YES'
        s.initialize();s.start();return s
    yield create
    for s in live:
        s.close();s.destroy()

def test_surface_replace_clear_bulk_and_exact_clock(model):
    s=model();sw=s.surface2d
    s.advance_to(2.375)
    sw.set_water_source([0,1],[0.01,0.02],concentrations={'TSS':7})
    s.advance_to(13.625)
    assert s.elapsed.total_seconds()==13.625
    assert sw.total_volume==pytest.approx(0.03*11.25,rel=1e-10)
    np.testing.assert_allclose(sw.concentrations(sw.species.index('TSS')),7,rtol=1e-10)
    assert sw.source_receipt(cell=0).applied_m3==pytest.approx(.1125)
    sw.set_water_source([0,1],[0.02,0.03],concentrations={'TSS':7})
    s.advance_to(17.875)
    sw.clear_source()
    v=sw.total_volume;s.advance_to(23.125)
    assert sw.total_volume==pytest.approx(v,abs=1e-12)
    assert sw.source_receipt(cell=0).applied_m3==pytest.approx(.1125+.02*4.25)

@pytest.mark.parametrize('expiry',[0,-1,float('nan'),float('inf')])
def test_python_expiry_is_not_confused_with_the_c_hold_sentinel(model,expiry):
    s=model()
    with pytest.raises(ValueError,match='until_seconds'):
        s.surface2d.set_water_source(0,.001,until_seconds=expiry)
    assert s.elapsed.total_seconds()==0

def test_runtime_prescriptions_explicitly_reject_incomplete_restart(model,tmp_path):
    from openswmm.engine import HotStart
    s=model()
    s.surface2d.set_water_source(0,.001)
    s.advance_to(1.25)
    with pytest.raises(HotStartError,match='runtime|Runtime'):
        HotStart.save_from(s,tmp_path/'runtime.bin')
    assert not (tmp_path/'runtime.bin').exists()
    s.close();s.open();s.initialize();s.start()
    HotStart.save_from(s,tmp_path/'cold.bin')
    assert (tmp_path/'cold.bin').exists()

@pytest.mark.parametrize('domain',['surface','groundwater'])
def test_runtime_msx_species_are_applied_after_start(model,domain):
    s=model(groundwater=domain=='groundwater',msx=True)
    view=s.surface2d if domain=='surface' else s.groundwater2d
    assert 'Tracer' in view.species
    s.advance_to(2.375)
    view.set_water_source(0,.02,concentrations={'Tracer':9})
    s.advance_to(7.625)
    receipt=view.source_receipt()
    assert receipt.applied_species['Tracer']==pytest.approx(.02*9*5.25)
    row=view.species.index('Tracer')
    if domain=='surface':
        assert view.concentrations(row)[0]==pytest.approx(9)
    else:
        assert view.concentrations(GroundwaterZone.SAT,row)[0]>0

def test_two_domain_example_runs_at_exact_exchange_endpoints(model,tmp_path,capsys,monkeypatch):
    import json,runpy
    from pathlib import Path
    s=model(groundwater=True,heat=True)
    authored=tmp_path/'coupled.inp'
    s.write(authored)
    example=Path(__file__).resolve().parents[2]/'examples/runtime_2d_coupling.py'
    monkeypatch.setattr('sys.argv',[str(example),str(authored),'--until','6.25',
                                  '--interval','2.375','--temperature','12','--heat-w','100'])
    runpy.run_path(str(example),run_name='__main__')
    records=[json.loads(line) for line in capsys.readouterr().out.splitlines()]
    assert [r['elapsed_seconds'] for r in records]==[2.375,4.75,6.25]
    for domain in ('surface','groundwater'):
        assert sum(r['exchange_m3'][domain] for r in records)==pytest.approx(.00625)

@pytest.mark.parametrize('closure',['CLOSED_FORM','ENSLAVED','SIGMA'])
def test_groundwater_runtime_conservation(model,closure):
    s=model(groundwater=True,closure=closure);gw=s.groundwater2d
    assert gw is s.surface2d.groundwater
    s.advance_to(7.125);v0=gw.ledger(GroundwaterLedger.STORAGE)
    gw.set_water_source(0,.001,concentrations={'TSS':12},until_seconds=19.625)
    s.advance_to(30.25)
    r=gw.source_receipt()
    assert r.applied_m3==pytest.approx(.0125,rel=1e-10)
    assert gw.ledger(GroundwaterLedger.STORAGE)-v0==pytest.approx(r.applied_m3,abs=1e-10)
    assert r.applied_species['TSS']==pytest.approx(.15,rel=1e-10)
    assert abs(gw.continuity_error)<1e-9

@pytest.mark.parametrize('domain',['surface','groundwater'])
def test_expiry_steps_and_callback_update(model,domain):
    s=model(groundwater=domain=='groundwater')
    view=s.surface2d if domain=='surface' else s.groundwater2d
    seen=[]
    def begin(t,dt):
        if not seen:
            view.set_water_source(0,.002,until_seconds=3.125)
            seen.append(True)
    s.set_step_begin_callback(begin)
    while s.elapsed.total_seconds()<8: s.step()
    assert view.source_receipt().applied_m3==pytest.approx(.002*3.125,rel=1e-10)

@pytest.mark.parametrize('domain',['surface','groundwater'])
def test_invalid_batch_is_atomic(model,domain):
    s=model(groundwater=True);s.advance_to(1.25)
    view=s.surface2d if domain=='surface' else s.groundwater2d
    source=RuntimeSource('test',domain,0,.001,{'TSS':3})
    s.coupling.set_sources([source])
    with pytest.raises(BadIndexError):
        s.coupling.set_sources([replace(source,flow_m3_s=.8),replace(source,cell=99)])
    assert s.elapsed.total_seconds()==1.25
    s.advance_to(6.25)
    assert view.source_receipt('test').applied_m3==pytest.approx(.005)

@pytest.mark.parametrize('domain',['surface','groundwater'])
def test_withdrawal_is_limited_and_preserves_mass_receipts(model,domain):
    s=model(groundwater=domain=='groundwater')
    view=s.surface2d if domain=='surface' else s.groundwater2d
    s.advance_to(1.25)
    view.set_water_source(0,.01,concentrations={'TSS':11})
    s.advance_to(6.25)
    view.clear_source()
    view.set_water_source(0,-100,source_id='pump')
    s.advance_to(16.25)
    r=view.source_receipt('pump')
    assert r.requested_m3==pytest.approx(-1000)
    assert -1000<r.applied_m3<0
    assert r.rejected_m3<0
    assert abs(r.applied_species['TSS'])<=.55+1e-9
    if domain=='surface':
        assert view.total_volume>=0
        assert abs(r.applied_species['TSS'])==pytest.approx(.55,abs=1e-10)

@pytest.mark.parametrize('domain',['surface','groundwater'])
def test_water_heat_and_independent_species(model,domain):
    s=model(groundwater=domain=='groundwater',heat=True)
    view=s.surface2d if domain=='surface' else s.groundwater2d
    s.advance_to(1.25)
    view.set_water_source(0,.1,concentrations={'__TEMPERATURE__':10,'TSS':2})
    view.set_heat_source(0,4186)
    view.set_species_source(0,{'TSS':.003})
    s.advance_to(11.25)
    assert view.source_receipt('heat').applied_heat_j==pytest.approx(41860)
    assert view.source_receipt('species').applied_species['TSS']==pytest.approx(.03)
    if domain=='surface':
        vol=view.get_depths()*np.array([view.get_triangle_area(i) for i in range(2)])
        assert np.sum(view.concentrations(view.species.index('TSS'))*vol)==pytest.approx(2.03,abs=1e-10)
        assert np.sum(view.concentrations(view.species.index('__TEMPERATURE__'))*vol)==pytest.approx(10.01,abs=1e-10)

def test_disabled_heat_and_dry_heat_rejection(model):
    s=model()
    with pytest.raises(ValueError,match='temperature transport'):s.surface2d.set_heat_source(0,10)
    warm=model(heat=True)
    warm.surface2d.set_heat_source(0,100)
    warm.advance_to(5.125)
    r=warm.surface2d.source_receipt('heat')
    assert r.requested_heat_j==pytest.approx(512.5)
    assert r.applied_heat_j==0
    assert r.rejected_heat_j==pytest.approx(512.5)

def test_callback_cannot_reenter_clock(model):
    s=model(); errors=[]
    def begin(*_):
        with pytest.raises(LifecycleError): s.advance_to(10)
        errors.append(True)
    s.set_step_begin_callback(begin);s.step();assert errors

@pytest.mark.parametrize('domain',['surface','groundwater'])
def test_runtime_flow_boundary_quality_and_clear(model,domain):
    s=model(groundwater=domain=='groundwater',heat=True)
    v=s.surface2d if domain=='surface' else s.groundwater2d
    s.advance_to(1.375)
    before=v.total_volume if domain=='surface' else v.ledger(GroundwaterLedger.STORAGE)+s.surface2d.total_volume
    v.set_flow_boundary(0,0,-.01,{'TSS':8,'__TEMPERATURE__':12})
    s.advance_to(11.375)
    after=v.total_volume if domain=='surface' else v.ledger(GroundwaterLedger.STORAGE)+s.surface2d.total_volume
    assert after-before==pytest.approx(.1,abs=1e-9)
    if domain=='groundwater':
        r=v.boundary_receipt(0,0)
        assert r.applied_species['TSS']==pytest.approx(.8,rel=1e-9)
        assert r.applied_species['__TEMPERATURE__']==pytest.approx(1.2,rel=1e-9)
    else:
        assert v.get_edge_bc_cum_flux(0,0)==pytest.approx(-.1,abs=1e-9)
        assert v.boundary_flow(0,0)==pytest.approx(-.01,abs=1e-9)
    v.clear_boundary(0,0)
    s.advance_to(16.375)
    assert v.boundary_flow(0,0)==0
    final=v.total_volume if domain=='surface' else v.ledger(GroundwaterLedger.STORAGE)+s.surface2d.total_volume
    assert final==pytest.approx(after,abs=1e-9)
    if domain=='surface':
        balance=v.get_mass_balance()
        assert balance['boundary_in']==pytest.approx(.1,abs=1e-9)
        assert balance['boundary_out']==pytest.approx(0,abs=1e-12)
        assert abs(balance['continuity_error'])<1e-9

@pytest.mark.parametrize('closure',['CLOSED_FORM','ENSLAVED','SIGMA'])
def test_groundwater_head_boundary_reversal_conserves(model,closure):
    s=model(groundwater=True,closure=closure);g=s.groundwater2d
    s.advance_to(.125)
    initial=g.ledger(GroundwaterLedger.STORAGE)
    g.set_head_boundary(0,0,-.1,{'TSS':4})
    s.advance_to(.625)
    first=g.boundary_receipt(0,0)
    assert first.applied_m3>0
    assert first.applied_species['TSS']==pytest.approx(first.applied_m3*4,abs=1e-12)
    assert g.ledger(GroundwaterLedger.STORAGE)-initial==pytest.approx(first.applied_m3,abs=1e-10)
    g.set_head_boundary(0,0,-.49)
    s.advance_to(1.125)
    second=g.boundary_receipt(0,0)
    assert second.applied_m3<first.applied_m3
    assert g.ledger(GroundwaterLedger.STORAGE)-initial==pytest.approx(second.applied_m3,abs=1e-10)

def test_invalid_boundary_does_not_open_wall(model):
    s=model(heat=True);sw=s.surface2d
    with pytest.raises(BadParamError):sw.set_flow_boundary(0,0,-.1,{'TSS':float('nan')})
    s.advance_to(5.125)
    assert sw.total_volume==0

def test_clear_restores_authored_stage_series(tmp_path):
    from tests.engine.test_runtime_coupling_r0 import _open,_close
    s=_open('restored_tide',SURFACE_MODEL+'''[TIMESERIES]
Tide 0.0 0.5
Tide 3.0 0.5
[2D_BOUNDARY_CONDITIONS]
0 0 TS_STAGE Tide * *
''')
    try:
        s.initialize();s.start();s.advance_to(900)
        sw=s.surface2d;sw.set_head_boundary(0,0,.1)
        s.advance_to(1800)
        assert sw.get_depths().mean()==pytest.approx(.1,abs=.05)
        sw.clear_boundary(0,0);s.advance_to(3000)
        assert sw.get_depths().mean()==pytest.approx(.5,abs=.05)
    finally:_close(s)

def test_exchange_callback_reads_live_clock(model):
    s=model();times=[]
    def begin(*_):times.append(s.elapsed.total_seconds())
    s.set_step_begin_callback(begin);s.advance_to(13.125)
    assert times[0]==0 and times[-1]>0
    assert all(a<b for a,b in zip(times,times[1:]))
    assert s.elapsed.total_seconds()==13.125

def test_independent_dry_species_survives_rewetting(model):
    s=model();sw=s.surface2d
    sw.set_species_source(0,{'TSS':.02},until_seconds=10)
    s.advance_to(10)
    assert sw.source_receipt('species').applied_species['TSS']==pytest.approx(.2)
    sw.set_water_source([0,1],.1)
    s.advance_to(20)
    vol=sw.get_depths()*np.array([sw.get_triangle_area(i) for i in range(2)])
    assert np.sum(sw.concentrations(sw.species.index('TSS'))*vol)==pytest.approx(.2,abs=1e-10)

@pytest.mark.parametrize('domain',['surface','groundwater'])
def test_independent_species_sink_is_limited(model,domain):
    s=model(groundwater=domain=='groundwater');v=s.surface2d if domain=='surface' else s.groundwater2d
    v.set_species_source(0,{'TSS':.02},until_seconds=10)
    s.advance_to(10)
    v.set_species_source(0,{'TSS':-100},source_id='remove',until_seconds=20)
    s.advance_to(20)
    r=v.source_receipt('remove')
    assert r.requested_species['TSS']==pytest.approx(-1000)
    assert r.applied_species['TSS']==pytest.approx(-.2,abs=1e-10)
    assert r.applied_m3==0

def test_groundwater_authored_well_override_and_restore(tmp_path):
    from tests.engine.test_runtime_coupling_r0 import _start_groundwater,_close
    s=_start_groundwater('well_runtime_override',source_flow=.0001)
    try:
        g=s.groundwater2d;s.advance_to(10)
        before=g.ledger(GroundwaterLedger.SOURCE_IN)
        g.set_water_source(0,.0002,source_id='well')
        s.advance_to(20)
        assert g.ledger(GroundwaterLedger.SOURCE_IN)-before==pytest.approx(.002,abs=1e-10)
        g.clear_source('well');before=g.ledger(GroundwaterLedger.SOURCE_IN)
        s.advance_to(30)
        assert g.ledger(GroundwaterLedger.SOURCE_IN)-before==pytest.approx(.001,abs=1e-10)
    finally:_close(s)


def test_gross_boundary_ledgers_do_not_cancel(model):
    s=model(heat=True);sw=s.surface2d
    sw.set_water_source([0,1],.1,concentrations={'TSS':3,'__TEMPERATURE__':12})
    s.advance_to(2);sw.clear_source()
    sw.set_flow_boundary(0,0,-.01,{'TSS':3,'__TEMPERATURE__':12})
    sw.set_flow_boundary(0,2,.01)
    s.advance_to(12)
    mb=sw.get_mass_balance()
    assert mb['boundary_in']==pytest.approx(.1)
    assert mb['boundary_out']==pytest.approx(.1)
    assert abs(mb['continuity_error'])<1e-9
    incoming=sw.boundary_receipt(0,0); outgoing=sw.boundary_receipt(0,2)
    assert incoming.applied_m3==pytest.approx(.1)
    assert outgoing.applied_m3==pytest.approx(-.1)
    assert incoming.applied_species['TSS']==pytest.approx(.3)
    assert outgoing.applied_species['TSS']==pytest.approx(-.3)
    assert incoming.applied_heat_j==pytest.approx(1.2*4186000)
    ledger=sw.species_ledger('TSS')
    assert ledger['boundary_in']==pytest.approx(.3)
    assert ledger['boundary_out']==pytest.approx(.3)


def test_atomic_frame_provider_ownership_and_selective_clear(model):
    from openswmm.engine import CouplingFrame,RuntimeBoundary,RuntimeForcing,RuntimeClear
    s=model(groundwater=True);a=s.coupling.for_provider('a');b=s.coupling.for_provider('b')
    a.apply_frame(CouplingFrame(sources=(RuntimeSource('x','surface',0,.01,{'TSS':2}),
                                       RuntimeSource('x','surface',1,.02,{'TSS':2}),
                                       RuntimeSource('x','groundwater',0,.001,{'TSS':2})),
                   boundaries=(RuntimeBoundary('surface',0,0,2,-.001,{'TSS':2}),),
                   forcings=(RuntimeForcing('rainfall',-1,.00001,concentrations={'TSS':2}),)))
    b.surface.set_source(0,.003,source_id='x')
    with pytest.raises(BadParamError):
        b.apply_frame(CouplingFrame(sources=(RuntimeSource('bad','surface',0,1),),
                                  boundaries=(RuntimeBoundary('surface',0,0,2,-1),)))
    with pytest.raises(BadIndexError): b.surface.source_receipt('bad')
    with pytest.raises(BadParamError):s.surface2d.set_flow_boundary(0,0,-1)
    with pytest.raises(BadParamError):s.surface2d.force_rainfall_uniform(.01)
    with pytest.raises(BadParamError):b.surface.set_forcings((RuntimeForcing('rainfall',0,.1),))
    assert s.elapsed.total_seconds()==0
    s.advance_to(5)
    assert [r.applied_m3 for r in a.surface.source_receipts([0,1],'x')]==pytest.approx([.05,.1])
    assert b.surface.source_receipt('x').applied_m3==pytest.approx(.015)
    assert a.groundwater.source_receipt('x').applied_m3==pytest.approx(.005)
    a.surface.clear_source('x',cell=0)
    a.surface.clear_boundary(0,0)
    b.surface.set_boundary(0,0,2,-.001)
    s.advance_to(10)
    assert [r.applied_m3 for r in a.surface.source_receipts([0,1],'x')]==pytest.approx([.05,.2])


def test_rainfall_quality_expiry_and_paired_infiltration(model):
    from openswmm.engine import RuntimeForcing
    s=model(groundwater=True,heat=True);p=s.coupling.for_provider('rain')
    sw=s.surface2d;gw=s.groundwater2d
    initial=sw.total_volume+gw.ledger(GroundwaterLedger.STORAGE)
    p.surface.set_forcings((RuntimeForcing('rainfall',-1,.0001,concentrations={'TSS':7,'__TEMPERATURE__':10},until_seconds=10),))
    s.advance_to(10)
    rain_volume=sum(sw.get_triangle_area(i) for i in range(2))*.0001*10
    assert sw.total_volume+gw.ledger(GroundwaterLedger.STORAGE)-initial==pytest.approx(rain_volume,abs=1e-10)
    assert sw.species_ledger('TSS')['rainfall_in']==pytest.approx(7*rain_volume)
    # Native Kinf is zero in this fixture; the paired override bypasses it.
    p.surface.set_forcings((RuntimeForcing('infiltration',0,.00001,until_seconds=15),))
    s.advance_to(15)
    receipt=p.surface.infiltration_receipt(0)
    assert receipt.applied_m3>0
    assert receipt.applied_species['TSS']==pytest.approx(receipt.applied_m3*7,abs=1e-10)
    assert receipt.applied_heat_j==pytest.approx(receipt.applied_species['__TEMPERATURE__']*4186000)
    assert sw.total_volume+gw.ledger(GroundwaterLedger.STORAGE)-initial==pytest.approx(rain_volume,abs=1e-10)
    s.advance_to(20)
    assert p.surface.infiltration_receipt(0).applied_m3==pytest.approx(receipt.applied_m3)
    assert abs(gw.continuity_error)<1e-8


@pytest.mark.parametrize('momentum',['LOCAL_INERTIAL','DIFFUSIVE_WAVE'])
@pytest.mark.parametrize('units',['CMS','CFS'])
def test_runtime_si_units_cooling_and_age(model,momentum,units):
    s=model(heat=True,age=True,momentum=momentum,units=units);sw=s.surface2d
    sw.set_water_source([0,1],.1,concentrations={'TSS':4,'__TEMPERATURE__':10,'__WATER_AGE__':20})
    sw.set_heat_source([0,1],-418600)
    s.advance_to(5)
    assert sw.total_volume==pytest.approx(1)
    assert sw.source_receipt(cell=0).applied_species['__WATER_AGE__']==pytest.approx(10)
    assert sw.source_receipt('heat',cell=0).applied_heat_j==pytest.approx(-2093000)
    assert sw.species_ledger('__TEMPERATURE__')['storage']==pytest.approx(9,abs=1e-9)
    assert sw.species_ledger('TSS')['storage']==pytest.approx(4)


def test_full_swe_runtime_capabilities_are_explicitly_unsupported(model):
    from openswmm.engine import PluginError
    s=model(momentum='FULL_SWE')
    assert not s.coupling.capabilities['surface_sources']
    assert not s.coupling.capabilities['atomic_frames']
    with pytest.raises(PluginError):s.surface2d.set_water_source(0,.01)
    assert s.elapsed.total_seconds()==0


def test_forcing_expiry_releases_provider_ownership(model):
    from openswmm.engine import RuntimeForcing
    s=model();a=s.coupling.for_provider('first');b=s.coupling.for_provider('second')
    a.surface.set_forcings((RuntimeForcing('rainfall',rate_m_s=.0001,until_seconds=2.5),))
    s.advance_to(2.5)
    b.surface.set_forcings((RuntimeForcing('rainfall',rate_m_s=.0002),))
    before=s.surface2d.total_volume
    s.advance_to(5)
    assert s.surface2d.total_volume-before==pytest.approx(.0002*100*2.5)


def test_many_provider_ids_replace_with_retained_bulk_receipts(model):
    s=model();p=s.coupling.for_provider('large')
    records=tuple(RuntimeSource(f'item{i}','surface',i%2,.000001,{'TSS':2}) for i in range(2000))
    p.set_sources(records);s.advance_to(1)
    assert s.surface2d.total_volume==pytest.approx(.002)
    p.set_sources(tuple(replace(r,flow_m3_s=.000002) for r in records));s.advance_to(2)
    assert s.surface2d.total_volume==pytest.approx(.006)
    assert p.surface.source_receipts([0],'item0')[0].applied_m3==pytest.approx(.000003)
    assert p.surface.species_ledger('TSS')['external_in']==pytest.approx(.012)


def test_surface_only_runtime_engine_has_an_active_routing_clock(model):
    s=model(standalone=True);s.surface2d.set_water_source([0,1],[.01,.02])
    s.advance_to(2.375)
    assert s.elapsed.total_seconds()==2.375
    assert s.surface2d.total_volume==pytest.approx(.03*2.375)
