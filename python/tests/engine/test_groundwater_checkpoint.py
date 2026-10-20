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


def _open(directory, chemistry, thermal, reactive=True, passive=False, output=0, rate='.001', age=True, porous=True, unit='MG'):
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
''')
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
    source = _open(tmp_path / 'source', chemistry, thermal, reactive, passive, output, age=age, porous=porous)
    target = _open(tmp_path / 'target', chemistry, thermal, reactive, passive, output, age=age, porous=porous)
    try:
        source.checkpoints.start_session(output)
        target.checkpoints.start_session(output)
        names = ('A', 'B') if reactive else (('Dye',) if passive else ())
        initial_amount = _amount(source, names)
        while source.elapsed.total_seconds() < 8:
            assert source.step().total_seconds() > 0
        checkpoint = tmp_path / 'accepted.chk'
        source.checkpoints.save(checkpoint)
        target.checkpoints.restore(checkpoint)
        assert source.elapsed == target.elapsed
        for a, b in zip(_snapshot(source, chemistry, porous), _snapshot(target, chemistry, porous)):
            np.testing.assert_array_equal(a, b)
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
        concentration = 4 if reactive else (3 if passive else 0)
        assert _amount(source, names) == pytest.approx(initial_amount + concentration * water_input,
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
