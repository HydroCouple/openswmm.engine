=====================================
Output reader  (binary ``.out`` file)
=====================================

.. note::

   **Engine:** OpenSWMM 6 — refactored.  Documents
   :class:`openswmm.engine.OutputReader`.  For the legacy SWMM 5
   reader see :doc:`../legacy/output`.

.. currentmodule:: openswmm.engine

The :class:`OutputReader` reads the binary ``.out`` file produced by
either the v6 or the legacy engine.  It is independent of any running
solver — open it on a path and query results.

Reference: ``openswmm_output.h``.

----

Class signature
===============

.. code-block:: python

    class OutputReader:
        def __init__(self, path: str) -> None: ...
        def __enter__(self) -> "OutputReader": ...
        def __exit__(self, exc_type, exc_value, traceback) -> None: ...
        def close(self) -> None: ...

Use the context-manager form so the file handle closes on error:

.. code-block:: python

    with OutputReader("model.out") as out:
        ...

----

Key methods
===========

File metadata
-------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Returns
   * - :meth:`get_version()`
     - Output-file format version.
   * - :meth:`get_flow_units()`
     - :class:`FlowUnits`.
   * - :meth:`get_period_count()`
     - Number of reporting periods written.
   * - :meth:`get_start_date()`
     - Decimal-day timestamp of the first report.
   * - :meth:`get_report_step()`
     - Reporting interval in seconds.
   * - :meth:`get_period_time(period)`
     - Time at reporting period ``period``.
   * - :meth:`get_error_code()`
     - Non-zero if the engine flagged an error.

Element counts & ids
--------------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Returns
   * - :meth:`get_subcatch_count` / :meth:`get_node_count` /
       :meth:`get_link_count` / :meth:`get_pollut_count`
     - Counts per element kind.
   * - :meth:`get_subcatch_id(idx)` / :meth:`get_node_id(idx)` /
       :meth:`get_link_id(idx)`
     - String id for an integer index.

Time-series queries  (one element, slice of times)
--------------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 55 45

   * - Method
     - Returns
   * - :meth:`get_subcatch_series(subcatch_idx, var, start, end)`
     - Time series of one subcatchment variable over periods
       ``start..end`` (inclusive); ``ndarray[float32]`` of shape
       ``(end - start + 1,)``.
   * - :meth:`get_node_series(node_idx, var, start, end)`
     - Time series for one node variable.
   * - :meth:`get_link_series(link_idx, var, start, end)`
     - Time series for one link variable.
   * - :meth:`get_system_series(var, start, end)`
     - Time series for a system-level variable.

The variable enums are :class:`OutSubcatchVar`, :class:`OutNodeVar`,
:class:`OutLinkVar`, :class:`OutSystemVar`.  All ``*_series`` methods
require zero-based ``start`` / ``end`` period indices — use
``0`` and ``out.get_period_count() - 1`` for the full run.

Snapshot queries  (all elements at one time)
--------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 55 45

   * - Method
     - Returns
   * - :meth:`get_subcatch_result(period, var)`
     - One variable, all subcatchments at ``period``;
       ``ndarray[float32]`` of shape ``(n_subcatch,)``.
   * - :meth:`get_node_result(period, var)`
     - One variable, all nodes at ``period``; shape ``(n_nodes,)``.
   * - :meth:`get_link_result(period, var)`
     - One variable, all links at ``period``; shape ``(n_links,)``.
   * - :meth:`get_system_result(period, var)`
     - One system-level value at ``period`` (scalar).

Per-element attribute queries  (one element, all variables)
-----------------------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 55 45

   * - Method
     - Returns
   * - :meth:`get_subcatch_attribute(subcatch_idx, period)`
     - All output attributes for one subcatchment at ``period``;
       ``ndarray[float32]``.
   * - :meth:`get_node_attribute(node_idx, period)`
     - All output attributes for one node at ``period``.
   * - :meth:`get_link_attribute(link_idx, period)`
     - All output attributes for one link at ``period``.

Use ``*_attribute`` when you want every reported variable for a single
element (e.g. depth + head + lateral inflow + flooding + … at one
snapshot); use ``*_result`` when you want one variable across every
element.

----

End-to-end example
==================

