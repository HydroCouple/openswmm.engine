"""New public APIs against the compiled engine, including failed-edit rollback."""
from copy import deepcopy
from pathlib import Path
import math
import unittest

from openswmm.engine import (
    Solver, EngineError, LidType, LidNodeLayer, LidNodeLayerKind as K,
    LidLayerTreatment, CellScope, SurfaceInfilMethod, SurfaceInfilDest,
)
from tests._paths import SITE_DRAINAGE_INP, artifact_dir
from tests.engine.test_lid_nodes import DECK


class TestCoreApiAdditions(unittest.TestCase):
    def open_deck(self, deck):
        directory = Path(artifact_dir(self))
        path = directory / "model.inp"
        path.write_text(deck)
        solver = Solver(path, path.with_suffix('.rpt'), path.with_suffix('.out'))
        solver.open()
        self.addCleanup(solver.destroy)
        self.addCleanup(solver.close)
        return solver

    def test_snowpack_assignment_clear_and_failed_edit(self):
        solver = self.open_deck(Path(SITE_DRAINAGE_INP).read_text())
        name = next(iter(solver.subcatchments)).id
        solver.snowpacks.add('Snow')
        subcatchment = solver.subcatchments[name]
        self.assertEqual(subcatchment.snowpack, '')
        subcatchment.snowpack = 'Snow'
        self.assertEqual(subcatchment.snowpack, 'Snow')
        with self.assertRaises(EngineError):
            subcatchment.snowpack = 'missing'
        self.assertEqual(subcatchment.snowpack, 'Snow')
        subcatchment.snowpack = None
        self.assertEqual(subcatchment.snowpack, '')
        solver.initialize()
        with self.assertRaises(EngineError):
            subcatchment.snowpack = 'Snow'

    def test_richards_atomic_configuration_and_live_state(self):
        solver = self.open_deck(DECK)
        lids = solver.infrastructure.lids
        lids.add('Column', LidType.NODE)
        layers = [LidNodeLayer(K.SURFACE, (6, .1)),
                  LidNodeLayer(K.MEDIA, (12, .45, .2, .08, 2, 10, 3)),
                  LidNodeLayer(K.AGGREGATE, (6, .4, 100)),
                  LidNodeLayer(K.BOTTOM, (0, 0))]
        lids.set_layers('Column', layers, treatments=[LidLayerTreatment(2, 'TSS', 25)])
        options = dict(model=1, cells_per_layer=2, atol=1e-7, rtol=1e-5, max_step=30.)
        material = dict(theta_r=.03, alpha=2., n=1.6, l=.5, specific_storage=.0001)
        materials = [{}, material, material, {}]
        lids.set_layers('Column', layers, flow=options, materials=materials)
        self.assertEqual(lids.get_flow_options('Column'), options)
        self.assertEqual(lids.get_materials('Column')[1], material)
        self.assertEqual(lids.get_treatments('Column'), [LidLayerTreatment(2, 'TSS', 25)])
        invalid = deepcopy(materials)
        invalid[1]['alpha'] = -1
        with self.assertRaises(EngineError):
            lids.set_layers('Column', layers, flow=options, materials=invalid)
        self.assertEqual(lids.get_flow_options('Column'), options)
        self.assertEqual(lids.get_materials('Column')[1], material)
        self.assertEqual(lids.get_layers('Column'), layers)
        with self.assertRaises(ValueError):
            lids.set_layers('Column', layers, flow=options, materials=[])
        lids.assign_node('S', 'Column', initial_saturation=25)
        self.assertEqual(lids.richards_profile('S'), [])
        solver.initialize()
        solver.start()
        solver.step()
        profile = lids.richards_profile('S')
        self.assertEqual(len(profile), 5)
        self.assertTrue(all(math.isfinite(value) for row in profile for value in row.values()))
        self.assertTrue(all(row['water'] >= 0 for row in profile))
        stats = lids.richards_statistics('S')
        self.assertGreater(stats['accepted'], 0)
        self.assertGreaterEqual(stats['rejected'], 0)
        self.assertTrue(math.isfinite(stats['balance_m3']))
        with self.assertRaises(EngineError):
            lids.set_layers('Column', layers, flow=options, materials=materials)
        solver.end()


try:
    import openswmm.engine._2d
    HAVE_2D = True
except ImportError:
    HAVE_2D = False


