=====================================
Output reader  (binary ``.out`` file)
=====================================

.. note::

   **Engine:** OpenSWMM 6 — refactored.

.. currentmodule:: openswmm.engine

The :class:`OutputReader` reads a SWMM binary ``.out`` file
**independently** of any running engine. It needs only the file path
and supports the context-manager protocol.

Reference: ``openswmm_output.h``.

----

Quickstart
==========

.. code-block:: python

    from pathlib import Path
    from openswmm.engine import OutputReader, OutNodeVar, OutLinkVar

    with OutputReader(Path("model.out")) as out:
        # Metadata is typed.
        print(out.start_datetime)        # datetime
        print(out.report_step)           # timedelta
        print(out.period_count)
        print(out.flow_units)            # FlowUnits enum

        # Enum-typed variable selection; int|str object selector.
        depths = out.node_series("J1", OutNodeVar.DEPTH)
        flows  = out.link_series(0,    OutLinkVar.FLOW)

        # Time axis as datetime64[s] — plug straight into matplotlib.
        ax.plot(out.period_times, depths)

----

Lifecycle
=========

.. code-block:: python

    out = OutputReader("model.out")
    # ... use ...
    out.close()

    # Or, more typically:
    with OutputReader("model.out") as out:
        ...

The reader does **not** require an active :class:`Solver`. It can be
opened on any historic ``.out`` file.

Path argument accepts ``str``, :class:`pathlib.Path`, or any
:class:`os.PathLike`. Open failures raise :class:`FileError` (which is
also an :exc:`IOError`).

----

Metadata properties
===================

.. list-table::
   :header-rows: 1
   :widths: 28 22 50

   * - Property
     - Type
     - Meaning
   * - ``version``
     - ``int``
     - SWMM version that produced the file.
   * - ``flow_units``
     - :class:`FlowUnits`
     - Enum, not bare int.
   * - ``start_datetime``
     - :class:`~datetime.datetime`
     - Simulation start moment.
   * - ``report_step``
     - :class:`~datetime.timedelta`
     - Reporting interval.
   * - ``period_count``
     - ``int``
     - Number of reporting periods written.
   * - ``period_times``
     - ``numpy.ndarray[datetime64[s]]``
     - One moment per period. Cached after first access.
   * - ``pollutant_count``
     - ``int``
     -
   * - ``node_count`` / ``link_count`` / ``subcatchment_count``
     - ``int``
     -
   * - ``node_ids`` / ``link_ids`` / ``subcatchment_ids``
     - ``list[str]``
     - Ordered by integer index.
   * - ``error_code``
     - ``int``
     - SWMM error code in the footer; ``0`` is a clean run.

----

Reading results
===============

Per-period (all objects at one period)
--------------------------------------

.. code-block:: python

    depths_at_t5 = out.node_result(5, OutNodeVar.DEPTH)        # → np.ndarray
    flows_at_t5  = out.link_result(5, OutLinkVar.FLOW)
    runoff_at_t5 = out.subcatchment_result(5, OutSubcatchVar.RUNOFF)
    sys_runoff   = out.system_result(5, OutSystemVar.RUNOFF)   # → float

The ``var`` argument is the enum, **not** a bare integer. IntEnum
implements ``__int__``, so passing an int still works but loses the
type-check benefit.

Time series (one object across periods)
---------------------------------------

.. code-block:: python

    # By string id:
    depths = out.node_series("J1", OutNodeVar.DEPTH)

    # By integer index:
    depths = out.node_series(0, OutNodeVar.DEPTH)

    # Slice by period range (defaults to the full run):
    snip = out.node_series(0, OutNodeVar.DEPTH, start=10, end=20)

Same shape for ``link_series``, ``subcatchment_series``, and
``system_series`` (which takes no object selector — it's the
system-wide series).

Unknown ids raise :exc:`KeyError`; out-of-range periods raise
:exc:`IndexError`.

----

All-attribute dict
==================

When you want every attribute for one object at one period, the
``*_attributes`` methods return a dictionary keyed by the appropriate
enum:

.. code-block:: python

    attrs = out.node_attributes("J1", period=10)
    # Base attributes are keyed by OutNodeVar; pollutant slots by int.
    print(attrs[OutNodeVar.DEPTH], attrs[OutNodeVar.HEAD])

Pollutant concentration slots live past ``OutNodeVar.POLLUT_BASE`` and
are keyed by their integer index (because the enum doesn't enumerate
individual pollutants).

----

Node summary statistics
=======================

The C API aggregates flooding/overflow stats per node *from the .out
file* (independently of the engine-side stats):

.. code-block:: python

    stats = out.node_stats("J1")
    print(stats.max_depth)
    print(stats.max_overflow)
    print(stats.vol_flooded)       # ft³ (US) / m³ (SI)
    print(stats.time_flooded)      # seconds

These values are aggregated over **report-step** samples, so for runs
with a finer routing step the engine-side :attr:`Node.stats` is more
precise.

----

Plotting recipe
===============

``period_times`` is a ``datetime64[s]`` numpy array that matplotlib
understands without further conversion:

.. code-block:: python

    import matplotlib.pyplot as plt
    from openswmm.engine import OutputReader, OutNodeVar

    with OutputReader("model.out") as out:
        depths = out.node_series("J1", OutNodeVar.DEPTH)
        fig, ax = plt.subplots()
        ax.plot(out.period_times, depths)
        ax.set_ylabel("Depth")
        ax.set_xlabel("Time")
        fig.autofmt_xdate()
        plt.show()

----

See also
========

* :doc:`solver` — :attr:`Solver.out` is the runtime equivalent for the
  same data while the engine is still open.
* :doc:`nodes`, :doc:`links` — engine-side wrappers cover the same
  variables for the active run.
* :doc:`datetime` — SWMM DateTime ↔ Python ``datetime`` rules.
* :doc:`error_handling`.
