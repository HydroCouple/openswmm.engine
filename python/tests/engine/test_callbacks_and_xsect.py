"""Tests for warning-callback contract and Link.xsect round-trip.

Migrated to the v1 Pythonic bindings.
"""

import unittest  # noqa: F401  (kept for parity with the unittest suite)

from openswmm.engine import XSectShape

from tests.engine._solver_cases import EngineSolverCase


# ---------------------------------------------------------------------------
# Solver.set_warning_callback
# ---------------------------------------------------------------------------
class TestWarningCallback(EngineSolverCase):
    """The warning callback fires for non-fatal engine diagnostics."""

    def test_register_and_unregister_does_not_raise(self):
        solver = self.opened_solver()
        events: list[tuple[int, str]] = []

        def cb(code: int, msg: str) -> None:
            events.append((code, msg))

        solver.set_warning_callback(cb)
        solver.set_warning_callback(None)

    def test_callback_lifetime_outlives_local_reference(self):
        solver = self.opened_solver()
        events: list[tuple[int, str]] = []

        def make_cb():
            def cb(code: int, msg: str) -> None:
                events.append((code, msg))
            return cb

        solver.set_warning_callback(make_cb())
        solver.initialize()
        solver.set_warning_callback(None)

    def test_unregister_with_none_clears_storage(self):
        solver = self.opened_solver()

        def cb(code: int, msg: str) -> None:
            pass

        solver.set_warning_callback(cb)
        solver.set_warning_callback(None)
        solver.set_warning_callback(cb)
        solver.set_warning_callback(None)


# ---------------------------------------------------------------------------
# Link.xsect round-trip
# ---------------------------------------------------------------------------
class TestLinkXSect(EngineSolverCase):
    """Round-trip cross-section geometry through Link.xsect."""

    def test_roundtrip_circular(self):
        solver = self.opened_solver()
        link = solver.links[0]
        link.xsect = (XSectShape.CIRCULAR, 1.5, 0.0, 0.0, 0.0)
        shape, g1, g2, g3, g4 = link.xsect.as_tuple()
        self.assertEqual(shape, XSectShape.CIRCULAR)
        self.assertAlmostEqual(g1, 1.5, places=6)

    def test_roundtrip_rectangular(self):
        solver = self.opened_solver()
        link = solver.links[0]
        link.xsect = (XSectShape.RECT_OPEN, 2.0, 1.0, 0.0, 0.0)
        shape, g1, g2, *_ = link.xsect.as_tuple()
        self.assertEqual(shape, XSectShape.RECT_OPEN)
        self.assertAlmostEqual(g1, 2.0, places=6)
        self.assertAlmostEqual(g2, 1.0, places=6)

    def test_accepts_link_id_string(self):
        solver = self.opened_solver()
        link_id = solver.links.get_id(0)
        link = solver.links[link_id]
        link.xsect = (XSectShape.CIRCULAR, 0.5, 0.0, 0.0, 0.0)
        shape, g1, *_ = link.xsect.as_tuple()
        self.assertEqual(shape, XSectShape.CIRCULAR)
        self.assertAlmostEqual(g1, 0.5, places=6)

    def test_invalid_link_id_raises_key_error(self):
        solver = self.opened_solver()
        with self.assertRaises(KeyError):
            _ = solver.links["DOES_NOT_EXIST"]
