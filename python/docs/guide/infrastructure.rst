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
   :widths: 50 50

   * - Method
     - Action
   * - :meth:`transect_count()`
     - Number of registered transects.
   * - :meth:`transect_add(transect_id)`
     - Create a new transect; returns its index.
   * - :meth:`transect_set_roughness(idx, n_left, n_right, n_channel)`
     - Manning's *n* for left bank / right bank / channel.
   * - :meth:`transect_add_station(idx, station, elevation)`
     - Append a station-elevation point.

Streets
-------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Method
     - Action
   * - :meth:`street_count()`
     - Number of registered street geometries.
   * - :meth:`street_add(street_id)`
     - Create a new street; returns its index.
   * - :meth:`street_set_params(idx, t_crown, h_curb, sx, n_road, gutter_depres, gutter_width, sides, back_width, back_slope, back_n)`
     - Crown / curb / cross-slope / Manning's *n*, gutter geometry,
       backing geometry.

Inlets
------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Method
     - Action
   * - :meth:`inlet_count()`
     - Number of registered inlets.
   * - :meth:`inlet_add(inlet_id, inlet_type)`
     - Create a new inlet; returns its index.
   * - :meth:`inlet_set_params(idx, length, width, grate_type, open_area, splash_veloc)`
     - Geometric parameters plus the grate-type identifier string
       (e.g. ``"P_BAR-50"``).

LID controls (definitions)
--------------------------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Method
     - Action
   * - :meth:`lid_count()`
     - Number of registered LID controls.
   * - :meth:`lid_add(lid_id, lid_type)`
     - Create a new LID control; returns its index.  ``lid_type`` is a
       :class:`LidType` code.
   * - :meth:`lid_set_surface(idx, storage, roughness, slope)`
     - Surface layer.
   * - :meth:`lid_set_soil(idx, thick, porosity, fc, wp, ksat, kslope)`
     - Soil layer (thickness, porosity, field capacity, wilting point,
       saturated conductivity, conductivity slope).
   * - :meth:`lid_set_storage(idx, thick, void_frac, ksat)`
     - Storage layer (thickness, void fraction, saturated conductivity).
   * - :meth:`lid_set_drain(idx, coeff, expon, offset)`
     - Underdrain (coefficient, exponent, offset height).

LID usage  (apply LID to a subcatchment)
----------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Method
     - Action
   * - :meth:`lid_usage_add(subcatch_idx, lid_idx, number, area, width, init_sat, from_imperv)`
     - Attach an LID to a subcatchment: number of units, per-unit area
       and width, initial saturation (0..1), and the fraction of
       impervious area treated (0..1).

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
            subcatch_idx=sc.get_index("S1"),
            lid_idx=0,                       # first LID control
            number=10,
            area=200.0,
            width=20.0,
            init_sat=0.0,                    # 0..1
            from_imperv=0.5,                 # 0..1: treats 50% of impervious area
        )

        from openswmm.engine import EngineState
        s.initialize()
        s.start()
        while s.state == EngineState.RUNNING:
            if s.step() != 0:
                break
        s.end()

----

Common recipes
==============

Configure a bio-retention LID control
-------------------------------------

.. code-block:: python

    # Layer parameters for "BIO_RETENTION_1" (LID control index 0)
    infra.lid_set_surface(idx=0, storage=6.0,
                          roughness=0.1, slope=0.0)
    infra.lid_set_soil(idx=0, thick=18.0, porosity=0.5,
                       fc=0.2, wp=0.05, ksat=0.5, kslope=10.0)
    infra.lid_set_storage(idx=0, thick=12.0, void_frac=0.75, ksat=0.5)
    infra.lid_set_drain(idx=0, coeff=1.0, expon=0.5, offset=6.0)

Apply an LID to multiple subcatchments
--------------------------------------

.. code-block:: python

    for sc_id in ["S1", "S3", "S5"]:
        infra.lid_usage_add(
            subcatch_idx=sc.get_index(sc_id),
            lid_idx=0,
            number=4,
            area=100.0,
            width=10.0,
            init_sat=0.0,
            from_imperv=0.3,                 # treats 30% of impervious area
        )

Add a transect with three stations
----------------------------------

.. code-block:: python

    # Trapezoidal channel: deeper in the middle
    t = infra.transect_add("TRAPEZOIDAL")
    infra.transect_set_roughness(t, n_left=0.05, n_right=0.05, n_channel=0.030)
    infra.transect_add_station(t, station=0.0,  elevation=10.0)
    infra.transect_add_station(t, station=10.0, elevation=7.0)
    infra.transect_add_station(t, station=20.0, elevation=10.0)

Configure a P_BAR-50 grate inlet
---------------------------------

.. code-block:: python

    # Create the inlet and configure its geometry + grate type.
    idx = infra.inlet_add("INLET_A", "GRATE")
    infra.inlet_set_params(
        idx=idx,
        length=3.0,                   # ft
        width=2.0,                    # ft
        grate_type="P_BAR-50",        # named grate from the SWMM library
        open_area=0.9,                # open-area fraction
        splash_veloc=4.0,             # ft/s
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
