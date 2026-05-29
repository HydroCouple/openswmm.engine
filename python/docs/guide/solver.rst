=============================
Running a simulation — Solver
=============================

.. note::

   **Engine:** OpenSWMM 6 — refactored.  This is
   :class:`openswmm.engine.Solver`.  The legacy SWMM 5 solver of the
   same name lives at :class:`openswmm.legacy.engine.Solver` — see
   :doc:`../legacy/solver` for its (different) API.

.. currentmodule:: openswmm.engine

The :class:`Solver` class is the entry point. It owns the SWMM engine
handle, parses the input file, drives the simulation forward in time,
and writes report and binary-output files. Every domain accessor —
nodes, links, subcatchments, gages, controls, output, etc. — hangs off
the Solver as a lazy attribute (``solver.nodes``, ``solver.links``, …).

----

Class signature
===============

.. code-block:: python

    class Solver:
        def __init__(
            self,
            inp: str | os.PathLike = "",
            rpt: str | os.PathLike | None = None,
            out: str | os.PathLike | None = None,
            *,
            plugin_lib: str | os.PathLike | None = None,
        ) -> None: ...

Every file argument accepts :class:`pathlib.Path` or any
:class:`os.PathLike`. The C engine handle is allocated lazily on the
first call to :meth:`open`; until then, only :attr:`state`,
:attr:`handle`, and the lifecycle helpers are valid.

----

Quickstart
==========

.. code-block:: python

    from datetime import timedelta
    from pathlib import Path
    from openswmm.engine import Solver

    with Solver(Path("model.inp"), Path("model.rpt"), Path("model.out")) as s:
        print(s.start_datetime, s.routing_step)   # datetime, timedelta
        for elapsed in s.steps():                 # iterates until END
            if elapsed >= timedelta(hours=24):
                break
        # Inside the `with` block we are RUNNING. On exit the context
        # manager runs end → report → close → destroy automatically.

That's the entire happy path. The remaining sections cover the manual
lifecycle, the property surface, and the view objects.

----

Lifecycle
=========

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Method
     - What it does
   * - :meth:`Solver.create`
     - Allocate the engine handle. Called implicitly by :meth:`open`.
   * - :meth:`Solver.open`
     - Parse the ``.inp`` and load plugins. Raises on failure.
   * - :meth:`Solver.initialize`
     - Allocate state arrays; apply initial conditions.
   * - :meth:`Solver.start`
     - Begin routing. ``save_results=True`` writes ``.out``.
   * - :meth:`Solver.step`
     - Advance one routing step. Returns a :class:`~datetime.timedelta`;
       ``timedelta(0)`` means the simulation has ended.
   * - :meth:`Solver.stride`
     - Advance ``n_steps`` routing steps in one call.
   * - :meth:`Solver.steps`
     - Iterator that yields per-step :class:`~datetime.timedelta` until
       the simulation ends.
   * - :meth:`Solver.until`
     - Stride forward until a target :class:`~datetime.datetime` /
       :class:`~datetime.timedelta` is reached.
   * - :meth:`Solver.end`
     - Finalize the simulation. Cumulative stats become available.
   * - :meth:`Solver.report`
     - Write the summary report to the ``.rpt`` file.
   * - :meth:`Solver.close`
     - Close all files. Idempotent.
   * - :meth:`Solver.destroy`
     - Free the C engine handle. Idempotent.

**Every lifecycle method raises** :class:`EngineError` (or a subclass —
see :doc:`error_handling`) on failure. None of them return an integer
status code.

Manual lifecycle (no context manager):

.. code-block:: python

    s = Solver("model.inp", "model.rpt", "model.out")
    s.open()             # state == OPENED   — inspect / edit the model here
    s.initialize()       # state == INITIALIZED
    s.start()            # state == STARTED → RUNNING after first step
    for _ in s.steps():
        pass
    s.end()              # state == ENDED — mass balance available
    s.report()
    s.close()            # state == CLOSED
    s.destroy()

Use :meth:`Solver.run` for a one-call run-to-completion when there's
nothing to do between steps; or the free function
:func:`openswmm.engine.run` if you have no Solver yet.

----

Typed property surface
======================

Every value that has a natural Python type now returns it:

.. list-table::
   :header-rows: 1
   :widths: 32 18 50

   * - Property
     - Type
     - Notes
   * - :attr:`state`
     - :class:`EngineState`
     - ``IntEnum``; compares cleanly with bare integers.
   * - :attr:`elapsed`
     - :class:`~datetime.timedelta`
     - Elapsed simulation time after the last :meth:`step`.
   * - :attr:`routing_step`
     - :class:`~datetime.timedelta`
     - The active routing step duration.
   * - :attr:`start_datetime`
     - :class:`~datetime.datetime`
     - Read/write. Microsecond round-trip ≤ 1 µs.
   * - :attr:`end_datetime`
     - :class:`~datetime.datetime`
     - Read/write.
   * - :attr:`report_start_datetime`
     - :class:`~datetime.datetime`
     - Read/write.
   * - :attr:`current_datetime`
     - :class:`~datetime.datetime`
     - Updated each step; equals ``start_datetime + elapsed``.
   * - :attr:`crs`
     - ``str``
     - CRS string from ``[OPTIONS]`` (or raises :class:`CRSError`).
   * - :attr:`is_between_events`
     - ``bool``
     - ``True`` when the current time falls outside any ``[EVENTS]``
       window.
   * - :attr:`steady_state_skip`
     - ``bool``
     - Read/write; toggles ``SKIP_STEADY_STATE``.
   * - :attr:`handle`
     - ``int``
     - Raw engine pointer for advanced interop.
   * - :attr:`generation`
     - ``int``
     - Monotonic counter bumped on structural mutations (add / delete /
       rename). Wrapper objects (P2+) check this to detect staleness.

