"""Tests for :mod:`openswmm.engine._enums` integer enum definitions."""

import pytest

from openswmm.engine import (
    ErrorCode, EngineState, NodeType, LinkType,
    XSectShape, FlowUnits, RouteModel, WarnCode, ObjectType,
)


# ---------------------------------------------------------------------------
# ErrorCode
# ---------------------------------------------------------------------------
class TestErrorCode:
    """Verify ErrorCode values and membership."""

    def test_ok_is_zero(self):
        assert ErrorCode.OK == 0

    def test_all_values_unique(self):
        vals = [e.value for e in ErrorCode]
        assert len(vals) == len(set(vals))

    def test_known_values(self):
        expected = {
            "OK": 0, "NOMEM": 1, "INPFILE": 2, "RPTFILE": 3,
            "OUTFILE": 4, "PARSE": 5, "LIFECYCLE": 6, "BADHANDLE": 7,
            "BADINDEX": 8, "BADPARAM": 9, "PLUGIN": 10, "IO": 11,
            "HOTSTART": 12, "CRS": 13, "NUMERICAL": 14, "INTERNAL": 99,
        }
        for name, val in expected.items():
            assert ErrorCode[name].value == val

    def test_member_count(self):
        assert len(ErrorCode) == 16


# ---------------------------------------------------------------------------
# EngineState
# ---------------------------------------------------------------------------
class TestEngineState:
    """Verify EngineState values."""

    def test_known_values(self):
        expected = {
            "NONE": 0, "CREATED": 1, "OPENED": 2, "INITIALIZED": 3,
            "STARTED": 4, "RUNNING": 5, "ENDED": 6, "CLOSED": 7,
            "BUILDING": 8,
        }
        for name, val in expected.items():
            assert EngineState[name].value == val

    def test_member_count(self):
        assert len(EngineState) == 9


# ---------------------------------------------------------------------------
# NodeType
# ---------------------------------------------------------------------------
class TestNodeType:
    """Verify NodeType values."""

    def test_known_values(self):
        expected = {"JUNCTION": 0, "OUTFALL": 1, "STORAGE": 2, "DIVIDER": 3}
        for name, val in expected.items():
            assert NodeType[name].value == val

    def test_member_count(self):
        assert len(NodeType) == 4

    def test_is_int(self):
        assert isinstance(NodeType.JUNCTION, int)


# ---------------------------------------------------------------------------
# LinkType
# ---------------------------------------------------------------------------
class TestLinkType:
    """Verify LinkType values."""

    def test_known_values(self):
        expected = {"CONDUIT": 0, "PUMP": 1, "ORIFICE": 2, "WEIR": 3, "OUTLET": 4}
        for name, val in expected.items():
            assert LinkType[name].value == val

    def test_member_count(self):
        assert len(LinkType) == 5


# ---------------------------------------------------------------------------
# XSectShape
# ---------------------------------------------------------------------------
class TestXSectShape:
    """Verify XSectShape values."""

    def test_circular_is_zero(self):
        assert XSectShape.CIRCULAR == 0

    def test_force_main(self):
        assert XSectShape.FORCE_MAIN == 18

    def test_member_count(self):
        assert len(XSectShape) == 19

    def test_known_values(self):
        expected = {
            "CIRCULAR": 0, "FILLED_CIRCULAR": 1, "RECT_CLOSED": 2,
            "RECT_OPEN": 3, "TRAPEZOIDAL": 4, "TRIANGULAR": 5,
            "PARABOLIC": 6, "POWER": 7, "MODBASKETHANDLE": 8,
            "EGGSHAPED": 9, "HORSESHOE": 10, "GOTHIC": 11,
            "CATENARY": 12, "SEMIELLIPTICAL": 13, "BASKETHANDLE": 14,
            "SEMICIRCULAR": 15, "IRREGULAR": 16, "CUSTOM": 17,
            "FORCE_MAIN": 18,
        }
        for name, val in expected.items():
            assert XSectShape[name].value == val


# ---------------------------------------------------------------------------
# FlowUnits
# ---------------------------------------------------------------------------
class TestFlowUnits:
    """Verify FlowUnits values."""

    def test_known_values(self):
        expected = {"CFS": 0, "GPM": 1, "MGD": 2, "CMS": 3, "LPS": 4, "MLD": 5}
        for name, val in expected.items():
            assert FlowUnits[name].value == val

    def test_member_count(self):
        assert len(FlowUnits) == 6


# ---------------------------------------------------------------------------
# RouteModel
# ---------------------------------------------------------------------------
class TestRouteModel:
    """Verify RouteModel values."""

    def test_known_values(self):
        expected = {"STEADY": 0, "KINWAVE": 1, "DYNWAVE": 2}
        for name, val in expected.items():
            assert RouteModel[name].value == val

    def test_member_count(self):
        assert len(RouteModel) == 3


# ---------------------------------------------------------------------------
# WarnCode
# ---------------------------------------------------------------------------
class TestWarnCode:
    """Verify WarnCode values."""

    def test_known_values(self):
        expected = {
            "NONE": 0, "HOTSTART_MISSING": 1, "UNKNOWN_SECTION": 2,
            "UNKNOWN_OPTION": 3, "DEPRECATED_KW": 4, "PLUGIN_INIT": 5,
            "NUMERICAL": 6, "STABILITY_LIMIT": 7,
        }
        for name, val in expected.items():
            assert WarnCode[name].value == val

    def test_member_count(self):
        assert len(WarnCode) == 8


# ---------------------------------------------------------------------------
# ObjectType
# ---------------------------------------------------------------------------
class TestObjectType:
    """Verify ObjectType values."""

    def test_known_values(self):
        expected = {
            "GAGE": 0, "SUBCATCH": 1, "NODE": 2, "LINK": 3,
            "POLLUT": 4, "LANDUSE": 5, "TIMESER": 6, "TABLE": 7,
            "RDII": 8, "UNITHYD": 9, "SNOWMELT": 10, "SHAPE": 11,
            "LID": 12,
        }
        for name, val in expected.items():
            assert ObjectType[name].value == val

    def test_member_count(self):
        assert len(ObjectType) == 13


# ---------------------------------------------------------------------------
# Cross-cutting
# ---------------------------------------------------------------------------
class TestEnumsCrossCutting:
    """General properties all enums should satisfy."""

    @pytest.mark.parametrize("enum_cls", [
        ErrorCode, EngineState, NodeType, LinkType,
        XSectShape, FlowUnits, RouteModel, WarnCode, ObjectType,
    ])
    def test_all_members_are_int(self, enum_cls):
        for member in enum_cls:
            assert isinstance(member.value, int)

    @pytest.mark.parametrize("enum_cls", [
        ErrorCode, EngineState, NodeType, LinkType,
        XSectShape, FlowUnits, RouteModel, WarnCode, ObjectType,
    ])
    def test_values_are_unique(self, enum_cls):
        vals = [e.value for e in enum_cls]
        assert len(vals) == len(set(vals)), f"Duplicate values in {enum_cls.__name__}"
