"""Tests for XSectionGeometry, Links.get_xsect_info and the XSectShape enum.

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT
"""

import math

import numpy as np
import pytest

from openswmm.engine import (
    CrossSection,
    Links,
    XSectionGeometry,
    XSectShape,
    shape_name,
)

# The engine reproduces legacy SWMM bit-for-bit, including its truncated PI
# literal, so analytic expectations use the same constant.
ENGINE_PI = 3.141592654


# ===========================================================================
# Shape-enum parity
# ===========================================================================

class TestShapeEnumParity:
    """Regression guard for the pre-6.0 numbering defect.

    ``XSectShape.IRREGULAR`` used to be 16, which the engine read as
    RECT_TRIANG — assigning it silently produced the wrong cross-section.
    These tests pin every member against the engine's own shape table.
    """

    def test_every_member_names_the_shape_the_engine_stores(self):
        # Both enums spell these two differently; same shape either way.
        for member in XSectShape:
            assert shape_name(int(member)) == member.name, (
                f"XSectShape.{member.name} = {member.value} but the engine "
                f"calls {member.value} {shape_name(int(member))!r}"
            )

    def test_enum_covers_every_engine_shape(self):
        assert len(list(XSectShape)) == 26
        assert [m.value for m in XSectShape] == list(range(26))

    def test_the_previously_broken_members_have_their_corrected_values(self):
        # Explicitly pinned: these are the three that silently mismapped.
        assert XSectShape.IRREGULAR == 21
        assert XSectShape.CUSTOM == 22
        assert XSectShape.FORCE_MAIN == 23

    def test_shape_name_rejects_invalid_codes(self):
        with pytest.raises(ValueError):
            shape_name(26)
        with pytest.raises(ValueError):
            shape_name(-1)


# ===========================================================================
# Standalone shapes
# ===========================================================================

