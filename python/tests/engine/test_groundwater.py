"""Mixed-cell aquifer authoring, lifecycle, units and snapshot regressions."""
import numpy as np
import pytest
from openswmm.engine import (
    CellScope, GroundwaterClosure, GroundwaterVariable, GroundwaterLedger,
    BadParamError, LifecycleError, StaleObjectError,
)
from tests.engine._groundwater_cases import open_model, cleanup


def test_groundwater_authoring_and_roundtrip(tmp_path):
    solver = open_model(tmp_path)
    try:
        gw = solver.surface2d.groundwater
        assert not gw.active
        assert gw.rows == ()
        gw.options['NODE_ENROLMENT'] = 'ROWS'
        assert gw.options['NODE_ENROLMENT'] == 'ROWS'
        gw.add_row(CellScope.GLOBAL, 36, 0.5, 0.45, 0.1, 2)
        gw.set_row_property(0, 'HG0', 0.2)
        gw.add_row(CellScope.TAG, 18, 0.6, 0.4, 0.1, 1, tag='PAN')
        assert gw.rows[1].tag == 'PAN'
        gw.remove_row(1)
        assert gw.rows[0].ks == 36  # authored mm/hr, not converted to m/s
        assert gw.row_property(0, 'HG0') == 0.2
        gw.add_node('J1', -1, 36, 0.1, 0.5)
        assert gw.nodes[0].locate
        assert not gw.nodes[0].automatic
        gw.set_node_exchange(0, False)
        assert not gw.nodes[0].exchange
        gw.remove_node(0)
        assert gw.nodes == ()
        with pytest.raises(BadParamError):
            gw.set_row_property(0, 'NO_SUCH_PROPERTY', 1)
        saved = tmp_path / 'saved.inp'
        solver.write(saved)
        from openswmm.engine import Solver
        reopened = Solver(saved, tmp_path / 'saved.rpt')
        try:
            reopened.open()
            assert reopened.surface2d.groundwater.rows == gw.rows
            assert reopened.surface2d.groundwater.row_property(0, 'HG0') == 0.2
        finally:
            cleanup(reopened)
        solver.close()
        with pytest.raises(StaleObjectError):
            _ = gw.rows
    finally:
        cleanup(solver)


@pytest.mark.parametrize('sigma', [False, True])
def test_groundwater_runtime_mixed_cells(tmp_path, sigma):
    solver = open_model(tmp_path)
    try:
        gw = solver.surface2d.groundwater
        gw.options['NODE_ENROLMENT'] = 'ROWS'
        gw.add_row(CellScope.GLOBAL, 36, 0.5, 0.45, 0.1, 2)
        gw.set_row_property(0, 'HG0', 0.2)
        if sigma:
            gw.options['M_LAYERS'] = '4'
            gw.set_row_property(0, 'CLOSURE', GroundwaterClosure.SIGMA)
            gw.set_row_property(0, 'M_LAYERS', 4)
        with pytest.raises(LifecycleError):
            gw.cells(GroundwaterVariable.HG)
        solver.initialize()
        solver.start(False)
        solver.step()
        assert gw.active
        assert gw.dimensions[0] == 2  # one triangle plus one quad
        values = gw.cells(GroundwaterVariable.HG)
        assert values.shape == (2,)
        assert values.dtype == np.float64
        assert np.isfinite(values).all()
        assert values[0] == pytest.approx(gw.cell(0, GroundwaterVariable.HG))
        values[:] = -999
        assert gw.cell(0, GroundwaterVariable.HG) >= 0  # independent snapshot
        assert gw.ledger(GroundwaterLedger.STORAGE) >= 0
        assert abs(gw.continuity_error) < 1e-7
        assert gw.tier_histogram.dtype == np.dtype('l')
        assert gw.tier_histogram.ndim == 1
        assert isinstance(gw.species, tuple)
        if sigma:
            assert gw.column(0).shape == (4,)
        else:
            with pytest.raises(BadParamError):
                gw.column(0)
        with pytest.raises(LifecycleError):
            gw.add_row(CellScope.GLOBAL, 1, 1, 0.4, 0.1, 1)
        solver.end()
    finally:
        cleanup(solver)


