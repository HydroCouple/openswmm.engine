=============================
Running a simulation — Solver
=============================

.. note::

   **Engine:** OpenSWMM 6 — refactored.  This is
   :class:`openswmm.engine.Solver`.  The legacy SWMM 5 solver of the
   same name lives at :class:`openswmm.legacy.engine.Solver` — see
   :doc:`../legacy/solver` for its (different) API.

.. currentmodule:: openswmm.engine

The :class:`Solver` class is the entry point.  It owns the SWMM engine
handle, parses the input file, drives the simulation forward in time,
and writes report and binary-output files.  Every other domain class
(:class:`Nodes`, :class:`Links`, :class:`Forcing`, …) takes a Solver in
its constructor.

----

Class signature
===============

.. code-block:: python

    class Solver:
        def __init__(self, inp: str = "", rpt: str = "", out: str = "") -> None: ...

* ``inp`` — path to the SWMM ``.inp`` input file.
* ``rpt`` — path for the human-readable ``.rpt`` report.  Empty string
  to skip.
* ``out`` — path for the binary ``.out`` results file.  Empty string
  to skip.

The C engine handle is allocated lazily on the first call to
:meth:`open`; until then, only ``state``, ``handle``, and the lifecycle
helpers are valid.

Lifecycle methods
-----------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - What it does
   * - :meth:`Solver.create`
     - Allocate the engine handle.  Implicit on ``open()``.
   * - :meth:`Solver.open`
     - Parse the ``.inp`` and load plugins.
   * - :meth:`Solver.initialize`
     - Allocate state arrays; apply initial conditions.
   * - :meth:`Solver.start`
     - Start routing.  ``save_results=True`` writes ``.out``.
   * - :meth:`Solver.step`
     - Advance one routing step.  Returns the C error code (``0`` on
       success).  Poll :attr:`state` for completion.
   * - :meth:`Solver.stride`
     - Advance ``n_steps`` steps in one call.  Returns the C error code.
   * - :meth:`Solver.end`
     - Stop routing; finalize cumulative outputs.
   * - :meth:`Solver.report`
     - Write the human-readable ``.rpt`` summary.
   * - :meth:`Solver.close`
     - Flush ``.rpt`` / ``.out`` and close files.
   * - :meth:`Solver.destroy`
     - Free the engine handle.  *Always* call this.
   * - :meth:`Solver.__enter__` / ``__exit__``
     - Context-manager wrapper for the full lifecycle.

Inspection / time
-----------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Property / method
     - Returns
   * - :attr:`Solver.state`
     - Current :class:`EngineState` (int).
   * - :attr:`Solver.handle`
     - Opaque engine handle (mostly for plugin authors).
   * - :attr:`Solver.elapsed`
     - Elapsed simulation time in days.
   * - :meth:`get_start_time`
     - Simulation start time (decimal days since 1899-12-30).
   * - :meth:`get_end_time`
     - Simulation end time (same epoch).
   * - :meth:`get_current_time`
     - Current simulation time (same epoch).
   * - :meth:`get_routing_step`
     - Current routing time step in seconds.
   * - :attr:`Solver.start_datetime`
     - Simulation start as a :class:`datetime.datetime`.
   * - :attr:`Solver.end_datetime`
     - Simulation end as a :class:`datetime.datetime`.
   * - :attr:`Solver.report_start_datetime`
     - Report start as a :class:`datetime.datetime`.
   * - :meth:`get_option` / :meth:`set_option`
     - Read or update ``[OPTIONS]`` entries (e.g. ``FLOW_UNITS``).
   * - :meth:`get_option_ext` / :meth:`set_option_ext`
     - Read or update extension options unknown to base SWMM.
   * - :meth:`get_crs`
     - Coordinate reference system string (EPSG / PROJ / WKT).

Event windows, steady-state, and callbacks
------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - What it does
   * - :meth:`Solver.events_count` / :meth:`events_get` / :meth:`events_set`
     - Inspect or update the event-window list (start/end pairs).
   * - :meth:`Solver.events_add` / :meth:`events_remove` / :meth:`events_clear`
     - Programmatically grow or trim the event-window list.
   * - :meth:`Solver.get_steady_state_skip` / :meth:`set_steady_state_skip`
     - Enable/disable the steady-state skip optimisation.
   * - :meth:`Solver.set_step_begin_callback`
     - Register a callable invoked before each routing step.
   * - :meth:`Solver.set_step_end_callback`
     - Register a callable invoked after each routing step.
   * - :meth:`Solver.set_warning_callback`
     - Register a callable invoked when the engine raises a warning.

Module-level helpers
--------------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Function
     - What it does
   * - :func:`openswmm.engine.run`
     - One-call helper that opens, runs, reports, and closes a model.
   * - :func:`openswmm.engine.run_with_callback`
     - Same as :func:`run` but invokes a progress callback each step.

