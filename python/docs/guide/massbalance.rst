============
Mass balance
============

.. note::

   **Engine:** OpenSWMM 6 — refactored.

.. currentmodule:: openswmm.engine

Continuity errors, flux totals, and routing diagnostics for a completed
simulation. Reach the view via :attr:`Solver.mass_balance`:

.. code-block:: python

    with Solver("model.inp") as s:
        for _ in s.steps():
            pass

        mb = s.mass_balance
        print(mb.runoff_continuity_error)        # fraction
        print(mb.routing_continuity_error)       # fraction

Reference: ``openswmm_massbalance.h``.

----

Continuity errors
=================

.. list-table::
   :header-rows: 1
   :widths: 38 14 48

   * - Property / method
     - Type
     - Meaning
   * - ``runoff_continuity_error``
     - ``float``
     - Runoff continuity error (fraction).
   * - ``routing_continuity_error``
     - ``float``
     - Flow routing continuity error (fraction).
   * - ``quality_continuity_error(pollutant)``
     - ``float``
     - Per-pollutant quality continuity error (fraction); accepts id or index.

----

Flux totals
===========

The flux total accessors are enum-typed: passing an int still works
(``IntEnum``), but the enum gives you discoverability and type
checking:

.. code-block:: python

    from openswmm.engine import RunoffTotal, RoutingTotal

    mb.runoff_total(RunoffTotal.RAINFALL)
    mb.runoff_total(RunoffTotal.EVAP)
    mb.routing_total(RoutingTotal.OUTFLOW)
    mb.routing_total(RoutingTotal.FLOODING)

----

Quality mass losses
===================

.. code-block:: python

    mb.quality_seep_loss("TSS")       # cumulative mass lost to seepage
    mb.quality_evap_loss("TSS")       # cumulative mass lost to evaporation

Pollutant argument accepts id (``str``) or integer index.

----

Routing diagnostics
===================

The combined routing-solver diagnostics are exposed as a single
:class:`RoutingDiagnostics` dataclass — typed and attribute-keyed
rather than a free-form dict:

.. code-block:: python

    diag = mb.routing_diagnostics
    print(diag.avg_time_step, diag.max_courant)
    print(diag.n_steps, diag.n_steps_not_converged)
    print(diag.pct_not_converged, diag.avg_iterations)

For the bare maximum Courant number on its own:

.. code-block:: python

    print(mb.max_courant)

----

When the values are meaningful
==============================

Mass-balance results are only finalised after :meth:`Solver.end`
(equivalently: after the simulation loop terminates). Reading them
before that gives partial / zero values without raising.

----

See also
========

* :doc:`statistics` — bulk per-object cumulative statistics.
* :doc:`output_reader` — read mass-balance data from a written ``.out``
  file without an active engine.
* :doc:`error_handling`.

Snapshot accounting
===================

The three ``MassBalance`` continuity-error accessors return fractions: 0.001
means 0.1 percent. ``get_report_snapshot(solver)`` converts these to percentages
in fields named ``continuity_error_pct`` and uses the current typed routing
diagnostics. Its runoff record includes initial/final snow storage. Its routing
record includes forcing inflow, coupling outflow and link-groundwater inflow.
Forcing inflow is a subset of external inflow, so do not add those two totals.
