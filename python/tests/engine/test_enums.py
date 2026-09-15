"""Tests for :mod:`openswmm.engine._enums` integer enum definitions."""

import unittest

from openswmm.engine import (
    ErrorCode, EngineState, NodeType, LinkType,
    XSectShape, FlowUnits, RouteModel, WarnCode, ObjectType,
    HeatFluxModule, HeatShortwaveMode, HeatRadiativeParam, HeatSolarParam,
    HeatCloudParam, HeatSourceKind, WaterAgeSource,
    ReactionScope, ReactionExprForm,
)


# ---------------------------------------------------------------------------
# ErrorCode
# ---------------------------------------------------------------------------
class TestErrorCode(unittest.TestCase):
    """Verify ErrorCode values and membership."""

    def test_ok_is_zero(self):
        self.assertEqual(ErrorCode.OK, 0)

    def test_all_values_unique(self):
        vals = [e.value for e in ErrorCode]
        self.assertEqual(len(vals), len(set(vals)))

    def test_known_values(self):
        expected = {
            "OK": 0, "NOMEM": 1, "INPFILE": 2, "RPTFILE": 3,
            "OUTFILE": 4, "PARSE": 5, "LIFECYCLE": 6, "BADHANDLE": 7,
            "BADINDEX": 8, "BADPARAM": 9, "PLUGIN": 10, "IO": 11,
            "HOTSTART": 12, "CRS": 13, "NUMERICAL": 14, "DEPENDENCY": 15,
            "INTERNAL": 99,
        }
        for name, val in expected.items():
            self.assertEqual(ErrorCode[name].value, val)

    def test_member_count(self):
        self.assertEqual(len(ErrorCode), 17)


# ---------------------------------------------------------------------------
# EngineState
# ---------------------------------------------------------------------------
class TestEngineState(unittest.TestCase):
    """Verify EngineState values."""

    def test_known_values(self):
        expected = {
            "NONE": 0, "CREATED": 1, "OPENED": 2, "INITIALIZED": 3,
            "STARTED": 4, "RUNNING": 5, "ENDED": 6, "CLOSED": 7,
            "BUILDING": 8,
        }
        for name, val in expected.items():
            self.assertEqual(EngineState[name].value, val)

    def test_member_count(self):
        self.assertEqual(len(EngineState), 9)


# ---------------------------------------------------------------------------
# NodeType
# ---------------------------------------------------------------------------
class TestNodeType(unittest.TestCase):
    """Verify NodeType values."""

    def test_known_values(self):
        expected = {"JUNCTION": 0, "OUTFALL": 1, "STORAGE": 2, "DIVIDER": 3}
        for name, val in expected.items():
            self.assertEqual(NodeType[name].value, val)

    def test_member_count(self):
        self.assertEqual(len(NodeType), 4)

    def test_is_int(self):
        self.assertIsInstance(NodeType.JUNCTION, int)


# ---------------------------------------------------------------------------
# LinkType
# ---------------------------------------------------------------------------
class TestLinkType(unittest.TestCase):
    """Verify LinkType values."""

    def test_known_values(self):
        expected = {"CONDUIT": 0, "PUMP": 1, "ORIFICE": 2, "WEIR": 3, "OUTLET": 4}
        for name, val in expected.items():
            self.assertEqual(LinkType[name].value, val)

    def test_member_count(self):
        self.assertEqual(len(LinkType), 5)


