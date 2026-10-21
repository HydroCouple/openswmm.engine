"""Exact mesh-only groundwater chemistry/heat sessions through the public API."""
import numpy as np
import pytest
import shutil
import subprocess
from openswmm.engine import (
    Solver, GroundwaterVariable, GroundwaterZone, GroundwaterSpeciesLedger, GroundwaterLedger,
    GroundHeatValue, HeatSedimentParam, FileError,
)
from tests.engine._groundwater_cases import cleanup


def _open(directory, chemistry, thermal, reactive=True, passive=False, output=0, rate='.001', age=True, porous=True, unit='MG', boundary=None, rk2=False, latent=False, authored_surface=None):
    directory.mkdir()
    path = directory / 'model.inp'
    reaction = ''
    if reactive:
        reaction = f'''
[REACTION_OPTIONS]
SOLVER RK5
RATE_UNITS SEC
COUPLING FULL
ATOL 1e-13
RTOL 1e-11
[REACTION_SPECIES]
BULK A {unit}
BULK B MG
[REACTION_SUBSURFACE]
* SAT A RATE -{rate}*A
* SAT B RATE {rate}*A
* UNSAT A RATE -{rate}*A
* UNSAT B RATE {rate}*A
[2D_INITIAL_QUALITY]
* A 1
[GW_INITIAL_QUALITY]
* SAT A 7
* UNSAT A 11
'''
    pollutant = '''
[POLLUTANTS]
Dye MG/L 0 0 0 0 NO * 0 0
[GW_INITIAL_QUALITY]
* SAT Dye 10
* UNSAT Dye 10
''' if passive else ''
    forcing = (' A CONC 3 B CONC 1' if reactive else '') + (' Dye CONC 3' if passive else '')
    edge = ''
    if boundary:
        values = ('.000001', '-.000001', '.000001') if boundary == 'FLOW' else ('.05', '-.45', '0')
        rows = [('A', 3), ('B', 1)] if reactive else []
        if passive:
            rows.append(('Dye', 3))
        if porous:
            rows.append(('__TEMPERATURE__', -4))
        if age:
            rows.append(('__WATER_AGE__', 2))
        quality = ''.join(f'1 1 {name} CONC {value}\n' for name, value in rows)
        edge = f'''[TIMESERIES]
EDGE 01/31/2026 23:59:30 {values[0]}
EDGE 01/31/2026 23:59:39 {values[1]}
EDGE 01/31/2026 23:59:47 {values[2]}
EDGE 02/01/2026 00:01:30 {values[2]}
[2D_AQUIFER_BOUNDARIES]
CELL 1 EDGE 1 {boundary} SERIES EDGE
[GW_BOUNDARY_QUALITY]
{quality}1 1 A MASSFLUX .000002
'''
    initial_heat = '[GW_INITIAL_QUALITY]\n* SAT __TEMPERATURE__ 8\n* UNSAT __TEMPERATURE__ 8\n' if porous else ''
    variables = 'DEPTH HEAT GROUNDWATER' if porous else 'DEPTH GROUNDWATER'
    mesh_output = (f'REPORT_2D YES\nOUTPUT_FILE "{directory / "mesh.h5"}"\n'
                   f'OUTPUT_PRECISION FLOAT64\nREPORT_2D_VARIABLES {variables}\n') if output else 'REPORT_2D NO\n'
    path.write_text(f'''[OPTIONS]
FLOW_UNITS CMS
FLOW_ROUTING DYNWAVE
HEAT_TRANSPORT {'YES' if porous else 'NO'}
WATER_AGE {'ON' if age else 'OFF'}
START_DATE 01/31/2026
START_TIME 23:59:30
END_DATE 02/01/2026
END_TIME 00:00:45
REPORT_START_DATE 01/31/2026
REPORT_START_TIME 23:59:30
REPORT_STEP 00:00:07
ROUTING_STEP 1
[2D_OPTIONS]
INTEGRATOR EXPLICIT
LTS_TIERS 1
MAX_TIMESTEP .2
EVAPORATION NO
TRANSPORT_POLLUTANTS YES
TRANSPORT_MSX YES
TRANSPORT_AGE {'YES' if age else 'NO'}
TRANSPORT_TEMPERATURE {'YES' if porous else 'NO'}
{mesh_output}
[2D_VERTICES]
0 0 .5
4 0 .5
0 2 .5
4 2 .5
[2D_TRIANGLES]
0 1 2 .03 .04
1 3 2 .03 .03
[2D_AQUIFER_OPTIONS]
CLOSURE {'SIGMA' if chemistry else 'CLOSED_FORM'}
M_LAYERS {chemistry or 4}
NODE_ENROLMENT ROWS
LINK_SEEPAGE NONE
GW_ET NONE
[2D_AQUIFER]
* 36 1 .45 .1 2 HG0 .3 C_LOSS .02
[GW_TRANSPORT_OPTIONS]
UNSAT_TRANSPORT {'LAYERED' if chemistry else 'BULK'}
THERMAL_MODEL {'POROUS_EQUILIBRIUM' if porous else 'WATER_ONLY'}
THERMAL_LAYERS {thermal}
CONDUCTION {'YES' if porous else 'NO'}
DISPERSION NO
SURFACE_THERMAL_BC ADIABATIC
DEEP_THERMAL_BC ADIABATIC
{initial_heat}
[TIMESERIES]
WELL 01/31/2026 23:59:30 .000001
WELL 01/31/2026 23:59:39 .000002
WELL 01/31/2026 23:59:47 .000001
WELL 02/01/2026 00:01:30 .000001
[GW_SOURCES]
W CELL 1 FLOW WELL{forcing}{' __TEMPERATURE__ CONC -4' if porous else ''}{' __WATER_AGE__ CONC 2' if age else ''}
{pollutant}
{reaction}
{edge}
''')
    if rk2:
        path.write_text(path.read_text()+'\n[2D_OPTIONS]\nMOMENTUM_EQUATION FULL_SWE\nRECONSTRUCTION_ORDER 2\nLTS_TIERS 1\n')
    if authored_surface:
        kind = 'SPECIFIED_STAGE .56' if authored_surface == 1 else 'SPECIFIED_FLOW -.00001'
        rows = ([('A', 1), ('B', 0)] if reactive else []) + ([('Dye', 1)] if passive else [])
        if porous:
            rows.append(('__TEMPERATURE__', -12))
        if age:
            rows.append(('__WATER_AGE__', 1))
        quality = ''.join(f'0 1 {name} {value}\n' for name, value in rows)
        path.write_text(path.read_text()+f'\n[2D_BOUNDARY_CONDITIONS]\n0 1 {kind}\n[2D_BOUNDARY_QUALITY]\n{quality}')
    if latent:
        path.write_text(path.read_text()+'\n[EVAPORATION]\nCONSTANT 18\n')
    solver = Solver(path, path.with_suffix('.rpt'))
    solver.create()
    capabilities = solver.checkpoints.capabilities
    if not capabilities & (1 << 4):  # SWMM_CHECKPOINT_SURFACE_PROFILE
        cleanup(solver)
        pytest.skip('Mesh checkpoint support is omitted from this build')
    if output and not capabilities & (1 << 6):  # HDF5-backed SWMM_CHECKPOINT_FILE_PROFILE
        cleanup(solver)
        pytest.skip('Mesh HDF5 checkpoint support is omitted from this build')
    solver.open()
    if porous:
        solver.options.ext['HEAT_MODEL'] = 'COUPLED'
        solver.options.ext['BED_HEAT_MODEL'] = 'GROUND_COLUMN'
        solver.options.ext['HEAT_SURFACE_EXCHANGE'] = 'NO'
        solver.options.ext['HEAT_RADIATIVE_EXCHANGE'] = 'NO'
        if latent:
            solver.options.ext['HEAT_SURFACE_EXCHANGE'] = 'YES'
            solver.options.ext['HEAT_RADIATIVE_EXCHANGE'] = 'YES'
            solver.options.ext['EVAPORATION'] = 'CLIMATE'
        solver.heat.sediment[HeatSedimentParam.HYPORHEIC_VELOCITY] = .001
    return solver


