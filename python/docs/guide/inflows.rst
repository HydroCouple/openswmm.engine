================
External inflows
================

.. note::

   **Engine:** OpenSWMM 6 — refactored.  Documents
   :class:`openswmm.engine.Inflows`.

.. currentmodule:: openswmm.engine

The :class:`Inflows` class registers static external inflows on nodes
at configure time:

* **External inflows** — time-series flow with optional scale/baseline.
* **Dry-Weather Flow (DWF)** — average flow with up to 4 multiplicative
  patterns (monthly / weekly / daily / hourly).
* **Rainfall-Dependent Inflow & Infiltration (RDII)** — driven by a
  unit hydrograph attached to a sewershed area.

For overrides applied **during** a running simulation (per-step
lateral inflows), see :class:`Nodes.set_lateral_inflow` (one-shot)
or :class:`Forcing.node_lat_inflow` (persistent).

Reference: ``openswmm_inflows.h``.

----

Class signature
===============

.. code-block:: python

    class Inflows:
        def __init__(self, solver: Solver) -> None: ...

----

Key methods
===========

External inflows  (time-series)
-------------------------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action / returns
   * - :meth:`add_external(node_idx, ts_idx, type, mfactor, sfactor, baseline, basepat_idx)`
     - Attach a time-series-driven inflow to a node.
   * - :meth:`ext_inflow_count()`
     - Number of registered external inflows.

* ``type``      — pollutant index, or ``-1`` for hydraulic flow.
* ``mfactor``   — units conversion multiplier.
* ``sfactor``   — scaling factor.
* ``baseline``  — baseline flow added on top of the time series.
* ``basepat_idx`` — optional pattern index multiplying the baseline.

Dry-Weather Flow
----------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action / returns
   * - :meth:`add_dwf(node_idx, type, base_value, pattern_idxs)`
     - Attach DWF with up to 4 patterns.
   * - :meth:`dwf_count()`
     - Number of registered DWF entries.

* ``type``           — pollutant index, or ``-1`` for hydraulic flow.
* ``base_value``     — average dry-weather flow.
* ``pattern_idxs``   — list of up to 4 :class:`Pattern` indices
  (monthly, daily, hourly, weekend-hourly).  Use ``-1`` to skip a slot.

RDII
----

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action / returns
   * - :meth:`add_rdii(node_idx, uh_name, area)`
     - Attach RDII at a node, driven by a named unit hydrograph and
       a sewershed area.
   * - :meth:`rdii_count()`
     - Number of registered RDII entries.

----

End-to-end example
==================

.. code-block:: python

    from openswmm.engine import Solver, Inflows, Nodes, Tables

    with Solver("site_drainage.inp", "site_drainage.rpt", "site_drainage.out") as s:
        nodes = Nodes(s)
        tables = Tables(s)
        inflows = Inflows(s)

        ts_idx = tables.get_index("WET_WEATHER_TS")
        node_idx = nodes.get_index("J1")

        # Attach a flow time series to J1 (during model edit phase, before initialize)
        s.open()
        inflows.add_external(
            node_idx=node_idx,
            ts_idx=ts_idx,
            type=-1,                  # hydraulic flow
            mfactor=1.0,              # no unit conversion
            sfactor=1.0,              # no scaling
            baseline=0.05,            # cfs baseline
            basepat_idx=-1,           # no pattern on baseline
        )
        s.initialize()
        s.start()
        while s.step():
            pass
        s.end()

        print(f"External inflows registered: {inflows.ext_inflow_count()}")

----

Common recipes
==============

Add Dry-Weather Flow with monthly + hourly patterns
---------------------------------------------------

.. code-block:: python

    monthly_idx = tables.get_index("MONTHLY_PATTERN")
    daily_idx   = tables.get_index("DAILY_PATTERN")
    hourly_idx  = tables.get_index("HOURLY_PATTERN")

    inflows.add_dwf(
        node_idx=nodes.get_index("J1"),
        type=-1,
        base_value=0.5,                    # average DWF
        pattern_idxs=[monthly_idx, daily_idx, hourly_idx, -1],
    )

Add RDII to a node
------------------

.. code-block:: python

    inflows.add_rdii(
        node_idx=nodes.get_index("J1"),
        uh_name="UH1",                     # unit hydrograph defined in [HYDROGRAPHS]
        area=12.5,                         # sewershed area
    )

Combine baseline + scaling for unit conversion
----------------------------------------------

.. code-block:: python

    # Time series in m³/s, model in CFS:
    inflows.add_external(
        node_idx=nodes.get_index("J1"),
        ts_idx=tables.get_index("INFLOW_M3S"),
        type=-1,
        mfactor=35.3147,                   # m³/s → cfs
        sfactor=1.0,
        baseline=0.0,
        basepat_idx=-1,
    )

----

Bulk arrays
===========

The :class:`Inflows` class is a registration surface, not a per-step
data accessor.  For per-step inflow values, use
:meth:`Nodes.get_lateral_inflow` (per-node) or :class:`OutputReader`
post-run (:doc:`output_reader`).

----

EngineState requirements & exceptions
=====================================

.. list-table::
   :header-rows: 1
   :widths: 30 25 45

   * - Method group
     - Required state
     - Notes
   * - registration setters (``add_*``)
     - ``OPENED``
     - Must be done before ``initialize()`` to take effect on the run.
   * - count accessors
     - ``OPENED`` or later
     - n/a

Common :class:`EngineError` codes:

* ``INVALID_INDEX``  — node, table, or pattern index out of range.
* ``INVALID_TYPE``   — pollutant index given for a non-quality model.

----

See also
========

* :doc:`forcing` — runtime overrides that bypass the registered
  inflows.
* :doc:`tables` — define the time series and patterns that drive
  inflows.
* :doc:`nodes` — observe the inflow state during the run.
