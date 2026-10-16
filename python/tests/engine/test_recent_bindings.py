"""Runtime contracts for shared snow, atomic groundwater edits and checkpoints."""
from dataclasses import replace
from datetime import timedelta
import pytest
import openswmm.engine as engine
from tests.engine.test_snow_process import opened
from tests.engine.test_lid_nodes import DECK as LID_DECK
from tests.engine._groundwater_cases import open_model, cleanup


def test_shared_radiation_and_cloud_rollback(tmp_path):
    solver = opened(tmp_path)
    try:
        snow = solver.snow
        radiation = snow.radiation
        snow.radiation = dict(radiation, elevation_m=1234)
        assert snow.radiation['elevation_m'] == 1234
        before = snow.radiation
        with pytest.raises(engine.EngineError):
            snow.radiation = dict(before, latitude=999)
        assert snow.radiation == before
        assert snow.radiation_source(0) == 'sw'
        snow.set_radiation_source(0, 'lw')
        assert snow.radiation_source(0) == 'lw'
        cloud = dict(snow.cloud, mode=1, fraction=.3)
        temperature = 'SNOWMELT_MODEL UEB\nSITE 45 -120 -8 1000'
        snow.apply_configuration(temperature, cloud=cloud)
        assert snow.cloud['fraction'] == .3
        before = (snow.cloud, snow.radiation)
        with pytest.raises(ValueError):
            snow.apply_configuration('SNOW_STEP -1', cloud=dict(cloud, fraction=.6))
        assert (snow.cloud, snow.radiation) == before
        with pytest.raises(ValueError):
            snow.set_radiation_source(0, 'bad\0name')
    finally:
        cleanup(solver)


def test_meteorology_preview_does_not_apply_forcing(tmp_path):
    solver = opened(tmp_path)
    try:
        path = tmp_path / 'hourly.met'
        path.write_text('01/01/2026 00:00 2 50 1 100 300\n01/01/2026 01:00 4 60 2 200 310\n')
        before = solver.snow.radiation_source(2)
        summary = solver.snow.preview_meteorology(path)
        assert summary['records'] == 2
        assert summary['minimum'] == [2, 50, 1, 100, 300]
        assert summary['maximum'] == [4, 60, 2, 200, 310]
        assert summary['last_date'] - summary['first_date'] == pytest.approx(2/24)
        assert solver.snow.radiation_source(2) == before
        path.write_text('bad record\n')
        with pytest.raises(ValueError, match='[Mm]eteorology'):
            solver.snow.preview_meteorology(path)
    finally:
        cleanup(solver)


def test_snowpack_batch_rollback_and_stale_views(tmp_path):
    solver = opened(tmp_path)
    try:
        snow = solver.snow
        ueb = dict(snow.pack('SP'), model=1, forest_fraction=.25)
        row = {'name': 'SP', 'surface': [[.5, 1.5, 0, .1, 150, 0, 0]] * 3,
               'removal': [0] * 6, 'ueb': ueb}
        old = snow.pack('SP')
        with pytest.raises(ValueError):
            snow.apply_packs([row, dict(row, name='bad', removal=[0, 2, 0, 0, 0, 0])])
        assert snow.pack('SP') == old
        snow.apply_packs([row, dict(row, name='New')])
        assert solver.snow.pack('SP')['forest_fraction'] == .25
        assert solver.snow.pack('New')['model'] == 1
        with pytest.raises(engine.StaleObjectError):
            snow.pack('SP')
    finally:
        cleanup(solver)


@pytest.mark.skipif(not engine.HAS_2D, reason='mesh feature omitted')
def test_mesh_snow_authored_rows(tmp_path):
    solver = opened(tmp_path)
    try:
        mesh = solver.surface2d.snow
        mesh.apply_configuration(options='SNOWMELT NO\nSNOW_STEP 30', defaults='* SNOWPACK SP')
        assert mesh.options == {'mode': 'NO', 'step_seconds': 30}
        rows = mesh.rows()
        assert len(rows) == 1
        assert rows[0]['tag'] == '*'
        assert rows[0]['pack'] == 'SP'
        assert rows[0]['cell'] == -1
        assert mesh.rows('cells') == []
        with pytest.raises(ValueError):
            mesh.rows('options')
    finally:
        cleanup(solver)


@pytest.mark.skipif(not engine.HAS_2D, reason='mesh feature omitted')
def test_groundwater_compare_replace_and_private_validation(tmp_path):
    solver = open_model(tmp_path)
    try:
        gw = solver.surface2d.groundwater
        gw.add_node('J1', -1, 36, .1, .5)
        expected = gw.nodes
        replacement = [replace(expected[0], area=.75, exchange=False)]
        gw.replace_nodes(expected, replacement, apply=False)
        assert gw.nodes == expected
        gw.replace_nodes(expected, replacement)
        assert gw.nodes == tuple(replacement)
        with pytest.raises(engine.EngineError):
            gw.replace_nodes(expected, [])
        assert gw.nodes == tuple(replacement)
        with pytest.raises(engine.EngineError):
            gw.replace_nodes(replacement, [], bottoms=[(999, 0, 1)])
        assert gw.nodes == tuple(replacement)
    finally:
        cleanup(solver)


def test_lid_base_area_and_uninitialized_bottom_refusal(tmp_path):
    path = tmp_path / 'lid.inp'
    path.write_text(LID_DECK)
    solver = engine.Solver(path, tmp_path / 'lid.rpt')
    try:
        solver.open()
        lids = solver.infrastructure.lids
        lids.add('Stack', engine.LidType.NODE)
        lids.set_layers('Stack', [engine.LidNodeLayer(engine.LidNodeLayerKind.AGGREGATE, (12, .4, 100))])
        lids.assign_node('S', 'Stack')
        assert lids.node_base_area('S') == pytest.approx(100)
        with pytest.raises(engine.EngineError):
            lids.node_bottom_state('S')
        assert lids.node_assignment('S') == (0, 0)
    finally:
        cleanup(solver)


