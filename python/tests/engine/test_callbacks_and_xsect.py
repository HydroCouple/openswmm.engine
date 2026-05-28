"""Tests for warning-callback contract and Link.xsect round-trip.

Migrated to the v1 Pythonic bindings.
"""

import pytest

from openswmm.engine import XSectShape


# ---------------------------------------------------------------------------
# Solver.set_warning_callback
# ---------------------------------------------------------------------------
class TestWarningCallback:
    """The warning callback fires for non-fatal engine diagnostics."""

    def test_register_and_unregister_does_not_raise(self, opened_solver):
        events: list[tuple[int, str]] = []

        def cb(code: int, msg: str) -> None:
            events.append((code, msg))

        opened_solver.set_warning_callback(cb)
        opened_solver.set_warning_callback(None)

    def test_callback_lifetime_outlives_local_reference(self, opened_solver):
        events: list[tuple[int, str]] = []

        def make_cb():
            def cb(code: int, msg: str) -> None:
                events.append((code, msg))
            return cb

        opened_solver.set_warning_callback(make_cb())
        opened_solver.initialize()
        opened_solver.set_warning_callback(None)

    def test_unregister_with_none_clears_storage(self, opened_solver):
        def cb(code: int, msg: str) -> None:
            pass

        opened_solver.set_warning_callback(cb)
        opened_solver.set_warning_callback(None)
        opened_solver.set_warning_callback(cb)
        opened_solver.set_warning_callback(None)


# ---------------------------------------------------------------------------
# Link.xsect round-trip
# ---------------------------------------------------------------------------
class TestLinkXSect:
    """Round-trip cross-section geometry through Link.xsect."""

    def test_roundtrip_circular(self, opened_solver):
        link = opened_solver.links[0]
        link.xsect = (XSectShape.CIRCULAR, 1.5, 0.0, 0.0, 0.0)
        shape, g1, g2, g3, g4 = link.xsect.as_tuple()
        assert shape == XSectShape.CIRCULAR
        assert g1 == pytest.approx(1.5)

    def test_roundtrip_rectangular(self, opened_solver):
        link = opened_solver.links[0]
        link.xsect = (XSectShape.RECT_OPEN, 2.0, 1.0, 0.0, 0.0)
        shape, g1, g2, *_ = link.xsect.as_tuple()
        assert shape == XSectShape.RECT_OPEN
        assert g1 == pytest.approx(2.0)
        assert g2 == pytest.approx(1.0)

    def test_accepts_link_id_string(self, opened_solver):
        link_id = opened_solver.links.get_id(0)
        link = opened_solver.links[link_id]
        link.xsect = (XSectShape.CIRCULAR, 0.5, 0.0, 0.0, 0.0)
        shape, g1, *_ = link.xsect.as_tuple()
        assert shape == XSectShape.CIRCULAR
        assert g1 == pytest.approx(0.5)

    def test_invalid_link_id_raises_key_error(self, opened_solver):
        with pytest.raises(KeyError):
            _ = opened_solver.links["DOES_NOT_EXIST"]
