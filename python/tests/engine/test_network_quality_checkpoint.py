"""Native pollutant owners resume together with their accepted mesh transfers."""
import shutil
import subprocess

import numpy as np
import pytest
from openswmm.engine import ConcentrationUnits, FileError
from tests.engine._groundwater_cases import cleanup
from tests.engine.test_groundwater_checkpoint import _open, _snapshot


def _state(solver, layers):
    return _snapshot(solver, layers, False) + [solver.nodes.qualities('Dye'), solver.links.qualities('Dye')]


@pytest.mark.parametrize('layers', [0, 5])
@pytest.mark.parametrize('output', [0, 1, 2, 3])
@pytest.mark.parametrize('rk2', [False, True])
def test_native_network_pollutants_continue_complete_mesh_and_output_state(tmp_path, layers, output, rk2):
    options = dict(reactive=False, passive=True, age=False, porous=False, output=output,
                   rk2=rk2, network=True if rk2 else 'shared')
    a = _open(tmp_path / 'a', layers, 3, **options)
    b = _open(tmp_path / 'b', layers, 3, **options)
    try:
        for solver in (a, b):
            solver.pollutants['Dye'].init_conc = 2
            if rk2:
                solver.pollutants['Dye'].kdecay = 864  # 0.01 per second, authored per day
                solver.surface2d.set_initial_quality('Dye', 9)
        a.checkpoints.start_session(output)
        b.checkpoints.start_session(output)
        assert np.any(a.nodes.qualities('Dye') > 0)
        while a.elapsed.total_seconds() < 8:
            a.step()
        path = tmp_path / 'accepted.chk'
        a.checkpoints.save(path)
        b.checkpoints.restore(path)
        for x, y in zip(_state(a, layers), _state(b, layers)):
            np.testing.assert_array_equal(x, y)
        while a.elapsed.total_seconds() < 75:
            a.step()
            assert b.step() == a.elapsed
            for x, y in zip(_state(a, layers), _state(b, layers)):
                np.testing.assert_array_equal(x, y)
        if rk2:
            a.advance_to(75)
            b.advance_to(75)
        a.step()
        b.step()
        for x, y in zip(_state(a, layers), _state(b, layers)):
            np.testing.assert_array_equal(x, y)
        if rk2:
            assert a.surface2d.species_ledger('Dye')['reacted'] > 0
        if not rk2:
            ledger = a.surface2d.species_ledger('Dye')
            assert ledger['coupling_out'] > 0 or ledger['coupling_in'] > 0
        a.end()
        b.end()
        if output in (1, 3):
            assert (tmp_path / 'a/model.out').read_bytes() == (tmp_path / 'b/model.out').read_bytes()
        if output in (2, 3):
            diff, dump = shutil.which('h5diff'), shutil.which('h5dump')
            if not diff or not dump:
                pytest.skip('HDF5 comparison tools are omitted from this test installation')
            files = [tmp_path / 'a/mesh.h5', tmp_path / 'b/mesh.h5']
            result = subprocess.run([diff, '-r', *map(str, files)], capture_output=True, text=True)
            assert result.returncode == 0, result.stdout + result.stderr
            schemas = [subprocess.run([dump, '-H', str(f)], check=True, capture_output=True,
                                      text=True).stdout.splitlines()[1:] for f in files]
            assert schemas[0] == schemas[1]
    finally:
        cleanup(a)
        cleanup(b)


@pytest.mark.parametrize('mutation', ['unit', 'initial'])
def test_native_network_identity_refusal_preserves_target_and_output(tmp_path, mutation):
    options = dict(reactive=False, passive=True, age=False, porous=False, output=3,
                   network='shared')
    a = _open(tmp_path / 'a', 5, 3, **options)
    b = _open(tmp_path / 'b', 5, 3, **options)
    try:
        if mutation == 'unit':
            b.pollutants['Dye'].units = ConcentrationUnits.UG_PER_L
        else:
            b.pollutants['Dye'].init_conc = 12
        a.checkpoints.start_session(3)
        b.checkpoints.start_session(3)
        while a.elapsed.total_seconds() < 8:
            a.step()
        path, before, after = [tmp_path / name for name in ('source.chk', 'before.chk', 'after.chk')]
        a.checkpoints.save(path)
        b.checkpoints.save(before)
        # Combined checkpoints embed both full output prefixes. Compare the closed
        # snapshots without reopening the live, exclusively locked HDF5 writer.
        before_bytes = before.read_bytes()
        for operation in (b.checkpoints.inspect, b.checkpoints.restore):
            with pytest.raises(FileError):
                operation(path)
            b.checkpoints.save(after)
            assert before_bytes == after.read_bytes()
            after.unlink()
            assert b.elapsed.total_seconds() == 0
    finally:
        cleanup(a)
        cleanup(b)
