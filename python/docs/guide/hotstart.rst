=========
Hot start
=========

.. note::

   **Engine:** OpenSWMM 6 — refactored.  Documents
   :class:`openswmm.engine.HotStart`.

.. currentmodule:: openswmm.engine

A **hot start file** captures the full hydraulic state of a model at
a single instant: every node depth, link flow, subcatchment runoff
state, and (optionally) pollutant concentrations.  Hot starts let you:

* Skip the warm-up period when running many short simulations.
* Save state at end-of-day / end-of-storm for downstream analysis.
* Patch state programmatically before applying it (e.g. to study a
  perturbation from a known operating point).

Reference: ``openswmm_hotstart.h``.

----

Class signature
===============

.. code-block:: python

    class HotStart:
        def __init__(self) -> None: ...

        # Class methods (alternative constructors)
        @staticmethod
        def save(solver: Solver, path: str) -> None: ...
        @classmethod
        def open(cls, path: str) -> "HotStart": ...

The :class:`HotStart` is **not** tied to a Solver.  You create one,
mutate its state if you wish, then apply it to a Solver.

----

Key methods
===========

Lifecycle
---------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Action
   * - :meth:`HotStart.save(solver, path)`
     - Snapshot the solver's state to ``path``.
   * - :meth:`HotStart.open(path)`
     - Load a previously-saved hot start file.
   * - :meth:`apply(solver)`
     - Initialise ``solver`` from this hot start.
   * - :meth:`close()`
     - Release any underlying resources.
   * - ``with HotStart.open(path) as hs:``
     - Context-manager wrapper.

State patching
--------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action
   * - :meth:`set_node_depth(id, value)`
     - Override a node's depth in this hot start.
   * - :meth:`set_node_head(id, value)`
     - Override a node's head.
   * - :meth:`set_link_flow(id, value)`
     - Override a link's flow.
   * - :meth:`set_link_depth(id, value)`
     - Override a link's mid-point depth.
   * - :meth:`set_subcatch_runoff(id, value)`
     - Override a subcatchment's runoff.

Inspection
----------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Returns
   * - :meth:`get_sim_time()`
     - Simulation time captured in this hot start (decimal days).
   * - :meth:`get_crs()`
     - CRS string captured at save time.
   * - :meth:`node_count()` / :meth:`link_count()`
     - Counts in the hot start.
   * - :meth:`warning_count()` / :meth:`get_warning(idx)`
     - Diagnostic warnings emitted at save / load.

----

End-to-end example
==================

Save state at end of warm-up, restart later
-------------------------------------------

.. code-block:: python

    from openswmm.engine import Solver, HotStart

    # Step 1: warm up and save
    with Solver("model.inp", "warmup.rpt", "warmup.out") as s:
        while s.elapsed < 0.5:                    # half a day of warm-up
            s.step()
        HotStart.save(s, "warmup.hsf")
        # context manager finishes the run for clean .rpt/.out

    # Step 2: re-run from the warmed-up state
    with Solver("model.inp", "main.rpt", "main.out") as s, \
         HotStart.open("warmup.hsf") as hs:
        hs.apply(s)
        while s.step():
            pass

The hot-start file is portable: same machine or any machine with the
matching engine version.

Patch state before applying
---------------------------

.. code-block:: python

    with Solver("model.inp", "patched.rpt", "patched.out") as s, \
         HotStart.open("baseline.hsf") as hs:
        # Counterfactual: J1 depth bumped from baseline to test response
        hs.set_node_depth("J1", 4.0)
        hs.apply(s)
        while s.step():
            pass

----

Common recipes
==============

Inspect a hot-start file without applying it
---------------------------------------------

.. code-block:: python

    with HotStart.open("warmup.hsf") as hs:
        print(f"Captured at sim-day {hs.get_sim_time():.4f}")
        print(f"  nodes: {hs.node_count()}, links: {hs.link_count()}")
        print(f"  CRS:   {hs.get_crs() or '<none>'}")
        for i in range(hs.warning_count()):
            print(f"  warning: {hs.get_warning(i)}")

End-of-day chain  (Monte Carlo storm sweep)
-------------------------------------------

.. code-block:: python

    # Build a baseline hot start once
    with Solver("baseline.inp", "", "") as s:
        while s.elapsed < 1.0:                     # 1 day baseline
            s.step()
        HotStart.save(s, "baseline.hsf")

    # Run 50 storms from the baseline, in parallel
    from concurrent.futures import ProcessPoolExecutor

    def run_storm(seed):
        rpt = f"storm_{seed:03d}.rpt"
        out = f"storm_{seed:03d}.out"
        with Solver("storm_template.inp", rpt, out) as s, \
             HotStart.open("baseline.hsf") as hs:
            hs.apply(s)
            inject_random_storm(s, seed=seed)
            while s.step():
                pass
        return out

    with ProcessPoolExecutor(max_workers=8) as pool:
        for path in pool.map(run_storm, range(50)):
            print("done:", path)

----

EngineState requirements & exceptions
=====================================

.. list-table::
   :header-rows: 1
   :widths: 30 25 45

   * - Method
     - Required state
     - Notes
   * - :meth:`HotStart.save(solver, path)`
     - solver in ``RUNNING`` or ``ENDED``
     - Captures whatever the engine has at the moment of the call.
   * - :meth:`apply(solver)`
     - solver in ``OPENED`` or ``INITIALIZED``
     - Replaces the solver's initial conditions.
   * - state-setter methods (``set_*``)
     - any state of the HotStart
     - Modifies the in-memory hot start; persists when applied.

Common exceptions:

* :exc:`FileNotFoundError`     — hot-start path missing.
* :exc:`ValueError`            — schema mismatch (saved by an
  incompatible engine version).
* :class:`EngineError`         — application failed (model topology
  mismatch, unknown id in a setter).

Always use :meth:`HotStart.open` as a context manager; manual
:meth:`close` is allowed but the context-manager form is more robust.

----

See also
========

* :doc:`solver` — the lifecycle that hot starts plug into.
* :doc:`spatial` — CRS captured in / restored from the hot start.