def test_authored_edges_and_layer_inventory(tmp_path):
    from openswmm.engine import GroundwaterBoundary, GroundwaterInitialQuality, GroundwaterTransportZone
    solver = open_model(tmp_path)
    try:
        gw = solver.surface2d.groundwater
        gw.options['NODE_ENROLMENT'] = 'ROWS'
        gw.options['CLOSURE'] = 'SIGMA'
        gw.options['M_LAYERS'] = '4'
        gw.add_row(CellScope.GLOBAL, 36, 0.5, 0.45, 0.1, 2)
        gw.set_row_property(0, 'HG0', 0.2)
        gw.transport.options['TRANSPORT_POLLUTANTS'] = 'YES'
        gw.transport.options['UNSAT_TRANSPORT'] = 'LAYERED'
        gw.set_authored_boundary(0, 0, 'FLOW', 1e-6)
        gw.transport.set_boundary(GroundwaterBoundary(cell=0, edge=0, species='TSS', value=3))
        gw.transport.set_boundary(GroundwaterBoundary(cell=0, edge=0, species='TSS', kind='MASSFLUX', value=2))
        assert len(gw.transport.boundaries) == 2
        assert gw.authored_boundaries[0]['kind'] == 'FLOW'
        gw.transport.set_initial_quality(GroundwaterInitialQuality(
            zone=GroundwaterTransportZone.LAYER, layer=2, species='TSS', value=7))
        with pytest.raises(LifecycleError):
            gw.layer_quality(0, 1, 0)
        solver.initialize()
        assert gw.layer_quality(0, 1, 0)['dissolved_concentration'] == pytest.approx(7)
        assert gw.layer_quality(0, 0, 0)['dissolved_concentration'] == 0
        solver.start(False)
        for _ in range(100):
            solver.step()
            if gw.authored_boundary_receipt(0)['accepted_in'] > 0:
                break
        assert gw.authored_boundary_receipt(0)['accepted_in'] > 0
        assert gw.authored_boundary_receipt(0, 0)['flux_in'] > 0
        with pytest.raises(LifecycleError):
            gw.remove_authored_boundary(0)
        solver.end()
    finally:
        cleanup(solver)


def test_subsurface_reaction_text_authoring_is_atomic(tmp_path):
    from openswmm.engine import ReactionScope, GroundwaterInitialQuality
    solver = open_model(tmp_path)
    try:
        gw = solver.surface2d.groundwater
        gw.options['NODE_ENROLMENT'] = 'ROWS'
        gw.add_row(CellScope.GLOBAL, 36, 0.5, 0.45, 0.1, 2)
        gw.set_row_property(0, 'HG0', 0.2)
        rx = solver.reactions
        rx.apply_text('[REACTION_SPECIES]\nBULK A MG\nWALL W MG/FT2\n'
                      '[REACTION_TERMS]\nsoil GW_THETA/GW_POROSITY\n'
                      '[REACTION_SUBSURFACE]\n* SAT A RATE -0.001*A*soil\n')
        saved = rx.serialize()
        assert '[REACTION_SUBSURFACE]' in saved
        assert rx.validate('-A*soil', ReactionScope.SUBSURFACE).valid
        assert not rx.validate('-A*HRT', ReactionScope.SUBSURFACE).valid
        with pytest.raises(BadParamError):
            rx.apply_text(saved.replace('-0.001*A*soil', '-A*HRT'))
        assert rx.serialize() == saved
        gw = solver.surface2d.groundwater  # reaction-text replacement invalidates old views
        gw.transport.set_initial_quality(GroundwaterInitialQuality(species='A', value=7))
        solver.initialize()
        assert 'A' in gw.species
    finally:
        cleanup(solver)