class TestStandaloneShapes:

    def test_circular_matches_analytic(self):
        xs = XSectionGeometry(XSectShape.CIRCULAR, 1.0, units="SI")
        assert xs.full_depth == pytest.approx(1.0)
        assert xs.full_area == pytest.approx(ENGINE_PI / 4.0)
        assert xs.full_hyd_radius == pytest.approx(0.25)
        assert xs.max_width == pytest.approx(1.0)
        # Half-full pipe.
        assert xs.area(0.5) == pytest.approx(ENGINE_PI / 8.0, rel=1e-4)
        assert xs.width(0.5) == pytest.approx(1.0)
        assert not xs.is_open

    def test_rectangles(self):
        closed = XSectionGeometry(XSectShape.RECT_CLOSED, 3.0, 5.0, units="US")
        opench = XSectionGeometry(XSectShape.RECT_OPEN, 3.0, 5.0, units="US")
        assert closed.area(1.5) == pytest.approx(7.5)
        assert closed.width(1.5) == pytest.approx(5.0)
        assert not closed.is_open
        assert opench.is_open
        # R = A/P with P = 2y + w.
        assert opench.hyd_radius(1.5) == pytest.approx(7.5 / (2 * 1.5 + 5.0))

    def test_trapezoid_uses_both_side_slopes(self):
        xs = XSectionGeometry(XSectShape.TRAPEZOIDAL, 4.0, 2.0, 1.0, 3.0,
                              units="US")
        # T(y) = bottom + (m1+m2)*y
        assert xs.width(2.5) == pytest.approx(2.0 + 4.0 * 2.5)
        # A(y) = (bottom + m_avg*y)*y
        assert xs.area(2.5) == pytest.approx((2.0 + 2.0 * 2.5) * 2.5)

    def test_triangular(self):
        xs = XSectionGeometry(XSectShape.TRIANGULAR, 2.0, 6.0, units="US")
        assert xs.area(2.0) == pytest.approx(0.5 * 6.0 * 2.0)
        assert xs.width(1.0) == pytest.approx(3.0)

    def test_force_main_is_geometrically_circular(self):
        circ = XSectionGeometry(XSectShape.CIRCULAR, 1.5, units="US")
        fm = XSectionGeometry(XSectShape.FORCE_MAIN, 1.5, 130.0, units="US")
        for y in (0.2, 0.75, 1.5):
            assert fm.area(y) == pytest.approx(circ.area(y))

    def test_dummy_is_all_zeros(self):
        xs = XSectionGeometry(XSectShape.DUMMY, 0.0, units="US")
        assert xs.area(1.0) == 0.0
        assert xs.width(1.0) == 0.0
        assert xs.critical_depth(5.0) == 0.0

    def test_every_self_contained_shape_constructs(self):
        tabulated = {XSectShape.IRREGULAR, XSectShape.CUSTOM,
                     XSectShape.STREET_XSECT}
        for member in XSectShape:
            if member in tabulated:
                continue
            xs = XSectionGeometry(member, 4.0, 2.0, 1.0, 1.0, units="US")
            assert xs.shape == member
            if member is not XSectShape.DUMMY:
                assert xs.full_depth > 0.0
                assert xs.full_area > 0.0

    def test_inverse_round_trip(self):
        for shape in (XSectShape.CIRCULAR, XSectShape.RECT_CLOSED,
                      XSectShape.TRAPEZOIDAL, XSectShape.TRIANGULAR):
            xs = XSectionGeometry(shape, 3.0, 2.0, 1.0, 1.0, units="US")
            for frac in (0.1, 0.5, 0.95):
                y = frac * xs.full_depth
                assert xs.depth_from_area(xs.area(y)) == pytest.approx(
                    y, abs=1e-3 * xs.full_depth), f"{shape.name} @ {frac}"

    def test_critical_depth_increases_with_flow(self):
        xs = XSectionGeometry(XSectShape.CIRCULAR, 3.0, units="US")
        ycs = [xs.critical_depth(q) for q in (0.5, 2.0, 8.0, 20.0)]
        assert ycs == sorted(ycs)
        assert all(y > 0 for y in ycs)
        assert xs.critical_depth(0.0) == 0.0

    def test_rectangular_critical_depth_matches_closed_form(self):
        b, q, g = 4.0, 30.0, 32.2
        xs = XSectionGeometry(XSectShape.RECT_OPEN, 10.0, b, units="US")
        assert xs.critical_depth(q) == pytest.approx((q**2 / (g * b**2)) ** (1 / 3),
                                                     abs=1e-3)

    def test_section_factor_inverse(self):
        xs = XSectionGeometry(XSectShape.CIRCULAR, 2.0, units="US")
        a = xs.area(0.8)
        sf = xs.section_factor(a)
        assert sf > 0
        assert xs.area_from_section_factor(sf) == pytest.approx(a, rel=1e-3)

    def test_dsda_is_the_derivative_of_section_factor(self):
        # Catches a wrong unit factor on dsda: it is dS/dA, so it must track a
        # finite difference of section_factor with respect to area.
        xs = XSectionGeometry(XSectShape.CIRCULAR, 2.0, units="US")
        h = 1e-6
        for a in (0.5, 1.0, 2.0):
            fd = (xs.section_factor(a + h) - xs.section_factor(a - h)) / (2 * h)
            assert xs.dsda(a) == pytest.approx(fd, rel=1e-6)


class TestUnitConversionExponents:
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
        assert si.area(0.5) == pytest.approx(
            us.area(0.5 / self.M_PER_FT) * self.M_PER_FT ** 2)

    def test_length_quantities_convert_linearly(self):
        si, us = self._pair(XSectShape.CIRCULAR, 1.0)
        y_si, y_us = 0.5, 0.5 / self.M_PER_FT
        assert si.width(y_si) == pytest.approx(us.width(y_us) * self.M_PER_FT)
        assert si.hyd_radius(y_si) == pytest.approx(
            us.hyd_radius(y_us) * self.M_PER_FT)

    def test_section_factor_converts_as_length_to_the_eight_thirds(self):
        si, us = self._pair(XSectShape.RECT_OPEN, 2.0, 3.0)
        assert si.full_section_factor == pytest.approx(
            us.full_section_factor * self.M_PER_FT ** (8 / 3))

    def test_dsda_converts_as_length_to_the_two_thirds(self):
        si, us = self._pair(XSectShape.CIRCULAR, 1.0)
        a_si = si.area(0.5)
        a_us = us.area(0.5 / self.M_PER_FT)
        assert si.dsda(a_si) == pytest.approx(
            us.dsda(a_us) * self.M_PER_FT ** (2 / 3))

    def test_critical_depth_converts_with_its_flow_units(self):
        # SI takes CMS, US takes CFS — 1 cms = 35.3147 cfs.
        si, us = self._pair(XSectShape.CIRCULAR, 1.0)
        q_cms = 0.5
        assert si.critical_depth(q_cms) == pytest.approx(
            us.critical_depth(q_cms / 0.02832) * self.M_PER_FT, rel=1e-3)