def _snapshot(solver, chemistry, porous=True):
    surface = solver.surface2d
    gw = surface.groundwater
    values = [surface.get_depths(), gw.cells(GroundwaterVariable.HG), gw.cells(GroundwaterVariable.HU)]
    values += [surface.concentrations(row) for row in range(len(surface.species))]
    values += [gw.concentrations(zone, row) for zone in (GroundwaterZone.SAT, GroundwaterZone.UNSAT)
               for row in range(len(gw.species))]
    values += [np.asarray([[gw.layer_quality(cell, layer, row)['total_mass']
                           for row in range(len(gw.species))] for layer in range(chemistry)])
               for cell in range(gw.dimensions[0])] if chemistry else []
    values += [np.asarray([gw.species_ledger(row, term) for term in GroundwaterSpeciesLedger])
               for row in range(len(gw.species))]
    if porous:
        values += [np.asarray(solver.heat.ground_values(GroundHeatValue.ENERGY, 0, gw.dimensions[0]))]
    for edge in range(len(gw.authored_boundaries)):
        values.append(np.asarray(list(gw.authored_boundary_receipt(edge).values())))
        values += [np.asarray(list(gw.authored_boundary_receipt(edge, row).values()))
                   for row in range(len(gw.species))]
    return values


def _amount(solver, names):
    gw, surface = solver.surface2d.groundwater, solver.surface2d
    amount = 0.0
    for name in names:
        row = gw.species.index(name)
        amount += sum(gw.species_ledger(row, term) for term in (
            GroundwaterSpeciesLedger.STORAGE, GroundwaterSpeciesLedger.DEEP_OUT,
            GroundwaterSpeciesLedger.SOURCE_OUT, GroundwaterSpeciesLedger.NODE_OUT,
            GroundwaterSpeciesLedger.LINK_OUT, GroundwaterSpeciesLedger.BOUNDARY_OUT,
            GroundwaterSpeciesLedger.BOUNDARY_FLUX_OUT))
        amount += float(np.sum(surface.concentrations(surface.species.index(name)) * surface.get_depths() * 4.0))
    return amount