See :doc:`datetime` for the SWMM DateTime ↔ Python ``datetime`` rules
behind the date properties.

----

Iteration helpers
=================

``Solver.steps()`` — per-step iteration
---------------------------------------

.. code-block:: python

    with Solver("model.inp") as s:
        for elapsed in s.steps():
            # elapsed is a datetime.timedelta after the most recent step
            if s.nodes["J1"].depth > 5.0:        # depth wrapper lands in P2
                break

The iterator terminates **automatically** when the engine reports zero
elapsed time. A bare ``for x in solver`` is intentionally not provided —
the ``steps()`` method makes the intent explicit.

``Solver.until(target)`` — advance to a moment
----------------------------------------------

.. code-block:: python

    from datetime import timedelta

    with Solver("model.inp") as s:
        s.until(timedelta(hours=12))          # elapsed-time target
        # ... read state at t = 12 h ...
        s.until(s.end_datetime)               # finish the run

``target`` may be a :class:`~datetime.datetime` (absolute moment) or a
:class:`~datetime.timedelta` (duration from start). The return value is
the actual elapsed :class:`~datetime.timedelta` reached — it can be
**less** than the requested target if the simulation ended first.

----

View objects
============

``solver.options``
------------------

A :class:`MutableMapping` view over the ``[OPTIONS]`` block, plus typed
shortcuts:

.. code-block:: python

    solver.options["FLOW_UNITS"] = "CMS"          # string-keyed access
    solver.options.start_datetime                  # datetime
    solver.options.routing_step                    # timedelta

Iteration yields every key the C API confirms is present:

.. code-block:: python

    for k in solver.options:
        print(k, "=", solver.options[k])

``solver.options.ext`` is a parallel sub-mapping for the
``[OPTIONS_EXT]`` block.

``solver.userflags``
--------------------

Typed user-flag access:

.. code-block:: python

    solver.userflags["MY_FLAG"] = True             # picks the bool setter
    solver.userflags["MAX_PATHS"] = 4              # picks the int setter
    solver.userflags["TOLERANCE"] = 1e-6           # picks the real setter

Reading returns a Python ``bool``, ``int``, or ``float`` depending on
which C getter responds first. Unknown flag names raise :exc:`KeyError`.

``solver.events``
-----------------

A :class:`MutableSequence` over the ``[EVENTS]`` block:

.. code-block:: python

    from datetime import datetime
    from openswmm.engine._solver import Event

    solver.events.append(Event(
        start=datetime(2024, 1, 1, 0, 0),
        end=datetime(2024, 1, 1, 12, 0),
    ))
    for ev in solver.events:
        print(ev.start, ev.end)

    del solver.events[0]
    solver.events.clear()

``insert`` is supported (the C API only allows append, so it's emulated
by clearing and rebuilding).

----

Domain-collection accessors
===========================

Every domain hangs off the Solver as a lazy attribute, materialised on
first access and cached:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Attribute
     - What it is
   * - ``solver.nodes``
     - Node collection (see :doc:`nodes`).
   * - ``solver.links``
     - Link collection (see :doc:`links`).
   * - ``solver.subcatchments``
     - Subcatchment collection (see :doc:`subcatchments`).
   * - ``solver.gages``
     - Rain-gage collection (see :doc:`gages`).
   * - ``solver.pollutants``
     - Pollutant collection (see :doc:`pollutants`).
   * - ``solver.tables``
     - Time series, curves, patterns (see :doc:`tables`).
   * - ``solver.inflows``
     - External / DWF / RDII inflows (see :doc:`inflows`).
   * - ``solver.controls``
     - Control rules and direct actions (see :doc:`controls`).
   * - ``solver.forcing``
     - Runtime forcing (see :doc:`forcing`).
   * - ``solver.infrastructure``
     - Transects, streets, inlets, LIDs (see :doc:`infrastructure`).
   * - ``solver.spatial``
     - Coordinates, CRS, vertices, polygons (see :doc:`spatial`).
   * - ``solver.quality``
     - Landuse, buildup, washoff, treatment (see :doc:`quality`).
   * - ``solver.statistics``
     - Cumulative simulation statistics (see :doc:`statistics`).
   * - ``solver.mass_balance``
     - Continuity errors and flux totals (see :doc:`massbalance`).
   * - ``solver.editor``
     - In-place model editing (see :doc:`editing`).

----

Callbacks
=========

Three callback registration points:

.. code-block:: python

    def on_step_begin(sim_time, dt):
        ...   # inject forcings, modify boundary conditions

    def on_step_end(sim_time, dt):
        ...   # read state for real-time monitoring

    def on_warning(code, message):
        log.warning("engine: %s (%s)", message, code)

    solver.set_step_begin_callback(on_step_begin)
    solver.set_step_end_callback(on_step_end)
    solver.set_warning_callback(on_warning)

Pass ``None`` to unregister any of them.

----

Convenience module-level helpers
================================

.. code-block:: python

    from openswmm.engine import run, run_with_callback

    run("model.inp", "model.rpt", "model.out")     # one-call run-to-completion

    def progress(frac):
        print(f"{frac:.0%}")
    run_with_callback("model.inp", callback=progress)

Both raise :class:`EngineError` on failure.

----

See also
========

* :doc:`concepts` — engine lifecycle and the :class:`EngineState` machine.
* :doc:`error_handling` — exception hierarchy and recommended idioms.
* :doc:`datetime` — SWMM DateTime ↔ Python ``datetime`` conversion.
* :doc:`nodes`, :doc:`links`, :doc:`subcatchments`, :doc:`output_reader` —
  the most-used domain accessors.