# ===========================================================================
# Units
# ===========================================================================

class TestUnits:

    def test_us_and_si_agree(self):
        m_per_ft = 0.3048
        si = XSectionGeometry(XSectShape.CIRCULAR, 1.0, units="SI")
        us = XSectionGeometry(XSectShape.CIRCULAR, 1.0 / m_per_ft, units="US")
        assert si.area(0.5) == pytest.approx(us.area(0.5 / m_per_ft) * m_per_ft**2)
        assert si.units == "SI"
        assert us.units == "US"

    def test_flow_units_default_per_system(self):
        assert XSectionGeometry(XSectShape.CIRCULAR, 1.0, units="SI").flow_units == "CMS"
        assert XSectionGeometry(XSectShape.CIRCULAR, 1.0, units="US").flow_units == "CFS"

    def test_units_is_required_and_validated(self):
        with pytest.raises(TypeError):
            XSectionGeometry(XSectShape.CIRCULAR, 1.0)  # no units
        with pytest.raises(ValueError):
            XSectionGeometry(XSectShape.CIRCULAR, 1.0, units="metric")
        with pytest.raises(TypeError):
            XSectionGeometry(XSectShape.CIRCULAR, 1.0, units=1)

    def test_units_accepts_either_case(self):
        assert XSectionGeometry(XSectShape.CIRCULAR, 1.0, units="si").units == "SI"
        assert XSectionGeometry(XSectShape.CIRCULAR, 1.0, units="us").units == "US"


# ===========================================================================
# Tabulated shapes
# ===========================================================================

class TestTabulatedShapes:

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
        assert irr.shape == XSectShape.IRREGULAR
        assert irr.full_depth == pytest.approx(4.0)
        assert irr.max_width == pytest.approx(10.0)
        assert irr.is_open

        tz = XSectionGeometry(XSectShape.TRAPEZOIDAL, 4.0, 2.0, 1.0, 1.0,
                              units="US")
        for y in (1.0, 2.0, 3.0, 4.0):
            assert irr.area(y) == pytest.approx(tz.area(y), rel=0.05)
            assert irr.width(y) == pytest.approx(tz.width(y), rel=0.05)

    def test_roughness_changes_hydraulic_radius_but_not_area(self):
        # The conveyance-weighted hyd-radius table depends on the overbank n,
        # which is why from_transect takes them at all.
        uniform = self._transect(n_left=0.03, n_right=0.03)
        rough = self._transect(n_left=0.15, n_right=0.15)
        assert uniform.area(3.0) == pytest.approx(rough.area(3.0))
        assert uniform.hyd_radius(3.0) != rough.hyd_radius(3.0)

    def test_from_transect_validates_input(self):
        with pytest.raises(ValueError):
            self._transect(n_channel=0.0)          # n_channel must be > 0
        with pytest.raises(ValueError):
            XSectionGeometry.from_transect([0.0], [1.0], left_bank=0,
                                           right_bank=0, n_channel=0.03,
                                           units="US")   # < 2 points
        with pytest.raises(ValueError):
            XSectionGeometry.from_transect([0.0, 1.0], [1.0], left_bank=0,
                                           right_bank=1, n_channel=0.03,
                                           units="US")   # length mismatch

    def test_from_curve(self):
        # A constant-width ("rectangular") shape curve.
        xs = XSectionGeometry.from_curve(5.0, [0.0, 0.5, 1.0], [1.0, 1.0, 1.0],
                                         units="US")
        assert xs.shape == XSectShape.CUSTOM
        assert xs.full_depth == pytest.approx(5.0)
        assert xs.width(1.0) == pytest.approx(xs.width(4.0), rel=1e-4)

    def test_from_curve_validates_input(self):
        with pytest.raises(ValueError):
            XSectionGeometry.from_curve(0.0, [0.0, 1.0], [1.0, 1.0], units="US")
        with pytest.raises(ValueError):
            XSectionGeometry.from_curve(5.0, [0.0], [1.0], units="US")

    def test_from_street(self):
        # 2% over 20 ft rises 0.4 ft — below the 0.5 ft curb, so the curb sets
        # the full depth.
        xs = XSectionGeometry.from_street(20.0, 0.5, 2.0, 0.016, units="US")
        assert xs.shape == XSectShape.STREET_XSECT
        assert xs.full_depth == pytest.approx(0.5)
        assert xs.max_width == pytest.approx(40.0)   # full street = two halves
        assert xs.full_area > 0

    def test_from_street_half_is_half_as_wide(self):
        full = XSectionGeometry.from_street(20.0, 0.5, 2.0, 0.016, sides=2,
                                            units="US")
        half = XSectionGeometry.from_street(20.0, 0.5, 2.0, 0.016, sides=1,
                                            units="US")
        assert half.max_width == pytest.approx(full.max_width / 2.0)

    def test_from_street_validates_input(self):
        with pytest.raises(ValueError):
            XSectionGeometry.from_street(20.0, 0.5, 2.0, 0.016, sides=3,
                                         units="US")
        with pytest.raises(ValueError):
            XSectionGeometry.from_street(20.0, 0.5, 2.0, 0.0, units="US")

    def test_tabulated_shapes_reject_the_plain_constructor(self):
        for shape in (XSectShape.IRREGULAR, XSectShape.CUSTOM,
                      XSectShape.STREET_XSECT):
            with pytest.raises(ValueError):
                XSectionGeometry(shape, 1.0, units="US")


