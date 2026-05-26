=============
Subcatchments
=============

.. note::

   **Engine:** OpenSWMM 6 — refactored.  This page documents the
   :class:`openswmm.engine.Subcatchments` class.  Legacy SWMM 5 users
   access subcatchments through the enum-driven ``getValue`` /
   ``setValue`` API on :class:`openswmm.legacy.engine.Solver` — see
   :doc:`../legacy/solver`.

.. currentmodule:: openswmm.engine

Sub-areas of urban land that produce runoff in response to rainfall.
The :class:`Subcatchments` class is the entry point for reading and
writing subcatchment geometry, infiltration parameters, landuse
coverage, and per-step hydrology state.

Reference: ``openswmm_subcatchments.h``.

----

Class signature
===============

.. code-block:: python

    class Subcatchments:
        def __init__(self, solver: Solver) -> None: ...

A single :class:`Subcatchments` instance covers every subcatchment in
the model.  Address by string id or integer index.

----

Key methods
===========

Identity
--------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Returns
   * - :meth:`count`
     - Number of subcatchments.
   * - :meth:`get_index(id)`
     - Integer index for a string id.
   * - :meth:`get_id(idx)`
     - String id for an integer index.
   * - :meth:`add(id)`
     - Append a new subcatchment.

Geometry & connectivity
-----------------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action / returns
   * - :meth:`get_area` / :meth:`set_area`
     - Total subcatchment area.
   * - :meth:`get_width` / :meth:`set_width`
     - Characteristic flow width.
   * - :meth:`get_slope` / :meth:`set_slope`
     - Average surface slope.
   * - :meth:`get_imperv_pct` / :meth:`set_imperv_pct`
     - Impervious-area fraction (0–100%).
   * - :meth:`get_outlet` / :meth:`set_outlet(node_idx)`
     - Receiving node index.
   * - :meth:`get_outlet_subcatch` / :meth:`set_outlet_subcatch(sc_idx)`
     - Receiving subcatchment (chained routing).
   * - :meth:`get_gage` / :meth:`set_gage(gage_idx)`
     - Rain gage index that drives this subcatchment.

Surface roughness & depression storage
--------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action / returns
   * - :meth:`get_n_imperv` / :meth:`set_n_imperv`
     - Manning's *n* over impervious area.
   * - :meth:`get_n_perv` / :meth:`set_n_perv`
     - Manning's *n* over pervious area.
   * - :meth:`get_ds_imperv` / :meth:`set_ds_imperv`
     - Depression storage on impervious area.
   * - :meth:`get_ds_perv` / :meth:`set_ds_perv`
     - Depression storage on pervious area.

Infiltration
------------

The infiltration model is selected globally in ``[OPTIONS] INFILTRATION``.
Setters write the parameters appropriate for the active model.

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action / returns
   * - :meth:`get_infil_model`
     - Active :class:`InfilModel`.
   * - :meth:`set_infil_horton(...)` / :meth:`get_infil_horton`
     - Max + min infiltration rate (``f0``, ``fmin``), decay constant
       (``decay``), drying time (``dry_time``).
   * - :meth:`set_infil_green_ampt(...)` / :meth:`get_infil_green_ampt`
     - Suction head, conductivity, initial deficit.
   * - :meth:`set_infil_curve_number(cn)` / :meth:`get_infil_curve_number`
     - SCS Curve Number.

Per-step hydrology state
------------------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Returns
   * - :meth:`get_rainfall`
     - Current rainfall intensity over the subcatchment.
   * - :meth:`get_runoff`
     - Current runoff flow leaving the subcatchment.
   * - :meth:`get_groundwater`
     - Groundwater contribution to the receiving node.
   * - :meth:`get_evap`
     - Surface evaporation flux.
   * - :meth:`get_infil`
     - Infiltration flux.
   * - :meth:`get_snow_depth`
     - Snowpack water-equivalent depth.

