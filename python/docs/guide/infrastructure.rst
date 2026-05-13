==================================================
Infrastructure  (transects, streets, inlets, LIDs)
==================================================

.. note::

   **Engine:** OpenSWMM 6 — refactored.  Documents
   :class:`openswmm.engine.Infrastructure`.

.. currentmodule:: openswmm.engine

The :class:`Infrastructure` class consolidates the SWMM features that
configure complex hydraulic infrastructure:

* **Transects** — irregular channel cross-sections
  (``[TRANSECTS]``).
* **Streets** — HEC-22 street geometry for inlet capture
  (``[STREETS]``).
* **Inlets** — grate / curb / slot inlets and their capacity curves
  (``[INLETS]`` / ``[INLET_USAGE]``).
* **LIDs** — Low-Impact Development controls
  (``[LID_CONTROLS]`` / ``[LID_USAGE]``):
  bio-retention, infiltration trench, porous pavement, rain gardens,
  green roofs, swales.

Reference: ``openswmm_infrastructure.h``.

----

Class signature
===============

.. code-block:: python

    class Infrastructure:
        def __init__(self, solver: Solver) -> None: ...

----

Key methods
===========

Transects
---------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action
   * - :meth:`transect_count()`
     - Number of registered transects.
   * - :meth:`transect_set_roughness(idx, ...)`
     - Manning's *n* for left bank / channel / right bank.
   * - :meth:`transect_add_station(idx, x, elev)`
     - Append a station-elevation point.

Streets
-------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action
   * - :meth:`street_count()`
     - Number of registered street geometries.
   * - :meth:`street_set_params(idx, ...)`
     - Width, slopes, depressions, manning, sidewalk geometry.

Inlets
------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action
   * - :meth:`inlet_count()`
     - Number of registered inlets.
   * - :meth:`inlet_set_params(idx, ...)`
     - Inlet type, geometry, capacity curve, clogging.

LID controls (definitions)
--------------------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action
   * - :meth:`lid_count()`
     - Number of registered LID controls.
   * - :meth:`lid_set_surface(idx, ...)`
     - Surface layer (storage depth, roughness, slope).
   * - :meth:`lid_set_soil(idx, ...)`
     - Soil layer (thickness, porosity, conductivity, …).
   * - :meth:`lid_set_storage(idx, ...)`
     - Storage layer (gravel thickness, void ratio, …).
   * - :meth:`lid_set_drain(idx, ...)`
     - Underdrain (offset, coefficient, exponent, delay).

LID usage  (apply LID to a subcatchment)
----------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action
   * - :meth:`lid_usage_add(sc_idx, lid_idx, ...)`
     - Attach an LID to a subcatchment with area, # units, %
       impervious treated, and drainage routing.

----

End-to-end example
==================

.. code-block:: python

    from openswmm.engine import Solver, Infrastructure, Subcatchments

    with Solver("urban_streets.inp", "urb.rpt", "urb.out") as s:
        infra = Infrastructure(s)
        sc = Subcatchments(s)

        print(
            f"transects={infra.transect_count()}  "
            f"streets={infra.street_count()}  "
            f"inlets={infra.inlet_count()}  "
            f"lid_controls={infra.lid_count()}"
        )

        # During edit phase: add a bio-retention LID on subcatchment S1
        s.open()
        infra.lid_usage_add(
            sc_idx=sc.get_index("S1"),
            lid_idx=0,                       # first LID control
            number=10,
            area=200.0,
            width=20.0,
            init_sat=0.0,
            from_impervious=50.0,            # treats 50% of impervious area
            to_pervious=False,
            drain_subcatch_idx=-1,
            drain_node_idx=-1,
            full_coverage=False,
            from_pervious=0.0,
        )
        s.initialize()
        s.start()
        while s.step():
            pass
        s.end()

----

Common recipes
==============

Configure a bio-retention LID control
-------------------------------------

.. code-block:: python

    # Layer parameters for "BIO_RETENTION_1" (LID control index 0)
    infra.lid_set_surface(idx=0, depth=6.0, vol_frac=0.0,
                          roughness=0.1, slope=0.0)
    infra.lid_set_soil(idx=0, thickness=18.0, porosity=0.5,
                       fcap=0.2, wp=0.05, conductivity=0.5,
                       slope=10.0, suction=3.5)
    infra.lid_set_storage(idx=0, thickness=12.0, void_ratio=0.75,
                          k_seepage=0.5, clog_factor=0.0)
    infra.lid_set_drain(idx=0, coeff=1.0, exponent=0.5,
                        offset=6.0, delay=6.0,
                        h_open=0.0, h_close=0.0)

Apply an LID to multiple subcatchments
--------------------------------------

.. code-block:: python

    for sc_id in ["S1", "S3", "S5"]:
        infra.lid_usage_add(
            sc_idx=sc.get_index(sc_id),
            lid_idx=0,
            number=4,
            area=100.0,
            width=10.0,
            init_sat=0.0,
            from_impervious=30.0,
            to_pervious=False,
            drain_subcatch_idx=-1,
            drain_node_idx=-1,
            full_coverage=False,
            from_pervious=0.0,
        )

Add a transect with three stations
----------------------------------

.. code-block:: python

    # Trapezoidal channel: deeper in the middle
    t = 0
    infra.transect_set_roughness(t, n_left=0.05, n_channel=0.030, n_right=0.05)
    infra.transect_add_station(t, x=0.0,  elev=10.0)
    infra.transect_add_station(t, x=10.0, elev=7.0)
    infra.transect_add_station(t, x=20.0, elev=10.0)

Configure a curb-opening street inlet
-------------------------------------

.. code-block:: python

    infra.inlet_set_params(
        idx=0,
        inlet_type=int(InletType.CURB),
        height=0.5,                   # opening height
        length=10.0,
        # ... per-type parameters ...
    )

----

EngineState requirements & exceptions
=====================================

.. list-table::
   :header-rows: 1
   :widths: 30 25 45

   * - Method group
     - Required state
     - Notes
   * - all setters
     - ``OPENED``
     - Apply before ``initialize()`` to take effect on the run.
   * - count accessors
     - ``OPENED`` or later
     - n/a

Common :class:`EngineError` codes:

* ``INVALID_INDEX`` — out-of-range transect / street / inlet / LID index.
* ``INVALID_TYPE``  — wrong layer setter for the LID kind, etc.

----

See also
========

* :doc:`subcatchments` — LID usage attaches to subcatchments.
* :doc:`tables` — capacity curves consumed by inlets / pumps /
  ratings.
* :doc:`links` — conduit cross-section (``set_xsect``) for
  non-irregular shapes; transects are referenced by conduits with
  ``IRREGULAR`` cross-sections.
