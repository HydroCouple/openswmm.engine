============================================
Legacy SWMM 5 Solver
============================================

.. note::

   **Engine:** SWMM 5.x — *legacy*.  This page documents the
   :class:`openswmm.legacy.engine.Solver` class.  For the modern
   reentrant OpenSWMM 6 ``Solver`` see :doc:`../guide/solver`.

The legacy :class:`Solver` is a thin Cython wrapper around the original
EPA SWMM 5.x C solver.  The class is preserved verbatim so that scripts
written against the EPA SWMM 5.x C API or earlier :mod:`openswmm`
releases continue to run unchanged.

If you are starting fresh, prefer :class:`openswmm.engine.Solver`.  If
you have existing legacy code, this page documents what's available and
:doc:`../migration/swmm5_to_swmm6` shows how to migrate.

----

Class signature
===============

.. code-block:: python

    from openswmm.legacy.engine import Solver

    class Solver:
        def __init__(
            self,
            inp_file: str,
            rpt_file: str = "",
            out_file: str = "",
            swmm_progress_callback: Callable[[float], None] = None,
            stride_step: int = 0,
        ) -> None: ...

* ``inp_file`` / ``rpt_file`` / ``out_file`` — same role as the v6
  Solver: input, human-readable report, binary output.
* ``swmm_progress_callback`` — called periodically with the simulation
  fraction (0.0 → 1.0).  Frequency controlled via
  :attr:`progress_callbacks_per_second`.
* ``stride_step`` — number of routing steps to advance per
  :meth:`step` call (0 = single step).

The class supports the context-manager and iterator protocols.

----

Lifecycle
=========

The legacy lifecycle is simpler than v6 (no explicit ``create``):

.. list-table::
   :header-rows: 1
   :widths: 38 62

   * - Method
     - Action
   * - :meth:`Solver.open`
     - Parse the ``.inp``.
   * - :meth:`Solver.initialize`
     - Allocate state arrays.
   * - :meth:`Solver.start`
     - Start routing.
   * - :meth:`Solver.step(steps=1)`
     - Advance ``steps`` routing steps.
   * - :meth:`Solver.end`
     - Stop routing.
   * - :meth:`Solver.report`
     - Write the human-readable ``.rpt``.
   * - :meth:`Solver.close`
     - Close output files.
   * - :meth:`Solver.finalize`
     - Free engine memory.
   * - :meth:`Solver.execute`
     - Convenience: open/initialize/start/step-to-end/end/report/close.
   * - :meth:`Solver.__enter__` / ``__exit__``
     - Context-manager wrapper.
   * - :meth:`Solver.__iter__` / ``__next__``
     - Iterator over ``(time, mass_bal_err)`` tuples.

State machine
-------------