@pytest.mark.parametrize('chemistry_layers,thermal_layers', [(5, 3), (3, 7)])
def test_layered_chemistry_with_porous_heat(tmp_path, chemistry_layers, thermal_layers):
    from openswmm.engine import GroundwaterInitialQuality, GroundwaterTransportZone
    solver = open_model(tmp_path, '[OPTIONS]\nHEAT_TRANSPORT YES\n')
    try:
        gw = solver.surface2d.groundwater
        gw.options['NODE_ENROLMENT'] = 'ROWS'
        gw.options['CLOSURE'] = 'SIGMA'
        gw.options['M_LAYERS'] = str(chemistry_layers)
        gw.add_row(CellScope.GLOBAL, 1e-9, 0.5, 0.45, 0.1, 2)
        gw.set_row_property(0, 'HG0', 0.2)
        gw.transport.options['UNSAT_TRANSPORT'] = 'LAYERED'
        gw.transport.options['THERMAL_MODEL'] = 'POROUS_EQUILIBRIUM'
        gw.transport.options['THERMAL_LAYERS'] = str(thermal_layers)
        gw.transport.options['CONDUCTION'] = 'NO'
        for zone in (GroundwaterTransportZone.SAT, GroundwaterTransportZone.UNSAT):
            for species, value in [('TSS', 7), ('__TEMPERATURE__', 20)]:
                gw.transport.set_initial_quality(GroundwaterInitialQuality(
                    zone=zone, species=species, value=value))
        gw.transport.set_initial_quality(GroundwaterInitialQuality(
            zone=GroundwaterTransportZone.LAYER, layer=chemistry_layers,
            species='__TEMPERATURE__', value=-5))
        solver.write(tmp_path / 'layered_porous.inp')
        solver.initialize()
        temperature = gw.species.index('__TEMPERATURE__')
        for cell in range(gw.dimensions[0]):
            for layer in range(chemistry_layers):
                solute = gw.layer_quality(cell, layer, 0)
                assert solute['dissolved_concentration'] == pytest.approx(7)
                heat = gw.layer_quality(cell, layer, temperature)
                assert np.isfinite(heat['dissolved_concentration'])
                assert -5 - 1e-10 <= heat['dissolved_concentration'] <= 20 + 1e-10
        solver.start(False)
        for _ in range(5):
            solver.step()
        assert abs(gw.continuity_error) < 1e-7
        for cell in range(gw.dimensions[0]):
            for layer in range(chemistry_layers):
                assert gw.layer_quality(cell, layer, 0)['total_mass'] >= 0
        solver.end()
    finally:
        cleanup(solver)


