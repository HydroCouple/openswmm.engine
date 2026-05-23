==========
Pollutants
==========

.. note::

   **Engine:** OpenSWMM 6 — refactored.  Documents
   :class:`openswmm.engine.Pollutants`.

.. currentmodule:: openswmm.engine

The :class:`Pollutants` class manages the list of modeled pollutants
and their solute properties (decay rate, rainfall / groundwater /
RDII concentrations, co-pollutant pairing, snow-only flag, molecular
weight).  It is also the gateway for injecting pollutant
concentrations into the model at runtime.

For the kinetics — buildup, washoff, treatment, sweeping — see
:doc:`quality`.

Reference: ``openswmm_pollutants.h``.

----

Class signature
===============

.. code-block:: python

    class Pollutants:
        def __init__(self, solver: Solver) -> None: ...

----

Key methods
===========

Identity
--------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Returns
   * - :meth:`count()`
     - Number of registered pollutants.
   * - :meth:`get_index(id)`
     - Integer index for a string id.
   * - :meth:`get_id(idx)`
     - String id for an integer index.

Pollutant properties  (read & write)
------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action / returns
   * - :meth:`get_units(idx)`
     - :class:`ConcentrationUnits` (mg/L, μg/L, count/L).
   * - :meth:`get_kdecay` / :meth:`set_kdecay`
     - First-order decay constant (per day).
   * - :meth:`get_rain_conc` / :meth:`set_rain_conc`
     - Concentration in incoming rainfall.
   * - :meth:`get_gw_conc` / :meth:`set_gw_conc`
     - Concentration in groundwater inflow.
   * - :meth:`get_init_conc` / :meth:`set_init_conc`
     - Initial concentration in the system.
   * - :meth:`get_rdii_conc` / :meth:`set_rdii_conc`
     - Concentration in RDII inflows.
   * - :meth:`get_mwt` / :meth:`set_mwt`
     - Molecular weight (used by some quality models).
   * - :meth:`get_co_pollutant` / :meth:`set_co_pollutant`
     - ``(other_pollut_idx, fraction)`` for a co-pollutant relationship.
   * - :meth:`get_snow_only` / :meth:`set_snow_only`
     - Pollutant only released by snowmelt.

Runtime injection
-----------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action
   * - :meth:`set_node_quality(node_idx, p, value)`
     - Set the concentration of pollutant ``p`` at a node (one-shot).
   * - :meth:`set_link_quality(link_idx, p, value)`
     - Set the concentration of pollutant ``p`` in a link (one-shot).

For sticky cross-step injections, use
:meth:`Forcing.node_quality` (see :doc:`forcing`).  For mass-flux
injection (rather than concentration) at a node, see
:meth:`Nodes.set_quality_mass_flux` in :doc:`nodes`.

----

End-to-end example
==================

.. code-block:: python

    from openswmm.engine import Solver, Pollutants, Nodes

    with Solver("water_quality.inp", "wq.rpt", "wq.out") as s:
        polls = Pollutants(s)
        nodes = Nodes(s)

        print(f"{polls.count()} pollutants:")
        for i in range(polls.count()):
            print(
                f"  {polls.get_id(i):<10}  "
                f"k={polls.get_kdecay(i):5.3f}/d  "
                f"init={polls.get_init_conc(i):6.2f}"
            )

        tss = polls.get_index("TSS")
        outfall = nodes.get_index("OUT1")
        history = []
        while s.step():
            history.append(nodes.get_quality(outfall, tss))
        print(f"Mean outfall TSS: {sum(history)/len(history):.2f}")

----

Common recipes
==============

Bump a decay constant for sensitivity analysis
----------------------------------------------

.. code-block:: python

    base_k = polls.get_kdecay("TSS")
    polls.set_kdecay("TSS", base_k * 1.5)
    # ... run ...
    polls.set_kdecay("TSS", base_k)             # restore for next sweep

Inject a pollutant at a node mid-run
------------------------------------

.. code-block:: python

    j1, tss = nodes.get_index("J1"), polls.get_index("TSS")
    while s.step():
        h = s.elapsed * 24.0
        if 1.0 <= h <= 2.0:
            polls.set_node_quality(j1, tss, 50.0)   # mg/L for one hour

For a persistent injection, prefer :meth:`Forcing.node_quality`.

Set up a co-pollutant relationship
----------------------------------

.. code-block:: python

    # Assume TSS is already defined; couple FECAL to TSS
    fecal = polls.get_index("FECAL")
    tss = polls.get_index("TSS")
    polls.set_co_pollutant(fecal, (tss, 0.05))   # FECAL = 5% of TSS

----

EngineState requirements & exceptions
=====================================

.. list-table::
   :header-rows: 1
   :widths: 30 25 45

   * - Method group
     - Required state
     - Notes
   * - identity / count
     - ``OPENED`` or later
     - Pollutant count is fixed at parse time.
   * - property setters
     - ``OPENED``
     - Best applied before ``initialize()``.
   * - runtime injection
     - ``RUNNING``
     - One-shot per call.

Common :class:`EngineError` codes:

* ``INVALID_INDEX`` — pollutant / node / link index out of range.
* ``INVALID_TYPE``  — calling an injection method on a model with
  no pollutants.

----

See also
========

* :doc:`quality` — buildup / washoff / treatment kinetics.
* :doc:`forcing` — persistent runtime concentration overrides.
* :doc:`nodes` — :meth:`Nodes.set_quality_mass_flux` for mass-flux
  injection.
* :doc:`output_reader` — post-run pollutant time series.
