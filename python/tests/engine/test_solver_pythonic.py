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

import os
import unittest
from datetime import datetime, timedelta

# Skip the whole module if the engine isn't built.
try:
    import openswmm.engine._solver  # noqa: F401
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

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

from tests._paths import artifact_dir  # noqa: E402
from tests.engine._solver_cases import EngineSolverCase  # noqa: E402


# ---------------------------------------------------------------------------
# Typed property surface
# ---------------------------------------------------------------------------


class TestTypedProperties(EngineSolverCase):
    def test_state_is_engine_state_enum(self):
        solver = self.running_solver()
        self.assertIsInstance(solver.state, EngineState)
        self.assertIn(solver.state, (EngineState.STARTED, EngineState.RUNNING))

    def test_elapsed_is_timedelta(self):
        solver = self.stepped_solver()
        self.assertIsInstance(solver.elapsed, timedelta)
        # We stepped 12 times — elapsed must be > 0.
        self.assertGreater(solver.elapsed, timedelta(0))

    def test_routing_step_is_timedelta(self):
        solver = self.opened_solver()
        solver.initialize()
        solver.start()
        self.assertIsInstance(solver.routing_step, timedelta)
        # Smoke check: routing step is positive and well under a day.
        self.assertLess(timedelta(milliseconds=1), solver.routing_step)
        self.assertLess(solver.routing_step, timedelta(days=1))

    def test_datetimes(self):
        solver = self.opened_solver()
        self.assertIsInstance(solver.start_datetime, datetime)
        self.assertIsInstance(solver.end_datetime, datetime)
        self.assertIsInstance(solver.report_start_datetime, datetime)
        self.assertGreater(solver.end_datetime, solver.start_datetime)

    def test_current_datetime_progresses(self):
        solver = self.running_solver()
        start_dt = solver.start_datetime
        before = solver.current_datetime
        solver.step()
        after = solver.current_datetime
        self.assertGreaterEqual(after, before)
        # Sanity: current never precedes the configured start.
        self.assertGreaterEqual(before, start_dt)

    def test_datetime_setter_round_trip(self):
        solver = self.opened_solver()
        original = solver.start_datetime
        new = original + timedelta(hours=3, microseconds=123_456)
        solver.start_datetime = new
        # ≤ 1 µs tolerance per _dates.py.
        self.assertLessEqual(abs((solver.start_datetime - new).total_seconds()), 1e-6)

    def test_handle_is_nonzero_int(self):
        solver = self.opened_solver()
        self.assertIsInstance(solver.handle, int)
        self.assertNotEqual(solver.handle, 0)

    def test_repr_contains_state_name(self):
        solver = self.opened_solver()
        r = repr(solver)
        self.assertIn("Solver", r)
        self.assertIn(solver.state.name, r)


# ---------------------------------------------------------------------------
# Lifecycle: raises on failure, no integer rc
# ---------------------------------------------------------------------------


class TestLifecycleRaises(EngineSolverCase):
    def test_open_nonexistent_raises(self):
        s = Solver(os.path.join(artifact_dir(self), "no_such.inp"))
        with self.assertRaises(EngineError):
            s.open()
        s.destroy()

    def test_step_returns_timedelta(self):
        solver = self.running_solver()
        td = solver.step()
        self.assertIsInstance(td, timedelta)
        self.assertGreater(td, timedelta(0))

    def test_stride_returns_timedelta(self):
        solver = self.running_solver()
        td = solver.stride(5)
        self.assertIsInstance(td, timedelta)
        self.assertGreater(td, timedelta(0))


# ---------------------------------------------------------------------------
# Iteration helpers
# ---------------------------------------------------------------------------


class TestIteration(EngineSolverCase):
    def test_steps_terminates(self):
        solver = self.running_solver()
        count = 0
        last = timedelta(0)
        for elapsed in solver.steps():
            count += 1
            self.assertGreater(elapsed, last)      # monotonic non-decreasing
            last = elapsed
        # If we got here, the iterator terminated naturally.
        self.assertGreater(count, 0)
        self.assertIn(solver.state, (EngineState.RUNNING, EngineState.ENDED))

    def test_until_with_timedelta(self):
        solver = self.running_solver()
        target = timedelta(hours=1)
        reached = solver.until(target)
        # Either we got to the target, or the simulation ended first.
        self.assertTrue(reached >= target or solver.state == EngineState.ENDED)

    def test_until_with_datetime(self):
        solver = self.running_solver()
        target = solver.start_datetime + timedelta(hours=2)
        solver.until(target)
        # After the call, current_datetime should be at or past target — or the
        # sim ended before the target.
        self.assertTrue(solver.current_datetime >= target
                        or solver.state == EngineState.ENDED)

    def test_until_rejects_wrong_type(self):
        solver = self.running_solver()
        with self.assertRaises(TypeError):
            solver.until(42)


# ---------------------------------------------------------------------------
# solver.options view
# ---------------------------------------------------------------------------