# ===========================================================================
# NumPy batch
# ===========================================================================

class TestBatch:

    @pytest.fixture
    def pipe(self):
        return XSectionGeometry(XSectShape.CIRCULAR, 2.0, units="US")

    def test_array_matches_scalar_loop(self, pipe):
        depths = np.array([0.0, 0.25, 0.5, 1.0, 1.5, 2.0])
        got = pipe.area(depths)
        assert isinstance(got, np.ndarray)
        assert got.dtype == np.float64
        np.testing.assert_allclose(got, [pipe.area(float(d)) for d in depths])

    def test_every_method_accepts_arrays(self, pipe):
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

    def test_shape_is_preserved(self, pipe):
        d = np.array([[0.1, 0.2], [0.3, 0.4]])
        assert pipe.area(d).shape == (2, 2)

    def test_empty_array(self, pipe):
        got = pipe.area(np.array([]))
        assert isinstance(got, np.ndarray)
        assert got.size == 0

    def test_list_input_works(self, pipe):
        np.testing.assert_allclose(pipe.area([0.5, 1.0]),
                                   [pipe.area(0.5), pipe.area(1.0)])

    def test_scalar_returns_float(self, pipe):
        assert isinstance(pipe.area(0.5), float)

    def test_bad_array_values_rejected(self, pipe):
        with pytest.raises(ValueError):
            pipe.area(np.array([1.0, -1.0]))
        with pytest.raises(ValueError):
            pipe.area(np.array([1.0, np.nan]))


# ===========================================================================
# Link-bound
# ===========================================================================

class TestLinkBound:

    def test_geometry_matches_standalone(self, opened_solver):
        link = opened_solver.links[0]
        shape, g1, g2, g3, g4 = link.xsect.as_tuple()
        xs = link.xsect.geometry()
        assert xs.shape == shape

        standalone = XSectionGeometry(shape, g1, g2, g3, g4,
                                      units=xs.units)
        for frac in (0.25, 0.5, 0.9):
            y = frac * xs.full_depth
            assert xs.area(y) == pytest.approx(standalone.area(y))

    def test_inherits_the_models_units(self, opened_solver):
        xs = opened_solver.links[0].xsect.geometry()
        assert xs.units in ("US", "SI")
        assert xs.units == ("SI" if opened_solver.unit_system == "SI" else "US")

    def test_handle_outlives_the_solver(self, solver_files):
        from openswmm.engine import Solver
        inp, rpt, out = solver_files
        s = Solver(inp, rpt, out)
        s.open()
        xs = s.links[0].xsect.geometry()
        before = xs.area(0.5 * xs.full_depth)
        s.close()
        s.destroy()
        # The handle deep-copied its geometry, so it still answers.
        assert xs.area(0.5 * xs.full_depth) == pytest.approx(before)

    def test_from_link_classmethod(self, opened_solver):
        link = opened_solver.links[0]
        assert XSectionGeometry.from_link(link).full_depth == pytest.approx(
            link.xsect.geometry().full_depth)

    def test_every_conduit_yields_a_usable_geometry(self, opened_solver):
        from openswmm.engine import LinkType
        n = 0
        for link in opened_solver.links:
            if link.type != LinkType.CONDUIT:
                continue
            xs = link.xsect.geometry()
            assert xs.full_depth > 0
            assert xs.area(0.5 * xs.full_depth) > 0
            n += 1
        assert n > 0, "the fixture model should contain conduits"