# ---------------------------------------------------------------------------
# XSectShape
# ---------------------------------------------------------------------------
class TestXSectShape(unittest.TestCase):
    """Verify XSectShape values.

    These are the engine's storage codes (``openswmm::XsectShape`` /
    ``SWMM_XSectShape``), not the legacy SWMM 5 ``XsectType`` ordering.

    Renumbered in 6.0: IRREGULAR/CUSTOM/FORCE_MAIN previously carried 16/17/18,
    which the engine read as RECT_TRIANG/RECT_ROUND/HORIZ_ELLIPSE — so
    assigning them silently produced the wrong cross-section. The seven shapes
    the enum had been missing were added at the same time.
    ``test_xsect_geometry.TestShapeEnumParity`` pins these against the engine
    itself; this module just pins the literals.
    """

    def test_circular_is_zero(self):
        self.assertEqual(XSectShape.CIRCULAR, 0)

    def test_force_main(self):
        self.assertEqual(XSectShape.FORCE_MAIN, 23)

    def test_member_count(self):
        self.assertEqual(len(XSectShape), 26)

    def test_known_values(self):
        expected = {
            "CIRCULAR": 0, "FILLED_CIRCULAR": 1, "RECT_CLOSED": 2,
            "RECT_OPEN": 3, "TRAPEZOIDAL": 4, "TRIANGULAR": 5,
            "PARABOLIC": 6, "POWER": 7, "MODBASKETHANDLE": 8,
            "EGGSHAPED": 9, "HORSESHOE": 10, "GOTHIC": 11,
            "CATENARY": 12, "SEMIELLIPTICAL": 13, "BASKETHANDLE": 14,
            "SEMICIRCULAR": 15, "RECT_TRIANG": 16, "RECT_ROUND": 17,
            "HORIZ_ELLIPSE": 18, "VERT_ELLIPSE": 19, "ARCH": 20,
            "IRREGULAR": 21, "CUSTOM": 22, "FORCE_MAIN": 23,
            "STREET_XSECT": 24, "DUMMY": 25,
        }
        for name, val in expected.items():
            self.assertEqual(XSectShape[name].value, val)


# ---------------------------------------------------------------------------
# FlowUnits
# ---------------------------------------------------------------------------
class TestFlowUnits(unittest.TestCase):
    """Verify FlowUnits values."""

    def test_known_values(self):
        expected = {"CFS": 0, "GPM": 1, "MGD": 2, "CMS": 3, "LPS": 4, "MLD": 5}
        for name, val in expected.items():
            self.assertEqual(FlowUnits[name].value, val)

    def test_member_count(self):
        self.assertEqual(len(FlowUnits), 6)


# ---------------------------------------------------------------------------
# RouteModel
# ---------------------------------------------------------------------------
class TestRouteModel(unittest.TestCase):
    """Verify RouteModel values."""

    def test_known_values(self):
        expected = {"STEADY": 0, "KINWAVE": 1, "DYNWAVE": 2, "FV": 3}
        for name, val in expected.items():
            self.assertEqual(RouteModel[name].value, val)

    def test_member_count(self):
        self.assertEqual(len(RouteModel), 4)


# ---------------------------------------------------------------------------
# WarnCode
# ---------------------------------------------------------------------------
class TestWarnCode(unittest.TestCase):
    """Verify WarnCode values."""

    def test_known_values(self):
        expected = {
            "NONE": 0, "HOTSTART_MISSING": 1, "UNKNOWN_SECTION": 2,
            "UNKNOWN_OPTION": 3, "DEPRECATED_KW": 4, "PLUGIN_INIT": 5,
            "NUMERICAL": 6, "STABILITY_LIMIT": 7,
        }
        for name, val in expected.items():
            self.assertEqual(WarnCode[name].value, val)

    def test_member_count(self):
        self.assertEqual(len(WarnCode), 8)


# ---------------------------------------------------------------------------
# ObjectType
# ---------------------------------------------------------------------------
class TestObjectType(unittest.TestCase):
    """Verify ObjectType values."""

    def test_known_values(self):
        expected = {
            "GAGE": 0, "SUBCATCH": 1, "NODE": 2, "LINK": 3,
            "POLLUT": 4, "LANDUSE": 5, "TIMESER": 6, "TABLE": 7,
            "RDII": 8, "UNITHYD": 9, "SNOWMELT": 10, "SHAPE": 11,
            "LID": 12,
        }
        for name, val in expected.items():
            self.assertEqual(ObjectType[name].value, val)

    def test_member_count(self):
        self.assertEqual(len(ObjectType), 13)


