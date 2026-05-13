==========================================
Tables  (time series, curves, patterns)
==========================================

.. note::

   **Engine:** OpenSWMM 6 — refactored.  Documents
   :class:`openswmm.engine.Tables`.

.. currentmodule:: openswmm.engine

The :class:`Tables` class is the unified entry point for SWMM's
**time-series**, **curves**, and **patterns** — the (x, y) data
collections that drive inflows, ratings, pump curves, monthly
adjustments, and more.

In SWMM nomenclature:

* **Time series** — a series of (timestamp, value) pairs (rainfall
  hyetograph, inflow hydrograph, etc.).  Indexed via the table
  surface.
* **Curves** — (x, y) tables for ratings, pump curves, storage
  shapes, etc.  Same data structure as time series in this API.
* **Patterns** — periodic multipliers (monthly / weekly /
  daily / hourly).  Counted separately via
  :meth:`pattern_count`.

Reference: ``openswmm_tables.h``.

----

Class signature
===============

.. code-block:: python

    class Tables:
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
     - Number of registered tables (time series + curves).
   * - :meth:`get_index(id)`
     - Integer index for a string id.
   * - :meth:`get_id(idx)`
     - String id for an integer index.
   * - :meth:`pattern_count()`
     - Number of registered patterns (separate counter).

Reading / writing points
------------------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action / returns
   * - :meth:`get_point_count(idx)`
     - Number of (x, y) points in this table.
   * - :meth:`get_point(idx, pt_idx)`
     - ``(x, y)`` tuple for one point.
   * - :meth:`add_point(idx, x, y)`
     - Append a new (x, y) point.
   * - :meth:`clear(idx)`
     - Empty the table (keeps it registered).

Lookup
------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action / returns
   * - :meth:`lookup(idx, x)`
     - Linear-interpolated value at ``x``.

----

End-to-end example
==================

.. code-block:: python

    from openswmm.engine import Solver, Tables

    with Solver("model.inp", "model.rpt", "model.out") as s:
        tables = Tables(s)
        print(f"{tables.count()} tables, {tables.pattern_count()} patterns")

        # Build a flow time series in Python before initialize()
        s.open()
        idx = tables.get_index("WET_WEATHER_TS")
        tables.clear(idx)                       # wipe any existing points

        # Inject a triangular hyetograph: 0 → peak at hour 2 → 0 at hour 4
        for h in range(0, 5):
            value = max(0.0, 5.0 - abs(h - 2.0) * 2.5)
            tables.add_point(idx, h * 3600.0, value)

        s.initialize()
        s.start()
        while s.step():
            pass
        s.end()

----

Common recipes
==============

Replace a time series wholesale
-------------------------------

.. code-block:: python

    new_pts = [(0.0, 0.0), (3600.0, 1.0), (7200.0, 2.5), (10800.0, 0.0)]
    idx = tables.get_index("MY_TS")
    tables.clear(idx)
    for x, y in new_pts:
        tables.add_point(idx, x, y)

Walk every point of a registered curve
--------------------------------------

.. code-block:: python

    idx = tables.get_index("PUMP_CURVE")
    for i in range(tables.get_point_count(idx)):
        x, y = tables.get_point(idx, i)
        print(f"  {x:8.3f}  →  {y:8.3f}")

Use ``lookup`` to interpolate
-----------------------------

.. code-block:: python

    rating_idx = tables.get_index("RATING_CURVE")
    flow_at_h = tables.lookup(rating_idx, 1.5)   # head = 1.5 → flow

The lookup uses linear interpolation between adjacent points.  Outside
the range it clamps to the first / last value.

----

Bulk arrays
===========

The :class:`Tables` class is point-by-point.  For high-throughput
construction of large series, build the points in NumPy then push:

.. code-block:: python

    import numpy as np

    t = np.arange(0.0, 86400.0, 60.0)            # 1 day at 1-min res
    q = np.maximum(0.0, np.sin(2 * np.pi * t / 86400.0)) * 5.0

    idx = tables.get_index("MY_TS")
    tables.clear(idx)
    for ti, qi in zip(t, q):
        tables.add_point(idx, float(ti), float(qi))

----

EngineState requirements & exceptions
=====================================

.. list-table::
   :header-rows: 1
   :widths: 30 25 45

   * - Method group
     - Required state
     - Notes
   * - read accessors
     - ``OPENED`` or later
     - n/a
   * - mutating writers (``add_point``, ``clear``)
     - ``OPENED``
     - Apply before ``initialize()`` to take effect on the run.
   * - ``lookup``
     - any state with the table populated
     - Returns 0.0 for an empty table.

----

See also
========

* :doc:`gages` — bind rain gages to time series.
* :doc:`inflows` — bind external inflows / DWF baselines to
  patterns.
* :doc:`infrastructure` — pump curves and rating curves consumed
  by hydraulic infrastructure.