@pytest.mark.parametrize('output', [0, 1])
def test_checkpoint_roundtrip_and_destination_protection(tmp_path, output):
    path = tmp_path / 'checkpoint.inp'
    path.write_text(LID_DECK.replace('[POLLUTANTS]\nTSS MG/L 0 0 0 0 NO * 0 0 0\n', '') + '\n[OPTIONS]\nIGNORE_QUALITY YES\n')
    def make(name):
        solver = engine.Solver(path, tmp_path / (name + '.rpt'), tmp_path / (name + '.out') if output else '')
        solver.open()
        return solver
    a, b = make('a'), make('b')
    try:
        assert a.checkpoints.capabilities & 256
        a.checkpoints.start_session(output)
        b.checkpoints.start_session(output)
        assert a.checkpoints.session_status & 8
        for _ in range(3):
            a.step()
        checkpoint = tmp_path / 'state.chk'
        a.checkpoints.save(checkpoint)
        original = checkpoint.read_bytes()
        with pytest.raises(engine.EngineError):
            a.checkpoints.save(checkpoint)
        assert checkpoint.read_bytes() == original
        elapsed = a.elapsed
        metadata = b.checkpoints.inspect(checkpoint)
        assert b.elapsed == timedelta(0)
        assert isinstance(metadata, dict)
        assert metadata
        b.checkpoints.restore(checkpoint)
        assert b.elapsed == elapsed
        assert b.until(elapsed) == elapsed
        assert b.step() == a.step()
        assert b.nodes['S'].depth == pytest.approx(a.nodes['S'].depth)
        with pytest.raises(engine.EngineError) as error:
            b.checkpoints.inspect(tmp_path / 'missing.chk')
        assert str(error.value)
        stale = b.checkpoints
        b.close()
        with pytest.raises(engine.StaleObjectError):
            _ = stale.session_status
        with pytest.raises(ValueError):
            engine.Checkpoints.export_bundle('bad\0path', tmp_path / 'bundle')
    finally:
        cleanup(a)
        cleanup(b)


def test_output_variable_presence_and_closed_reader(tmp_path):
    solver = opened(tmp_path)
    try:
        solver.initialize()
        solver.start()
        for _ in solver.steps():
            pass
        solver.end()
        solver.close()
        reader = engine.OutputReader(tmp_path / 'snow.out')
        try:
            assert reader.has_subcatch_variable(engine.OutSubcatchVar.RAINFALL)
            assert not reader.has_subcatch_variable(-123)
        finally:
            reader.close()
        with pytest.raises(engine.BadHandleError):
            reader.has_subcatch_variable(0)
    finally:
        cleanup(solver)


@pytest.mark.skipif(not engine.HAS_2D, reason='mesh feature omitted')
def test_lid_mesh_bottom_diagnostics_and_atomic_owner_edit(tmp_path):
    deck = LID_DECK.replace('[POLLUTANTS]\nTSS MG/L 0 0 0 0 NO * 0 0 0\n', '')
    deck += '''
[OPTIONS]
IGNORE_QUALITY YES
IGNORE_SNOWMELT YES
[2D_OPTIONS]
MAX_TIMESTEP 1
REPORT_2D NO
[2D_VERTICES]
0 0 0
10 0 0
10 10 0
0 10 0
[2D_QUADS]
0 1 2 3 .03
[2D_TRIANGLE_NODE_MAP]
0 S 0 1
[2D_AQUIFER_OPTIONS]
FORCE_CLOSED_FORM YES
MODE MESH
NODE_ENROLMENT ROWS
LINK_SEEPAGE NONE
[2D_AQUIFER]
* 36 .5 .45 .1 2 HG0 .2
'''
    path = tmp_path / 'mesh-lid.inp'
    path.write_text(deck)
    solver = engine.Solver(path, tmp_path / 'mesh-lid.rpt', '')
    try:
        solver.open()
        lids = solver.infrastructure.lids
        lids.add('Stack', engine.LidType.NODE)
        lids.set_layers('Stack', [engine.LidNodeLayer(engine.LidNodeLayerKind.AGGREGATE, (12, .4, 100))])
        lids.assign_node('S', 'Stack')
        gw = solver.surface2d.groundwater
        gw.add_node('S', 0, 5, .1, .5)
        rows = gw.nodes
        gw.replace_nodes(rows, rows, bottoms=[(solver.nodes['S'].index, 0, 2)])
        assert lids.node_bottom('S') == 'MESH_AQUIFER'
        solver.checkpoints.start_session(source_history=tmp_path / 'source.history')
        initial = lids.node_bottom_state('S')
        assert initial['cell'] == 0
        assert initial['contact_area_m2'] == pytest.approx(.5 * .3048**2)
        assert not initial['valid_sample']
        for _ in range(5):
            solver.step()
        state = lids.node_bottom_state('S')
        assert state['valid_sample']
        assert state['net_cumulative_m3'] == pytest.approx(state['down_cumulative_m3'] - state['up_cumulative_m3'])
        assert state['accepted_down_m3'] <= state['reserved_down_m3']
        assert state['accepted_up_m3'] <= state['reserved_up_m3']
        assert lids.node_bottom_state('S') == state  # inspection does not advance
    finally:
        cleanup(solver)
