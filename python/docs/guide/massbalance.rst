============
Mass balance
============

.. note::

   **Engine:** OpenSWMM 6 — refactored.  Documents
   :class:`openswmm.engine.MassBalance`.

.. currentmodule:: openswmm.engine

The :class:`MassBalance` class exposes the engine's continuity
diagnostics:

* **Continuity error** — percentage discrepancy in the runoff,
  routing, and quality budgets.
* **Component totals** — per-component breakdown of inflow / outflow /
  storage.
* **Numerical health** — peak Courant number for the routing solver.

Use this class **at end-of-simulation** to verify the run was
numerically well-behaved before trusting downstream analysis.  A
runoff or routing continuity error larger than ±2% almost always
indicates a configuration or numerical-stability issue.

Reference: ``openswmm_massbalance.h``.

----

Class signature
===============

.. code-block:: python

    class MassBalance:
        def __init__(self, solver: Solver) -> None: ...

----

Key methods
===========

Continuity errors  (percent)
----------------------------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Method
     - Returns
   * - :meth:`get_runoff_continuity_error()`
     - Surface runoff continuity error.
   * - :meth:`get_routing_continuity_error()`
     - Pipe-network routing continuity error.
   * - :meth:`get_quality_continuity_error(p)`
     - Quality continuity error for pollutant ``p``.

Component totals
----------------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Method
     - Returns
   * - :meth:`get_runoff_total(component)`
     - Cumulative runoff component (precip / evap / infil / runoff).
   * - :meth:`get_routing_total(component)`
     - Cumulative routing component (DWF / WW / GW / RDII /
       external / flooding / outflow / evap / seep / storage).
   * - :meth:`get_routing_stats()`
     - All routing component totals as a dict.

The component selectors are :class:`RunoffTotal` and
:class:`RoutingTotal` enums.

Numerical health
----------------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Method
     - Returns
   * - :meth:`get_max_courant()`
     - Peak Courant number observed during routing.
   * - :meth:`get_quality_seep_loss(p)`
     - Cumulative pollutant ``p`` lost to seepage.
   * - :meth:`get_quality_evap_loss(p)`
     - Cumulative pollutant ``p`` lost to evaporation.

----

End-to-end example
==================

.. code-block:: python

    from openswmm.engine import Solver, MassBalance

    with Solver("model.inp", "model.rpt", "model.out") as s:
        while s.step():
            pass

        mb = MassBalance(s)
        print(f"  runoff continuity error  = {mb.get_runoff_continuity_error():+.4f}%")
        print(f"  routing continuity error = {mb.get_routing_continuity_error():+.4f}%")
        print(f"  peak Courant number      =  {mb.get_max_courant():.3f}")

        if abs(mb.get_routing_continuity_error()) > 2.0:
            print("  WARNING: routing continuity error > ±2% — review inputs.")

----

Common recipes
==============

Itemise routing totals
----------------------

.. code-block:: python

    stats = mb.get_routing_stats()
    for component, value in sorted(stats.items()):
        print(f"  {component:<28}  {value:>14.3f}")

Pollutant continuity sweep
--------------------------

.. code-block:: python

    from openswmm.engine import Pollutants

    polls = Pollutants(s)
    for i in range(polls.count()):
        err = mb.get_quality_continuity_error(i)
        seep = mb.get_quality_seep_loss(i)
        evap = mb.get_quality_evap_loss(i)
        print(f"  {polls.get_id(i):<10}  err={err:+.4f}%  seep={seep:.3f}  evap={evap:.3f}")

Numerical-stability check (CI gate)
-----------------------------------

.. code-block:: python

    def assert_continuity_ok(s):
        mb = MassBalance(s)
        assert abs(mb.get_runoff_continuity_error())  < 0.5
        assert abs(mb.get_routing_continuity_error()) < 1.0
        assert mb.get_max_courant() < 5.0

----

EngineState requirements & exceptions
=====================================

.. list-table::
   :header-rows: 1
   :widths: 30 25 45

   * - Method
     - Required state
     - Notes
   * - all accessors
     - ``ENDED``  (preferably) or ``RUNNING``
     - Mid-run values are valid but reflect the partial run; final
       values are only meaningful after :meth:`Solver.end`.
   * - quality continuity / loss
     - ``ENDED`` and at least one pollutant
     - Raises ``INVALID_INDEX`` for an out-of-range pollutant.

The :class:`MassBalance` constructor itself never fails — it just
binds to a Solver.  Errors surface from the accessors.

----

See also
========

* :doc:`statistics` — per-element peak / cumulative statistics
  (peak depth, surcharge time, etc.).
* :doc:`solver` — :meth:`Solver.report` writes a human-readable
  summary that includes these continuity errors.
