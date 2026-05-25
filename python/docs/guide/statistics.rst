==========
Statistics
==========

.. note::

   **Engine:** OpenSWMM 6 — refactored.  Documents
   :class:`openswmm.engine.Statistics`.

.. currentmodule:: openswmm.engine

The :class:`Statistics` class exposes per-element accumulated metrics
that the engine maintains during a run:

* **Per-node** — peak depth, peak overflow, total flooded volume,
  total flooded duration.
* **Per-link** — peak flow, peak velocity, peak filling fraction,
  total volume, surcharge duration.
* **Per-subcatchment** — total precipitation, total runoff volume,
  peak runoff.

These are zero-cost summaries — the engine accumulates them anyway,
this class just gives Python a typed read-out.

For continuity / mass-balance checks see :doc:`massbalance`.  For
post-run time series see :doc:`output_reader`.

Reference: ``openswmm_statistics.h``.

----

Class signature
===============

.. code-block:: python

    class Statistics:
        def __init__(self, solver: Solver) -> None: ...

----

Key methods
===========

Per-node
--------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Returns
   * - :meth:`node_max_depth(idx)`
     - Peak depth observed.
   * - :meth:`node_max_overflow(idx)`
     - Peak overflow flow.
   * - :meth:`node_vol_flooded(idx)`
     - Total volume that has overflowed.
   * - :meth:`node_time_flooded(idx)`
     - Cumulative time the node has been overflowing.

Per-link
--------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Returns
   * - :meth:`link_max_flow(idx)`
     - Peak flow magnitude.
   * - :meth:`link_max_velocity(idx)`
     - Peak velocity.
   * - :meth:`link_max_filling(idx)`
     - Peak ``|Q| / Q_full`` ratio.
   * - :meth:`link_vol_flow(idx)`
     - Cumulative volume routed.
   * - :meth:`link_surcharge_time(idx)`
     - Cumulative surcharge time.

Per-subcatchment
----------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Returns
   * - :meth:`subcatch_precip(idx)`
     - Total precipitation depth.
   * - :meth:`subcatch_runoff_vol(idx)`
     - Total runoff volume.
   * - :meth:`subcatch_max_runoff(idx)`
     - Peak runoff flow.

Bulk variants
-------------

Each bulk method makes a single C call and returns a contiguous
``np.ndarray[float64]``. The GIL is released for the duration of the
C call.

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Method
     - Returns
   * - :meth:`node_max_depth_bulk()`
     - All-node peak depths (``np.ndarray[float64]``).
   * - :meth:`link_max_flow_bulk()`
     - All-link peak flows.
   * - :meth:`subcatch_runoff_vol_bulk()`
     - All-subcatchment runoff volumes.
   * - :meth:`node_max_overflow_bulk()`
     - All-node peak overflow rate *(added 6.0.0)*.
   * - :meth:`node_vol_flooded_bulk()`
     - All-node total flooded volume *(added 6.0.0)*.
   * - :meth:`node_time_flooded_bulk()`
     - All-node cumulative time-flooded *(added 6.0.0)*.
   * - :meth:`subcatch_max_runoff_bulk()`
     - All-subcatchment peak runoff *(added 6.0.0)*.

Whole-network flooding summary in 4 C calls (replaces a ``4 * n_nodes``
Python loop):

.. code-block:: python

   from openswmm.engine import Nodes, Statistics

   stats   = Statistics(solver)
   nodes   = Nodes(solver)

   ids        = nodes.get_ids_bulk()
   max_over   = stats.node_max_overflow_bulk()
   vol_flood  = stats.node_vol_flooded_bulk()
   t_flood    = stats.node_time_flooded_bulk()

   # Worst-flooded node by cumulative volume:
   worst = int(vol_flood.argmax())
   print(f"{ids[worst]}: vol={vol_flood[worst]:.1f},  "
         f"peak overflow={max_over[worst]:.3f},  hours={t_flood[worst]:.2f}")

The standard memory-aliasing rule applies — ``.copy()`` if you need
to keep the array (the four pre-6.0 bulk getters share scratch
buffers; the 6.0 additions return freshly allocated arrays).

----

End-to-end example
==================

