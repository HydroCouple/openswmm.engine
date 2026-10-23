# SPDX-License-Identifier: Apache-2.0
"""End-to-end saved results and public C reader contracts.

All generated models/results remain under tests/_artifacts/results for review.
The fixture uses the real writer, not a second HDF5 implementation.
"""
from concurrent.futures import ThreadPoolExecutor
import ctypes as ct
from pathlib import Path

import numpy as np
import pytest

from openswmm.engine import (
    Results, Output2DReader, OutputReader, Solver, OutNodeVar, BadHandleError,
    LifecycleError, FileError, CellScope, GroundwaterClosure,
)
from tests._paths import ARTIFACTS_DIR
from tests.engine._groundwater_cases import MODEL, cleanup


@pytest.fixture(scope='module', params=['CMS', 'CFS'])
def saved(request):
    if not Output2DReader.available():
        pytest.skip('requires 2D HDF5 output')
    directory = Path(ARTIFACTS_DIR) / 'results' / request.param
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / 'model.inp'
    text = MODEL.replace('FLOW_UNITS CMS', 'FLOW_UNITS ' + request.param)
    text = text.replace('[REPORT]\nINPUT NO', '[REPORT]\nINPUT NO\nNODES ALL\nLINKS ALL')
    text = text.replace('REPORT_2D NO', '''REPORT_2D NO
OUTPUT_FILE "résults with spaces.2d.h5"
REPORT_2D_VARIABLES ALL
REPORT_2D_STEP 00:02:00
OUTPUT_PRECISION FLOAT64
TRANSPORT_POLLUTANTS YES''')
    path.write_text(text, encoding='utf-8')
    solver = Solver(path, path.with_suffix('.rpt'), path.with_suffix('.out'))
    try:
        solver.open()
        with pytest.raises(LifecycleError):
            solver.open_results()
        solver.initialize()
        solver.start()
        for _ in solver.steps():
            pass
        expected = solver.surface2d.get_depths()
        solver.end()
        with solver.open_results() as result:
            assert result.surface is not None
            np.testing.assert_allclose(result.surface.depth.read(periods=-1), expected, atol=1e-7)
        yield path.with_suffix('.out'), directory / 'résults with spaces.2d.h5'
    finally:
        cleanup(solver)


def test_named_1d_fields_and_python_slices(saved):
    out_path, surface = saved
    with Results.open(out_path, surface=surface) as result, OutputReader(out_path) as raw:
        assert tuple(result.nodes) == tuple(raw.node_ids)
        assert result.nodes['J1'].id == 'J1'
        field = result.nodes['J1'].depth
        np.testing.assert_array_equal(field.read(), raw.node_series('J1', OutNodeVar.DEPTH))
        np.testing.assert_array_equal(field[1:5:2], field.read()[1:5:2])
        np.testing.assert_array_equal(field[::-1], field.read()[::-1])
        np.testing.assert_array_equal(result.nodes.depth.read(objects=['O1', 'J1', 'O1']),
                                      result.nodes.depth.read()[:, [1, 0, 1]])
        np.testing.assert_array_equal(result.nodes.depth[-1, 0], field[-1])
        np.testing.assert_array_equal(result.nodes.depth[..., 0], field.read())
        assert field.read(periods=slice(0, 0)).shape == (0,)
        assert result.nodes.depth.read(objects=[]).shape == (raw.period_count, 0)
        assert result.system.storage.read().shape == (raw.period_count,)
        assert result.system.evap.units.endswith('/day')
        assert result.nodes['J1'].species['TSS'].units == 'mg/L'
        with pytest.raises(KeyError):
            _ = result.nodes['missing']
        with pytest.raises(IndexError):
            _ = result.nodes.depth[raw.period_count]
        with pytest.raises(ValueError):
            field.read(objects=[0])
        # 2D uses a different report cadence; it must keep its own time axis.
        assert len(result.times) > len(result.surface.period_times)
        assert result.times.dtype.kind == result.surface.period_times.dtype.kind == 'M'
        assert field.units == ('m' if raw.flow_units.name == 'CMS' else 'ft')
        assert result.surface.depth.units == 'm'


