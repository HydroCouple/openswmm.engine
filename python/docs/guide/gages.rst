==========
Rain gages
==========

.. note::

   **Engine:** OpenSWMM 6 — refactored.  Documents
   :class:`openswmm.engine.Gages`.  Legacy users see :doc:`../legacy/solver`.

.. currentmodule:: openswmm.engine

The :class:`Gages` class manages every rain gage in the model — the
sources of rainfall data that drive subcatchment runoff.  A gage may
draw rainfall from a time series, an external file (NWS / NCDC formats),
or live runtime overrides.

Reference: ``openswmm_gages.h``.

----

Class signature
===============

.. code-block:: python

    class Gages:
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
   * - :meth:`count`
     - Number of rain gages.
   * - :meth:`get_index(id)`
     - Integer index for a string id.
   * - :meth:`get_id(idx)`
     - String id for an integer index.
   * - :meth:`add(id)`
     - Append a new gage; returns its index.

Configuration
-------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action
   * - :meth:`set_rain_type(idx, type)`
     - :class:`GageRainType` (intensity / volume / cumulative).
   * - :meth:`set_rain_interval(idx, seconds)`
     - Time interval between recorded rainfall samples.
   * - :meth:`set_data_source(idx, source)`
     - :class:`GageDataSource` (timeseries vs. file).
   * - :meth:`set_timeseries(idx, ts_id)`
     - Bind to an existing time-series id.
   * - :meth:`set_filename(idx, path, station_id)`
     - Bind to an external NWS/NCDC file.

Per-step rainfall  (read & override)
------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action / returns
   * - :meth:`get_rainfall(idx)`
     - Current rainfall intensity at the gage.
   * - :meth:`set_rainfall(idx, r)`
     - Override the gage's rainfall this step (one-shot).
   * - :meth:`get_rainfall_bulk()`
     - All gage rainfall as ``np.ndarray[float64]``.

For sticky cross-step rainfall overrides, use
:meth:`Forcing.gage_rainfall` (see :doc:`forcing`).

----

End-to-end example
==================

.. code-block:: python

    from openswmm.engine import Solver, Gages

    with Solver("site_drainage.inp", "site_drainage.rpt", "site_drainage.out") as s:
        gages = Gages(s)
        print(f"{gages.count()} rain gages")

        rg1 = gages.get_index("RG1")
        peak_r, t_peak = 0.0, 0.0
        while s.step():
            r = gages.get_rainfall(rg1)
            if r > peak_r:
                peak_r, t_peak = r, s.elapsed
        print(f"RG1 peak rainfall = {peak_r:.4f} at t={t_peak*24:.2f} h")

----

Common recipes
==============

Inject a custom hyetograph (one-shot per step)
-----------------------------------------------

.. code-block:: python

    rg1 = gages.get_index("RG1")
    while s.step():
        h = s.elapsed * 24.0
        if 1.0 <= h < 4.0:
            gages.set_rainfall(rg1, 0.8)        # heavy rain hours 1-4
        else:
            gages.set_rainfall(rg1, 0.0)

Sticky override via Forcing
---------------------------

.. code-block:: python

    from openswmm.engine import Forcing, ForcingMode

    forcing = Forcing(s)
    forcing.gage_rainfall("RG1", 1.2, ForcingMode.REPLACE, persist=True)
    while s.step():
        pass
    forcing.clear_all()

Bulk rainfall snapshot for every gage
-------------------------------------

.. code-block:: python

    import numpy as np

    history = []
    while s.step():
        history.append(gages.get_rainfall_bulk().copy())
    R = np.stack(history)        # shape (T, n_gages)

Switch a gage from time-series to runtime control
-------------------------------------------------

.. code-block:: python

    from openswmm.engine import GageDataSource

    rg1 = gages.get_index("RG1")
    gages.set_data_source(rg1, int(GageDataSource.TIMESERIES))   # default
    # ... at runtime, drive via set_rainfall() each step

----

Bulk arrays
===========

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Returns / accepts
   * - :meth:`get_rainfall_bulk`
     - All gage rainfall (``np.ndarray[float64]``, shape ``(n_gages,)``).

The standard memory-aliasing rule applies: the array shares scratch
memory; call ``.copy()`` on it if you keep it across steps.

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
     - n/a
   * - configuration setters (data source, file, etc.)
     - ``OPENED``
     - Best done before ``initialize()``.
   * - ``get_rainfall``
     - ``RUNNING`` or ``ENDED``
     - n/a
   * - ``set_rainfall``
     - ``RUNNING``
     - One-shot.
   * - ``get_rainfall_bulk``
     - ``RUNNING`` or ``ENDED``
     - Same memory-aliasing rules as other ``*_bulk`` methods.

----

See also
========

* :doc:`subcatchments` — the consumers of gage rainfall.
* :doc:`forcing` — cross-step persistent rainfall overrides.
* :doc:`tables` — manage time series that gages can bind to.