Cumulative statistics
---------------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Returns
   * - :meth:`get_stat_precip`
     - Total precipitation depth so far.
   * - :meth:`get_stat_runoff_vol`
     - Total runoff volume so far.
   * - :meth:`get_stat_max_runoff`
     - Peak runoff seen so far.

For a richer cumulative-statistics surface (peak rates, durations,
quality components) see :doc:`statistics`.

Forcing
-------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action
   * - :meth:`set_rainfall(idx, r)`
     - Override rainfall for this subcatchment this step (one-shot).

For sticky cross-step rainfall overrides, use
:meth:`Forcing.subcatch_rainfall` (see :doc:`forcing`).

Landuse coverage & water quality
--------------------------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action / returns
   * - :meth:`get_coverage(idx, lu)` / :meth:`set_coverage(idx, lu, frac)`
     - Fraction of subcatchment covered by landuse ``lu``.
   * - :meth:`get_quality(idx, p)`
     - Pollutant ``p`` concentration in surface runoff.
   * - :meth:`get_ponded_quality(idx, p)`
     - Pollutant ``p`` concentration in ponded surface water.

----

End-to-end example
==================

.. code-block:: python

    from openswmm.engine import Solver, Subcatchments, EngineState

    with Solver("site_drainage.inp", "site_drainage.rpt", "site_drainage.out") as s:
        sc = Subcatchments(s)
        print(f"{sc.count()} subcatchments")

        # describe each subcatchment
        for i in range(sc.count()):
            print(
                f"  {sc.get_id(i):<12}  "
                f"area={sc.get_area(i):7.3f}  "
                f"imperv={sc.get_imperv_pct(i):4.1f}%  "
                f"outlet={sc.get_outlet(i)}"
            )

        # accumulate runoff per subcatchment
        total = [0.0] * sc.count()
        dt = s.get_routing_step()
        while s.state == EngineState.RUNNING:
            if s.step() != 0:
                break
            for i in range(sc.count()):
                total[i] += sc.get_runoff(i) * dt
        for i, vol in enumerate(total):
            print(f"  {sc.get_id(i):<12}  runoff vol = {vol:.3f}")

----

Common recipes
==============

Override rainfall on one subcatchment for the run
-------------------------------------------------

One-shot per step:

.. code-block:: python

    s1 = sc.get_index("S1")
    while s.state == EngineState.RUNNING:
        if s.step() != 0:
            break
        sc.set_rainfall(s1, 0.5)        # in/hr or mm/hr per RAINFALL units

Sticky:

.. code-block:: python

    from openswmm.engine import Forcing, ForcingMode

    forcing = Forcing(s)
    forcing.subcatch_rainfall("S1", 0.5, ForcingMode.REPLACE, persist=True)
    while s.state == EngineState.RUNNING:
        if s.step() != 0:
            break
    forcing.clear_all()

Switch infiltration model parameters at runtime
-----------------------------------------------

.. code-block:: python

    # Change Horton parameters before initialize()
    s.open()
    sc.set_infil_horton(
        sc.get_index("S1"),
        f0=3.0,         # initial rate
        fmin=0.05,      # final rate
        decay=4.0,      # /hr
        dry_time=7.0,   # /day
    )
    s.initialize()

Sweep multiple coverage fractions for sensitivity analysis
----------------------------------------------------------

.. code-block:: python

    # Snippet only — wrap in a loop over scenarios.
    sc.set_coverage("S1", lu_idx=0, fraction=0.4)   # 40% landuse 0
    sc.set_coverage("S1", lu_idx=1, fraction=0.6)   # 60% landuse 1
    # remaining must sum to ≤ 1.0; the engine fills the residual with default

Per-subcatchment runoff coefficient (post-run summary)
------------------------------------------------------