def test_surface_fields_mesh_and_metadata(saved):
    _, path = saved
    with Results.open(surface=path) as result:
        surface = result.surface
        raw = surface.reader
        assert 'depth' in surface.variables
        assert surface.depth.dimensions == ('time', 'cell')
        np.testing.assert_array_equal(surface.depth.read(), raw.read('/Mesh2_face_depth'))
        np.testing.assert_array_equal(surface.depth[1:4, 1], raw.read('/Mesh2_face_depth', (slice(1, 4), 1)))
        np.testing.assert_array_equal(surface.cells[-1].depth.read(), surface.depth.read()[:, -1])
        np.testing.assert_array_equal(surface.depth.read(objects=[1, 0, 1]), surface.depth.read()[:, [1, 0, 1]])
        np.testing.assert_array_equal(surface.depth[::-1, ::-1], surface.depth.read()[::-1, ::-1])
        assert surface.depth.read(objects=[]).shape == (surface.depth.shape[0], 0)
        assert surface.max_depth.dimensions == ('cell',)
        assert surface.max_depth.times is None
        assert surface.field('/Mesh2_edge_flux').dimensions == ('time', 'cell', 'edge')
        assert surface.field('/Mesh2_edge_flux').read(periods=-1).shape == (2, 4)
        with pytest.raises(KeyError):
            surface.field('/Mesh2_face_never_recorded')
        with pytest.raises(ValueError):
            surface.max_depth.read(periods=0)
        mesh = surface.mesh
        assert mesh.vertices.shape == (5, 3)
        assert mesh.cells.shape == (2, 4)
        assert sorted(mesh.vertex_counts.tolist()) == [3, 4]
        assert mesh.coordinate_units == 'm'
        scale = 1 if saved[0].parent.name == 'CMS' else 0.3048
        assert mesh.metres_per_model_unit == pytest.approx(scale)
        assert mesh.vertices[:, 0].max() == pytest.approx(20 * scale)
        assert 'rainfall_in' in surface.mass_balance
        assert abs(surface.mass_balance['continuity_error']) < 1e-3
        assert surface.species['TSS'].units.lower() == 'mg/l'
        np.testing.assert_array_equal(surface.species['TSS'].read(), surface.species_conc.read(species='TSS'))
        assert surface.cells[0].species['TSS'].read().ndim == 1
        values = surface.depth.read()
    # Independent snapshots survive file close; retained fields do not.
    assert np.isfinite(values).all()
    with pytest.raises(BadHandleError):
        surface.depth.read()
    with pytest.raises(BadHandleError):
        _ = surface.depth.times


def test_native_reader_errors_lifecycle_and_serialized_threads(saved):
    _, path = saved
    with Output2DReader(path) as reader:
        expected = reader.read('/Mesh2_face_depth')
        with ThreadPoolExecutor(max_workers=3) as pool:
            copies = list(pool.map(lambda _: reader.read('/Mesh2_face_depth'), range(9)))
        for values in copies:
            np.testing.assert_array_equal(values, expected)
        assert reader.shape('/Mesh2_face_nodes')[1] == np.dtype('int64')
        assert reader.unlimited_axes('/Mesh2_face_depth') == (0,)
        with pytest.raises(IndexError):
            reader.read('/Mesh2_face_depth', 9999)
        with pytest.raises(IndexError):
            reader.read('/Mesh2_face_depth', (0, 0, 0))
        with pytest.raises(ValueError):
            reader.read('/Mesh2_face_depth', slice(None, None, 0))
        with pytest.raises(IndexError):
            reader.attributes('/not_a_group')
    reader.close()
    for call in (lambda: reader.datasets, lambda: reader.read('/time'), reader.__enter__):
        with pytest.raises(BadHandleError):
            call()
    with pytest.raises(FileError):
        Output2DReader(path.with_name('missing.h5'))
    with pytest.raises(FileError):
        Output2DReader(saved[0])
    with pytest.raises(ValueError):
        Output2DReader(str(path) + '\0invalid')


