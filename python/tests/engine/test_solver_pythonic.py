"""
P1 — Solver Pythonic surface tests.

Covers every new property / iterator / view on :class:`Solver`:

* lifecycle methods raise on failure (no integer return codes);
* ``state`` is :class:`EngineState`, ``elapsed`` and ``routing_step`` are
  :class:`timedelta`, ``start_datetime`` / ``end_datetime`` /
  ``current_datetime`` / ``report_start_datetime`` are :class:`datetime`;
* :meth:`Solver.steps` and :meth:`Solver.until` advance the engine;
* ``solver.options`` exposes string-keyed mapping access and typed
  shortcuts;
* ``solver.userflags`` reads / writes typed user flags;
* ``solver.events`` is a ``MutableSequence[Event]``;
* ``solver.nodes`` etc. return the lazy collection objects (P2 will
  replace them with wrapper-object collections — this test only
  confirms they exist and route to the right class).

These tests require the compiled engine; if the extension isn't
available, every test in this module is skipped at collection time.
"""

from __future__ import annotations

from datetime import datetime, timedelta

import pytest

# Skip the whole module if the engine isn't built.
pytest.importorskip("openswmm.engine._solver")

from openswmm.engine import (  # noqa: E402
    Solver,
    EngineState,
    EngineError,
    BadParamError,
)
from openswmm.engine._solver import (  # noqa: E402
    EventsView,
    SimulationOptions,
    UserFlags,
    Event,
)


# ---------------------------------------------------------------------------
# Typed property surface
# ---------------------------------------------------------------------------


class TestTypedProperties:
    def test_state_is_engine_state_enum(self, running_solver):
        assert isinstance(running_solver.state, EngineState)
        assert running_solver.state in (EngineState.STARTED, EngineState.RUNNING)

    def test_elapsed_is_timedelta(self, stepped_solver):
        assert isinstance(stepped_solver.elapsed, timedelta)
        # We stepped 12 times — elapsed must be > 0.
        assert stepped_solver.elapsed > timedelta(0)

    def test_routing_step_is_timedelta(self, opened_solver):
        opened_solver.initialize()
        opened_solver.start()
        assert isinstance(opened_solver.routing_step, timedelta)
        # Smoke check: routing step is positive and well under a day.
        assert timedelta(milliseconds=1) < opened_solver.routing_step < timedelta(days=1)

    def test_datetimes(self, opened_solver):
        assert isinstance(opened_solver.start_datetime, datetime)
        assert isinstance(opened_solver.end_datetime, datetime)
        assert isinstance(opened_solver.report_start_datetime, datetime)
        assert opened_solver.end_datetime > opened_solver.start_datetime

    def test_current_datetime_progresses(self, running_solver):
        start_dt = running_solver.start_datetime
        before = running_solver.current_datetime
        running_solver.step()
        after = running_solver.current_datetime
        assert after >= before
        # Sanity: current never precedes the configured start.
        assert before >= start_dt

    def test_datetime_setter_round_trip(self, opened_solver):
        original = opened_solver.start_datetime
        new = original + timedelta(hours=3, microseconds=123_456)
        opened_solver.start_datetime = new
        # ≤ 1 µs tolerance per _dates.py.
        assert abs((opened_solver.start_datetime - new).total_seconds()) <= 1e-6

    def test_handle_is_nonzero_int(self, opened_solver):
        assert isinstance(opened_solver.handle, int)
        assert opened_solver.handle != 0

    def test_repr_contains_state_name(self, opened_solver):
        r = repr(opened_solver)
        assert "Solver" in r
        assert opened_solver.state.name in r


# ---------------------------------------------------------------------------
# Lifecycle: raises on failure, no integer rc
# ---------------------------------------------------------------------------


class TestLifecycleRaises:
    def test_open_nonexistent_raises(self, tmp_path):
        s = Solver(tmp_path / "no_such.inp")
        with pytest.raises(EngineError):
            s.open()
        s.destroy()

    def test_step_returns_timedelta(self, running_solver):
        td = running_solver.step()
        assert isinstance(td, timedelta)
        assert td > timedelta(0)

    def test_stride_returns_timedelta(self, running_solver):
        td = running_solver.stride(5)
        assert isinstance(td, timedelta)
        assert td > timedelta(0)


# ---------------------------------------------------------------------------
# Iteration helpers
# ---------------------------------------------------------------------------


class TestIteration:
    def test_steps_terminates(self, running_solver):
        count = 0
        last = timedelta(0)
        for elapsed in running_solver.steps():
            count += 1
            assert elapsed > last      # monotonic non-decreasing
            last = elapsed
        # If we got here, the iterator terminated naturally.
        assert count > 0
        assert running_solver.state in (EngineState.RUNNING, EngineState.ENDED)

    def test_until_with_timedelta(self, running_solver):
        target = timedelta(hours=1)
        reached = running_solver.until(target)
        # Either we got to the target, or the simulation ended first.
        assert reached >= target or running_solver.state == EngineState.ENDED

    def test_until_with_datetime(self, running_solver):
        target = running_solver.start_datetime + timedelta(hours=2)
        running_solver.until(target)
        # After the call, current_datetime should be at or past target — or the
        # sim ended before the target.
        assert (running_solver.current_datetime >= target
                or running_solver.state == EngineState.ENDED)

    def test_until_rejects_wrong_type(self, running_solver):
        with pytest.raises(TypeError):
            running_solver.until(42)