@pytest.mark.parametrize('forcing', ['none', 'well', 'edge', 'head'])
@pytest.mark.parametrize('closure', ['CLOSED_FORM', 'ENSLAVED', 'SIGMA'])
def test_water_only_coupled_rk2_advances_through_python(tmp_path, forcing, closure):
    well = """
[TIMESERIES]
PUMP 01/01/2026 00:00:00 .000001
PUMP 01/01/2026 00:00:01 .000003
PUMP 01/01/2026 00:00:02 -.000001
""" if forcing != 'none' else ''
    if forcing == 'well':
        well += '[GW_SOURCES]\nW CELL 1 FLOW PUMP\n'
    elif forcing == 'edge':
        well += '[2D_AQUIFER_BOUNDARIES]\nCELL 1 EDGE 0 FLOW SERIES PUMP\n'
    elif forcing == 'head':
        well = '''[TIMESERIES]
PUMP 01/01/2026 00:00:00 -1.4
PUMP 01/01/2026 00:00:01 -1.2
PUMP 01/01/2026 00:00:02 -1.5
[2D_AQUIFER_BOUNDARIES]
CELL 1 EDGE 0 HEAD SERIES PUMP
'''

    options = f"""
[OPTIONS]
END_DATE 01/01/2026
END_TIME 00:00:02
REPORT_STEP 00:00:01
[2D_OPTIONS]
MOMENTUM_EQUATION FULL_SWE
RECONSTRUCTION_ORDER 2
MAX_TIMESTEP .05
LTS_TIERS 1
TRANSPORT_POLLUTANTS NO
TRANSPORT_MSX NO
TRANSPORT_AGE NO
TRANSPORT_TEMPERATURE NO
[2D_AQUIFER_OPTIONS]
CLOSURE {closure}
NODE_ENROLMENT ROWS
LINK_SEEPAGE NONE
GW_ET NONE
[2D_AQUIFER]
* 18 2 .45 .1 2 HG0 .6 C_LOSS .5
[GW_TRANSPORT_OPTIONS]
TRANSPORT_POLLUTANTS NO
TRANSPORT_MSX NO
TRANSPORT_AGE NO
TRANSPORT_TEMPERATURE NO
""" + well
    from openswmm.engine import Solver
    from tests.engine._groundwater_cases import MODEL
    path = tmp_path / 'model.inp'
    # Set the base INP clock before parsing; repeated OPTIONS sections can
    # retain the original cached duration, and OLE date edits floor seconds.
    path.write_text(MODEL.replace('END_TIME 00:10:00', 'END_TIME 00:00:02')
                    .replace('ROUTING_STEP 5', 'ROUTING_STEP .1') + options)
    solver = Solver(path, path.with_suffix('.rpt'))
    solver.open()
    try:
        solver.initialize()
        solver.start(False)
        while solver.step().total_seconds() > 0:
            pass
        assert solver.elapsed.total_seconds() == 2
        gw = solver.surface2d.groundwater
        assert gw.active and gw.species == ()
        if closure in ('ENSLAVED', 'SIGMA'):
            status = gw.execution_status
            assert status['available_operators'] == 0
            assert status['executed_operators'] == 0
            assert status['accepted_cells'] == 0
            assert status['recharge_backend'] == 'cpu'
        if closure == 'SIGMA':
            for cell in range(gw.dimensions[0]):
                theta = gw.column(cell)
                assert np.isfinite(theta).all()
                assert np.all(theta >= .1 - 1e-13) and np.all(theta <= .45 + 1e-13)
                length = 2 - gw.cell(cell, GroundwaterVariable.HG)
                assert gw.cell(cell, GroundwaterVariable.HU) == pytest.approx(float(np.sum(theta)) * length / len(theta), abs=1e-13)
        assert gw.ledger(GroundwaterLedger.INFIL_IN) > 0
        assert gw.ledger(GroundwaterLedger.DEEP) > 0
        if forcing == 'head':
            assert gw.ledger(GroundwaterLedger.BOUNDARY_IN) > 0
            assert gw.ledger(GroundwaterLedger.BOUNDARY_OUT) > 0
        elif forcing != 'none':
            inward = GroundwaterLedger.SOURCE_IN if forcing == 'well' else GroundwaterLedger.BOUNDARY_IN
            outward = GroundwaterLedger.SOURCE_OUT if forcing == 'well' else GroundwaterLedger.BOUNDARY_OUT
            # Native dated tables use floating-point days. The volume bound
            # includes sub-microsecond calendar rounding at these small flows.
            assert gw.ledger(inward) == pytest.approx(3.125e-6, abs=1e-12)
            assert gw.ledger(outward) == pytest.approx(.125e-6, abs=1e-12)
        assert abs(gw.continuity_error) < 1e-9
        assert np.isfinite(gw.cells(GroundwaterVariable.HG)).all()
        stats = solver.surface2d.run_stats
        assert stats['backend'].startswith('cpu')
        assert stats['steps'] > 0
        solver.end()
    finally:
        cleanup(solver)


@pytest.mark.parametrize('mode', ['cpu', 'omp', 'cuda', 'hip', 'sycl'])
def test_groundwater_execution_status_lifecycle_and_actual_use(tmp_path, monkeypatch, mode):
    monkeypatch.setenv('OPENSWMM_2D_BACKEND', mode)
    solver = open_model(tmp_path, """
[OPTIONS]
END_DATE 01/01/2026
END_TIME 00:00:02
[2D_AQUIFER_OPTIONS]
CLOSURE CLOSED_FORM
NODE_ENROLMENT ROWS
LINK_SEEPAGE NONE
GW_ET NONE
[2D_AQUIFER]
* 36 1 .45 .1 2 HG0 .3
""")
    try:
        gw = solver.surface2d.groundwater
        with pytest.raises(LifecycleError):
            _ = gw.execution_status
        solver.initialize()
        initial = gw.execution_status
        assert initial['active']
        assert initial['executed_operators'] == 0
        assert initial['accepted_cells'] == 0
        if mode in ('cpu', 'cuda', 'hip', 'sycl'):
            assert initial['available_operators'] == 0
            assert initial['recharge_backend'] == 'cpu'
        solver.start(False)
        while solver.step().total_seconds() > 0:
            pass
        status = gw.execution_status
        assert status['host_owned_operations'] == 15
        if initial['available_operators']:
            assert mode == 'omp'
            assert status['recharge_backend'] == 'omp'
            assert status['executed_operators'] == 1
            assert status['accepted_batches'] > 0
            assert status['accepted_cells'] > 0
            assert status['effective_threads'] > 0
        else:
            assert status['executed_operators'] == 0
            assert status['accepted_cells'] == 0
        assert initial['accepted_cells'] == 0  # returned dictionaries are snapshots
        assert abs(gw.continuity_error) < 1e-9
        solver.end()
        solver.close()
        with pytest.raises(StaleObjectError):
            _ = gw.execution_status
    finally:
        cleanup(solver)