# ---------------------------------------------------------------------------
# HeatFluxModule
# ---------------------------------------------------------------------------
class TestHeatFluxModule(unittest.TestCase):
    """Verify HeatFluxModule values (mirrors ``SWMM_HeatFluxModule``)."""

    def test_known_values(self):
        expected = {
            "SURFACE_EXCHANGE": 0, "RADIATIVE_EXCHANGE": 1,
            "LAYER_CONDUCTION": 2,
        }
        for name, val in expected.items():
            self.assertEqual(HeatFluxModule[name].value, val)

    def test_member_count(self):
        self.assertEqual(len(HeatFluxModule), 3)


# ---------------------------------------------------------------------------
# HeatShortwaveMode
# ---------------------------------------------------------------------------
class TestHeatShortwaveMode(unittest.TestCase):
    """Verify HeatShortwaveMode values (mirrors ``SWMM_HeatShortwaveMode``)."""

    def test_known_values(self):
        expected = {"CONSTANT": 0, "TIMESERIES": 1, "COMPUTED": 2}
        for name, val in expected.items():
            self.assertEqual(HeatShortwaveMode[name].value, val)

    def test_member_count(self):
        self.assertEqual(len(HeatShortwaveMode), 3)


# ---------------------------------------------------------------------------
# HeatRadiativeParam
# ---------------------------------------------------------------------------
class TestHeatRadiativeParam(unittest.TestCase):
    """Verify HeatRadiativeParam values (mirrors ``SWMM_HeatRadiativeParam``)."""

    def test_known_values(self):
        expected = {
            "SHORTWAVE": 0, "ALBEDO": 1, "SHADE_FACTOR": 2, "SKY_VIEW": 3,
            "EMISS_WATER": 4, "EMISS_LANDCOVER": 5, "ATM_EMISS_COEFF": 6,
            "LW_REFLECTION": 7,
        }
        for name, val in expected.items():
            self.assertEqual(HeatRadiativeParam[name].value, val)

    def test_member_count(self):
        self.assertEqual(len(HeatRadiativeParam), 8)


# ---------------------------------------------------------------------------
# HeatSolarParam
# ---------------------------------------------------------------------------
class TestHeatSolarParam(unittest.TestCase):
    """Verify HeatSolarParam values (mirrors ``SWMM_HeatSolarParam``)."""

    def test_known_values(self):
        expected = {
            "LATITUDE": 0, "LONGITUDE": 1, "TIMEZONE": 2, "ELEVATION": 3,
            "TURBIDITY_380": 4, "TURBIDITY_500": 5, "PRECIP_WATER": 6,
            "OZONE": 7, "GROUND_ALBEDO": 8,
        }
        for name, val in expected.items():
            self.assertEqual(HeatSolarParam[name].value, val)

    def test_member_count(self):
        self.assertEqual(len(HeatSolarParam), 9)


# ---------------------------------------------------------------------------
# HeatCloudParam
# ---------------------------------------------------------------------------
class TestHeatCloudParam(unittest.TestCase):
    """Verify HeatCloudParam values (mirrors ``SWMM_HeatCloudParam``)."""

    def test_known_values(self):
        expected = {
            "FRACTION": 0, "SW_ATTEN_K": 1, "SW_ATTEN_N": 2,
            "LW_CLOUD_K": 3,
        }
        for name, val in expected.items():
            self.assertEqual(HeatCloudParam[name].value, val)

    def test_member_count(self):
        self.assertEqual(len(HeatCloudParam), 4)


