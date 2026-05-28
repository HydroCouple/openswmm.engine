==========================================
Tables  (time series, curves, patterns)
==========================================

.. note::

   **Engine:** OpenSWMM 6 — refactored.

.. currentmodule:: openswmm.engine

Three families of tabular objects live in the engine:

* **Time series** — ``(time, value)`` pairs where ``time`` is a
  :class:`~datetime.datetime`.
* **Curves** — generic ``(x, y)`` pairs.
* **Patterns** — periodic multiplier sequences keyed by
  :class:`PatternType`.

The first two share the C API's ``tables`` namespace and live on
``solver.tables``; patterns live on ``solver.patterns``.

Reference: ``openswmm_tables.h``.

----

Quickstart
==========

.. code-block:: python

    from datetime import datetime
    from openswmm.engine import Solver, PatternType, XSectShape

    with Solver("model.inp") as s:
        # Add a new time series and populate it.
        ts = s.tables.add_timeseries("rain1")
        ts.add(datetime(2024, 6, 15, 0, 0), 0.0)
        ts.add(datetime(2024, 6, 15, 1, 0), 0.5)

        # Add a curve.
        curve = s.tables.add_curve("storage1")
        curve.add_point(0.0, 0.0)
        curve.add_point(1.0, 100.0)
        print(curve.lookup(0.5))                # interpolation

        # Inspect existing tables — generic helper returns ._PointTable;
        # use as_timeseries / as_curve for typed views.
        ts = s.tables.as_timeseries("rain1")
        pts = ts.points                          # structured numpy array
        print(pts["time"], pts["value"])

        # Patterns.
        s.patterns.add("DLY1", PatternType.DAILY)
        s.patterns[0].set_factors([1.0]*7)

----

:class:`Tables` collection
==========================

* ``len(s.tables)`` / ``for t in s.tables:`` / ``s.tables[key]`` — standard.
* ``s.tables.add_timeseries(id)`` — returns a :class:`TimeSeries`.
* ``s.tables.add_curve(id, curve_type=0)`` — returns a :class:`Curve`.
* ``s.tables.as_timeseries(key)`` / ``as_curve(key)`` — typed view of an
  existing entry.

The C API does not currently expose a per-entry type discriminator, so
iteration yields a generic ``_PointTable``. Use the ``as_*`` helpers
when you need :class:`TimeSeries` or :class:`Curve` specifically.

----

:class:`TimeSeries`
===================

* ``.points`` returns a numpy **structured array** with fields
  ``time: datetime64[s]`` and ``value: float64``.
* ``.add(when, value)`` accepts ``when`` as :class:`~datetime.datetime`
  or a SWMM DateTime float.
* ``.lookup(x)`` runs the engine's interpolation (x can be any
  SWMM DateTime float).
* ``.add_point(x, y)`` / ``.clear()`` / ``len(ts)`` — common point-table
  operations.

----

:class:`Curve`
==============

* ``.points`` returns a ``float64`` ``(n_points, 2)`` numpy array.
* ``.add_point(x, y)`` / ``.lookup(x)`` / ``.clear()`` — common point-table
  operations.

----

:class:`Patterns` and :class:`Pattern`
======================================

``solver.patterns`` is indexable by integer only (the C API has no
id→index lookup for patterns):

.. code-block:: python

    p = s.patterns.add("DLY1", PatternType.DAILY)
    p.set_factors([1.0, 1.1, 1.2, 1.0, 0.9, 1.0, 1.0])

For monthly/daily/hourly/weekend factor counts, follow the SWMM 5
conventions (12 / 7 / 24 / 24).

----

See also
========

* :doc:`gages` — :meth:`Gage.set_timeseries` binds a gage to a
  :class:`TimeSeries`.
* :doc:`inflows` — DWF uses pattern ids; external inflows use time
  series ids.
* :doc:`datetime` — how the time axis works.