def test_openmp_recharge_and_surface_owners_match_sequential_engines(tmp_path, monkeypatch):
    from concurrent.futures import ThreadPoolExecutor
    from threading import Barrier
    from openswmm.engine import Solver
    from tests.engine._groundwater_cases import MODEL

    monkeypatch.setenv('OPENSWMM_2D_BACKEND', 'omp')
    # Both owners share the optional Kokkos runtime but keep separate stores.
    model = MODEL.replace('[2D_QUADS]\n0 1 2 3 0.03 0.1 PAN',
                          '0 1 2 0.03 0.1 PAN\n0 2 3 0.03 0.1 PAN')
    extra = """
[OPTIONS]
END_DATE 01/01/2026
END_TIME 00:00:02
ROUTING_STEP .1
[2D_OPTIONS]
LTS_TIERS 1
MAX_TIMESTEP .001
TRANSPORT_POLLUTANTS NO
TRANSPORT_MSX NO
TRANSPORT_AGE NO
TRANSPORT_TEMPERATURE NO
"""
    aquifer = """
[2D_AQUIFER_OPTIONS]
CLOSURE CLOSED_FORM
NODE_ENROLMENT ROWS
LINK_SEEPAGE NONE
GW_ET NONE
[2D_AQUIFER]
* 36 1 .45 .1 2 HG0 .3
"""
    solvers = []

    def create(name, ground):
        path = tmp_path / (name + '.inp')
        path.write_text(model + extra + (aquifer if ground else ''))
        solver = Solver(path, path.with_suffix('.rpt'))
        solvers.append(solver)
        solver.open()
        solver.initialize()
        return solver

    def snapshot(solver, ground):
        values = [solver.surface2d.get_depths()]
        if ground:
            gw = solver.surface2d.groundwater
            values += [gw.cells(GroundwaterVariable.HG), gw.cells(GroundwaterVariable.HU)]
            assert abs(gw.continuity_error) < 1e-9
            assert gw.execution_status['executed_operators'] == 1
        return values

    try:
        pair = [create('parallel-surface', False), create('parallel-ground', True)]
        if (not pair[0].surface2d.run_stats['backend'].startswith('omp') or
                pair[1].surface2d.groundwater.execution_status['available_operators'] == 0):
            pytest.skip('Optional Kokkos OpenMP plugin unavailable; concurrent host profile unqualified')
        for solver in pair:
            solver.start(False)
        barrier = Barrier(2)

        def advance(solver):
            for _ in range(20):
                barrier.wait(timeout=10)
                solver.step()

        with ThreadPoolExecutor(2) as pool:
            list(pool.map(advance, pair))
        for index, ground in enumerate((False, True)):
            observed = snapshot(pair[index], ground)
            control = create('sequential-' + str(index), ground)
            control.start(False)
            for _ in range(20):
                control.step()
            expected = snapshot(control, ground)
            for actual, reference in zip(observed, expected):
                np.testing.assert_array_equal(actual, reference)
            control.end()
            pair[index].end()
    finally:
        for solver in solvers:
            cleanup(solver)