@pytest.mark.parametrize('chemistry,thermal', [(0, 4), (5, 3), (3, 7)])
@pytest.mark.parametrize('reactive,passive', [(True, False), (True, True), (False, True), (False, False)])
@pytest.mark.parametrize('output', [0, 2])
@pytest.mark.parametrize('age', [True, False])
@pytest.mark.parametrize('porous', [True, False])
def test_mesh_groundwater_checkpoint_continues_reactions_and_heat(tmp_path, chemistry, thermal, reactive, passive, output, age, porous):
    _continue_session(tmp_path, chemistry, thermal, reactive, passive, output, age, porous)


@pytest.mark.parametrize('chemistry,thermal', [(0, 4), (5, 3), (3, 7)])
@pytest.mark.parametrize('passive', [True, False])
@pytest.mark.parametrize('output', [0, 2])
@pytest.mark.parametrize('porous', [True, False])
@pytest.mark.parametrize('boundary', ['FLOW', 'HEAD'])
def test_reactive_edge_session_preserves_reversals_fluxes_and_reports(tmp_path, chemistry, thermal, passive, output, porous, boundary):
    _continue_session(tmp_path, chemistry, thermal, True, passive, output, True, porous, boundary)


def _continue_session(tmp_path, chemistry, thermal, reactive, passive, output, age, porous, boundary=None, rk2=False, surface_boundary=None, latent=False, copied_et=False, authored_surface=None):
    source = _open(tmp_path / 'source', chemistry, thermal, reactive, passive, output, age=age, porous=porous, boundary=boundary, rk2=rk2, latent=latent, authored_surface=authored_surface)
    target = _open(tmp_path / 'target', chemistry, thermal, reactive, passive, output, age=age, porous=porous, boundary=boundary, rk2=rk2, latent=latent, authored_surface=authored_surface)
    try:
        source.checkpoints.start_session(output)
        target.checkpoints.start_session(output)
        names = ('A', 'B') if reactive else (('Dye',) if passive else ())
        initial_amount = _amount(source, names)
        initial_dye = _amount(source, ('Dye',)) if passive else 0.0
        if copied_et:
            from openswmm.engine import RuntimeForcing
            source.coupling.surface.set_forcings([RuntimeForcing('evaporation', rate_m_s=.00001, until_seconds=13.537)])
        if surface_boundary:
            kind, cleared = surface_boundary
            quality = {name: (-8 if name == '__TEMPERATURE__' else 2 if name == '__WATER_AGE__' else 3)
                       for name in source.surface2d.species}
            source.surface2d.sources.set_boundary(0, 1, kind, .56 if kind == 1 else -.0001, quality)
        while source.elapsed.total_seconds() < 8:
            assert source.step().total_seconds() > 0
        if surface_boundary and cleared:
            source.surface2d.sources.clear_boundary(0, 1)
        checkpoint = tmp_path / 'accepted.chk'
        source.checkpoints.save(checkpoint)
        if rk2:
            before, after = tmp_path / 'inspection-before.chk', tmp_path / 'inspection-after.chk'
            target.checkpoints.save(before)
            assert isinstance(target.checkpoints.inspect(checkpoint), dict)
            assert target.elapsed.total_seconds() == 0
            target.checkpoints.save(after)
            assert before.read_bytes() == after.read_bytes()
        target.checkpoints.restore(checkpoint)
        assert source.elapsed == target.elapsed
        if surface_boundary or authored_surface:
            assert source.surface2d.boundary_receipt(0, 1) == target.surface2d.boundary_receipt(0, 1)
        for a, b in zip(_snapshot(source, chemistry, porous), _snapshot(target, chemistry, porous)):
            np.testing.assert_array_equal(a, b)
        if rk2:
            while source.elapsed.total_seconds() < 75:
                assert source.step().total_seconds() > 0
                assert target.step() == source.elapsed
            for a, b in zip(_snapshot(source, chemistry, porous), _snapshot(target, chemistry, porous)):
                np.testing.assert_array_equal(a, b)
            # Settle the explicit public boundary while still RUNNING;
            # the following terminal step transitions the engine to ENDED.
            source.advance_to(75)
            target.advance_to(75)
            source.step()
            target.step()
        else:
            while source.step().total_seconds() > 0:
                assert target.step() == source.elapsed
            target.step()
        for a, b in zip(_snapshot(source, chemistry, porous), _snapshot(target, chemistry, porous)):
            np.testing.assert_array_equal(a, b)
        gw = source.surface2d.groundwater
        assert abs(gw.continuity_error) < 1e-9
        assert source.elapsed.total_seconds() == 75
        # Independent trapezoidal integral of the explicitly dated well series.
        water_input = 1.5e-6 * 9 + 1.5e-6 * 8 + 1e-6 * 58
        assert gw.ledger(GroundwaterLedger.SOURCE_IN) == pytest.approx(water_input, rel=1e-10, abs=1e-12)
        edge_input = 0.0
        imposed = 0.0
        if boundary:
            receipt = gw.authored_boundary_receipt(0)
            assert receipt['accepted_in'] > 0
            assert receipt['accepted_out'] > 0
            assert receipt['accepted_in'] == receipt['requested_in']
            assert receipt['accepted_out'] == receipt['requested_out']
            edge_input = receipt['accepted_in']
            if boundary == 'FLOW':
                # Two linear sign reversals, then 58 seconds of positive flow.
                assert edge_input == pytest.approx(62.25e-6, abs=1e-12)
                assert receipt['accepted_out'] == pytest.approx(4.25e-6, abs=1e-12)
            # Native mg/s/m becomes (mg/L)*m3 once; exterior edge length is 2 m.
            imposed = .000002 * .001 * 2 * 75
            a = gw.species.index('A')
            assert gw.authored_boundary_receipt(0, a)['flux_in'] == pytest.approx(imposed, rel=1e-10, abs=1e-14)
        concentration = 4 if reactive else (3 if passive else 0)
        surface_input = 0.0
        surface_dye = 0.0
        if surface_boundary or authored_surface:
            receipt = source.surface2d.boundary_receipt(0, 1)
            assert receipt == target.surface2d.boundary_receipt(0, 1)
            assert receipt.applied_m3 > 0
            surface_input = sum(receipt.applied_species[name] for name in names)
            surface_dye = receipt.applied_species.get('Dye', 0.0)
        assert _amount(source, names) == pytest.approx(initial_amount + concentration * (water_input + edge_input) + imposed + surface_input,
                                                     rel=1e-10, abs=1e-10)
        if passive:
            assert _amount(source, ('Dye',)) == pytest.approx(initial_dye + 3 * (water_input + edge_input) + surface_dye,
                                                            rel=1e-10, abs=1e-10)
        for name in gw.species:
            if name != '__TEMPERATURE__':
                row = gw.species.index(name)
                assert abs(gw.species_ledger(row, GroundwaterSpeciesLedger.RESIDUAL)) < 1e-8
        if porous:
            energy = source.heat.ground_values(GroundHeatValue.NORMALIZED_RESIDUAL, 0, gw.dimensions[0])
            assert np.max(np.abs(energy)) < 1e-10
        if reactive:
            b = gw.species.index('B')
            assert gw.species_ledger(b, GroundwaterSpeciesLedger.REACTED) < 0
            for row in (gw.species.index('A'), b):
                assert abs(gw.species_ledger(row, GroundwaterSpeciesLedger.RESIDUAL)) < 1e-8
        source.end()
        target.end()
        if output:
            # HDF5 allocation addresses need not match after reopening a saved
            # prefix. Require exact report values, attributes and schema instead.
            diff, dump = shutil.which('h5diff'), shutil.which('h5dump')
            if not diff or not dump:
                pytest.skip('Exact HDF5 history comparison requires h5diff/h5dump; native output tests cover this build')
            files = [tmp_path / 'source/mesh.h5', tmp_path / 'target/mesh.h5']
            result = subprocess.run([diff, '-r', *map(str, files)], capture_output=True, text=True)
            assert result.returncode == 0, result.stdout + result.stderr
            schemas = [subprocess.run([dump, '-H', str(path)], check=True, capture_output=True, text=True).stdout.splitlines()[1:]
                       for path in files]
            assert schemas[0] == schemas[1]
    finally:
        cleanup(source)
        cleanup(target)


