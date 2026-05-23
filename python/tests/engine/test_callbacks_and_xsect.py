"""Tests for Phase 1 additions: warning callback and Links.set_xsect.

These cover the two Phase 1 items that were genuine py-gaps (no existing
implementation in _solver.pyx / _links.pyx), as opposed to the larger
stub-sync work covered implicitly by other test modules.
"""

import pytest

from openswmm.engine import EngineError, Links, Solver


# ---------------------------------------------------------------------------
# Solver.set_warning_callback
# ---------------------------------------------------------------------------
class TestWarningCallback:
    """The warning callback fires for non-fatal engine diagnostics."""

    def test_register_and_unregister_does_not_raise(self, opened_solver):
        # Just registering and clearing must not raise; the engine may or may
        # not emit a warning during this short interaction.
        events: list[tuple[int, str]] = []

        def cb(code: int, msg: str) -> None:
            events.append((code, msg))

        opened_solver.set_warning_callback(cb)
        opened_solver.set_warning_callback(None)
        # No assertion on events list — the engine doesn't owe us a warning
        # in this lifecycle window. The point is the API contract.

    def test_callback_lifetime_outlives_local_reference(self, opened_solver):
        # Drop the local ref to the callable; the Solver must keep it alive
        # so the C trampoline's user_data does not point at freed memory.
        events: list[tuple[int, str]] = []

        def make_cb():
            def cb(code: int, msg: str) -> None:
                events.append((code, msg))
            return cb

        opened_solver.set_warning_callback(make_cb())
        # `cb` is now only referenced via the Solver instance.
        # Run the rest of the lifecycle; if the Solver dropped the ref the
        # next C call would segfault.
        opened_solver.initialize()
        opened_solver.set_warning_callback(None)

    def test_unregister_with_none_clears_storage(self, opened_solver):
        def cb(code: int, msg: str) -> None:
            pass

        opened_solver.set_warning_callback(cb)
        opened_solver.set_warning_callback(None)
        # Indirect check: we can register a different callable afterwards
        # without raising, which would fail if state was corrupted.
        opened_solver.set_warning_callback(cb)
        opened_solver.set_warning_callback(None)


# ---------------------------------------------------------------------------
# Links.set_xsect
# ---------------------------------------------------------------------------
class TestLinksSetXsect:
    """Round-trip cross-section geometry through set_xsect / get_xsect."""

    def test_roundtrip_circular(self, opened_solver):
        links = Links(opened_solver)
        # Conduit 0 in site_drainage_example.inp; reshape to circular d=1.5 m.
        # Shape code 1 = CIRCULAR per XSectShape enum.
        links.set_xsect(0, shape=1, geom1=1.5)
        shape, g1, g2, g3, g4 = links.get_xsect(0)
        assert shape == 1
        assert g1 == pytest.approx(1.5)

    def test_roundtrip_rectangular(self, opened_solver):
        links = Links(opened_solver)
        # Rectangular: shape code 3, geom1=depth, geom2=width.
        links.set_xsect(0, shape=3, geom1=2.0, geom2=1.0)
        shape, g1, g2, g3, g4 = links.get_xsect(0)
        assert shape == 3
        assert g1 == pytest.approx(2.0)
        assert g2 == pytest.approx(1.0)

    def test_accepts_link_id_string(self, opened_solver):
        links = Links(opened_solver)
        # Resolve link by ID instead of index.
        link_id = links.get_id(0)
        links.set_xsect(link_id, shape=1, geom1=0.5)
        shape, g1, *_ = links.get_xsect(link_id)
        assert shape == 1
        assert g1 == pytest.approx(0.5)

    def test_invalid_link_id_raises_key_error(self, opened_solver):
        links = Links(opened_solver)
        with pytest.raises(KeyError):
            links.set_xsect("DOES_NOT_EXIST", shape=1, geom1=1.0)