@pytest.mark.parametrize('closure,layered', [('CLOSED_FORM', False), ('ENSLAVED', False), ('SIGMA', False), ('SIGMA', True)])
@pytest.mark.parametrize('forcing', ['well', 'edge', 'head'])
def test_passive_and_age_coupled_rk2_public_profile(tmp_path, closure, layered, forcing):
    from openswmm.engine import Solver, GroundwaterSpeciesLedger, GroundwaterZone
    from tests.engine._groundwater_cases import MODEL
    if forcing == 'well':
        forcing_rows = '[GW_SOURCES]\nW CELL 1 FLOW PULSE TSS CONC 7 __WATER_AGE__ CONC 2\n'
    else:
        kind = 'HEAD' if forcing == 'head' else 'FLOW'
        forcing_rows = (f'[2D_AQUIFER_BOUNDARIES]\nCELL 1 EDGE 0 {kind} SERIES PULSE\n'
                        '[GW_BOUNDARY_QUALITY]\n1 0 TSS CONC 7\n1 0 __WATER_AGE__ CONC 2\n')
    values = ('-1.1', '-1.8', '-1.1') if forcing == 'head' else ('.00001', '-.00001', '.00001')
    extra = f'''[OPTIONS]
WATER_AGE ON
[2D_OPTIONS]
MOMENTUM_EQUATION FULL_SWE
RECONSTRUCTION_ORDER 2
MAX_TIMESTEP .025
LTS_TIERS 1
TRANSPORT_POLLUTANTS YES
TRANSPORT_MSX NO
TRANSPORT_AGE YES
TRANSPORT_TEMPERATURE NO
[2D_INITIAL_QUALITY]
* TSS 6
[2D_AQUIFER_OPTIONS]
CLOSURE {closure}
M_LAYERS 4
NODE_ENROLMENT ROWS
LINK_SEEPAGE NONE
GW_ET NONE
[2D_AQUIFER]
* 18 2 .45 .1 2 HG0 .6 C_LOSS .5
[GW_TRANSPORT_OPTIONS]
UNSAT_TRANSPORT {'LAYERED' if layered else 'BULK'}
TRANSPORT_POLLUTANTS YES
TRANSPORT_MSX NO
TRANSPORT_AGE YES
TRANSPORT_TEMPERATURE NO
[GW_INITIAL_QUALITY]
* SAT TSS 2
* UNSAT TSS 2
[TIMESERIES]
PULSE 01/01/2026 00:00:00 {values[0]}
PULSE 01/01/2026 00:00:01 {values[1]}
PULSE 01/01/2026 00:00:02 {values[2]}
{forcing_rows}'''
    path = tmp_path / 'passive-rk2.inp'
    path.write_text(MODEL.replace('END_TIME 00:10:00', 'END_TIME 00:00:02').replace('ROUTING_STEP 5', 'ROUTING_STEP .1') + extra)
    solver = Solver(path, path.with_suffix('.rpt'))
    solver.open()
    try:
        solver.initialize()
        surface, gw = solver.surface2d, solver.surface2d.groundwater
        assert surface.species == gw.species == ('TSS', '__WATER_AGE__')
        solver.start(False)
        while solver.step().total_seconds() > 0:
            pass
        assert solver.elapsed.total_seconds() == 2
        for species in range(2):
            assert np.isfinite(surface.concentrations(species)).all()
            assert np.all(surface.concentrations(species) >= 0)
            for zone in (GroundwaterZone.SAT, GroundwaterZone.UNSAT):
                assert np.isfinite(gw.concentrations(zone, species)).all()
                assert np.all(gw.concentrations(zone, species) >= 0)
            assert abs(gw.species_ledger(species, GroundwaterSpeciesLedger.RESIDUAL)) < 1e-8
        assert gw.species_ledger(0, GroundwaterSpeciesLedger.INFIL_IN) > 0
        assert gw.species_ledger(1, GroundwaterSpeciesLedger.AGE_GENERATED) > 0
        assert surface.species_ledger('__WATER_AGE__')['age_generated'] > 0
        if forcing == 'well':
            assert gw.species_ledger(0, GroundwaterSpeciesLedger.SOURCE_IN) > 0
        else:
            receipt = gw.authored_boundary_receipt(0, 0)
            assert receipt['advective_in'] > 0 and receipt['advective_out'] > 0
        if layered:
            assert all(gw.layer_quality(0, j, 0)['total_mass'] >= 0 for j in range(4))
        solver.end()
    finally:
        cleanup(solver)