@pytest.mark.parametrize('mutation', ['binary', 'combined', 'thermal', 'layout'])
def test_mesh_groundwater_session_refuses_unqualified_profiles_before_mesh_output(tmp_path, mutation):
    directory = tmp_path / 'refused'
    solver = _open(directory, 5, 3, output=2)
    try:
        if mutation == 'thermal':
            solver.surface2d.groundwater.transport.options['THERMAL_MODEL'] = 'WATER_ONLY'
        elif mutation == 'layout':
            solver.surface2d.groundwater.transport.options['TRANSPORT_MSX'] = 'NO'
        output = {'binary': 1, 'combined': 3}.get(mutation, 2)
        with pytest.raises(FileError):
            solver.checkpoints.start_session(output)
        assert not (directory / 'mesh.h5').exists()
    finally:
        cleanup(solver)


@pytest.mark.parametrize('porous', [True, False])
@pytest.mark.parametrize('mutation', ['reaction', 'unit', 'grid'])
def test_mesh_groundwater_checkpoint_mismatch_preserves_all_target_state(tmp_path, porous, mutation):
    source = _open(tmp_path / 'source', 5, 3, passive=True, porous=porous)
    layers = 6 if mutation == 'grid' else 5
    target = _open(tmp_path / 'target', layers, 3, passive=True, porous=porous,
                   rate='.002' if mutation == 'reaction' else '.001',
                   unit='UG' if mutation == 'unit' else 'MG')
    try:
        source.checkpoints.start_session(0)
        target.checkpoints.start_session(0)
        while source.elapsed.total_seconds() < 8:
            source.step()
        checkpoint = tmp_path / 'source.chk'
        before, after = tmp_path / 'before.chk', tmp_path / 'after.chk'
        source.checkpoints.save(checkpoint)
        target.checkpoints.save(before)
        with pytest.raises(FileError):
            target.checkpoints.restore(checkpoint)
        target.checkpoints.save(after)
        assert before.read_bytes() == after.read_bytes()
        assert target.elapsed.total_seconds() == 0
    finally:
        cleanup(source)
        cleanup(target)