# ===========================================================================
# get_xsect_info / CrossSection
# ===========================================================================

class TestXsectInfo:

    def test_get_xsect_info_returns_a_cross_section(self, opened_solver):
        links = Links(opened_solver)
        xs = links.get_xsect_info(0)
        assert isinstance(xs, CrossSection)
        assert xs.shape_name == XSectShape(xs.shape).name

    def test_by_id_and_by_index_agree(self, opened_solver):
        links = Links(opened_solver)
        first_id = links.get_id(0)
        assert links.get_xsect_info(first_id) == links.get_xsect_info(0)

    def test_geom_labels_are_named(self, opened_solver):
        links = Links(opened_solver)
        for i in range(len(links)):
            xs = links.get_xsect_info(i)
            labels = xs.geom_labels
            assert isinstance(labels, dict)
            if xs.shape == XSectShape.CIRCULAR:
                assert list(labels) == ["diameter"]
                assert labels["diameter"] == pytest.approx(xs.geom1)

    def test_link_view_info_agrees_with_collection(self, opened_solver):
        links = Links(opened_solver)
        assert opened_solver.links[0].xsect.info() == links.get_xsect_info(0)

    def test_from_raw(self):
        xs = CrossSection.from_raw(XSectShape.CIRCULAR, 1.2, 0.0, 0.0, 0.0)
        assert xs.shape_name == "CIRCULAR"
        assert xs.geom_labels == {"diameter": 1.2}

    def test_geom_labels_for_the_renumbered_shapes(self):
        # These labels were attached to the wrong codes before 6.0.
        assert list(CrossSection.from_raw(XSectShape.HORIZ_ELLIPSE, 3, 4, 0, 0)
                    .geom_labels) == ["height", "width"]
        assert list(CrossSection.from_raw(XSectShape.ARCH, 3, 4, 0, 0)
                    .geom_labels) == ["height", "width"]
        assert list(CrossSection.from_raw(XSectShape.IRREGULAR, 2, 0, 0, 0)
                    .geom_labels) == ["transect_index"]

    def test_unknown_code_falls_back(self):
        xs = CrossSection.from_raw(99, 1.0, 0.0, 0.0, 0.0)
        assert xs.shape_name == "UNKNOWN(99)"
        assert list(xs.geom_labels) == ["geom1", "geom2", "geom3", "geom4"]


# ===========================================================================
# Errors
# ===========================================================================

class TestErrors:

    @pytest.fixture
    def pipe(self):
        return XSectionGeometry(XSectShape.CIRCULAR, 2.0, units="US")

    def test_negative_input_rejected(self, pipe):
        with pytest.raises(ValueError):
            pipe.area(-1.0)
        with pytest.raises(ValueError):
            pipe.depth_from_area(-1.0)
        with pytest.raises(ValueError):
            pipe.critical_depth(-1.0)

    def test_nan_rejected(self, pipe):
        with pytest.raises(ValueError):
            pipe.area(float("nan"))

    def test_unknown_shape_rejected(self):
        with pytest.raises(ValueError):
            XSectionGeometry(99, 1.0, units="US")

    def test_degenerate_geometry_rejected(self):
        with pytest.raises(ValueError):
            XSectionGeometry(XSectShape.CIRCULAR, 0.0, units="US")
        with pytest.raises(ValueError):
            # Fill above the crown.
            XSectionGeometry(XSectShape.FILLED_CIRCULAR, 2.0, 3.0, units="US")

    def test_depth_above_full_is_clamped_not_rejected(self, pipe):
        # Closed shapes clamp, matching the routing solvers — a surcharged
        # conduit must not raise.
        assert pipe.area(100.0) == pytest.approx(pipe.full_area)

    def test_geometry_of_a_stale_link_raises(self, opened_solver):
        from openswmm.engine import StaleObjectError
        link = opened_solver.links[0]
        opened_solver._bump_generation()
        with pytest.raises(StaleObjectError):
            link.xsect.geometry()
