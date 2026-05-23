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
  patterns (monthly / daily / hourly / weekend).
* **Rainfall-Dependent Inflow & Infiltration (RDII)** — driven by a
  unit hydrograph attached to a sewershed area, with optional
  exponential initial-abstraction decay (``[RDII_DECAY]``).
* **Unit-hydrograph definitions (``[HYDROGRAPHS]``)** — UH parameter
  rows, gage bindings, and group metadata.

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
   :widths: 50 50

   * - Method
     - Action / returns
   * - :meth:`add_external(node_idx, constituent, ts_name="", type="FLOW", m_factor=1.0, s_factor=1.0, baseline=0.0, pattern="")`
     - Attach a time-series-driven inflow to a node.
   * - :meth:`ext_inflow_count()`
     - Number of registered external inflows.

* ``constituent``  — ``"FLOW"`` for hydraulic flow, or a pollutant ID
  for a water-quality inflow.
* ``ts_name``      — name of the driving time series (empty string for
  baseline-only).
* ``type``         — inflow-type string (default ``"FLOW"``).
* ``m_factor``     — units-conversion multiplier.
* ``s_factor``     — scaling factor.
* ``baseline``     — baseline value added on top of the time series.
* ``pattern``      — optional pattern name multiplying the baseline.

Dry-Weather Flow
----------------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Method
     - Action / returns
   * - :meth:`add_dwf(node_idx, constituent, avg_value, pat1="", pat2="", pat3="", pat4="")`
     - Attach DWF with up to 4 multiplicative patterns.
   * - :meth:`dwf_count()`
     - Number of registered DWF entries.

* ``constituent`` — ``"FLOW"`` or a pollutant ID.
* ``avg_value``   — average dry-weather flow.
* ``pat1`` / ``pat2`` / ``pat3`` / ``pat4`` — monthly / daily / hourly
  / weekend pattern names.  Pass an empty string to skip a slot.

RDII
----

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Method
     - Action / returns
   * - :meth:`add_rdii(node_idx, uh_name, area)`
     - Attach RDII at a node, driven by a named unit hydrograph and
       sewershed area.
   * - :meth:`get_rdii(entry_idx)`
     - Read back the ``(node_idx, uh_name, area)`` triple for an entry.
   * - :meth:`rdii_count()`
     - Number of registered RDII entries.

Unit hydrographs (``[HYDROGRAPHS]``)
------------------------------------

The unit-hydrograph parameter table feeds the RDII model.  Each row
defines the shape parameters (``r``, ``t``, ``k``) and initial-
abstraction defaults for a ``(uh_name, month, response)`` combination.

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Method
     - Action / returns
   * - :meth:`add_hydrograph(uh_name, month, response, r, t, k, dmax=0.0, drecov=0.0, dinit=0.0)`
     - Add a ``[HYDROGRAPHS]`` parameter row.  ``month`` is ``-1`` for
       *all months* or ``0..11`` for ``JAN..DEC``; ``response`` is
       ``0`` SHORT, ``1`` MEDIUM, ``2`` LONG.
   * - :meth:`get_hydrograph(entry_idx)` / :meth:`hydrograph_count()`
     - Read back a parameter row / count rows.
   * - :meth:`add_hydrograph_gage(uh_name, gage_name)` / :meth:`get_hydrograph_gage(idx)`
     - Bind a rain-gage to a unit-hydrograph group.
   * - :meth:`hydrograph_gage_count()` / :meth:`hydrograph_group_count()`
     - Counts for gage bindings and unique UH group names.
   * - :meth:`get_hydrograph_group_id(idx)`
     - Return the name of the *idx*-th UH group.

Exponential IA decay (``[RDII_DECAY]``)
---------------------------------------

