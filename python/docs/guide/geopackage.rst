==============
GeoPackage I/O
==============

.. note::

   **Engine:** OpenSWMM 6 — refactored. The
   :class:`openswmm.engine._geopackage.GeoPackage` reader is only available
   when the engine was built with ``OPENSWMM_WITH_GEOPACKAGE=ON``. Guard for
   it with :attr:`openswmm.engine.HAS_GEOPACKAGE`.

.. currentmodule:: openswmm.engine

A GeoPackage (``.gpkg``) is a single-file SQLite database holding model
geometry, simulation results, and observed data. :class:`GeoPackage`
opens one for reading results and writing observed series — it is a
standalone object (not reached through a Solver) and works as a context
manager:

.. code-block:: python

    from openswmm.engine import GeoPackage, HAS_GEOPACKAGE

    if HAS_GEOPACKAGE:
        with GeoPackage("results.gpkg") as gpkg:
            for sim_id in gpkg.simulation_ids():
                print(sim_id, gpkg.object_counts(sim_id))

Reference: ``openswmm_geopackage.h``.

----

Quickstart
==========

.. code-block:: python

    from openswmm.engine import GeoPackage

    with GeoPackage("results.gpkg") as gpkg:
        sim = gpkg.simulation_ids()[0]

        # Read a result time series as numpy arrays.
        times, values = gpkg.read_result_ts(sim, "node", "J1", "depth")

        # Read a single summary statistic.
        peak = gpkg.read_summary(sim, "node", "J1", "max_depth")

----

Reading results
===============

.. list-table::
   :header-rows: 1
   :widths: 44 56

   * - Member
     - What it returns
   * - ``simulation_count()`` / ``simulation_ids()``
     - Number of runs / list of run IDs in the file.
   * - ``object_counts(sim_id)``
     - ``dict`` of model-object counts for a run.
   * - ``variable_count()``
     - Number of output variables defined.
   * - ``topology_edge_count(sim_id)``
     - Number of topology edges for a run.
   * - ``result_ts_count(...)``
     - Number of records matching a result query.
   * - ``read_result_ts(sim_id, obj_type, obj_id, variable)``
     - Result time series as numpy ``(timestamps, values)`` (GIL released).
   * - ``read_summary(sim_id, obj_type, obj_id, variable)``
     - A single summary statistic value.

----

Observed data
=============

Write measured series alongside the simulated results for calibration and
comparison:

.. list-table::
   :header-rows: 1
   :widths: 44 56

   * - Member
     - What it does
   * - ``create_observed_series(name, variable, obj_type='', obj_id='', source='', units='')``
     - Create a series; returns its integer ``series_id``.
   * - ``write_observed_value(series_id, timestamp, value, flag='')``
     - Write a single point.
   * - ``write_observed_values(series_id, timestamps, values, flags=None)``
     - Bulk-write points (GIL released).
   * - ``observed_series_count()`` / ``observed_value_count(series_id)``
     - Series count / point count.
   * - ``read_observed_values(series_id)``
     - Read a series back as numpy arrays.

----

Transactions & raw SQL
======================

Wrap bulk writes in a transaction for speed, and drop to read-only SQL for
ad-hoc queries:

.. code-block:: python

    with GeoPackage("results.gpkg") as gpkg:
        sid = gpkg.create_observed_series("obs_J1", "depth",
                                          obj_type="node", obj_id="J1",
                                          units="m")
        gpkg.begin()
        gpkg.write_observed_values(sid, timestamps, values)
        gpkg.commit()                      # or gpkg.rollback()

        n = gpkg.query_int("SELECT COUNT(*) FROM gpkg_contents")
        x = gpkg.query_double("SELECT MAX(value) FROM observed_values")

``begin`` / ``commit`` / ``rollback`` manage the transaction;
``query_int`` / ``query_double`` execute a read-only query and return the
first column of the first row. The :attr:`GeoPackage.last_error` property
carries the last library error message.

----

See also
========

* :doc:`output_reader` — reading the binary ``.out`` file.
* :doc:`statistics` — in-memory cumulative statistics.
* :doc:`error_handling` — exception types.