.. code-block:: python

    for i in range(sc.count()):
        precip = sc.get_stat_precip(i)
        runoff = sc.get_stat_runoff_vol(i) / max(sc.get_area(i), 1e-12)
        rc = (runoff / precip) if precip > 0 else 0.0
        print(f"  {sc.get_id(i):<12}  precip={precip:.3f}  runoff={runoff:.3f}  RC={rc:.3f}")

----

Bulk arrays
===========

Each bulk method makes a single C call and returns a contiguous
``np.ndarray[float64]`` of shape ``(n_subcatchments,)`` (or a
``list[str]`` for ids). The GIL is released for the duration of the C
call, so multi-threaded consumers can read from independent solvers
in parallel.

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Returns / accepts
   * - :meth:`get_runoff_bulk`
     - Runoff rate per subcatchment (project flow units).
   * - :meth:`get_quality_bulk(p)`
     - Concentration of pollutant ``p`` per subcatchment.
   * - :meth:`get_rainfall_bulk`
     - Rainfall rate per subcatchment *(added 6.0.0)*.
   * - :meth:`get_evap_bulk`
     - Evaporation loss per subcatchment *(added 6.0.0)*.
   * - :meth:`get_infil_bulk`
     - Infiltration loss per subcatchment *(added 6.0.0)*.
   * - :meth:`get_snow_depth_bulk`
     - Snow depth per subcatchment *(added 6.0.0; placeholder zeros
       pending full snow-state integration)*.
   * - :meth:`get_ids_bulk`
     - ``list[str]`` of every subcatchment's id in one C call
       *(added 6.0.0; stride-packed UTF-8)*.

Whole-network water-balance pattern using the Phase 3 accessors:

.. code-block:: python

   ids    = sc.get_ids_bulk()
   rain   = sc.get_rainfall_bulk()
   infil  = sc.get_infil_bulk()
   evap   = sc.get_evap_bulk()
   runoff = sc.get_runoff_bulk()

   # Per-subcatch instantaneous residual (storage / snow can make this
   # noisy step-by-step but it should integrate near zero over a long
   # simulation if the model is well-balanced).
   residual = rain - infil - evap - runoff
   for i, name in enumerate(ids):
       print(f"  {name:<12}  R={rain[i]:.3f}  I={infil[i]:.3f}  "
             f"E={evap[i]:.3f}  Q={runoff[i]:.3f}  resid={residual[i]:+.3f}")

For *cumulative* (post-run) statistics, prefer the bulk accessors on
:class:`Statistics` (``subcatch_runoff_vol_bulk`` etc.) or the
:class:`OutputReader` — those are denser and pull from the report
file rather than recomputing from the live state.

----

EngineState requirements & exceptions
=====================================

.. list-table::
   :header-rows: 1
   :widths: 30 25 45

   * - Method group
     - Required state
     - Notes
   * - identity / count
     - ``OPENED`` or later
     - Identity is fixed at parse time.
   * - geometry / infiltration setters
     - ``OPENED``
     - Modify before ``initialize()`` for predictable results.
   * - hydrology accessors (``get_runoff`` etc.)
     - ``RUNNING`` or ``ENDED``
     - Need at least one runoff step.
   * - cumulative statistics
     - ``RUNNING`` or ``ENDED``
     - Accumulated since simulation start.
   * - ``set_rainfall``
     - ``RUNNING``
     - One-shot.
   * - quality accessors
     - ``RUNNING`` or ``ENDED``
     - Require at least one pollutant in the model.

Common :class:`EngineError` codes:

* ``INVALID_INDEX`` — index out of range.
* ``NOT_FOUND``     — string id not in the model.
* ``INVALID_TYPE``  — landuse / infiltration mismatch.

----

See also
========

* :doc:`gages` — rainfall sources that drive subcatchment runoff.
* :doc:`forcing` — sticky overrides on rainfall and evaporation.
* :doc:`pollutants` — the pollutants you can query via
  :meth:`get_quality`.
* :doc:`statistics` — richer accumulated subcatchment metrics.
* :doc:`output_reader` — post-run subcatchment time series.