# ---------------------------------------------------------------------------
# HeatSourceKind
# ---------------------------------------------------------------------------
class TestHeatSourceKind(unittest.TestCase):
    """Verify HeatSourceKind values (mirrors ``SWMM_HeatSourceKind``)."""

    def test_known_values(self):
        expected = {
            "RAINFALL": 0, "DWF": 1, "GW": 2, "RDII": 3,
            "EXTERNAL_INFLOW": 4, "IFACE": 5, "INITIAL_STATE": 6,
        }
        for name, val in expected.items():
            self.assertEqual(HeatSourceKind[name].value, val)

    def test_member_count(self):
        self.assertEqual(len(HeatSourceKind), 7)


# ---------------------------------------------------------------------------
# WaterAgeSource
# ---------------------------------------------------------------------------
class TestWaterAgeSource(unittest.TestCase):
    """Verify WaterAgeSource values (mirrors ``SWMM_WaterAgeSource``)."""

    def test_known_values(self):
        expected = {
            "RAINFALL": 0, "DWF": 1, "GW": 2, "RDII": 3,
            "EXTERNAL_INFLOW": 4, "IFACE": 5, "INITIAL_STATE": 6,
        }
        for name, val in expected.items():
            self.assertEqual(WaterAgeSource[name].value, val)

    def test_member_count(self):
        # SEVEN, not eight: the C enum's trailing ``SWMM_AGE_SRC_COUNT = 7``
        # sentinel is deliberately NOT reproduced as a Python member —
        # ``len(WaterAgeSource)`` is the count, so a sentinel member would
        # both double-count and show up in iteration as a fake pathway.
        self.assertEqual(len(WaterAgeSource), 7)

    def test_count_sentinel_is_not_a_member(self):
        self.assertNotIn("COUNT", WaterAgeSource.__members__)


# ---------------------------------------------------------------------------
# ReactionScope
# ---------------------------------------------------------------------------
class TestReactionScope(unittest.TestCase):
    """Verify ReactionScope values (mirrors the ``SWMM_RXN_SCOPE_*`` macros)."""

    def test_known_values(self):
        expected = {"TERM": 0, "PIPE": 1, "TANK": 2}
        for name, val in expected.items():
            self.assertEqual(ReactionScope[name].value, val)

    def test_member_count(self):
        self.assertEqual(len(ReactionScope), 3)


# ---------------------------------------------------------------------------
# ReactionExprForm
# ---------------------------------------------------------------------------
class TestReactionExprForm(unittest.TestCase):
    """Verify ReactionExprForm values (mirrors the ``SWMM_RXN_FORM_*`` macros)."""

    def test_known_values(self):
        expected = {"NONE": 0, "RATE": 1, "EQUIL": 2, "FORMULA": 3}
        for name, val in expected.items():
            self.assertEqual(ReactionExprForm[name].value, val)

    def test_member_count(self):
        self.assertEqual(len(ReactionExprForm), 4)


# ---------------------------------------------------------------------------
# Cross-cutting
# ---------------------------------------------------------------------------
_ALL_ENUMS = (
    ErrorCode, EngineState, NodeType, LinkType,
    XSectShape, FlowUnits, RouteModel, WarnCode, ObjectType,
    HeatFluxModule, HeatShortwaveMode, HeatRadiativeParam, HeatSolarParam,
    HeatCloudParam, HeatSourceKind, WaterAgeSource,
    ReactionScope, ReactionExprForm,
)


class TestEnumsCrossCutting(unittest.TestCase):
    """General properties all enums should satisfy."""

    def test_all_members_are_int(self):
        for enum_cls in _ALL_ENUMS:
            with self.subTest(enum_cls=enum_cls):
                for member in enum_cls:
                    self.assertIsInstance(member.value, int)

    def test_values_are_unique(self):
        for enum_cls in _ALL_ENUMS:
            with self.subTest(enum_cls=enum_cls):
                vals = [e.value for e in enum_cls]
                self.assertEqual(len(vals), len(set(vals)),
                                 f"Duplicate values in {enum_cls.__name__}")
