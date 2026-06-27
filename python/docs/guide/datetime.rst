=============================================
SWMM DateTime and Python ``datetime`` interop
=============================================

.. note::

   **Engine:** OpenSWMM 6 — refactored.

.. currentmodule:: openswmm.engine

SWMM's native date/time representation is a ``double`` whose **integer
part** is the number of days since **1899-12-30** (the OLE Automation /
Delphi ``TDateTime`` epoch) and whose **fractional part** is the
time-of-day fraction (``0.5`` = noon).

This is **not** an astronomical Julian Date. The epoch difference is
~2.4 million days and the precision characteristics are different.
Using "Julian" loosely is a long-standing source of confusion in SWMM
codebases; OpenSWMM 6 uses "SWMM DateTime" everywhere.

----

What you actually use
=====================

99% of users never see the raw double:

.. code-block:: python

    from openswmm.engine import Solver

    with Solver("model.inp") as s:
        s.start_datetime         # datetime.datetime, not a float
        s.report_start_datetime  # likewise
        s.end_datetime           # likewise

The conversion goes through helpers in :mod:`openswmm.engine`:

.. autofunction:: openswmm.engine.oadate_to_datetime
   :noindex:

.. autofunction:: openswmm.engine.datetime_to_oadate
   :noindex:

These two functions are calendar-arithmetic only — sub-second precision
is handled in Python so a round-trip through them is **lossless to ≤ 1
microsecond** (verified by ``scripts/verify_dates_python.py`` in the
repo).

.. code-block:: python

    from datetime import datetime
    from openswmm.engine import datetime_to_oadate, oadate_to_datetime

    dt = datetime(2024, 6, 15, 13, 30, 45, 123_456)
    assert oadate_to_datetime(datetime_to_oadate(dt)) == dt   # within 1 µs

----

When you might use the raw double
=================================

The low-level C API exposes encode / decode primitives that mirror the
legacy ``datetime_*`` C functions bit-for-bit (whole-second precision).
They are reachable from Python through :mod:`openswmm.engine.datetime_api`:

.. code-block:: python

    from openswmm.engine import datetime_api

    d = datetime_api.encode_date(2024, 6, 15)        # 45458.0 (integer days)
    t = datetime_api.encode_time(13, 30, 45)         # 0.563...
    dt_value = d + t                                  # combined SWMM DateTime

    y, m, day = datetime_api.decode_date(dt_value)
    h, mi, s  = datetime_api.decode_time(dt_value)

The full signature set is:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Function
     - Returns
   * - ``encode_date(year, month, day)``
     - Integer-valued SWMM DateTime ``float``.
   * - ``encode_time(hour, minute, second)``
     - Fractional-day ``float`` in ``[0.0, 1.0)``.
   * - ``decode_date(value)``
     - ``(year, month, day)`` tuple.
   * - ``decode_time(value)``
     - ``(hour, minute, second)`` tuple — **integer seconds**.
   * - ``add_seconds(value, seconds)``
     - SWMM DateTime ``float`` shifted by ``seconds``.
   * - ``time_diff(value1, value2)``
     - ``int`` — ``value1 - value2`` rounded to whole seconds.

When to reach for which:

* **You have a Python** ``datetime`` and **want a SWMM DateTime**:
  :func:`datetime_to_oadate` (preserves microseconds).
* **You have a SWMM DateTime** and **want a Python** ``datetime``:
  :func:`oadate_to_datetime` (preserves microseconds).
* **You're driving a C API that takes the double directly** (e.g.,
  hot-start save schedules, custom plugin code): use
  :mod:`openswmm.engine.datetime_api` for the integer-second primitives.

----

Precision notes
===============

* The double has ~15 significant decimal digits. For dates in the
  2000-2100 range that's a precision floor of ~10 nanoseconds — well
  below the microsecond resolution Python's ``datetime`` exposes.
* :func:`datetime_to_oadate` / :func:`oadate_to_datetime` are exact
  for whole-second values, and lossless to **≤ 1 µs** for values with
  microseconds (verified in ``scripts/verify_dates_python.py``).
* The C API's ``decode_time`` rounds via
  ``floor(fracDay * 86400 + 0.5)``, matching the legacy engine
  bit-for-bit. Use it when you need parity with a legacy SWMM run;
  use :func:`oadate_to_datetime` when you want sub-second fidelity.

----

Durations vs. moments
=====================

OpenSWMM 6 distinguishes the two more carefully than legacy SWMM did:

* **Moments** (the start of the simulation, the time of a save) are
  Python ``datetime`` objects throughout the public API.
* **Durations** (a timestep, an elapsed time, the report step) are
  Python ``timedelta`` objects.

The conversion is straightforward when you need to drop into the C API
yourself:

.. code-block:: python

    from datetime import timedelta

    dt_seconds = 60.0
    routing_step = timedelta(seconds=dt_seconds)   # for Python consumers
    # ... or back ...
    dt_seconds = routing_step.total_seconds()

----

See also
========

* :doc:`concepts` — the engine lifecycle and where dates show up.
* :doc:`error_handling` — what happens when an out-of-range date is
  passed to a setter.
* :doc:`solver` — the :class:`Solver` properties that surface dates.