def test_c_api_validates_buffers_and_bounds_before_writing(saved):
    import openswmm.engine._output2d as extension
    api = ct.CDLL(extension.__file__)
    handle = ct.c_void_p()
    api.swmm_output2d_open.argtypes = [ct.c_char_p, ct.POINTER(ct.c_void_p)]
    api.swmm_output2d_close.argtypes = [ct.c_void_p]
    api.swmm_output2d_read_double.argtypes = [ct.c_void_p, ct.c_char_p,
        ct.POINTER(ct.c_uint64), ct.POINTER(ct.c_uint64), ct.POINTER(ct.c_uint64),
        ct.POINTER(ct.c_double), ct.c_size_t]
    api.swmm_output2d_datasets.argtypes = [ct.c_void_p, ct.c_char_p, ct.c_size_t, ct.POINTER(ct.c_size_t)]
    assert api.swmm_output2d_open(bytes(saved[1]), ct.byref(handle)) == 0
    try:
        start = (ct.c_uint64 * 2)(0, 0)
        count = (ct.c_uint64 * 2)(1, 2)
        stride = (ct.c_uint64 * 2)(1, 1)
        sentinel = (ct.c_double * 2)(-999, -999)
        read = api.swmm_output2d_read_double
        assert read(handle, b'/Mesh2_face_depth', start, count, stride, sentinel, 1) == 9
        assert list(sentinel) == [-999, -999]
        stride[0] = 0
        assert read(handle, b'/Mesh2_face_depth', start, count, stride, sentinel, 2) == 9
        stride[0] = 1
        count[0] = 2**64 - 1
        assert read(handle, b'/Mesh2_face_depth', start, count, stride, sentinel, 2) == 8
        assert list(sentinel) == [-999, -999]
        required = ct.c_size_t()
        assert api.swmm_output2d_datasets(handle, None, 0, ct.byref(required)) == 0
        tiny = ct.create_string_buffer(b'X', 2)
        assert api.swmm_output2d_datasets(handle, tiny, 2, ct.byref(required)) == 9
        assert tiny.value == b'X'
        assert read(None, b'/time', start, count, stride, sentinel, 2) == 7
    finally:
        api.swmm_output2d_close(handle)


def test_minimal_output_optional_fields_and_groundwater_layers():
    if not Output2DReader.available():
        pytest.skip('requires 2D HDF5 output')
    directory = Path(ARTIFACTS_DIR) / 'results' / 'groundwater'
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / 'layered.inp'
    path.write_text(MODEL.replace('REPORT_2D NO', '''REPORT_2D NO
OUTPUT_FILE layered.2d.h5
REPORT_2D_VARIABLES MINIMAL GROUNDWATER GW_DETAILED
TRANSPORT_POLLUTANTS YES'''))
    solver = Solver(path, path.with_suffix('.rpt'), path.with_suffix('.out'))
    try:
        solver.open()
        gw = solver.surface2d.groundwater
        gw.options['NODE_ENROLMENT'] = 'ROWS'
        gw.options['M_LAYERS'] = '4'
        gw.add_row(CellScope.GLOBAL, 36, 0.5, 0.45, 0.1, 2)
        gw.set_row_property(0, 'HG0', 0.2)
        gw.set_row_property(0, 'CLOSURE', GroundwaterClosure.SIGMA)
        gw.set_row_property(0, 'M_LAYERS', 4)
        gw.transport.options['TRANSPORT_POLLUTANTS'] = 'YES'
        gw.transport.options['UNSAT_TRANSPORT'] = 'LAYERED'
        solver.initialize()
        solver.start()
        for _ in solver.steps():
            pass
        solver.end()
    finally:
        cleanup(solver)
    with Results.open(surface=path.with_suffix('.2d.h5')) as result:
        assert 'vx' not in result.surface.variables
        field = result.surface.gw_layer_conc
        assert field.dimensions == ('time', 'species', 'layer', 'cell')
        assert field.shape[1:] == (1, 4, 2)
        values = field.read(species='TSS', objects=[1], periods=-1)
        assert values.shape == (4, 1)
        assert np.isfinite(values).all()
        np.testing.assert_array_equal(field[..., 0], field.read()[..., 0])


def test_missing_arguments_and_closed_1d_timestamps(saved):
    with pytest.raises(ValueError):
        Results.open()
    with Results.open(surface=saved[1]) as result:
        with pytest.raises(ValueError):
            _ = result.times
    raw = OutputReader(saved[0])
    _ = raw.period_times  # Ensure the cache cannot bypass the closed-handle check.
    raw.close()
    with pytest.raises(BadHandleError):
        _ = raw.period_times