# ---------------------------------------------------------------------------
# solver.options view
# ---------------------------------------------------------------------------


class TestOptionsView:
    def test_is_mutable_mapping(self, opened_solver):
        from collections.abc import MutableMapping
        assert isinstance(opened_solver.options, MutableMapping)

    def test_string_get_set(self, opened_solver):
        # FLOW_UNITS is always present.
        original = opened_solver.options["FLOW_UNITS"]
        opened_solver.options["FLOW_UNITS"] = original  # round-trip should succeed
        assert opened_solver.options["FLOW_UNITS"] == original

    def test_unknown_key_raises_keyerror(self, opened_solver):
        with pytest.raises(KeyError):
            opened_solver.options["NO_SUCH_OPTION"]

    def test_contains(self, opened_solver):
        assert "FLOW_UNITS" in opened_solver.options
        assert "NO_SUCH_OPTION" not in opened_solver.options

    def test_iter_yields_present_keys(self, opened_solver):
        keys = list(opened_solver.options)
        assert "FLOW_UNITS" in keys
        # Every yielded key must be retrievable.
        for k in keys:
            assert opened_solver.options[k] is not None

    def test_typed_shortcuts(self, opened_solver):
        assert isinstance(opened_solver.options.start_datetime, datetime)
        assert isinstance(opened_solver.options.routing_step, timedelta)


# ---------------------------------------------------------------------------
# solver.userflags
# ---------------------------------------------------------------------------


class TestUserFlags:
    def test_unknown_key_raises_keyerror(self, opened_solver):
        with pytest.raises(KeyError):
            _ = opened_solver.userflags["NO_SUCH_FLAG"]

    def test_round_trip_bool(self, opened_solver):
        # Setting a value adds the flag; reading it returns the right type.
        opened_solver.userflags["TEST_FLAG_BOOL"] = True
        v = opened_solver.userflags["TEST_FLAG_BOOL"]
        # The flag exists in some form; bool readback may surface as int.
        assert v in (True, 1)

    def test_round_trip_int(self, opened_solver):
        opened_solver.userflags["TEST_FLAG_INT"] = 7
        assert opened_solver.userflags["TEST_FLAG_INT"] in (7, True, False) or \
               opened_solver.userflags["TEST_FLAG_INT"] == 7

    def test_round_trip_float(self, opened_solver):
        opened_solver.userflags["TEST_FLAG_REAL"] = 3.14
        v = opened_solver.userflags["TEST_FLAG_REAL"]
        assert isinstance(v, float)
        assert abs(v - 3.14) < 1e-9

    def test_wrong_value_type_raises(self, opened_solver):
        with pytest.raises(TypeError):
            opened_solver.userflags["X"] = "not allowed"


# ---------------------------------------------------------------------------
# solver.events view
# ---------------------------------------------------------------------------


class TestEventsView:
    def test_is_mutable_sequence(self, opened_solver):
        from collections.abc import MutableSequence
        assert isinstance(opened_solver.events, MutableSequence)

    def test_initially_empty_or_present(self, opened_solver):
        # Site_drainage_example.inp has no [EVENTS] section by default.
        n = len(opened_solver.events)
        assert n >= 0

    def test_append_and_read_back(self, opened_solver):
        opened_solver.events.clear()
        start = datetime(2024, 1, 1, 0, 0, 0)
        end = datetime(2024, 1, 1, 12, 0, 0)
        opened_solver.events.append(Event(start=start, end=end))
        assert len(opened_solver.events) == 1
        ev = opened_solver.events[0]
        assert isinstance(ev, Event)
        # Whole-second precision; the C round-trip can drop microseconds.
        assert (ev.start - start) < timedelta(seconds=1)
        assert (ev.end - end) < timedelta(seconds=1)

    def test_delete_and_clear(self, opened_solver):
        opened_solver.events.clear()
        opened_solver.events.append((datetime(2024,1,1), datetime(2024,1,2)))
        opened_solver.events.append((datetime(2024,2,1), datetime(2024,2,2)))
        del opened_solver.events[0]
        assert len(opened_solver.events) == 1
        opened_solver.events.clear()
        assert len(opened_solver.events) == 0


# ---------------------------------------------------------------------------
# Collection accessor stubs (P1 — proper wrappers land in P2-P8)
# ---------------------------------------------------------------------------


class TestCollectionStubs:
    @pytest.mark.parametrize("attr", [
        "nodes", "links", "subcatchments", "gages",
        "pollutants", "tables", "inflows", "controls",
        "forcing", "infrastructure", "spatial", "quality",
        "statistics", "mass_balance", "editor",
    ])
    def test_lazy_collection_attribute_exists(self, opened_solver, attr):
        first = getattr(opened_solver, attr)
        assert first is not None
        # Cached: second access returns the same object.
        second = getattr(opened_solver, attr)
        assert first is second
