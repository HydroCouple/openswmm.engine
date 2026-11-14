==========
Statistics
==========

.. note::

   **Engine:** OpenSWMM 6 — refactored.

.. currentmodule:: openswmm.engine

Bulk views of the per-object cumulative simulation statistics —
flooding, max flows, surcharge time, etc. Reach via
:attr:`Solver.statistics`.

**Single-object access** lives on the wrapper sub-views
(:attr:`Node.stats`, :attr:`Link.stats`, :attr:`Subcatchment.stats`).
This page covers the bulk numpy accessors — the right tool when you
want every node's flooded volume in one C call.

Reference: ``openswmm_statistics.h``.

----

Bulk numpy properties
=====================

Every property returns a fresh ``float64`` numpy array, shape matching
the domain count.

Node stats
----------

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Property
     - Meaning
   * - ``node_max_depth``
     - Maximum depth per node.
   * - ``node_max_overflow``
     - Maximum overflow rate per node.
   * - ``node_vol_flooded``
     - Cumulative flooded volume per node.
   * - ``node_time_flooded``
     - Cumulative flooded duration (seconds) per node.

Link stats
----------

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Property
     - Meaning
   * - ``link_max_flow``
     - Maximum flow rate per link.
   * - ``link_max_velocity``
     - Maximum velocity per link.
   * - ``link_max_filling``
     - Maximum filling ratio per link.
   * - ``link_vol_flow``
     - Cumulative flow volume per link.
   * - ``link_surcharge_time``
     - Cumulative surcharge duration (seconds) per link.

Subcatchment stats
------------------

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Property
     - Meaning
   * - ``subcatchment_runoff_vol``
     - Cumulative runoff volume per subcatchment.
   * - ``subcatchment_max_runoff``
     - Peak runoff rate per subcatchment.
   * - ``subcatchment_precip``
     - Cumulative precipitation per subcatchment.

----

Example
=======

.. code-block:: python

    from openswmm.engine import Solver

    with Solver("model.inp") as s:
        for _ in s.steps():
            pass
        stats = s.statistics
        worst_flooded = stats.node_vol_flooded.argmax()
        print(f"node {s.nodes.get_id(worst_flooded)} flooded {stats.node_vol_flooded[worst_flooded]:.1f} ft³")

----

When the values are meaningful
==============================

All of these values are cumulative aggregates finalised at
:meth:`Solver.end`. Reading mid-run yields partial values.

----

Report summary tables
=====================

The summary tables of the ``.rpt`` file (Node Depth, Node Inflow, Node
Surcharge, Node Flooding, Storage Volume, Outfall Loading, Link Flow, Flow
Classification, Conduit Surcharge, Pumping and LID Performance) can be read
after :meth:`Solver.end` and until the solver closes. The engine computes
them with the code that writes the report, so the values are the report's,
unrounded.

.. code-block:: python

    from openswmm.engine import Solver, SummaryObject, SummaryTable

    with Solver("model.inp") as s:
        for _ in s.steps():
            pass
        s.end()
        table = s.statistics.summary_table(SummaryTable.OUTFALL_LOADING)
        print(table.title, [f"{c.name} ({c.units})" for c in table.columns])
        for row, values in zip(table.rows, table.values):
            name = ("System" if row.object_type == SummaryObject.SYSTEM
                    else s.nodes.get_id(row.object_index))
            print(name, values)

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Method
     - Returns
   * - ``summary_table(table)``
     - A :class:`ReportTable`: ``title``, ``columns`` (each a heading and the
       report's unit word for this run, such as ``"CMS"``, ``"%"`` or
       ``"days"``), ``rows`` (each the object it describes and the report's
       text column, such as the node type or LID control) and ``values``, a
       ``(rows, columns)`` array with NaN where the report prints nothing.
   * - ``summary_tables()``
     - Every table, in report order, including tables without rows.
   * - ``summary_row_at(table, row)``
     - One :class:`SummaryRow`.
   * - ``summary_value_at(table, row, column)``
     - One value. Each call rebuilds the table, so read whole tables with
       ``summary_table``.

Rows are in report order. A ``SYSTEM`` row (the last row of Outfall Loading)
has ``object_index`` -1, and LID Performance rows point at the subcatchment
that holds the unit. Times of maxima are in days from the report start.
Reading a table before the run ends raises :class:`LifecycleError`.

----

See also
========

* :doc:`nodes` — :attr:`Node.stats` for one node at a time.
* :doc:`links` — :attr:`Link.stats`.
* :doc:`subcatchments` — :attr:`Subcatchment.stats`.
* :doc:`massbalance` — continuity errors and flux totals.