@unittest.skipUnless(HAVE_2D, '2D bindings not built')
class TestSurfaceApiAdditions(unittest.TestCase):
    def setUp(self):
        from tests.engine._groundwater_cases import open_model, cleanup
        extra = '''[RAINGAGES]
G INTENSITY 0:01 1 TIMESERIES RAIN
[SUBCATCHMENTS]
S G O1 0.01 50 10 1 0
[SUBAREAS]
S 0.01 0.1 0.1 0.1 25 OUTLET
[INFILTRATION]
S 50 5 3 7 0
[POLYGONS]
S 0 0
S 10 0
S 10 10
S 0 10
[TIMESERIES]
RAIN 0:00 0.5
RAIN 0:01 1.0
RAIN 0:02 0.0
'''
        self.solver = open_model(artifact_dir(self), extra)
        self.addCleanup(cleanup, self.solver)
        self.surface = self.solver.surface2d

    def test_ordered_infiltration_records_undo_and_bulk_ownership(self):
        from openswmm.engine import Infil2DRow
        infil = self.surface.infiltration
        infil.defaults['*'] = Infil2DRow(SurfaceInfilMethod.CONSTANT, (1.,))
        baseline = infil.authored_rows()
        proposed = deepcopy(baseline + baseline)
        proposed[1]['dest_explicit'] = False
        cell = dict(cell=0, tag='', row=baseline[0]['row']._replace(dest=SurfaceInfilDest.AQUIFER_2D),
                    dest_explicit=True)
        proposed.append(cell)
        infil.replace_authored_rows(proposed)
        self.assertEqual(infil.authored_rows(), proposed)
        self.assertEqual(infil.ownership(0)[2], 2)
        owners, rows, conflicts = infil.ownership_bulk()
        for cell_index in range(len(owners)):
            self.assertEqual(infil.ownership(cell_index),
                             (owners[cell_index], rows[cell_index], conflicts[cell_index]))
        invalid = deepcopy(proposed)
        invalid[-1]['cell'] = 1000000
        with self.assertRaises(EngineError):
            infil.replace_authored_rows(invalid)
        self.assertEqual(infil.authored_rows(), proposed)
        invalid = deepcopy(proposed)
        invalid[0]['tag'] = 'x' * 4096
        with self.assertRaises(ValueError):
            infil.replace_authored_rows(invalid)
        self.assertEqual(infil.authored_rows(), proposed)
        infil.replace_authored_rows(baseline)
        self.assertEqual(infil.authored_rows(), baseline)
        infil.replace_authored_rows([])
        self.assertEqual(infil.authored_rows(), [])

    def test_groundwater_process_options_are_atomic(self):
        options = self.surface.groundwater.options
        options.set_process_options(et='NONE', link='ONE_WAY', wilting=5., authored=True)
        before = [options[key] for key in ('GW_ET', 'LINK_SEEPAGE', 'WILTING_SUCTION', 'OPTIONS_AUTHORED')]
        self.assertEqual(before[:2], ['NONE', 'ONE_WAY'])
        self.assertAlmostEqual(float(before[2]), 5.)
        with self.assertRaises(EngineError):
            options.set_process_options(et='BOTH', link='invalid', wilting=7.)
        self.assertEqual([options[key] for key in ('GW_ET', 'LINK_SEEPAGE', 'WILTING_SUCTION', 'OPTIONS_AUTHORED')], before)
        options.set_process_options(et='AUTO', link='DEFAULT', authored=False)
        self.assertEqual(options['OPTIONS_AUTHORED'], 'NO')
        self.solver.initialize()
        with self.assertRaises(EngineError):
            self.surface.replace_surface_owners([], '0' * 16)
        with self.assertRaises(EngineError):
            options.set_process_options(et='NONE', link='ONE_WAY')

    def test_surface_ownership_preview_revision_and_clear(self):
        self.surface.groundwater.add_row(CellScope.GLOBAL, 5., 2., .45, .05, 3.)
        self.assertEqual(list(self.surface.get_surface_owners()), [])
        preview = self.surface.preview_surface_owners([0])
        self.assertTrue(preview['valid'], preview['diagnostics'])
        self.assertEqual(preview['objects'][0]['name'], 'S')
        self.assertAlmostEqual(preview['objects'][0]['declared_area'], 100.)
        self.assertTrue(preview['shares'])
        self.assertEqual(len(preview['token']), 16)
        self.surface.replace_surface_owners([0], preview['token'])
        self.assertEqual(list(self.surface.get_surface_owners()), [0])
        with self.assertRaisesRegex(EngineError, 'stale'):
            self.surface.replace_surface_owners([], preview['token'])
        self.assertEqual(list(self.surface.get_surface_owners()), [0])
        invalid = self.surface.preview_surface_owners([0, 0])
        self.assertFalse(invalid['valid'])
        with self.assertRaises(EngineError):
            self.surface.replace_surface_owners([0, 0], invalid['token'])
        self.assertEqual(list(self.surface.get_surface_owners()), [0])
        with self.assertRaises(TypeError):
            self.surface.preview_surface_owners([0.5])
        with self.assertRaises(ValueError):
            self.surface.replace_surface_owners([2**32], invalid['token'])
        fresh = self.surface.preview_surface_owners()
        self.surface.replace_surface_owners([], fresh['token'])
        self.assertEqual(list(self.surface.get_surface_owners()), [])
        fresh = self.surface.preview_surface_owners([0])
        self.surface.replace_surface_owners([0], fresh['token'])
        # Stored authoring is explicitly barred from runtime until R4 qualifies.
        with self.assertRaises(EngineError):
            self.solver.initialize()

    def test_report_rainfall_uses_report_instant_in_si(self):
        self.solver.initialize()
        from openswmm.engine._datetime import encode_date
        self.solver.start()
        self.solver.step()
        current = list(self.surface.get_rainfall_bulk())
        values = self.surface.get_report_rainfall_bulk(encode_date(2026, 1, 1) + 60 / 86400)
        self.assertEqual(len(values), self.surface.n_triangles)
        # The future report interval is 1 mm/h, independent of the live window.
        for value in values:
            self.assertAlmostEqual(value, 1. / 1000 / 3600, places=12)
        self.assertEqual(list(self.surface.get_rainfall_bulk()), current)
        self.solver.end()


if __name__ == '__main__':
    unittest.main()