The legacy solver uses :class:`SolverState` (different from
v6's ``EngineState``):

.. code-block:: python

    from openswmm.legacy.engine import SolverState

    if solver.solver_state == SolverState.RUNNING:
        ...

----

End-to-end example
==================

.. code-block:: python

    from openswmm.legacy.engine import Solver

    # Context-manager style — easiest:
    with Solver("model.inp", "model.rpt", "model.out") as s:
        for time, mb_err in s:               # iterator stops at end of sim
            if int(time * 24) % 1 == 0:      # every hour
                print(f"t={time*24:.2f} h  mb_err={mb_err:+.4f}%")

The iterator yields ``(elapsed_days, continuity_error_percent)`` tuples
and stops naturally at the simulation end.

Manual lifecycle (when you need to inspect parsed objects between
``open`` and ``initialize``):

.. code-block:: python

    from openswmm.legacy.engine import Solver, SWMMObjects

    s = Solver("model.inp", "model.rpt", "model.out")
    s.open()
    print("nodes:", s.get_object_count(SWMMObjects.NODE))
    print("links:", s.get_object_count(SWMMObjects.LINK))

    s.initialize()
    s.start()
    while True:
        time, _ = s.step()
        if time >= s.end_datetime.timestamp():
            break
    s.end()
    s.report()
    s.close()
    s.finalize()        # free engine memory

----

Reading object state — getValue / setValue
==========================================

The legacy API is **enum-driven**: every property read or write goes
through :meth:`get_value` / :meth:`set_value` keyed by an
:class:`SWMMObjects` (object kind) and an attribute enum specific to
that kind.

.. code-block:: python

    from openswmm.legacy.engine import (
        Solver, SWMMObjects, SWMMNodeProperties, SWMMLinkProperties,
    )

    with Solver("model.inp", "model.rpt", "model.out") as s:
        # Resolve names to integer indices once
        j1 = s.get_object_index(SWMMObjects.NODE, "J1")
        c1 = s.get_object_index(SWMMObjects.LINK, "C1")

        for _ in s:
            d = s.get_value(SWMMObjects.NODE, SWMMNodeProperties.DEPTH, j1)
            q = s.get_value(SWMMObjects.LINK, SWMMLinkProperties.FLOW, c1)
            ...

The corresponding attribute enums are:

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Attribute enum
     - Object kind
   * - :class:`SWMMRainGageProperties`
     - Rain gages
   * - :class:`SWMMSubcatchmentProperties`
     - Subcatchments
   * - :class:`SWMMNodeProperties`
     - Nodes
   * - :class:`SWMMLinkProperties`
     - Links
   * - :class:`SWMMSystemProperties`
     - System-wide values

For the value-oriented type tags:

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Type enum
     - Used by
   * - :class:`SWMMNodeTypes`
     - Identify node kind from ``get_value(NODE, ..., NODE_TYPE)``.
   * - :class:`SWMMLinkTypes`
     - Identify link kind.
   * - :class:`SWMMFlowUnits`
     - Decode the ``FLOW_UNITS`` system value.

----

Common recipes
==============

Inject a lateral inflow at a node
---------------------------------

.. code-block:: python

    from openswmm.legacy.engine import (
        Solver, SWMMObjects, SWMMNodeProperties,
    )

    with Solver("model.inp", "model.rpt", "model.out") as s:
        j1 = s.get_object_index(SWMMObjects.NODE, "J1")
        for _ in s:
            s.set_value(
                SWMMObjects.NODE, SWMMNodeProperties.LATERAL_INFLOW,
                j1, 1.5,
            )

Override a rain-gage rainfall reading
-------------------------------------

.. code-block:: python

    from openswmm.legacy.engine import (
        Solver, SWMMObjects, SWMMRainGageProperties,
    )

    with Solver("model.inp", "model.rpt", "model.out") as s:
        rg1 = s.get_object_index(SWMMObjects.RAIN_GAGE, "RG1")
        for time, _ in s:
            # custom rainfall hyetograph injected each step
            r = compute_rainfall(time)
            s.set_value(
                SWMMObjects.RAIN_GAGE, SWMMRainGageProperties.RAINFALL,
                rg1, r,
            )

Continuity / mass-balance error
-------------------------------

.. code-block:: python

    runoff_err, flow_err, qual_err = s.get_mass_balance_error()
    print(f"runoff: {runoff_err:+.4f}%   routing: {flow_err:+.4f}%")

Hot start — save / load
-----------------------

.. code-block:: python

    s.save_hotstart("scenario_a.hsf")
    # ... later, in a new Solver:
    s.use_hotstart("scenario_a.hsf")

Progress callback
-----------------

.. code-block:: python

    def on_progress(fraction):
        print(f"  {fraction*100:5.1f}%")

    with Solver("model.inp", "model.rpt", "model.out",
                swmm_progress_callback=on_progress) as s:
        s.progress_callbacks_per_second = 2     # twice a second
        for _ in s:
            pass

Per-step callbacks (start, before-step, after-step, end)
--------------------------------------------------------

.. code-block:: python

    from openswmm.legacy.engine import CallbackType

    def post_step(solver):
        print("step completed at t =", solver.current_datetime)

    with Solver("model.inp", "model.rpt", "model.out") as s:
        s.add_callback(CallbackType.POST_STEP, post_step)
        for _ in s:
            pass

----

Object-oriented wrappers (LegacyNodes, LegacyLinks, …)
======================================================

For convenience, the legacy package also exposes thin Pythonic
wrappers around the enum-driven API:

* :class:`~openswmm.legacy.engine.LegacyNodes` /
  :class:`~openswmm.legacy.engine.LegacyNode`
* :class:`~openswmm.legacy.engine.LegacyLinks` /
  :class:`~openswmm.legacy.engine.LegacyLink`
* :class:`~openswmm.legacy.engine.LegacySubcatchments` /
  :class:`~openswmm.legacy.engine.LegacySubcatchment`
* :class:`~openswmm.legacy.engine.LegacyRainGages` /
  :class:`~openswmm.legacy.engine.LegacyRainGage`
* :class:`~openswmm.legacy.engine.LegacySystem`

These are not new API — they delegate to ``get_value`` / ``set_value``
under the hood.  They exist purely so existing object-oriented code
that was built against earlier :mod:`openswmm` versions continues to
work.

.. code-block:: python

    from openswmm.legacy.engine import Solver, LegacyNodes

    with Solver("model.inp", "model.rpt", "model.out") as s:
        nodes = LegacyNodes(s)
        j1 = nodes["J1"]
        for _ in s:
            print(j1.depth)

----

Exceptions
==========

The legacy solver raises :exc:`SWMMSolverException` on engine errors.
The error code → message mapping is exposed through
:class:`SWMMAPIErrors` and the helper :func:`get_error_message`.

.. code-block:: python

    from openswmm.legacy.engine import (
        Solver, SWMMSolverException, get_error_message,
    )

    try:
        with Solver("nonexistent.inp", "out.rpt", "out.out") as s:
            for _ in s:
                pass
    except SWMMSolverException as e:
        print("legacy solver failed:", e)

----

See also
========

* :doc:`output` — companion reader for the binary ``.out`` file.
* :doc:`../guide/solver` — the modern v6 ``Solver`` (recommended).
* :doc:`../migration/swmm5_to_swmm6` — translate this code to
  OpenSWMM 6.