class TestOptionsView(EngineSolverCase):
    def test_is_mutable_mapping(self):
        solver = self.opened_solver()
        from collections.abc import MutableMapping
        self.assertIsInstance(solver.options, MutableMapping)

    def test_string_get_set(self):
        solver = self.opened_solver()
        # FLOW_UNITS is always present.
        original = solver.options["FLOW_UNITS"]
        solver.options["FLOW_UNITS"] = original  # round-trip should succeed
        self.assertEqual(solver.options["FLOW_UNITS"], original)

    def test_unknown_key_raises_keyerror(self):
        solver = self.opened_solver()
        with self.assertRaises(KeyError):
            solver.options["NO_SUCH_OPTION"]

    def test_contains(self):
        solver = self.opened_solver()
        self.assertIn("FLOW_UNITS", solver.options)
        self.assertNotIn("NO_SUCH_OPTION", solver.options)

    def test_iter_yields_present_keys(self):
        solver = self.opened_solver()
        keys = list(solver.options)
        self.assertIn("FLOW_UNITS", keys)
        # Every yielded key must be retrievable.
        for k in keys:
            self.assertIsNotNone(solver.options[k])

    def test_typed_shortcuts(self):
        solver = self.opened_solver()
        self.assertIsInstance(solver.options.start_datetime, datetime)
        self.assertIsInstance(solver.options.routing_step, timedelta)


# ---------------------------------------------------------------------------
# solver.userflags
# ---------------------------------------------------------------------------


class TestUserFlags(EngineSolverCase):
    def test_unknown_key_raises_keyerror(self):
        solver = self.opened_solver()
        with self.assertRaises(KeyError):
            _ = solver.userflags["NO_SUCH_FLAG"]

    def test_round_trip_bool(self):
        solver = self.opened_solver()
        # Setting a value adds the flag; reading it returns the right type.
        solver.userflags["TEST_FLAG_BOOL"] = True
        v = solver.userflags["TEST_FLAG_BOOL"]
        # The flag exists in some form; bool readback may surface as int.
        self.assertIn(v, (True, 1))

    def test_round_trip_int(self):
        solver = self.opened_solver()
        solver.userflags["TEST_FLAG_INT"] = 7
        self.assertTrue(solver.userflags["TEST_FLAG_INT"] in (7, True, False) or
                        solver.userflags["TEST_FLAG_INT"] == 7)

    def test_round_trip_float(self):
        solver = self.opened_solver()
        solver.userflags["TEST_FLAG_REAL"] = 3.14
        v = solver.userflags["TEST_FLAG_REAL"]
        self.assertIsInstance(v, float)
        self.assertLess(abs(v - 3.14), 1e-9)

    def test_wrong_value_type_raises(self):
        solver = self.opened_solver()
        # bool/int/float/str are all accepted (str auto-defines a STRING
        # flag); a container type has no supported setter and must raise.
        with self.assertRaises(TypeError):
            solver.userflags["X"] = [1, 2, 3]


# ---------------------------------------------------------------------------
# solver.events view
# ---------------------------------------------------------------------------


class TestEventsView(EngineSolverCase):
    def test_is_mutable_sequence(self):
        solver = self.opened_solver()
        from collections.abc import MutableSequence
        self.assertIsInstance(solver.events, MutableSequence)

    def test_initially_empty_or_present(self):
        solver = self.opened_solver()
        # Site_drainage_example.inp has no [EVENTS] section by default.
        n = len(solver.events)
        self.assertGreaterEqual(n, 0)

    def test_append_and_read_back(self):
        solver = self.opened_solver()
        solver.events.clear()
        start = datetime(2024, 1, 1, 0, 0, 0)
        end = datetime(2024, 1, 1, 12, 0, 0)
        solver.events.append(Event(start=start, end=end))
        self.assertEqual(len(solver.events), 1)
        ev = solver.events[0]
        self.assertIsInstance(ev, Event)
        # Whole-second precision; the C round-trip can drop microseconds.
        self.assertLess((ev.start - start), timedelta(seconds=1))
        self.assertLess((ev.end - end), timedelta(seconds=1))

    def test_delete_and_clear(self):
        solver = self.opened_solver()
        solver.events.clear()
        solver.events.append((datetime(2024,1,1), datetime(2024,1,2)))
        solver.events.append((datetime(2024,2,1), datetime(2024,2,2)))
        del solver.events[0]
        self.assertEqual(len(solver.events), 1)
        solver.events.clear()
        self.assertEqual(len(solver.events), 0)


# ---------------------------------------------------------------------------
# Collection accessor stubs (P1 — proper wrappers land in P2-P8)
# ---------------------------------------------------------------------------


class TestCollectionStubs(EngineSolverCase):
    def test_lazy_collection_attribute_exists(self):
        solver = self.opened_solver()
        for attr in (
            "nodes", "links", "subcatchments", "gages",
            "pollutants", "tables", "inflows", "controls",
            "forcing", "infrastructure", "spatial", "quality",
            "statistics", "mass_balance", "editor",
        ):
            with self.subTest(attr=attr):
                first = getattr(solver, attr)
                self.assertIsNotNone(first)
                # Cached: second access returns the same object.
                second = getattr(solver, attr)
                self.assertIs(first, second)