@pytest.mark.parametrize('porous', [True, False])
@pytest.mark.parametrize('boundary', ['FLOW', 'HEAD'])
def test_reactive_edge_changed_forcing_preserves_target_and_mesh_output(tmp_path, porous, boundary):
    source = _open(tmp_path / 'source', 5, 3, passive=True, output=2, porous=porous, boundary=boundary)
    target = _open(tmp_path / 'target', 5, 3, passive=True, output=2, porous=porous, boundary=boundary)
    try:
        # Keep the edge/schema/species fixed, but change its future forcing.
        target.surface2d.groundwater.set_authored_boundary(0, 1, boundary, value=.000001 if boundary == 'FLOW' else -.1)
        source.checkpoints.start_session(2)
        target.checkpoints.start_session(2)
        while source.elapsed.total_seconds() < 8:
            source.step()
        checkpoint, before, after = [tmp_path / name for name in ('source.chk', 'before.chk', 'after.chk')]
        source.checkpoints.save(checkpoint)
        target.checkpoints.save(before)
        mesh = tmp_path / 'target/mesh.h5'
        prefix = mesh.read_bytes()
        with pytest.raises(FileError):
            target.checkpoints.restore(checkpoint)
        assert mesh.read_bytes() == prefix
        target.checkpoints.save(after)
        assert after.read_bytes() == before.read_bytes()
        assert target.elapsed.total_seconds() == 0
    finally:
        cleanup(source)
        cleanup(target)


