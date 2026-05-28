==========
Rain gages
==========

.. note::

   **Engine:** OpenSWMM 6 — refactored.

.. currentmodule:: openswmm.engine

Rain gages drive rainfall onto subcatchments. The shape mirrors
:doc:`nodes` and :doc:`links` — a collection on ``solver.gages`` that
yields :class:`Gage` wrappers.

Reference: ``openswmm_gages.h``.

----

Quickstart
==========

.. code-block:: python

    from openswmm.engine import Solver, GageDataSource, GageRainType

    with Solver("model.inp") as s:
        g = s.gages["RG1"]
        print(g.rain_type, g.data_source)

        # Read / inject runtime rainfall.
        print(g.rainfall)
        g.rainfall = 25.4

        # Bulk numpy read across all gages.
        arr = s.gages.rainfalls

----

Collection: :class:`Gages`
==========================

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Operation
     - What it does
   * - ``len(s.gages)``
     - Gage count.
   * - ``s.gages[key]``
     - Returns a :class:`Gage` (``key`` is ``int`` or ``str``).
   * - ``for g in s.gages:``
     - Yields a fresh :class:`Gage` per index.
   * - ``s.gages.get_index(id)`` / ``get_id(idx)``
     - Id ↔ index lookup.
   * - ``s.gages.add(id)``
     - Append a gage. Returns the :class:`Gage`.
   * - ``s.gages.rename(key, new_id)``
     - Rename in place. Invalidates wrappers.
   * - ``s.gages.rainfalls``
     - ``float64`` numpy array, shape ``(n_gages,)``.
   * - ``s.gages.ids``
     - ``object`` numpy array of string ids.

----

Wrapper: :class:`Gage`
======================

.. list-table::
   :header-rows: 1
   :widths: 22 18 14 46

   * - Property
     - Type
     - Mode
     - Meaning
   * - ``id``
     - ``str``
     - read-only
     -
   * - ``index``
     - ``int``
     - read-only
     -
   * - ``rain_type``
     - :class:`GageRainType`
     - read/write
     - INTENSITY / VOLUME / CUMULATIVE.
   * - ``data_source``
     - :class:`GageDataSource`
     - read/write
     - TIMESERIES / FILE.
   * - ``rainfall``
     - ``float``
     - read/write
     - Runtime rainfall.

Methods:

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - What it does
   * - ``set_rain_interval(seconds)``
     - Set the interval. Accepts ``float`` seconds or a
       :class:`~datetime.timedelta`.
   * - ``set_timeseries(ts_id)``
     - Configure ``data_source = TIMESERIES`` and bind the named time series.
   * - ``set_file(path, station_id)``
     - Configure ``data_source = FILE`` and bind the external file.

The wrapper has no sub-views (gages are simple). Staleness and equality
follow the same model as :class:`Node` / :class:`Link`.

----

See also
========

* :doc:`solver` — where ``s.gages`` comes from.
* :doc:`subcatchments` — :attr:`Subcatchment.gage` yields a :class:`Gage`.
* :doc:`error_handling`.