@pytest.mark.parametrize('closure,layered', [('CLOSED_FORM', False), ('ENSLAVED', False), ('SIGMA', False), ('SIGMA', True)])
@pytest.mark.parametrize('reaction_solver', ['RK5', 'ROS2'])
@pytest.mark.parametrize('surface_kinetics', [False, True])
def test_groundwater_rk2_symmetric_reaction_split_public_profile(tmp_path, closure, layered, reaction_solver, surface_kinetics):
    from openswmm.engine import Solver, GroundwaterSpeciesLedger
    from tests.engine._groundwater_cases import MODEL
    path = tmp_path / 'reactive-rk2.inp'
    text = MODEL.replace('END_TIME 00:10:00', 'END_TIME 00:00:02').replace('ROUTING_STEP 5', 'ROUTING_STEP .1')
    text += f'''[2D_OPTIONS]
MOMENTUM_EQUATION FULL_SWE
RECONSTRUCTION_ORDER 2
MAX_TIMESTEP .05
LTS_TIERS 1
TRANSPORT_POLLUTANTS NO
TRANSPORT_MSX YES
TRANSPORT_AGE NO
TRANSPORT_TEMPERATURE NO
[2D_INITIAL_QUALITY]
* A 1
[2D_AQUIFER_OPTIONS]
CLOSURE {closure}
M_LAYERS 4
NODE_ENROLMENT ROWS
LINK_SEEPAGE NONE
GW_ET NONE
[2D_AQUIFER]
* 1e-15 2 .45 .1 2 HG0 .6 C_LOSS 0
[GW_TRANSPORT_OPTIONS]
UNSAT_TRANSPORT {'LAYERED' if layered else 'BULK'}
TRANSPORT_POLLUTANTS NO
TRANSPORT_MSX YES
TRANSPORT_AGE NO
TRANSPORT_TEMPERATURE NO
[GW_INITIAL_QUALITY]
* SAT A 2
* UNSAT A 2
[REACTION_OPTIONS]
SOLVER {reaction_solver}
RATE_UNITS SEC
COUPLING FULL
ATOL 1e-13
RTOL 1e-11
[REACTION_SPECIES]
BULK A MG
BULK B MG
[REACTION_SUBSURFACE]
* SAT A RATE -.2*A
* SAT B RATE .2*A
* UNSAT A RATE -.2*A
* UNSAT B RATE .2*A
'''
    if surface_kinetics:
        text += '[REACTION_PIPES]\nRATE A -.3*A\nRATE B .3*A\n'
        # An unrelated disabled pollutant must not block this MSX-only route.
        text = text.replace('TSS MG/L 0 0 0 0', 'TSS MG/L 0 0 0 86400')
    path.write_text(text)
    solver = Solver(path, path.with_suffix('.rpt'))
    solver.open()
    try:
        solver.initialize()
        gw = solver.surface2d.groundwater
        assert gw.species == solver.surface2d.species == ('A', 'B')
        initial = gw.species_ledger(0, GroundwaterSpeciesLedger.INIT)
        surface_initial = solver.surface2d.species_ledger('A')['storage']
        solver.start(False)
        while solver.step().total_seconds() > 0:
            pass
        assert solver.elapsed.total_seconds() == 2
        a = gw.species_ledger(0, GroundwaterSpeciesLedger.STORAGE)
        b = gw.species_ledger(1, GroundwaterSpeciesLedger.STORAGE)
        # Authored aquifers require positive KS; the public fixture uses a
        # measured negligible transfer, while native gates use exactly zero.
        assert gw.ledger(GroundwaterLedger.INFIL_IN) < 1e-8
        assert a == pytest.approx(initial * np.exp(-.4), rel=1e-8)
        assert a + b == pytest.approx(initial, rel=1e-10)
        for species in range(2):
            assert abs(gw.species_ledger(species, GroundwaterSpeciesLedger.RESIDUAL)) < 1e-8
        # Each domain selects its explicit laws; surface PIPE kinetics are
        # staged once, with their own signed losses/production.
        keep = np.exp(-.6) if surface_kinetics else 1
        assert solver.surface2d.concentrations(0) == pytest.approx(np.full(2, keep), abs=1e-10)
        for name, expected_initial in [('A', surface_initial), ('B', 0)]:
            ledger = solver.surface2d.species_ledger(name)
            accounted = (ledger['storage'] + ledger['reacted'] + ledger['infiltration_out']
                         - ledger['exfiltration_in'])
            assert accounted == pytest.approx(expected_initial, abs=1e-8)
        assert solver.surface2d.species_ledger('A')['reacted'] == pytest.approx(surface_initial * (1-keep), abs=1e-8)
        assert solver.surface2d.species_ledger('B')['reacted'] == pytest.approx(-surface_initial * (1-keep), abs=1e-8)
        solver.end()
    finally:
        cleanup(solver)