.. code-block:: python

    from openswmm.engine import OutputReader, OutNodeVar, OutLinkVar

    with OutputReader("model.out") as out:
        n = out.get_period_count()
        print(f"{n} reporting periods, step = {out.get_report_step()} s")

        # Time series for one node and one link over the full run:
        t  = [out.get_period_time(i) for i in range(n)]
        d  = out.get_node_series(0, OutNodeVar.DEPTH,     start=0, end=n - 1)
        q  = out.get_link_series(0, OutLinkVar.FLOW_RATE, start=0, end=n - 1)
        print(f"first node depth: peak = {d.max():.3f}")
        print(f"first link flow:  peak = {q.max():.3f}")

----

Common recipes
==============

Build a NumPy depth matrix (T × n_nodes)
----------------------------------------

.. code-block:: python

    import numpy as np
    from openswmm.engine import OutputReader, OutNodeVar

    with OutputReader("model.out") as out:
        T = out.get_period_count()
        N = out.get_node_count()
        depths = np.empty((T, N), dtype=np.float32)
        for t in range(T):
            depths[t] = out.get_node_result(t, OutNodeVar.DEPTH)
        # depths.shape == (T, N) ; ready for vectorised analysis

Plot one node's depth with matplotlib
-------------------------------------

.. code-block:: python

    import matplotlib.pyplot as plt
    from openswmm.engine import OutputReader, OutNodeVar

    with OutputReader("model.out") as out:
        n = out.get_period_count()
        # Resolve "J1" to its zero-based index by scanning ids
        j1 = next(i for i in range(out.get_node_count())
                  if out.get_node_id(i) == "J1")
        t = [out.get_period_time(i) for i in range(n)]
        d = out.get_node_series(j1, OutNodeVar.DEPTH, start=0, end=n - 1)

    plt.plot(t, d); plt.title("J1 depth"); plt.xlabel("time (days)")
    plt.ylabel("depth"); plt.show()

Convert one snapshot to pandas
------------------------------

.. code-block:: python

    import pandas as pd
    from openswmm.engine import OutputReader, OutLinkVar

    with OutputReader("model.out") as out:
        last = out.get_period_count() - 1
        flows = out.get_link_result(last, OutLinkVar.FLOW_RATE)
        ids = [out.get_link_id(i) for i in range(out.get_link_count())]
    df = pd.DataFrame({"link": ids, "flow": flows}).set_index("link")

Detect peak flooding across the run
-----------------------------------

.. code-block:: python

    import numpy as np

    with OutputReader("model.out") as out:
        T = out.get_period_count()
        N = out.get_node_count()
        peak = np.zeros(N, dtype=np.float32)
        for t in range(T):
            peak = np.maximum(peak, out.get_node_result(t, OutNodeVar.OVERFLOW))

        flooded = [
            (out.get_node_id(i), peak[i])
            for i in range(N) if peak[i] > 0.0
        ]
        for name, q in sorted(flooded, key=lambda kv: -kv[1]):
            print(f"  {name:<12}  peak overflow = {q:.3f}")

----

Bulk arrays
===========

The ``*_result`` methods are the bulk surface — they return
``np.ndarray[float32]`` of the appropriate length:

* ``get_subcatch_result(period, var)`` → shape ``(n_subcatch,)``
* ``get_node_result(period, var)``     → shape ``(n_nodes,)``
* ``get_link_result(period, var)``     → shape ``(n_links,)``

For one-element-many-times queries, the ``*_series`` methods also
return ``ndarray[float32]`` — shape ``(end - start + 1,)``.

The ``*_attribute`` methods are the orthogonal slice: one element,
every output variable at the given period.  They are useful when
hooking a row of values into a tabular post-processor.

----

EngineState requirements & exceptions
=====================================

The :class:`OutputReader` is independent of the engine — there is no
:class:`EngineState` to honour.  All queries are valid as long as the
file is open.

Common exceptions:

* :exc:`FileNotFoundError`     — path does not exist.
* :exc:`ValueError`            — file format incompatible.
* :class:`EngineError`         — file present but corrupt
  (truncated, bad header, schema mismatch).

Open files are closed automatically when the context manager exits or
when the object is garbage-collected; calling :meth:`close` is safe at
any time and is a no-op on a closed reader.

----

See also
========

* :doc:`solver` — runs that produce the ``.out`` file.
* :doc:`statistics` — accumulated cumulative statistics during the
  run (peaks, durations) without needing the ``.out`` file.
* :doc:`../legacy/output` — the legacy SWMM 5 reader.
