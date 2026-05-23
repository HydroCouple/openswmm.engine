=====================================================
Water quality  (landuse, buildup, washoff, treatment)
=====================================================

.. note::

   **Engine:** OpenSWMM 6 — refactored.  Documents
   :class:`openswmm.engine.Quality`.

.. currentmodule:: openswmm.engine

The :class:`Quality` class manages water-quality processes that act
on top of the pollutant list (see :doc:`pollutants`):

* **Landuse** — categorical surface types attached to subcatchments.
* **Buildup** — pollutant accumulation on the surface during dry weather.
* **Washoff** — release of accumulated pollutants during runoff events.
* **Treatment** — node-based concentration reductions
  (settling, removal, custom expressions).
* **Sweeping** — periodic mechanical removal of buildup.

Reference: ``openswmm_quality.h``.

----

Class signature
===============

.. code-block:: python

    class Quality:
        def __init__(self, solver: Solver) -> None: ...

----

Key methods
===========

Landuse
-------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action / returns
   * - :meth:`landuse_count()`
     - Number of registered landuses.
   * - :meth:`landuse_index(id)`
     - Integer index for a landuse string id.
   * - :meth:`landuse_id(idx)`
     - String id for an integer index.
   * - :meth:`landuse_set_sweep_interval(idx, days)` / :meth:`landuse_get_sweep_interval`
     - Days between street sweepings.
   * - :meth:`landuse_set_sweep_removal(idx, frac)` / :meth:`landuse_get_sweep_removal`
     - Fraction of buildup removed per sweep.

Buildup
-------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action / returns
   * - :meth:`buildup_set(lu_idx, p, ...)`
     - Configure :class:`BuildupFunc` (NONE / POW / EXP / SAT) and its
       coefficients for landuse ``lu_idx``, pollutant ``p``.
   * - :meth:`buildup_get(lu_idx, p)`
     - Returns a dict of the active buildup model parameters.

Washoff
-------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action / returns
   * - :meth:`washoff_set(lu_idx, p, ...)`
     - Configure :class:`WashoffFunc` (EXP / RATING / EMC) and its
       coefficients.
   * - :meth:`washoff_get(lu_idx, p)`
     - Returns a dict of the active washoff model parameters.

Treatment
---------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action / returns
   * - :meth:`treatment_set(node_idx, p, expr)`
     - Set a treatment expression at a node for pollutant ``p``.
   * - :meth:`treatment_get(node_idx, p)`
     - Retrieve the expression text.
   * - :meth:`treatment_clear(node_idx, p)`
     - Remove the treatment.

Treatment expressions are full SWMM math expressions evaluated each
step — e.g. ``"R = 0.85 * EXP(-0.05 * HRT)"`` for first-order
removal in a settling tank.

----

End-to-end example
==================

.. code-block:: python

    from openswmm.engine import (
        Solver, Quality, Pollutants, Nodes,
        BuildupFunc, WashoffFunc,
    )

    with Solver("water_quality.inp", "wq.rpt", "wq.out") as s:
        q = Quality(s)
        polls = Pollutants(s)
        nodes = Nodes(s)

        s.open()
        # Strengthen TSS buildup on RESIDENTIAL landuse
        lu = q.landuse_index("RESIDENTIAL")
        tss = polls.get_index("TSS")
        q.buildup_set(
            lu, tss,
            func=int(BuildupFunc.SAT),
            c1=80.0,         # max buildup (lb/acre)
            c2=10.0,         # half-saturation time (days)
            normalizer=0,    # buildup per unit area
        )
        # Add settling-tank treatment at outfall OUT1
        q.treatment_set(
            nodes.get_index("OUT1"),
            tss,
            "R = 0.85 * EXP(-0.05 * HRT)",
        )
        s.initialize()
        s.start()
        while s.step():
            pass
        s.end()

----

Common recipes
==============

Add street sweeping to a landuse
--------------------------------

.. code-block:: python

    lu = q.landuse_index("RESIDENTIAL")
    q.landuse_set_sweep_interval(lu, 7.0)        # weekly
    q.landuse_set_sweep_removal(lu, 0.4)         # 40% removed per sweep

Remove a treatment expression mid-edit
--------------------------------------

.. code-block:: python

    q.treatment_clear(nodes.get_index("OUT1"), polls.get_index("TSS"))

Inspect the active buildup parameters
-------------------------------------

.. code-block:: python

    params = q.buildup_get(q.landuse_index("RESIDENTIAL"),
                           polls.get_index("TSS"))
    print(params)  # → {'func': 'SAT', 'c1': 80.0, 'c2': 10.0, ...}

Run a sensitivity sweep on washoff coefficient
----------------------------------------------

.. code-block:: python

    for c1 in [0.005, 0.01, 0.02, 0.05]:
        # Re-open / re-edit / re-run pattern
        with Solver("model.inp", f"sweep_{c1}.rpt", f"sweep_{c1}.out") as s:
            q = Quality(s); polls = Pollutants(s)
            s.open()
            q.washoff_set(
                q.landuse_index("RESIDENTIAL"),
                polls.get_index("TSS"),
                func=int(WashoffFunc.EXP),
                c1=c1,
                c2=2.0,
                cleaning_efficiency=0.0,
                bmp_efficiency=0.0,
            )
            s.initialize(); s.start()
            while s.step():
                pass
            s.end()

----

EngineState requirements & exceptions
=====================================

.. list-table::
   :header-rows: 1
   :widths: 30 25 45

   * - Method group
     - Required state
     - Notes
   * - landuse identity / count
     - ``OPENED`` or later
     - n/a
   * - buildup / washoff setters
     - ``OPENED``
     - Apply before ``initialize()`` to take effect.
   * - sweep parameter setters
     - ``OPENED`` or ``RUNNING``
     - Mid-run changes are honoured at the next sweep.
   * - treatment setters
     - ``OPENED``
     - Treatment expressions are compiled at ``initialize()``.

Common :class:`EngineError` codes:

* ``INVALID_INDEX`` — landuse / pollutant / node out of range.
* ``INVALID_TYPE``  — function code or expression rejected by the
  parser; the message includes the offending text.

----

See also
========

* :doc:`pollutants` — the pollutant list this module operates on.
* :doc:`subcatchments` — assign landuse coverage fractions
  (:meth:`Subcatchments.set_coverage`).
* :doc:`output_reader` — post-run pollutant time series.