@pytest.mark.parametrize('chemistry,thermal', [(0, 4), (5, 3), (3, 7)])
@pytest.mark.parametrize('porous', [False, True])
@pytest.mark.parametrize('output', [0, 2])
@pytest.mark.parametrize('boundary', [None, 'FLOW', 'HEAD'])
def test_managed_rk2_groundwater_session_continues_calendar_quality_heat_and_history(tmp_path, chemistry, thermal, porous, output, boundary):
    _continue_session(tmp_path, chemistry, thermal, True, True, output, True, porous, boundary, rk2=True)


@pytest.mark.parametrize('chemistry,thermal', [(0, 4), (5, 3), (3, 7)])
@pytest.mark.parametrize('porous', [False, True])
@pytest.mark.parametrize('output', [0, 2])
@pytest.mark.parametrize('kind', [1, 2])
@pytest.mark.parametrize('cleared', [False, True])
def test_managed_rk2_surface_boundary_preserves_chemistry_heat_history_and_clear(tmp_path, chemistry, thermal, porous, output, kind, cleared):
    _continue_session(tmp_path, chemistry, thermal, True, True, output, True, porous,
                      rk2=True, surface_boundary=(kind, cleared))


def test_managed_rk2_surface_boundary_owner_and_invalid_frame_preserve_accepted_session(tmp_path):
    solver = _open(tmp_path / 'source', 0, 3, True, True, 0, porous=False, rk2=True)
    try:
        from openswmm.engine import BadParamError, CouplingFrame, RuntimeBoundary
        solver.checkpoints.start_session(0)
        owner = solver.coupling.for_provider('perimeter')
        other = solver.coupling.for_provider('other')
        quality = {name: 3 for name in solver.surface2d.species}
        owner.surface.set_boundary(0, 1, 2, -.0001, quality)
        solver.advance_to(.735)
        before, after = tmp_path / 'before.chk', tmp_path / 'after.chk'
        solver.checkpoints.save(before)
        with pytest.raises(BadParamError):
            other.surface.set_boundary(0, 1, 1, .56, quality)
        with pytest.raises(BadParamError):
            other.surface.clear_boundary(0, 1)
        with pytest.raises(BadParamError):
            owner.apply_frame(CouplingFrame(boundaries=(RuntimeBoundary('surface', 0, 1, 2, -.002, quality),
                                                        RuntimeBoundary('surface', 0, 0, 2, -.002, quality))))
        with pytest.raises(BadParamError):
            owner.surface.set_boundary(0, 1, 2, -.0001, dict(quality, Dye=-1))
        solver.checkpoints.save(after)
        assert before.read_bytes() == after.read_bytes()
        receipt = solver.surface2d.boundary_receipt(0, 1)
        owner.surface.clear_boundary(0, 1)
        solver.advance_to(1)
        assert receipt == solver.surface2d.boundary_receipt(0, 1)
    finally:
        cleanup(solver)


@pytest.mark.parametrize('chemistry,thermal', [(0, 4), (5, 3), (3, 7)])
@pytest.mark.parametrize('output', [0, 2])
@pytest.mark.parametrize('copied', [False, True])
def test_managed_rk2_surface_latent_continues_native_weather_receipts_and_history(tmp_path, chemistry, thermal, output, copied):
    _continue_session(tmp_path, chemistry, thermal, True, True, output, True, True,
                      rk2=True, latent=True, copied_et=copied)


@pytest.mark.parametrize('chemistry,thermal', [(0, 4), (5, 3), (3, 7)])
@pytest.mark.parametrize('porous', [False, True])
@pytest.mark.parametrize('output', [0, 2])
@pytest.mark.parametrize('kind', [1, 2])
@pytest.mark.parametrize('override', [None, False, True])
def test_managed_rk2_authored_surface_boundary_restores_overrides_and_fallback_history(tmp_path, chemistry, thermal, porous, output, kind, override):
    _continue_session(tmp_path, chemistry, thermal, True, True, output, True, porous,
                      rk2=True, authored_surface=kind,
                      surface_boundary=None if override is None else (kind, override))
