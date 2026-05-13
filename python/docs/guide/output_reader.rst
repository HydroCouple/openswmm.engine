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

Time-series queries  (one element, all times)
---------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Method
     - Returns
   * - :meth:`get_subcatch_series(idx, var)`
     - Time series for one subcatchment variable.
   * - :meth:`get_node_series(idx, var)`
     - Time series for one node variable.
   * - :meth:`get_link_series(idx, var)`
     - Time series for one link variable.
   * - :meth:`get_system_series(var)`
     - Time series for a system-level variable.

The variable enums are :class:`OutSubcatchVar`, :class:`OutNodeVar`,
:class:`OutLinkVar`, :class:`OutSystemVar`.

Snapshot queries  (all elements, one time)
------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Method
     - Returns
   * - :meth:`get_subcatch_result(period, idx, var)` /
       :meth:`get_subcatch_attribute(period, var)`
     - One subcatchment value / all subcatchments at one time.
   * - :meth:`get_node_result(period, idx, var)` /
       :meth:`get_node_attribute(period, var)`
     - One node value / all nodes at one time.
   * - :meth:`get_link_result(period, idx, var)` /
       :meth:`get_link_attribute(period, var)`
     - One link value / all links at one time.
   * - :meth:`get_system_result(period, var)`
     - One system-level value at one time.

The ``*_attribute`` flavours return numpy arrays — use them for
post-processing every element at once.

----

End-to-end example
==================

.. code-block:: python

    from openswmm.engine import OutputReader, OutNodeVar, OutLinkVar

    with OutputReader("model.out") as out:
        n = out.get_period_count()
        print(f"{n} reporting periods, step = {out.get_report_step()} s")

        # Time series for one node and one link:
        t  = [out.get_period_time(i) for i in range(n)]
        d  = out.get_node_series(out.get_node_id(0), OutNodeVar.DEPTH)
        q  = out.get_link_series(out.get_link_id(0), OutLinkVar.FLOW_RATE)
        print(f"first node depth: peak = {max(d):.3f}")
        print(f"first link flow:  peak = {max(q):.3f}")

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
        depths = np.empty((T, N), dtype=np.float64)
        for t in range(T):
            depths[t] = out.get_node_attribute(t, OutNodeVar.DEPTH)
        # depths.shape == (T, N) ; ready for vectorised analysis

Plot one node's depth with matplotlib
-------------------------------------

.. code-block:: python

    import matplotlib.pyplot as plt
    from openswmm.engine import OutputReader, OutNodeVar

    with OutputReader("model.out") as out:
        n = out.get_period_count()
        t = [out.get_period_time(i) for i in range(n)]
        d = out.get_node_series("J1", OutNodeVar.DEPTH)

    plt.plot(t, d); plt.title("J1 depth"); plt.xlabel("time (days)")
    plt.ylabel("depth"); plt.show()

Convert one snapshot to pandas
------------------------------

.. code-block:: python

    import pandas as pd
    from openswmm.engine import OutputReader, OutLinkVar

    with OutputReader("model.out") as out:
        last = out.get_period_count() - 1
        flows = out.get_link_attribute(last, OutLinkVar.FLOW_RATE)
        ids = [out.get_link_id(i) for i in range(out.get_link_count())]
    df = pd.DataFrame({"link": ids, "flow": flows}).set_index("link")

Detect peak flooding across the run
-----------------------------------

.. code-block:: python

    import numpy as np

    with OutputReader("model.out") as out:
        T = out.get_period_count()
        N = out.get_node_count()
        peak = np.zeros(N)
        for t in range(T):
            peak = np.maximum(peak, out.get_node_attribute(t, OutNodeVar.OVERFLOW))

        flooded = [
            (out.get_node_id(i), peak[i])
            for i in range(N) if peak[i] > 0.0
        ]
        for name, q in sorted(flooded, key=lambda kv: -kv[1]):
            print(f"  {name:<12}  peak overflow = {q:.3f}")

----

Bulk arrays
===========

The ``*_attribute`` methods are the bulk surface — they return
``np.ndarray[float64]`` of the appropriate length:

* ``get_subcatch_attribute(period, var)`` → shape ``(n_subcatch,)``
* ``get_node_attribute(period, var)``     → shape ``(n_nodes,)``
* ``get_link_attribute(period, var)``     → shape ``(n_links,)``

For per-element series (one element, all times), the ``*_series``
methods return Python lists.  Wrap with ``np.asarray()`` as needed.

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
