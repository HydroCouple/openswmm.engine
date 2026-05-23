==============================================
Legacy SWMM 5 Output Reader
==============================================

.. note::

   **Engine:** SWMM 5.x — *legacy*.  This page documents
   :class:`openswmm.legacy.output.Output` — the binary ``.out`` file
   reader that ships with the legacy compatibility layer.  For the
   OpenSWMM 6 reader see :doc:`../guide/output_reader`.

The :class:`Output` class reads the binary results file produced by:

* The legacy SWMM 5.x solver (:class:`openswmm.legacy.engine.Solver`).
* The new OpenSWMM 6 engine (:class:`openswmm.engine.Solver`) — both
  engines write the same on-disk format.

You can therefore use this reader with results from either engine.  In
new code, however, the ergonomics of
:class:`openswmm.engine.OutputReader` (typed enums, simpler element
addressing, bulk-array methods) are usually preferable.

----

Class signature
===============

.. code-block:: python

    from openswmm.legacy.output import Output

    class Output:
        def __init__(self, output_file: str) -> None: ...
        def __enter__(self): ...
        def __exit__(self, exc_type, exc_value, traceback): ...

* ``output_file`` — path to a SWMM ``.out`` binary results file.

The class supports the context-manager protocol — using ``with`` is
strongly recommended so the file handle closes on error.

----

Key methods
===========

File-level metadata
-------------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Returns
   * - :meth:`version`
     - Output-file format version.
   * - :meth:`output_size`
     - ``{element_type: count}`` mapping.
   * - :meth:`units`
     - ``(unit_system, flow_units, [pollutant_units])``.
   * - :meth:`flow_units`
     - :class:`FlowUnits`.
   * - :meth:`pollutant_units`
     - :class:`ConcentrationUnits` per pollutant.
   * - :meth:`start_date`
     - ``datetime`` of the first reporting period.
   * - :meth:`times`
     - ``List[datetime]`` of every reporting time.

Element / variable enumeration
------------------------------

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Method
     - Returns
   * - :meth:`get_num_variables(element_type)`
     - Number of variables for that kind.
   * - :meth:`get_variable_codes(element_type)`
     - Variable codes for that kind.
   * - :meth:`get_num_properties(element_type)`
     - Number of element properties.
   * - :meth:`get_property_codes(element_type)`
     - Property codes for that kind.
   * - :meth:`get_property_value(et, idx, code)`
     - Property value for one element.
   * - :meth:`get_element_name(et, idx)`
     - Name of element ``idx``.
   * - :meth:`get_element_names(element_type)`
     - All element names of that kind.

The :class:`ElementType` enum identifies what's being queried —
``ElementType.SUBCATCH``, ``NODE``, ``LINK``, ``SYSTEM``, ``POLLUTANT``.

Time-series queries  (one element, all times)
---------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Method
     - Returns
   * - :meth:`get_subcatchment_timeseries`
     - ``Dict[datetime, float]`` for one subcatch attribute.
   * - :meth:`get_node_timeseries`
     - ``Dict[datetime, float]`` for one node attribute.
   * - :meth:`get_link_timeseries`
     - ``Dict[datetime, float]`` for one link attribute.
   * - :meth:`get_system_timeseries`
     - ``Dict[datetime, float]`` for one system-level attribute.

Snapshot queries  (all elements, one time)
------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 55 45

   * - Method
     - Returns
   * - :meth:`get_subcatchment_values_by_time_and_attribute`
     - ``Dict[name, value]``
   * - :meth:`get_node_values_by_time_and_attribute`
     - ``Dict[name, value]``
   * - :meth:`get_link_values_by_time_and_attribute`
     - ``Dict[name, value]``

The corresponding attribute enums are:

* :class:`SubcatchAttribute`
* :class:`NodeAttribute`
* :class:`LinkAttribute`
* :class:`SystemAttribute`
* :class:`TimeAttribute`

----

End-to-end example
==================

.. code-block:: python

    from openswmm.legacy.output import Output, NodeAttribute, ElementType

    with Output("model.out") as out:
        print("Format version:", out.version())
        print("Flow units:    ", out.flow_units().name)
        print("Reporting times:", len(out.times()))
        print("Element counts: ", out.output_size())

        # Time series for one node:
        depth_series = out.get_node_timeseries("J1", NodeAttribute.DEPTH)
        peak_t, peak_d = max(depth_series.items(), key=lambda kv: kv[1])
        print(f"J1 peak depth: {peak_d:.3f} at {peak_t}")

        # Snapshot for every node at the final reporting time:
        last_t = len(out.times()) - 1
        depths_at_end = out.get_node_values_by_time_and_attribute(
            last_t, NodeAttribute.DEPTH,
        )
        for name, d in sorted(depths_at_end.items()):
            print(f"  {name:<12}  {d:.3f}")

----

Common recipes
==============

Plot one node's depth time-series with matplotlib
--------------------------------------------------

.. code-block:: python

    import matplotlib.pyplot as plt
    from openswmm.legacy.output import Output, NodeAttribute

    with Output("model.out") as out:
        series = out.get_node_timeseries("J1", NodeAttribute.DEPTH)
        plt.plot(list(series.keys()), list(series.values()))
        plt.title("J1 depth")
        plt.xlabel("time")
        plt.ylabel("depth")
        plt.show()

Convert a snapshot to a pandas DataFrame
-----------------------------------------

.. code-block:: python

    import pandas as pd
    from openswmm.legacy.output import Output, LinkAttribute

    with Output("model.out") as out:
        rows = []
        for t_idx, t in enumerate(out.times()):
            row = out.get_link_values_by_time_and_attribute(
                t_idx, LinkAttribute.FLOW_RATE,
            )
            row["time"] = t
            rows.append(row)
        df = pd.DataFrame(rows).set_index("time")

System-wide cumulative continuity
---------------------------------

.. code-block:: python

    from openswmm.legacy.output import Output, SystemAttribute

    with Output("model.out") as out:
        ts = out.get_system_timeseries(SystemAttribute.RUNOFF_FLOW)
        total_runoff = sum(ts.values()) * (
            (out.times()[1] - out.times()[0]).total_seconds()
        )
        print(f"Total runoff volume ≈ {total_runoff:.0f}")

----

Exceptions
==========

The reader raises :exc:`SWMMOutputException` on file-format errors
(corrupt header, mismatched version, missing pollutant section, etc.).
A common cause is opening a partial file written by an aborted
simulation — only files whose last write was through ``close()`` are
guaranteed consistent.

.. code-block:: python

    from openswmm.legacy.output import Output, SWMMOutputException

    try:
        with Output("aborted.out") as out:
            ...
    except SWMMOutputException as e:
        print("output file unreadable:", e)

----

See also
========

* :doc:`solver` — the legacy ``Solver`` that produces the ``.out`` file.
* :doc:`../guide/output_reader` — the modern v6 ``OutputReader``.
* :doc:`../migration/swmm5_to_swmm6` — translate this code to
  OpenSWMM 6.