.. code-block:: python

    from openswmm.engine import Solver, Statistics, Nodes, Links, EngineState

    with Solver("model.inp", "model.rpt", "model.out") as s:
        while s.state == EngineState.RUNNING:
            if s.step() != 0:
                break
            pass                    # run to completion

        stats = Statistics(s)
        nodes = Nodes(s)
        links = Links(s)

        # node summary
        print("Node peaks:")
        for i in range(nodes.count()):
            print(
                f"  {nodes.get_id(i):<12}  "
                f"max_d={stats.node_max_depth(i):6.3f}  "
                f"flooded_vol={stats.node_vol_flooded(i):8.2f}  "
                f"flooded_t={stats.node_time_flooded(i)*24:6.2f} h"
            )

        # link summary
        print("Link peaks:")
        for i in range(links.count()):
            print(
                f"  {links.get_id(i):<12}  "
                f"max|Q|={stats.link_max_flow(i):7.3f}  "
                f"surcharge_t={stats.link_surcharge_time(i)*24:6.2f} h"
            )

----

Common recipes
==============

Top-N flooded nodes
-------------------

.. code-block:: python

    flooded = sorted(
        ((nodes.get_id(i), stats.node_vol_flooded(i))
         for i in range(nodes.count())),
        key=lambda kv: -kv[1],
    )
    for name, vol in flooded[:10]:
        if vol > 0:
            print(f"  {name:<12}  flood vol = {vol:.2f}")

Bulk peak-flow histogram
------------------------

.. code-block:: python

    import numpy as np

    peaks = stats.link_max_flow_bulk()
    bins = np.histogram(peaks, bins=10)[0]
    print("link peak-flow histogram (10 bins):", bins)

CI gate on flooding
-------------------

.. code-block:: python

    def assert_no_flooding(s):
        stats = Statistics(s)
        nodes = Nodes(s)
        for i in range(nodes.count()):
            assert stats.node_vol_flooded(i) == 0.0, (
                f"node {nodes.get_id(i)} flooded "
                f"{stats.node_vol_flooded(i):.2f} units"
            )

Compute surcharge fraction across the network
---------------------------------------------

.. code-block:: python

    sim_duration = s.get_end_time() - s.get_start_time()    # days
    surcharged = 0
    for i in range(links.count()):
        if stats.link_surcharge_time(i) / sim_duration > 0.05:
            surcharged += 1
    print(f"{surcharged}/{links.count()} links surcharged > 5% of run")

----

Bulk arrays
===========

The ``*_bulk`` family is the vectorised path. Each call returns a
contiguous ``np.ndarray[float64]`` of the indicated shape. GIL is
released during the underlying C call.

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Shape
   * - :meth:`node_max_depth_bulk`
     - ``(n_nodes,)``
   * - :meth:`link_max_flow_bulk`
     - ``(n_links,)``
   * - :meth:`subcatch_runoff_vol_bulk`
     - ``(n_subcatch,)``
   * - :meth:`node_max_overflow_bulk`
     - ``(n_nodes,)`` *(added 6.0.0)*
   * - :meth:`node_vol_flooded_bulk`
     - ``(n_nodes,)`` *(added 6.0.0)*
   * - :meth:`node_time_flooded_bulk`
     - ``(n_nodes,)`` *(added 6.0.0)*
   * - :meth:`subcatch_max_runoff_bulk`
     - ``(n_subcatch,)`` *(added 6.0.0)*

Memory-aliasing rule: the three pre-6.0 bulk methods share scratch
buffers with engine state — ``.copy()`` if you need to retain them
past the next call. The 6.0 additions return freshly allocated
arrays.

----

EngineState requirements & exceptions
=====================================

.. list-table::
   :header-rows: 1
   :widths: 30 25 45

   * - Method group
     - Required state
     - Notes
   * - all accessors
     - ``RUNNING`` (partial) or ``ENDED`` (final)
     - Statistics accumulate as the run progresses.
   * - bulk accessors
     - same as scalar
     - n/a

Common :class:`EngineError` codes:

* ``INVALID_INDEX`` — element index out of range.

----

See also
========

* :doc:`massbalance` — system-wide continuity diagnostics.
* :doc:`output_reader` — full time-series data for any element.