def test_groundwater_rk2_surface_decaying_pollutant_drives_msx_and_preserves_age(tmp_path):
    from openswmm.engine import Solver
    from tests.engine._groundwater_cases import MODEL
    previous = None
    for dt in [.1, .05, .025, .0125]:
        path = tmp_path / f'surface-precursor-{dt}.inp'
        text = MODEL.replace('END_TIME 00:10:00', 'END_TIME 00:00:02').replace('ROUTING_STEP 5', 'ROUTING_STEP .1')
        text = text.replace('TSS MG/L 0 0 0 0', 'TSS MG/L 0 0 0 25920')  # .3/s, authored per day
        # Surface initial age is authored in hours; the public view is seconds.
        text += f'''[OPTIONS]
WATER_AGE ON
[2D_OPTIONS]
MOMENTUM_EQUATION FULL_SWE
RECONSTRUCTION_ORDER 2
MAX_TIMESTEP {dt}
LTS_TIERS 1
TRANSPORT_POLLUTANTS YES
TRANSPORT_MSX YES
TRANSPORT_AGE YES
TRANSPORT_TEMPERATURE NO
[2D_INITIAL_QUALITY]
* TSS 2
* __WATER_AGE__ {5/3600:.17g}
[2D_AQUIFER_OPTIONS]
CLOSURE SIGMA
M_LAYERS 4
NODE_ENROLMENT ROWS
LINK_SEEPAGE NONE
GW_ET NONE
[2D_AQUIFER]
* 1e-15 2 .45 .1 2 HG0 .6 C_LOSS 0
[GW_TRANSPORT_OPTIONS]
UNSAT_TRANSPORT LAYERED
TRANSPORT_POLLUTANTS YES
TRANSPORT_MSX YES
TRANSPORT_AGE YES
TRANSPORT_TEMPERATURE NO
[GW_INITIAL_QUALITY]
* SAT TSS 2
* UNSAT TSS 2
[REACTION_OPTIONS]
SOLVER RK5
RATE_UNITS SEC
COUPLING FULL
ATOL 1e-13
RTOL 1e-11
[REACTION_SPECIES]
BULK A MG
BULK B MG
[REACTION_PIPES]
RATE A TSS
'''
        path.write_text(text)
        solver = Solver(path, path.with_suffix('.rpt'))
        solver.open()
        try:
            solver.initialize()
            initial = solver.surface2d.species_ledger('TSS')['storage']
            solver.start(False)
            while solver.step().total_seconds() > 0:
                pass
            tss, produced, age = (solver.surface2d.concentrations(row) for row in [0, 1, 3])
            assert tss == pytest.approx(np.full(2, 2*np.exp(-.6)), abs=1e-10)
            error = max(abs(produced - 2*(1-np.exp(-.6))/.3))
            assert error < 1e-4
            if previous is not None:
                assert previous/error > 3.8
            previous = error
            assert age == pytest.approx(np.full(2, 7), abs=1e-10)
            assert solver.surface2d.species_ledger('__WATER_AGE__')['reacted'] == 0
            loss = solver.surface2d.species_ledger('TSS')
            production = solver.surface2d.species_ledger('A')
            assert loss['storage'] + loss['reacted'] == pytest.approx(initial, abs=1e-8)
            assert production['storage'] + production['reacted'] == pytest.approx(0, abs=1e-8)
            assert production['reacted'] < 0
            solver.end()
        finally:
            cleanup(solver)