Save / serialise
----------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Action
   * - :meth:`model_write`
     - Write the current model state back out as ``.inp``.

----

End-to-end example
==================

.. code-block:: python

    from openswmm.engine import Solver, EngineState

    with Solver("site_drainage.inp", "site_drainage.rpt", "site_drainage.out") as s:
        print(f"Routing step: {s.get_routing_step():.1f}s")
        print(f"Sim window:   day {s.get_start_time():.4f} → day {s.get_end_time():.4f}")

        steps = 0
        while s.state == EngineState.RUNNING:
            rc = s.step()
            if rc != 0:
                break
            steps += 1
            if steps % 240 == 0:                      # every ~hour at 15s step
                print(f"  t = {s.elapsed*24:5.2f} h")

        print(f"Done — {steps} routing steps.")

The context manager guarantees ``end() → report() → close() → destroy()``
runs even if the loop body raises.

Manual lifecycle (when you need to inspect the parsed model
before initialisation):

.. code-block:: python

    from openswmm.engine import Solver, Nodes, EngineState

    s = Solver("model.inp", "model.rpt", "model.out")
    s.create()
    s.open()                              # state == OPENED

    nodes = Nodes(s)
    print(f"Model has {nodes.count()} nodes")
    for i in range(nodes.count()):
        print(f"  {nodes.get_id(i)} (type={nodes.get_type(i)})")

    s.initialize()
    s.start(save_results=True)            # state == RUNNING
    while s.state == EngineState.RUNNING:
        if s.step() != 0:
            break
    s.end()
    s.report()
    s.close()
    s.destroy()                           # release engine handle

----

Common recipes
==============

Report progress every wall-clock second
---------------------------------------

.. code-block:: python

    import time
    from openswmm.engine import Solver, EngineState

    with Solver("model.inp", "model.rpt", "model.out") as s:
        last = time.monotonic()
        total = s.get_end_time() - s.get_start_time()
        while s.state == EngineState.RUNNING:
            if s.step() != 0:
                break
            now = time.monotonic()
            if now - last >= 1.0:
                pct = 100.0 * s.elapsed / total
                print(f"{pct:5.1f}%  ({s.elapsed:.4f} d / {total:.4f} d)")
                last = now

Run multiple scenarios in parallel
----------------------------------

.. code-block:: python

    from concurrent.futures import ProcessPoolExecutor
    from openswmm.engine import Solver, EngineState

    def run_one(inp_path):
        rpt = inp_path.replace(".inp", ".rpt")
        out = inp_path.replace(".inp", ".out")
        with Solver(inp_path, rpt, out) as s:
            while s.state == EngineState.RUNNING:
                if s.step() != 0:
                    break
        return out

    inputs = ["scenario_a.inp", "scenario_b.inp", "scenario_c.inp"]
    with ProcessPoolExecutor(max_workers=4) as pool:
        for out_file in pool.map(run_one, inputs):
            print("done:", out_file)

Each child process has its own engine handle; no global state to
collide.

Stop simulation early on a custom condition
-------------------------------------------

.. code-block:: python

    from openswmm.engine import Solver, Nodes, EngineState

    with Solver("model.inp", "model.rpt", "model.out") as s:
        nodes = Nodes(s)
        flooded = nodes.get_index("J1")
        while s.state == EngineState.RUNNING:
            if s.step() != 0:
                break
            if nodes.get_depth(flooded) > 5.0:
                print(f"Flood threshold hit at t={s.elapsed:.4f} d")
                break       # context manager still runs end/report/close

Skip writing the binary ``.out``
--------------------------------

.. code-block:: python

    # Pass empty string for `out`
    with Solver("model.inp", "model.rpt", "") as s:
        ...

This is faster (no per-step output writes) and saves disk; you trade
away the ability to use :class:`OutputReader` afterwards.

Skip writing the report too
---------------------------

.. code-block:: python

    with Solver("model.inp", "", "") as s:
        ...

Useful in tight Monte-Carlo loops or as part of a CI smoke test.

Save the modified model back to disk
------------------------------------

After you've used :class:`ModelEditor` or :class:`ModelBuilder` to
mutate the model, persist the result:

.. code-block:: python

    s.model_write("modified.inp")

The output is a fully-valid SWMM ``.inp`` file (round-trippable
through any SWMM reader).

----

Bulk arrays
===========

The Solver itself does not expose a bulk-array surface — those live on
the domain classes (:class:`Nodes`, :class:`Links`, etc.).  The Solver
does, however, expose **scalar** time accessors that you'll often
combine with bulk reads:

.. code-block:: python

    import numpy as np
    from openswmm.engine import Solver, Nodes, EngineState

    times, depths = [], []
    with Solver("model.inp", "model.rpt", "") as s:
        nodes = Nodes(s)
        while s.state == EngineState.RUNNING:
            if s.step() != 0:
                break
            times.append(s.elapsed)                          # days since start
            depths.append(nodes.get_depths_bulk().copy())    # (n_nodes,)
    times  = np.array(times)              # shape (T,)
    depths = np.stack(depths)             # shape (T, n_nodes)

----

EngineState requirements & exceptions
=====================================

.. list-table::
   :header-rows: 1
   :widths: 30 25 45

   * - Method
     - Required state
     - Notes
   * - ``create``
     - any (no-op if already created)
     - Idempotent.
   * - ``open``
     - ``CREATED``
     - Raises if the file fails to parse.
   * - ``initialize``
     - ``OPENED``
     - Allocates per-element state arrays.
   * - ``start``
     - ``INITIALIZED``
     - Transitions to ``RUNNING``.  ``save_results=False`` skips
       ``.out`` writes.
   * - ``step``
     - ``RUNNING``
     - Returns the C error code (``0`` on success).  Engine transitions
       to ``ENDED`` when the end-time is reached; poll :attr:`state` to
       detect completion.
   * - ``stride``
     - ``RUNNING``
     - As ``step``, but advances ``n_steps`` timesteps in one call.
   * - ``end``
     - ``RUNNING`` or ``ENDED``
     - Idempotent; second call is a no-op.
   * - ``report``
     - ``ENDED``
     - Writes the ``.rpt`` summary.
   * - ``close``
     - any (after ``ENDED`` for a sensible report)
     - Flushes ``.out`` and ``.rpt``.
   * - ``destroy``
     - any
     - Frees the engine handle.

Calling a method out of order raises :class:`EngineError`.  Common
codes you'll see:

.. list-table::
   :header-rows: 1
   :widths: 12 22 66

   * - Code
     - Name
     - Meaning
   * - ``20``
     - ``ERR_API_NOT_OPEN``
     - You called a method that needs ``OPENED`` while still ``CREATED``.
   * - ``21``
     - ``ERR_API_NOT_STARTED``
     - You called ``step()`` before ``start()``.
   * - ``22``
     - ``ERR_API_NOT_ENDED``
     - You called ``report()`` before ``end()``.
   * - ``23``
     - ``ERR_API_INVALID_TYPE``
     - Wrong object type for the operation (e.g. setting an outfall
       parameter on a junction).

For the full list see :class:`~openswmm.engine.ErrorCode`.

----

Runoff interface file (Phase 1b)
================================

Persists per-subcatchment runoff snapshots to a binary file matching
the legacy SWMM-5 ``Frunoff`` format.  Useful when a slow-running
runoff phase needs to be cached and replayed against a downstream
routing-only run (e.g. design-storm sensitivity analyses).

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Description
   * - :meth:`open_runoff_iface_write(path)`
     - Open the file in SAVE mode.  The engine then auto-emits one
       record per runoff substep until :meth:`close_runoff_iface`
       is called.
   * - :meth:`open_runoff_iface_read(path)`
     - Open the file in USE mode.  Currently exposes the file but
       does **not** yet skip the engine's runoff computation —
       see the note below.
   * - :meth:`save_runoff_step(dt)`
     - Manually force one snapshot.  Normally unnecessary because the
       engine auto-saves; provided for plugin authors and tests.
   * - :meth:`read_runoff_step()`
     - Read one record from the open USE file into the current
       subcatchment runoff/quality vectors.  Returns ``False`` on EOF.
   * - :meth:`close_runoff_iface()`
     - Close the file (idempotent).  Called automatically when the
       solver is closed.

.. note::

   USE-mode auto-skip — making the engine bypass its own runoff
   computation when a USE file is open — is a follow-up to Phase 1b.
   Today's USE mode is an advanced manual feature: callers must invoke
   :meth:`read_runoff_step` between :meth:`Solver.step` calls *and*
   understand that the engine will overwrite the loaded state when it
   runs its own runoff phase.

End-to-end SAVE example:

.. code-block:: python

   from openswmm.engine import EngineState, Solver

   with Solver("design_storm.inp", "design_storm.rpt", "design_storm.out") as s:
       s.open()
       s.initialize()
       s.start()
       s.open_runoff_iface_write("design_storm.rfi")
       while s.state == EngineState.RUNNING:
           if s.step() != 0:
               break
       s.end()
       s.close_runoff_iface()
       # design_storm.rfi now contains one record per runoff substep and
       # can be reopened by a downstream routing-only run via
       # ``s.open_runoff_iface_read(...)``.

----

See also
========

* :doc:`concepts` — the broader engine-state and exception model.
* :doc:`error_handling` — programmatic error handling patterns.
* :doc:`nodes`, :doc:`links` — query and modify state during a run.
* :doc:`output_reader` — post-process the ``.out`` file after the run.
