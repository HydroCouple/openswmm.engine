# SPDX-License-Identifier: Apache-2.0
#
# Copyright 2026 Caleb Buahin
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Tests for XSectionGeometry, Links.get_xsect_info and the XSectShape enum.

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: Apache-2.0
"""

import math
import unittest

import numpy as np

try:
    import openswmm.engine  # noqa: F401
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from openswmm.engine import (  # noqa: E402
    CrossSection,
    Links,
    XSectionGeometry,
    XSectShape,
    shape_name,
)

from tests.engine._solver_cases import EngineSolverCase  # noqa: E402

# The engine reproduces legacy SWMM bit-for-bit, including its truncated PI
# literal, so analytic expectations use the same constant.
ENGINE_PI = 3.141592654


# ===========================================================================
# Shape-enum parity
# ===========================================================================

class TestShapeEnumParity(unittest.TestCase):
    """Regression guard for the pre-6.0 numbering defect.

    ``XSectShape.IRREGULAR`` used to be 16, which the engine read as
    RECT_TRIANG — assigning it silently produced the wrong cross-section.
    These tests pin every member against the engine's own shape table.
    """

    def test_every_member_names_the_shape_the_engine_stores(self):
        # Both enums spell these two differently; same shape either way.
        for member in XSectShape:
            self.assertEqual(
                shape_name(int(member)), member.name,
                f"XSectShape.{member.name} = {member.value} but the engine "
                f"calls {member.value} {shape_name(int(member))!r}"
            )

    def test_enum_covers_every_engine_shape(self):
        self.assertEqual(len(list(XSectShape)), 26)
        self.assertEqual([m.value for m in XSectShape], list(range(26)))

    def test_the_previously_broken_members_have_their_corrected_values(self):
        # Explicitly pinned: these are the three that silently mismapped.
        self.assertEqual(XSectShape.IRREGULAR, 21)
        self.assertEqual(XSectShape.CUSTOM, 22)
        self.assertEqual(XSectShape.FORCE_MAIN, 23)

    def test_shape_name_rejects_invalid_codes(self):
        with self.assertRaises(ValueError):
            shape_name(26)
        with self.assertRaises(ValueError):
            shape_name(-1)


# ===========================================================================
# Standalone shapes
# ===========================================================================

class TestStandaloneShapes(unittest.TestCase):

    def test_circular_matches_analytic(self):
        xs = XSectionGeometry(XSectShape.CIRCULAR, 1.0, units="SI")
        self.assertAlmostEqual(xs.full_depth, 1.0, places=6)
        self.assertAlmostEqual(xs.full_area, ENGINE_PI / 4.0, places=6)
        self.assertAlmostEqual(xs.full_hyd_radius, 0.25, places=6)
        self.assertAlmostEqual(xs.max_width, 1.0, places=6)
        # Half-full pipe.
        self.assertAlmostEqual(xs.area(0.5), ENGINE_PI / 8.0,
                               delta=1e-4 * (ENGINE_PI / 8.0))
        self.assertAlmostEqual(xs.width(0.5), 1.0, places=6)
        self.assertFalse(xs.is_open)

    def test_rectangles(self):
        closed = XSectionGeometry(XSectShape.RECT_CLOSED, 3.0, 5.0, units="US")
        opench = XSectionGeometry(XSectShape.RECT_OPEN, 3.0, 5.0, units="US")
        self.assertAlmostEqual(closed.area(1.5), 7.5, places=6)
        self.assertAlmostEqual(closed.width(1.5), 5.0, places=6)
        self.assertFalse(closed.is_open)
        self.assertTrue(opench.is_open)
        # R = A/P with P = 2y + w.
        self.assertAlmostEqual(opench.hyd_radius(1.5), 7.5 / (2 * 1.5 + 5.0),
                               places=6)

    def test_trapezoid_uses_both_side_slopes(self):
        xs = XSectionGeometry(XSectShape.TRAPEZOIDAL, 4.0, 2.0, 1.0, 3.0,
                              units="US")
        # T(y) = bottom + (m1+m2)*y
        self.assertAlmostEqual(xs.width(2.5), 2.0 + 4.0 * 2.5, places=6)
        # A(y) = (bottom + m_avg*y)*y
        self.assertAlmostEqual(xs.area(2.5), (2.0 + 2.0 * 2.5) * 2.5, places=6)

    def test_triangular(self):
        xs = XSectionGeometry(XSectShape.TRIANGULAR, 2.0, 6.0, units="US")
        self.assertAlmostEqual(xs.area(2.0), 0.5 * 6.0 * 2.0, places=6)
        self.assertAlmostEqual(xs.width(1.0), 3.0, places=6)

    def test_force_main_is_geometrically_circular(self):
        circ = XSectionGeometry(XSectShape.CIRCULAR, 1.5, units="US")
        fm = XSectionGeometry(XSectShape.FORCE_MAIN, 1.5, 130.0, units="US")
        for y in (0.2, 0.75, 1.5):
            self.assertAlmostEqual(fm.area(y), circ.area(y), places=6)

    def test_dummy_is_all_zeros(self):
        xs = XSectionGeometry(XSectShape.DUMMY, 0.0, units="US")
        self.assertEqual(xs.area(1.0), 0.0)
        self.assertEqual(xs.width(1.0), 0.0)
        self.assertEqual(xs.critical_depth(5.0), 0.0)

    def test_every_self_contained_shape_constructs(self):
        tabulated = {XSectShape.IRREGULAR, XSectShape.CUSTOM,
                     XSectShape.STREET_XSECT}
        for member in XSectShape:
            if member in tabulated:
                continue
            xs = XSectionGeometry(member, 4.0, 2.0, 1.0, 1.0, units="US")
            self.assertEqual(xs.shape, member)
            if member is not XSectShape.DUMMY:
                self.assertGreater(xs.full_depth, 0.0)
                self.assertGreater(xs.full_area, 0.0)

    def test_inverse_round_trip(self):
        for shape in (XSectShape.CIRCULAR, XSectShape.RECT_CLOSED,
                      XSectShape.TRAPEZOIDAL, XSectShape.TRIANGULAR):
            xs = XSectionGeometry(shape, 3.0, 2.0, 1.0, 1.0, units="US")
            for frac in (0.1, 0.5, 0.95):
                y = frac * xs.full_depth
                self.assertAlmostEqual(
                    xs.depth_from_area(xs.area(y)), y,
                    delta=1e-3 * xs.full_depth, msg=f"{shape.name} @ {frac}")

    def test_critical_depth_increases_with_flow(self):
        xs = XSectionGeometry(XSectShape.CIRCULAR, 3.0, units="US")
        ycs = [xs.critical_depth(q) for q in (0.5, 2.0, 8.0, 20.0)]
        self.assertEqual(ycs, sorted(ycs))
        self.assertTrue(all(y > 0 for y in ycs))
        self.assertEqual(xs.critical_depth(0.0), 0.0)

    def test_rectangular_critical_depth_matches_closed_form(self):
        b, q, g = 4.0, 30.0, 32.2
        xs = XSectionGeometry(XSectShape.RECT_OPEN, 10.0, b, units="US")
        self.assertAlmostEqual(xs.critical_depth(q),
                               (q**2 / (g * b**2)) ** (1 / 3), delta=1e-3)

    def test_section_factor_inverse(self):
        xs = XSectionGeometry(XSectShape.CIRCULAR, 2.0, units="US")
        a = xs.area(0.8)
        sf = xs.section_factor(a)
        self.assertGreater(sf, 0)
        self.assertAlmostEqual(xs.area_from_section_factor(sf), a,
                               delta=1e-3 * a)

    def test_dsda_is_the_derivative_of_section_factor(self):
        # Catches a wrong unit factor on dsda: it is dS/dA, so it must track a
        # finite difference of section_factor with respect to area.
        xs = XSectionGeometry(XSectShape.CIRCULAR, 2.0, units="US")
        h = 1e-6
        for a in (0.5, 1.0, 2.0):
            fd = (xs.section_factor(a + h) - xs.section_factor(a - h)) / (2 * h)
            self.assertAlmostEqual(xs.dsda(a), fd, delta=1e-6 * abs(fd))


class TestUnitConversionExponents(unittest.TestCase):
    """Each quantity must convert by the right power of the length factor.

    A wrong exponent is invisible in a single unit system — these compare the
    same physical section built in both.
    """

    M_PER_FT = 0.3048

    def _pair(self, shape, *geoms):
        si = XSectionGeometry(shape, *geoms, units="SI")
        us = XSectionGeometry(shape, *[g / self.M_PER_FT for g in geoms],
                              units="US")
        return si, us

    def test_area_converts_as_length_squared(self):
        si, us = self._pair(XSectShape.CIRCULAR, 1.0)
        self.assertAlmostEqual(
            si.area(0.5),
            us.area(0.5 / self.M_PER_FT) * self.M_PER_FT ** 2, places=6)

    def test_length_quantities_convert_linearly(self):
        si, us = self._pair(XSectShape.CIRCULAR, 1.0)
        y_si, y_us = 0.5, 0.5 / self.M_PER_FT
        self.assertAlmostEqual(si.width(y_si),
                               us.width(y_us) * self.M_PER_FT, places=6)
        self.assertAlmostEqual(si.hyd_radius(y_si),
                               us.hyd_radius(y_us) * self.M_PER_FT, places=6)

    def test_section_factor_converts_as_length_to_the_eight_thirds(self):
        si, us = self._pair(XSectShape.RECT_OPEN, 2.0, 3.0)
        self.assertAlmostEqual(
            si.full_section_factor,
            us.full_section_factor * self.M_PER_FT ** (8 / 3), places=6)

    def test_dsda_converts_as_length_to_the_two_thirds(self):
        si, us = self._pair(XSectShape.CIRCULAR, 1.0)
        a_si = si.area(0.5)
        a_us = us.area(0.5 / self.M_PER_FT)
        self.assertAlmostEqual(
            si.dsda(a_si),
            us.dsda(a_us) * self.M_PER_FT ** (2 / 3), places=6)

    def test_critical_depth_converts_with_its_flow_units(self):
        # SI takes CMS, US takes CFS — 1 cms = 35.3147 cfs.
        si, us = self._pair(XSectShape.CIRCULAR, 1.0)
        q_cms = 0.5
        expected = us.critical_depth(q_cms / 0.02832) * self.M_PER_FT
        self.assertAlmostEqual(si.critical_depth(q_cms), expected,
                               delta=1e-3 * expected)


# ===========================================================================
# Units
# ===========================================================================

class TestUnits(unittest.TestCase):

    def test_us_and_si_agree(self):
        m_per_ft = 0.3048
        si = XSectionGeometry(XSectShape.CIRCULAR, 1.0, units="SI")
        us = XSectionGeometry(XSectShape.CIRCULAR, 1.0 / m_per_ft, units="US")
        self.assertAlmostEqual(si.area(0.5),
                               us.area(0.5 / m_per_ft) * m_per_ft**2, places=6)
        self.assertEqual(si.units, "SI")
        self.assertEqual(us.units, "US")

    def test_flow_units_default_per_system(self):
        self.assertEqual(
            XSectionGeometry(XSectShape.CIRCULAR, 1.0, units="SI").flow_units,
            "CMS")
        self.assertEqual(
            XSectionGeometry(XSectShape.CIRCULAR, 1.0, units="US").flow_units,
            "CFS")

    def test_units_is_required_and_validated(self):
        with self.assertRaises(TypeError):
            XSectionGeometry(XSectShape.CIRCULAR, 1.0)  # no units
        with self.assertRaises(ValueError):
            XSectionGeometry(XSectShape.CIRCULAR, 1.0, units="metric")
        with self.assertRaises(TypeError):
            XSectionGeometry(XSectShape.CIRCULAR, 1.0, units=1)

    def test_units_accepts_either_case(self):
        self.assertEqual(
            XSectionGeometry(XSectShape.CIRCULAR, 1.0, units="si").units, "SI")
        self.assertEqual(
            XSectionGeometry(XSectShape.CIRCULAR, 1.0, units="us").units, "US")


# ===========================================================================
# Tabulated shapes
# ===========================================================================

class TestTabulatedShapes(unittest.TestCase):

    # A symmetric trapezoid: 2 ft bottom at elev 0, 1:1 banks rising 4 ft.
    STATIONS = [0.0, 4.0, 6.0, 10.0]
    ELEVATIONS = [4.0, 0.0, 0.0, 4.0]

    def _transect(self, **kw):
        kw.setdefault("left_bank", 4.0)
        kw.setdefault("right_bank", 6.0)
        kw.setdefault("n_channel", 0.03)
        kw.setdefault("units", "US")
        return XSectionGeometry.from_transect(self.STATIONS, self.ELEVATIONS, **kw)

    def test_from_transect_approximates_the_analytic_trapezoid(self):
        irr = self._transect()
        self.assertEqual(irr.shape, XSectShape.IRREGULAR)
        self.assertAlmostEqual(irr.full_depth, 4.0, places=6)
        self.assertAlmostEqual(irr.max_width, 10.0, places=6)
        self.assertTrue(irr.is_open)

        tz = XSectionGeometry(XSectShape.TRAPEZOIDAL, 4.0, 2.0, 1.0, 1.0,
                              units="US")
        for y in (1.0, 2.0, 3.0, 4.0):
            self.assertAlmostEqual(irr.area(y), tz.area(y),
                                   delta=0.05 * tz.area(y))
            self.assertAlmostEqual(irr.width(y), tz.width(y),
                                   delta=0.05 * tz.width(y))

    def test_roughness_changes_hydraulic_radius_but_not_area(self):
        # The conveyance-weighted hyd-radius table depends on the overbank n,
        # which is why from_transect takes them at all.
        uniform = self._transect(n_left=0.03, n_right=0.03)
        rough = self._transect(n_left=0.15, n_right=0.15)
        self.assertAlmostEqual(uniform.area(3.0), rough.area(3.0), places=6)
        self.assertNotEqual(uniform.hyd_radius(3.0), rough.hyd_radius(3.0))

    def test_from_transect_validates_input(self):
        with self.assertRaises(ValueError):
            self._transect(n_channel=0.0)          # n_channel must be > 0
        with self.assertRaises(ValueError):
            XSectionGeometry.from_transect([0.0], [1.0], left_bank=0,
                                           right_bank=0, n_channel=0.03,
                                           units="US")   # < 2 points
        with self.assertRaises(ValueError):
            XSectionGeometry.from_transect([0.0, 1.0], [1.0], left_bank=0,
                                           right_bank=1, n_channel=0.03,
                                           units="US")   # length mismatch

    def test_from_curve(self):
        # A constant-width ("rectangular") shape curve.
        xs = XSectionGeometry.from_curve(5.0, [0.0, 0.5, 1.0], [1.0, 1.0, 1.0],
                                         units="US")
        self.assertEqual(xs.shape, XSectShape.CUSTOM)
        self.assertAlmostEqual(xs.full_depth, 5.0, places=6)
        self.assertAlmostEqual(xs.width(1.0), xs.width(4.0),
                               delta=1e-4 * xs.width(4.0))

    def test_from_curve_validates_input(self):
        with self.assertRaises(ValueError):
            XSectionGeometry.from_curve(0.0, [0.0, 1.0], [1.0, 1.0], units="US")
        with self.assertRaises(ValueError):
            XSectionGeometry.from_curve(5.0, [0.0], [1.0], units="US")

    def test_from_street(self):
        # 2% over 20 ft rises 0.4 ft — below the 0.5 ft curb, so the curb sets
        # the full depth.
        xs = XSectionGeometry.from_street(20.0, 0.5, 2.0, 0.016, units="US")
        self.assertEqual(xs.shape, XSectShape.STREET_XSECT)
        self.assertAlmostEqual(xs.full_depth, 0.5, places=6)
        self.assertAlmostEqual(xs.max_width, 40.0, places=6)   # full street = two halves
        self.assertGreater(xs.full_area, 0)

    def test_from_street_half_is_half_as_wide(self):
        full = XSectionGeometry.from_street(20.0, 0.5, 2.0, 0.016, sides=2,
                                            units="US")
        half = XSectionGeometry.from_street(20.0, 0.5, 2.0, 0.016, sides=1,
                                            units="US")
        self.assertAlmostEqual(half.max_width, full.max_width / 2.0, places=6)

    def test_from_street_validates_input(self):
        with self.assertRaises(ValueError):
            XSectionGeometry.from_street(20.0, 0.5, 2.0, 0.016, sides=3,
                                         units="US")
        with self.assertRaises(ValueError):
            XSectionGeometry.from_street(20.0, 0.5, 2.0, 0.0, units="US")

    def test_tabulated_shapes_reject_the_plain_constructor(self):
        for shape in (XSectShape.IRREGULAR, XSectShape.CUSTOM,
                      XSectShape.STREET_XSECT):
            with self.assertRaises(ValueError):
                XSectionGeometry(shape, 1.0, units="US")


# ===========================================================================
# NumPy batch
# ===========================================================================

class TestBatch(unittest.TestCase):

    def setUp(self):
        self.pipe = XSectionGeometry(XSectShape.CIRCULAR, 2.0, units="US")

    def test_array_matches_scalar_loop(self):
        pipe = self.pipe
        depths = np.array([0.0, 0.25, 0.5, 1.0, 1.5, 2.0])
        got = pipe.area(depths)
        self.assertIsInstance(got, np.ndarray)
        self.assertEqual(got.dtype, np.float64)
        np.testing.assert_allclose(got, [pipe.area(float(d)) for d in depths])

    def test_every_method_accepts_arrays(self):
        pipe = self.pipe
        d = np.array([0.4, 0.9, 1.6])
        a = pipe.area(d)
        for got, ref in (
            (pipe.width(d), [pipe.width(float(x)) for x in d]),
            (pipe.hyd_radius(d), [pipe.hyd_radius(float(x)) for x in d]),
            (pipe.depth_from_area(a), [pipe.depth_from_area(float(x)) for x in a]),
            (pipe.hyd_radius_from_area(a), [pipe.hyd_radius_from_area(float(x)) for x in a]),
            (pipe.section_factor(a), [pipe.section_factor(float(x)) for x in a]),
            (pipe.dsda(a), [pipe.dsda(float(x)) for x in a]),
            (pipe.critical_depth(d), [pipe.critical_depth(float(x)) for x in d]),
        ):
            np.testing.assert_allclose(got, ref)

    def test_shape_is_preserved(self):
        d = np.array([[0.1, 0.2], [0.3, 0.4]])
        self.assertEqual(self.pipe.area(d).shape, (2, 2))

    def test_empty_array(self):
        got = self.pipe.area(np.array([]))
        self.assertIsInstance(got, np.ndarray)
        self.assertEqual(got.size, 0)

    def test_list_input_works(self):
        pipe = self.pipe
        np.testing.assert_allclose(pipe.area([0.5, 1.0]),
                                   [pipe.area(0.5), pipe.area(1.0)])

    def test_scalar_returns_float(self):
        self.assertIsInstance(self.pipe.area(0.5), float)

    def test_bad_array_values_rejected(self):
        with self.assertRaises(ValueError):
            self.pipe.area(np.array([1.0, -1.0]))
        with self.assertRaises(ValueError):
            self.pipe.area(np.array([1.0, np.nan]))


# ===========================================================================
# Link-bound
# ===========================================================================

class TestLinkBound(EngineSolverCase):

    def test_geometry_matches_standalone(self):
        solver = self.opened_solver()
        link = solver.links[0]
        shape, g1, g2, g3, g4 = link.xsect.as_tuple()
        xs = link.xsect.geometry()
        self.assertEqual(xs.shape, shape)

        standalone = XSectionGeometry(shape, g1, g2, g3, g4,
                                      units=xs.units)
        for frac in (0.25, 0.5, 0.9):
            y = frac * xs.full_depth
            self.assertAlmostEqual(xs.area(y), standalone.area(y), places=6)

    def test_inherits_the_models_units(self):
        solver = self.opened_solver()
        xs = solver.links[0].xsect.geometry()
        self.assertIn(xs.units, ("US", "SI"))
        self.assertEqual(xs.units,
                         ("SI" if solver.unit_system == "SI" else "US"))

    def test_handle_outlives_the_solver(self):
        from openswmm.engine import Solver
        inp, rpt, out = self.solver_files()
        s = Solver(inp, rpt, out)
        s.open()
        xs = s.links[0].xsect.geometry()
        before = xs.area(0.5 * xs.full_depth)
        s.close()
        s.destroy()
        # The handle deep-copied its geometry, so it still answers.
        self.assertAlmostEqual(xs.area(0.5 * xs.full_depth), before, places=6)

    def test_from_link_classmethod(self):
        solver = self.opened_solver()
        link = solver.links[0]
        self.assertAlmostEqual(XSectionGeometry.from_link(link).full_depth,
                               link.xsect.geometry().full_depth, places=6)

    def test_every_conduit_yields_a_usable_geometry(self):
        from openswmm.engine import LinkType
        solver = self.opened_solver()
        n = 0
        for link in solver.links:
            if link.type != LinkType.CONDUIT:
                continue
            xs = link.xsect.geometry()
            self.assertGreater(xs.full_depth, 0)
            self.assertGreater(xs.area(0.5 * xs.full_depth), 0)
            n += 1
        self.assertGreater(n, 0, "the fixture model should contain conduits")


# ===========================================================================
# get_xsect_info / CrossSection
# ===========================================================================

class TestXsectInfo(EngineSolverCase):

    def test_get_xsect_info_returns_a_cross_section(self):
        links = Links(self.opened_solver())
        xs = links.get_xsect_info(0)
        self.assertIsInstance(xs, CrossSection)
        self.assertEqual(xs.shape_name, XSectShape(xs.shape).name)

    def test_by_id_and_by_index_agree(self):
        links = Links(self.opened_solver())
        first_id = links.get_id(0)
        self.assertEqual(links.get_xsect_info(first_id), links.get_xsect_info(0))

    def test_geom_labels_are_named(self):
        links = Links(self.opened_solver())
        for i in range(len(links)):
            xs = links.get_xsect_info(i)
            labels = xs.geom_labels
            self.assertIsInstance(labels, dict)
            if xs.shape == XSectShape.CIRCULAR:
                self.assertEqual(list(labels), ["diameter"])
                self.assertAlmostEqual(labels["diameter"], xs.geom1, places=6)

    def test_link_view_info_agrees_with_collection(self):
        solver = self.opened_solver()
        links = Links(solver)
        self.assertEqual(solver.links[0].xsect.info(), links.get_xsect_info(0))

    def test_from_raw(self):
        xs = CrossSection.from_raw(XSectShape.CIRCULAR, 1.2, 0.0, 0.0, 0.0)
        self.assertEqual(xs.shape_name, "CIRCULAR")
        self.assertEqual(xs.geom_labels, {"diameter": 1.2})

    def test_geom_labels_for_the_renumbered_shapes(self):
        # These labels were attached to the wrong codes before 6.0.
        self.assertEqual(
            list(CrossSection.from_raw(XSectShape.HORIZ_ELLIPSE, 3, 4, 0, 0)
                 .geom_labels), ["height", "width"])
        self.assertEqual(
            list(CrossSection.from_raw(XSectShape.ARCH, 3, 4, 0, 0)
                 .geom_labels), ["height", "width"])
        self.assertEqual(
            list(CrossSection.from_raw(XSectShape.IRREGULAR, 2, 0, 0, 0)
                 .geom_labels), ["transect_index"])

    def test_unknown_code_falls_back(self):
        xs = CrossSection.from_raw(99, 1.0, 0.0, 0.0, 0.0)
        self.assertEqual(xs.shape_name, "UNKNOWN(99)")
        self.assertEqual(list(xs.geom_labels),
                         ["geom1", "geom2", "geom3", "geom4"])


# ===========================================================================
# Errors
# ===========================================================================

class TestErrors(EngineSolverCase):

    def setUp(self):
        self.pipe = XSectionGeometry(XSectShape.CIRCULAR, 2.0, units="US")

    def test_negative_input_rejected(self):
        with self.assertRaises(ValueError):
            self.pipe.area(-1.0)
        with self.assertRaises(ValueError):
            self.pipe.depth_from_area(-1.0)
        with self.assertRaises(ValueError):
            self.pipe.critical_depth(-1.0)

    def test_nan_rejected(self):
        with self.assertRaises(ValueError):
            self.pipe.area(float("nan"))

    def test_unknown_shape_rejected(self):
        with self.assertRaises(ValueError):
            XSectionGeometry(99, 1.0, units="US")

    def test_degenerate_geometry_rejected(self):
        with self.assertRaises(ValueError):
            XSectionGeometry(XSectShape.CIRCULAR, 0.0, units="US")
        with self.assertRaises(ValueError):
            # Fill above the crown.
            XSectionGeometry(XSectShape.FILLED_CIRCULAR, 2.0, 3.0, units="US")

    def test_depth_above_full_is_clamped_not_rejected(self):
        # Closed shapes clamp, matching the routing solvers — a surcharged
        # conduit must not raise.
        self.assertAlmostEqual(self.pipe.area(100.0), self.pipe.full_area,
                               places=6)

    def test_geometry_of_a_stale_link_raises(self):
        from openswmm.engine import StaleObjectError
        solver = self.opened_solver()
        link = solver.links[0]
        solver._bump_generation()
        with self.assertRaises(StaleObjectError):
            link.xsect.geometry()