The exponential initial-abstraction decay model replaces the legacy
linear-recovery formulation for storm-by-storm IA bookkeeping.
Depletion follows ``exp(-k_dep * rainfall)``; recovery during dry
periods uses ``k_rec(T) = k_0 + k_T * exp(theta_rec * (T - T_ref))``
with suppression when ``T <= T_freeze``.

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Method
     - Action / returns
   * - :meth:`add_rdii_decay(uh_name, response, k_dep, k_0, k_T, T_ref=10.0, theta_rec=0.0, T_freeze=0.0)`
     - Add an exponential-decay row for a ``(uh_name, response)`` pair.
   * - :meth:`get_rdii_decay(entry_idx)` / :meth:`rdii_decay_count()`
     - Read back a decay row / count rows.

----

End-to-end example
==================

.. code-block:: python

    from openswmm.engine import Solver, Inflows, Nodes, EngineState

    with Solver("site_drainage.inp", "site_drainage.rpt", "site_drainage.out") as s:
        nodes = Nodes(s)
        inflows = Inflows(s)

        node_idx = nodes.get_index("J1")

        # Attach a flow time series to J1 (during model edit phase, before initialize)
        s.open()
        inflows.add_external(
            node_idx=node_idx,
            constituent="FLOW",          # hydraulic flow
            ts_name="WET_WEATHER_TS",    # named time series
            m_factor=1.0,                # no unit conversion
            s_factor=1.0,                # no scaling
            baseline=0.05,               # cfs baseline
        )
        s.initialize()
        s.start()
        while s.state == EngineState.RUNNING:
            if s.step() != 0:
                break
        s.end()

        print(f"External inflows registered: {inflows.ext_inflow_count()}")

----

Common recipes
==============

Add Dry-Weather Flow with monthly + daily + hourly patterns
-----------------------------------------------------------

.. code-block:: python

    inflows.add_dwf(
        node_idx=nodes.get_index("J1"),
        constituent="FLOW",
        avg_value=0.5,                     # average DWF
        pat1="MONTHLY_PATTERN",
        pat2="DAILY_PATTERN",
        pat3="HOURLY_PATTERN",
        # pat4 unset -- no weekend override
    )

Add RDII to a node
------------------

.. code-block:: python

    inflows.add_rdii(
        node_idx=nodes.get_index("J1"),
        uh_name="UH1",                     # unit hydrograph defined in [HYDROGRAPHS]
        area=12.5,                         # sewershed area
    )

Define a unit hydrograph and exponential-decay row
--------------------------------------------------

.. code-block:: python

    # Build a fresh UH group for the medium-response component
    inflows.add_hydrograph(
        uh_name="UH1",
        month=-1,           # all months
        response=1,         # MEDIUM
        r=0.05, t=2.0, k=2.0,
        dmax=0.2, drecov=0.0, dinit=0.0,
    )

    # Bind the gage that drives this UH group
    inflows.add_hydrograph_gage("UH1", "RG1")

    # Replace legacy linear IA recovery with the exponential model
    inflows.add_rdii_decay(
        uh_name="UH1",
        response=1,         # MEDIUM
        k_dep=0.5,          # depletion rate per inch of rain
        k_0=0.01,           # base recovery (1/hr)
        k_T=0.005,          # thermal recovery at T_ref (1/hr)
        T_ref=10.0,         # deg C
        theta_rec=0.05,
        T_freeze=0.0,
    )

Combine baseline + scaling for unit conversion
----------------------------------------------

.. code-block:: python

    # Time series in m³/s, model in CFS:
    inflows.add_external(
        node_idx=nodes.get_index("J1"),
        constituent="FLOW",
        ts_name="INFLOW_M3S",
        m_factor=35.3147,                  # m³/s → cfs
        s_factor=1.0,
        baseline=0.0,
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

* ``INVALID_INDEX``  — node index out of range.
* ``INVALID_TYPE``   — pollutant constituent given for a non-quality model.
* ``NOT_FOUND``      — ``ts_name``, ``pattern``, ``uh_name`` or
  ``gage_name`` does not exist in the model.

----

See also
========

* :doc:`forcing` — runtime overrides that bypass the registered
  inflows.
* :doc:`tables` — define the time series and patterns that drive
  inflows.
* :doc:`nodes` — observe the inflow state during the run.
